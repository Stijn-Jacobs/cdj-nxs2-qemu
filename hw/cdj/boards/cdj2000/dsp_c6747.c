/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dsp_host.h"
#include "cdj_getenv.h"
#include "dsp_edma3.h"
/*
 * The CDJ-2000NXS's DSP: a C674x (C6747 class) that MAIN boots and feeds
 * through its host port (UHPI). The C674x instruction set is a subset of the
 * C66x's, so the project's C66x core runs it; this file is the chip around
 * that core as far as the boot needs it.
 *
 * MAIN's side, its area 3: four 32-bit registers 256 KiB apart --
 *   +0x00000 HPIC  control (HWOB, DSPINT, HINT, HRDY)
 *   +0x40000 HPIA  the DSP address the next data access goes to
 *   +0x80000 HPID  data, HPIA advancing by 4 after each access
 *   +0xC0000 HPID  data, HPIA left alone
 * The HPI bus is 16 bits wide, so each 32-bit access is two half-word cycles
 * and a register write carries the same half-word twice (HPIC 0x00010001).
 *
 * The boot, as the firmware does it: MAIN writes the first image into L2 at
 * 0x11801DA0 and its entry into the boot word at 0x11800000, then sets
 * DSPINT, which starts the DSP there. That image is a resident loader: it
 * clears a mailbox at 0x1183FFF0/F4, then takes the second image in 32 KiB
 * chunks through a window at 0x11837800, copying each to SDRAM at 0xC0000000.
 * The loader takes its orders on two GPIO pins (bank 4/5 bits 5 and 2, read
 * by 0x118027EC) and answers each by raising HINT (0x11802918); it never
 * writes a GPIO output. The handshake itself is in dsp_host.c.
 *
 * Unmodelled DSP bus addresses read 0; the busiest are listed at exit.
 *
 *   CDJ_C6747_MHZ=<n>       DSP clock (default 300)
 *   CDJ_C6747=0             no core: the host port is plain memory
 *   CDJ_C6X_RECORD=<path>   record what the core receives, for c6xreplay
 *   CDJ_HPI_DUMP=<dir>      at exit, save each run of host-port writes as
 *                           <dir>/hpi_<addr>.bin, and L2 and the first MiB
 *                           of SDRAM as <dir>/l2.bin and <dir>/sdram.bin
 */

#define L2_BASE         0x11800000u
#define L2_ALIAS        0x00800000u
#define L2_SIZE         (256 * KiB)
#define L1P_BASE        0x11E00000u
#define L1D_BASE        0x11F00000u
#define L1_SIZE         (32 * KiB)
#define SHRAM_BASE      0x80000000u
#define SHRAM_SIZE      (128 * KiB)
#define SDRAM_BASE      0xC0000000u
#define SDRAM_SIZE      (64 * MiB)

/* MAIN's DSPINT is INT15: the application maps event 0x22 there (INTMUX3 =
 * 0x220E0D0C) and enables INT4, INT8 and INT15. With the line left unwired
 * MAIN's mailbox poll never gets an answer and the deck shows E-8302. */
#define DSPINT_IRQ      15
#define EDMA_IRQ        8

#define BOOT_WORD       0x11800000u
#define UHPI_BASE       0x01E10000u
#define UHPI_HPIC       (UHPI_BASE + 0x30)

/* GPIO: five bank pairs, 0x28 apart from +0x10. */
#define GPIO_BASE       0x01E26000u
#define GPIO_PAIRS      5
#define GPIO_DIR        0x00            /* 1 = input */
#define GPIO_OUT        0x04
#define GPIO_SET        0x08
#define GPIO_CLR        0x0C
#define GPIO_IN         0x10
#define GPIO_CMD_PAIR   2               /* banks 4/5 */
#define GPIO_CMD_BIT0   (1u << 5)       /* MAIN's command bit 0 */
#define GPIO_CMD_BIT1   (1u << 2)       /* MAIN's command bit 1 */

#define MCASP_BASE      0x01D00000u
#define MCASP_STRIDE    0x4000

#define SPI1_SPIBUF     0x01E12040u

#define MAX_RUNS        64
#define MAX_POLLS       16

typedef struct HpiRun {
    uint32_t start, end;        /* [start, end) in DSP bytes */
} HpiRun;

typedef struct CdjC6747 {
    MemoryRegion iomem;
    uint8_t *l2, *l1p, *l1d, *shram, *sdram;
    CdjDspHost host;

    uint32_t hpia;
    uint64_t writes, reads, dspints, dropped;
    HpiRun runs[MAX_RUNS];
    unsigned nruns;
    bool runs_overflowed;
    DspAddrTable polls;

    uint32_t gpio_dir[GPIO_PAIRS], gpio_out[GPIO_PAIRS], gpio_ext[GPIO_PAIRS];
    DspMcasps mcasps;
    CdjEdma3 edma;

    Notifier exit;
} CdjC6747;

static CdjC6747 c6747;

/* The host pointer behind a DSP address, NULL for anything that is not RAM. */
static uint8_t *dsp_ram(CdjC6747 *s, uint32_t addr)
{
    if (addr - L2_BASE < L2_SIZE) {
        return s->l2 + (addr - L2_BASE);
    }
    if (addr - L2_ALIAS < L2_SIZE) {
        return s->l2 + (addr - L2_ALIAS);
    }
    if (addr - SDRAM_BASE < SDRAM_SIZE) {
        return s->sdram + (addr - SDRAM_BASE);
    }
    if (addr - SHRAM_BASE < SHRAM_SIZE) {
        return s->shram + (addr - SHRAM_BASE);
    }
    if (addr - L1P_BASE < L1_SIZE) {
        return s->l1p + (addr - L1P_BASE);
    }
    if (addr - L1D_BASE < L1_SIZE) {
        return s->l1d + (addr - L1D_BASE);
    }
    return NULL;
}

static bool gpio_reg(uint32_t addr, unsigned *pair, unsigned *reg)
{
    uint32_t off = addr - GPIO_BASE - 0x10;

    if (addr < GPIO_BASE + 0x10 || off >= GPIO_PAIRS * 0x28) {
        return false;
    }
    *pair = off / 0x28;
    *reg = off % 0x28;
    return true;
}

static uint32_t gpio_in(CdjC6747 *s, unsigned pair)
{
    return (s->gpio_ext[pair] & s->gpio_dir[pair])
         | (s->gpio_out[pair] & ~s->gpio_dir[pair]);
}

static void gpio_write(CdjC6747 *s, unsigned pair, unsigned reg, uint32_t val)
{
    switch (reg) {
    case GPIO_DIR:
        s->gpio_dir[pair] = val;
        break;
    case GPIO_OUT:
        s->gpio_out[pair] = val;
        break;
    case GPIO_SET:
        s->gpio_out[pair] |= val;
        break;
    case GPIO_CLR:
        s->gpio_out[pair] &= ~val;
        break;
    }
}

static uint8_t *edma_ram(void *opaque, uint32_t addr)
{
    return dsp_ram(opaque, addr);
}

/* The core keeps predecoded code and records every store it did not make
 * itself, so it has to hear about each transfer into its RAM. */
static void edma_stored(void *opaque, uint32_t addr, uint32_t len)
{
    CdjC6747 *s = opaque;

    c66x_invalidate(s->host.core, addr, len);
}

static void edma_write_word(void *opaque, uint32_t addr, uint32_t val)
{
    CdjC6747 *s = opaque;

    cdj_dsp_mcasp_write(&s->mcasps, addr, val);
}

static void edma_set_irq(void *opaque, int level)
{
    CdjC6747 *s = opaque;

    c66x_set_irq(s->host.core, EDMA_IRQ, level);
}

static void edma_after_chunk(void *opaque, int64_t dsp_ns)
{
    CdjC6747 *s = opaque;

    cdj_edma3_paced(&s->edma, dsp_ns);
}

static void set_command_pins(void *opaque, unsigned bits)
{
    CdjC6747 *s = opaque;
    uint32_t *ext = &s->gpio_ext[GPIO_CMD_PAIR];

    *ext &= ~(GPIO_CMD_BIT0 | GPIO_CMD_BIT1);
    *ext |= (bits & 1 ? GPIO_CMD_BIT0 : 0) | (bits & 2 ? GPIO_CMD_BIT1 : 0);
}

static uint32_t dsp_bus_read(void *opaque, uint32_t addr, unsigned size)
{
    CdjC6747 *s = opaque;
    unsigned pair, reg;
    uint32_t val = 0;

    if (addr == UHPI_HPIC) {
        val = cdj_dsp_host_hpic(&s->host);
    } else if (addr == SPI1_SPIBUF) {
        val = SPIBUF_RXEMPTY;
    } else if (addr - EDMA3_BASE < EDMA3_SIZE) {
        val = cdj_edma3_read(&s->edma, addr);
    } else if (gpio_reg(addr, &pair, &reg)) {
        val = reg == GPIO_DIR ? s->gpio_dir[pair]
            : reg == GPIO_OUT ? s->gpio_out[pair]
            : reg == GPIO_IN  ? gpio_in(s, pair) : 0;
        if (pair == GPIO_CMD_PAIR && reg == GPIO_IN) {
            s->host.pin_reads++;
        }
    } else {
        val = cdj_dsp_mcasp_read(&s->mcasps, addr);
    }
    cdj_dsp_count(&s->host.busr, addr, val);
    return val;
}

static void dsp_bus_write(void *opaque, uint32_t addr, uint32_t val,
                          unsigned size)
{
    CdjC6747 *s = opaque;
    unsigned pair, reg;

    if (addr == UHPI_HPIC) {
        cdj_dsp_host_dsp_hpic(&s->host, val);
    } else if (addr - EDMA3_BASE < EDMA3_SIZE) {
        cdj_edma3_write(&s->edma, addr, val);
    } else if (gpio_reg(addr, &pair, &reg)) {
        gpio_write(s, pair, reg, val);
    } else {
        cdj_dsp_mcasp_write(&s->mcasps, addr, val);
    }
    cdj_dsp_count(&s->host.busw, addr, val);
}

/* A fresh core at the boot word's entry, as the chip does on the first
 * DSPINT after the host has loaded it. */
static void dsp_start(CdjC6747 *s)
{
    c66x_bus bus = { s, dsp_bus_read, dsp_bus_write };
    c66x_core *core = cdj_dsp_host_core(&s->host, &bus);

    if (c66x_record_open(core, getenv("CDJ_C6X_RECORD"))) {
        warn_report("c6747: cannot open CDJ_C6X_RECORD %s",
                    getenv("CDJ_C6X_RECORD"));
    }

    c66x_map_ram(core, L2_BASE, L2_SIZE, s->l2);
    c66x_map_ram(core, L2_ALIAS, L2_SIZE, s->l2);
    c66x_map_ram(core, SDRAM_BASE, SDRAM_SIZE, s->sdram);
    c66x_map_ram(core, SHRAM_BASE, SHRAM_SIZE, s->shram);
    c66x_map_ram(core, L1P_BASE, L1_SIZE, s->l1p);
    c66x_map_ram(core, L1D_BASE, L1_SIZE, s->l1d);
    cdj_dsp_host_run(&s->host, ldl_le_p(s->l2 + (BOOT_WORD - L2_BASE)));
}

static void hpi_note_write(CdjC6747 *s, uint32_t addr)
{
    unsigned i;

    for (i = 0; i < s->nruns; i++) {
        HpiRun *r = &s->runs[i];

        if (addr == r->end) {
            r->end += 4;
            return;
        }
        if (addr >= r->start && addr < r->end) {
            return;
        }
    }
    if (s->nruns < MAX_RUNS) {
        s->runs[s->nruns++] = (HpiRun){ addr, addr + 4 };
    } else {
        s->runs_overflowed = true;
    }
}

static uint64_t hpi_read(void *opaque, hwaddr off, unsigned size)
{
    CdjC6747 *s = opaque;
    uint8_t *p;
    uint32_t val;
    CDJ_DSP_HOST_GUARD(&s->host);

    switch (off >> 18) {
    case 0:
        return cdj_dsp_host_hpic(&s->host);
    case 1:
        return s->hpia;
    default:
        p = dsp_ram(s, s->hpia & ~3u);
        val = p ? ldl_le_p(p) : 0;
        s->reads++;
        cdj_dsp_count(&s->polls, s->hpia, val);
        if ((off >> 18) == 2) {
            s->hpia += 4;
        }
        return val;
    }
}

static void hpi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjC6747 *s = opaque;
    uint8_t *p;
    CDJ_DSP_HOST_GUARD(&s->host);

    switch (off >> 18) {
    case 0:
        if (cdj_dsp_host_main_hpic(&s->host, val)) {
            s->dspints++;
            if (s->host.enabled && !s->host.core) {
                dsp_start(s);
            }
        }
        break;
    case 1:
        s->hpia = val;
        break;
    default:
        p = dsp_ram(s, s->hpia & ~3u);
        if (p) {
            stl_le_p(p, val);
            if (s->host.core) {
                c66x_invalidate(s->host.core, s->hpia & ~3u, 4);
            }
        } else {
            s->dropped++;
        }
        s->writes++;
        hpi_note_write(s, s->hpia);
        if ((off >> 18) == 2) {
            s->hpia += 4;
        }
        break;
    }
}

static const MemoryRegionOps hpi_ops = {
    .read = hpi_read,
    .write = hpi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void hpi_dump_run(CdjC6747 *s, const char *dir, const HpiRun *r)
{
    g_autofree char *path = g_strdup_printf("%s/hpi_%08x.bin", dir, r->start);
    FILE *f = fopen(path, "wb");
    uint32_t a;

    if (!f) {
        warn_report("hpi: cannot write %s", path);
        return;
    }
    for (a = r->start; a < r->end; a += 4) {
        uint8_t *p = dsp_ram(s, a);
        uint8_t zero[4] = { 0 };

        fwrite(p ? p : zero, 1, 4, f);
    }
    fclose(f);
}

static void c6747_exit_report(Notifier *n, void *data)
{
    CdjC6747 *s = container_of(n, CdjC6747, exit);
    const char *dir = getenv("CDJ_HPI_DUMP");
    unsigned i;

    info_report("hpi: %" PRIu64 " data writes (%" PRIu64 " outside RAM), "
                "%" PRIu64 " data reads, %" PRIu64 " DSPINT, HPIC 0x%08x",
                s->writes, s->dropped, s->reads, s->dspints, s->host.hpic);
    for (i = 0; i < s->nruns; i++) {
        info_report("hpi: run 0x%08x..0x%08x (%u bytes)", s->runs[i].start,
                    s->runs[i].end, s->runs[i].end - s->runs[i].start);
        if (dir && *dir) {
            hpi_dump_run(s, dir, &s->runs[i]);
        }
    }
    if (dir && *dir) {
        cdj_dsp_dump_mem(dir, "l2.bin", s->l2, L2_SIZE);
        cdj_dsp_dump_mem(dir, "sdram.bin", s->sdram, MiB);
    }
    if (s->runs_overflowed) {
        info_report("hpi: more than %d runs; later ones not listed", MAX_RUNS);
    }
    for (i = 0; i < s->polls.n; i++) {
        info_report("hpi: host read 0x%08x x%" PRIu64 ", last 0x%08x",
                    s->polls.e[i].addr, s->polls.e[i].count,
                    s->polls.e[i].last);
    }
    if (s->host.core) {
        c66x_stats st;
        unsigned line;

        c66x_get_stats(s->host.core, &st);
        info_report("c6747: IER 0x%08x ISTP 0x%08x CSR 0x%08x, %" PRIu64
                    " interrupts taken", c66x_get_creg(s->host.core, 4),
                    c66x_get_creg(s->host.core, 5),
                    c66x_get_creg(s->host.core, 1), st.interrupts);
        info_report("c6747: edma %" PRIu64 " events, %" PRIu64
                    " transfers complete", s->edma.events,
                    s->edma.completions);
        for (line = C66X_INT_FIRST; line <= C66X_INT_LAST; line++) {
            if (st.irq_edges[line]) {
                info_report("c6747: INT%u %u edges", line, st.irq_edges[line]);
            }
        }
    }
    cdj_dsp_host_report(&s->host);
    if (s->host.core) {
        cdj_dsp_host_lock(&s->host);
        c66x_record_close(s->host.core);
        cdj_dsp_host_unlock(&s->host);
    }
}

const CdjDspWires *cdj_c6747_init(MemoryRegion *sysmem, hwaddr hpi_base)
{
    CdjC6747 *s = &c6747;
    unsigned i;

    s->l2 = g_malloc0(L2_SIZE);
    s->l1p = g_malloc0(L1_SIZE);
    s->l1d = g_malloc0(L1_SIZE);
    s->shram = g_malloc0(SHRAM_SIZE);
    s->sdram = g_malloc0(SDRAM_SIZE);
    cdj_dsp_host_init(&s->host, "c6747", "C6747", 300, set_command_pins, s);
    s->mcasps = (DspMcasps){ MCASP_BASE, MCASP_STRIDE };
    s->host.dspint_line = DSPINT_IRQ;
    s->edma.ram = edma_ram;
    s->edma.stored = edma_stored;
    s->edma.write_word = edma_write_word;
    s->edma.set_irq = edma_set_irq;
    s->edma.chip = s;
    s->host.after_chunk = edma_after_chunk;

    memory_region_init_io(&s->iomem, NULL, &hpi_ops, s, "c6747.hpi", 0x100000);
    memory_region_add_subregion(sysmem, hpi_base, &s->iomem);
    s->exit.notify = c6747_exit_report;
    qemu_add_exit_notifier(&s->exit);

    for (i = 0; i < GPIO_PAIRS; i++) {
        s->gpio_dir[i] = 0xFFFFFFFF;            /* every pin an input at reset */
    }
    return &s->host.wires;
}
