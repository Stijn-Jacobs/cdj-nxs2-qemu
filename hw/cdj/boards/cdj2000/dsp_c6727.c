/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dsp_host.h"
#include "cdj_getenv.h"
/*
 * The CDJ-2000's DSP: a C6727 (C67x+) that MAIN boots and feeds through its
 * host port (UHPI) in full-address mode. The C67x+ instruction set is a
 * subset of the C66x's, so the project's C66x core runs it; this file is the
 * chip around that core as far as the boot needs it.
 *
 * MAIN's side, its area 3:
 *   +0x00000         HPIC
 *   +0xC0000..CFFFF  a 64 KiB window onto DSP memory. Its base is the HPI
 *                    page the DSP sets in CFGHPIAMSB/CFGHPIAUMB (0x4000000C,
 *                    0x40000010: address bits 31:24 and 23:16).
 * MAIN never writes an address register: its DMA goes to the window address
 * directly (0x041C747A).
 *
 * The boot, as the firmware does it (0x041C77DC): MAIN copies the first image
 * (0xD3B0 bytes) to window +0x1DA0, writes its entry to +0x714 and 1 to
 * +0x718, and clears HINT; the ROM loader, whose words those are, starts it.
 * That image is a resident loader: it moves the page to 0x10030000 on its
 * first command, then takes the second image in 32 KiB chunks through window
 * +0x7800 (0x10037800) and copies each to SDRAM at 0x80000000. MAIN polls
 * the mailbox at +0xFFF0. The loader takes its orders on two SPI0 pins used
 * as GPIO (SPIPC2 0x4700001C bits 11 and 9; 0x800 = copy a chunk, 0x200 =
 * next page, 0xA00 = last chunk) and answers each by raising HINT; the
 * handshake itself is in dsp_host.c. The ROM loader is not modelled: a
 * write of the start word is taken as its signal.
 *
 * Image B's 0x8004CDA0 moves A4 into control register 9 (mvc at 0x8004CDB0),
 * which the core does not decode; a hook stands in for that function and
 * counts the values.
 *
 * Unmodelled DSP bus addresses read 0; the busiest are listed at exit.
 *
 *   CDJ_C6727_MHZ=<n>       DSP clock (default 300)
 *   CDJ_C6727=0             no core: the window is plain memory
 *   CDJ_HPI_DUMP=<dir>      at exit, save internal RAM and the first MiB of
 *                           SDRAM as <dir>/iram.bin and <dir>/sdram.bin
 */

#define IRAM_BASE       0x10000000u
#define IRAM_SIZE       (256 * KiB)
#define SDRAM_BASE      0x80000000u
#define SDRAM_SIZE      (64 * MiB)

#define ROM_ENTRY       (IRAM_BASE + 0x714)
#define ROM_START       (IRAM_BASE + 0x718)

#define CFG_BASE        0x40000000u
#define CFG_SIZE        0x40
#define CFG_HPIAMSB     0x0C
#define CFG_HPIAUMB     0x10
#define UHPI_HPIC       0x43000030u
#define SPI0_PC2        0x4700001Cu     /* pin data in */
#define SPI1_SPIBUF     0x48000040u
#define MCASP_BASE      0x44000000u
#define MCASP_STRIDE    0x01000000
#define SPI_CMD_BIT0    (1u << 9)       /* MAIN's command bit 0 */
#define SPI_CMD_BIT1    (1u << 11)      /* MAIN's command bit 1 */

#define WIN_OFF         0xC0000
#define WIN_SIZE        0x10000

#define CREG9_FUNC      0x8004CDA0u
#define REG_A4          4
#define REG_B3          (32 + 3)

typedef struct CdjC6727 {
    MemoryRegion iomem;
    uint8_t *iram, *sdram;
    CdjDspHost host;

    uint32_t cfg[CFG_SIZE / 4];
    uint32_t spi_in;
    DspMcasps mcasps;
    uint64_t win_writes, win_reads, dropped;
    uint64_t outside;           /* MAIN accesses beyond HPIC and the window */
    hwaddr outside_last;
    uint64_t creg9_writes;
    uint32_t creg9_last;
    DspAddrTable polls;

    Notifier exit;
} CdjC6727;

static CdjC6727 c6727;

static uint8_t *dsp_ram(CdjC6727 *s, uint32_t addr)
{
    if (addr - IRAM_BASE < IRAM_SIZE) {
        return s->iram + (addr - IRAM_BASE);
    }
    if (addr - SDRAM_BASE < SDRAM_SIZE) {
        return s->sdram + (addr - SDRAM_BASE);
    }
    return NULL;
}

static uint32_t hpi_page(CdjC6727 *s)
{
    return (s->cfg[CFG_HPIAMSB / 4] & 0xFF) << 24
         | (s->cfg[CFG_HPIAUMB / 4] & 0xFF) << 16;
}

static void set_command_pins(void *opaque, unsigned bits)
{
    CdjC6727 *s = opaque;

    s->spi_in = (bits & 1 ? SPI_CMD_BIT0 : 0) | (bits & 2 ? SPI_CMD_BIT1 : 0);
}

static uint32_t dsp_bus_read(void *opaque, uint32_t addr, unsigned size)
{
    CdjC6727 *s = opaque;
    uint32_t val = 0;

    if (addr == UHPI_HPIC) {
        val = cdj_dsp_host_hpic(&s->host);
    } else if (addr == SPI0_PC2) {
        val = s->spi_in;
        s->host.pin_reads++;
    } else if (addr == SPI1_SPIBUF) {
        val = SPIBUF_RXEMPTY;
    } else if (addr - CFG_BASE < CFG_SIZE) {
        val = s->cfg[(addr - CFG_BASE) / 4];
    } else {
        val = cdj_dsp_mcasp_read(&s->mcasps, addr);
    }
    cdj_dsp_count(&s->host.busr, addr, val);
    return val;
}

static void dsp_bus_write(void *opaque, uint32_t addr, uint32_t val,
                          unsigned size)
{
    CdjC6727 *s = opaque;

    if (addr == UHPI_HPIC) {
        cdj_dsp_host_dsp_hpic(&s->host, val);
    } else if (addr - CFG_BASE < CFG_SIZE) {
        s->cfg[(addr - CFG_BASE) / 4] = val;
    } else {
        cdj_dsp_mcasp_write(&s->mcasps, addr, val);
    }
    cdj_dsp_count(&s->host.busw, addr, val);
}

static int creg9_hook(c66x_core *core, void *opaque)
{
    CdjC6727 *s = opaque;

    s->creg9_last = c66x_get_reg(core, REG_A4);
    if (!s->creg9_writes++) {
        info_report("c6727: control register 9 <- 0x%08x", s->creg9_last);
    }
    c66x_set_pc(core, c66x_get_reg(core, REG_B3));
    return 0;
}

static void dsp_start(CdjC6727 *s)
{
    c66x_bus bus = { s, dsp_bus_read, dsp_bus_write };
    c66x_core *core = cdj_dsp_host_core(&s->host, &bus);

    c66x_map_ram(core, IRAM_BASE, IRAM_SIZE, s->iram);
    c66x_map_ram(core, SDRAM_BASE, SDRAM_SIZE, s->sdram);
    c66x_hook_pc(core, CREG9_FUNC, creg9_hook, s);
    cdj_dsp_host_run(&s->host, ldl_le_p(s->iram + (ROM_ENTRY - IRAM_BASE)));
}

/* MAIN has code that reaches mailbox +16 and +24 through a pointer, past the
 * window. The boot does not get there; counted, not modelled. */
static void note_outside(CdjC6727 *s, hwaddr off, const char *what)
{
    s->outside++;
    s->outside_last = off;
    qemu_log_mask(LOG_UNIMP, "c6727.hpi: %s at +0x%05" HWADDR_PRIx "\n",
                  what, off);
}

static uint64_t hpi_read(void *opaque, hwaddr off, unsigned size)
{
    CdjC6727 *s = opaque;
    uint32_t addr;
    uint8_t *p;
    uint64_t val = 0;

    if (off == 0) {
        return cdj_dsp_host_hpic(&s->host);
    }
    if (off - WIN_OFF >= WIN_SIZE) {
        note_outside(s, off, "read");
        return 0;
    }
    addr = hpi_page(s) + (off - WIN_OFF);
    p = dsp_ram(s, addr);
    if (p) {
        val = ldn_le_p(p, size);
    }
    s->win_reads++;
    cdj_dsp_count(&s->polls, addr, val);
    return val;
}

static void hpi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjC6727 *s = opaque;
    uint32_t addr;
    uint8_t *p;

    if (off == 0) {
        cdj_dsp_host_main_hpic(&s->host, val);
        return;
    }
    if (off - WIN_OFF >= WIN_SIZE) {
        note_outside(s, off, "write");
        return;
    }
    addr = hpi_page(s) + (off - WIN_OFF);
    p = dsp_ram(s, addr);
    s->win_writes++;
    if (!p) {
        s->dropped++;
        return;
    }
    stn_le_p(p, size, val);
    if (s->host.core) {
        c66x_invalidate(s->host.core, addr, size);
    } else if (addr == ROM_START && val && s->host.enabled) {
        dsp_start(s);
    }
}

static const MemoryRegionOps hpi_ops = {
    .read = hpi_read,
    .write = hpi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void c6727_exit_report(Notifier *n, void *data)
{
    CdjC6727 *s = container_of(n, CdjC6727, exit);
    const char *dir = getenv("CDJ_HPI_DUMP");
    unsigned i;

    info_report("c6727.hpi: %" PRIu64 " window writes (%" PRIu64
                " outside RAM), %" PRIu64 " window reads, page 0x%08x",
                s->win_writes, s->dropped, s->win_reads, hpi_page(s));
    info_report("c6727.hpi: %" PRIu64 " accesses outside HPIC and the window"
                ", last at +0x%05" HWADDR_PRIx, s->outside, s->outside_last);
    info_report("c6727: control register 9 written %" PRIu64 " times, last "
                "0x%08x", s->creg9_writes, s->creg9_last);
    for (i = 0; i < s->polls.n; i++) {
        info_report("c6727.hpi: host read 0x%08x x%" PRIu64 ", last 0x%08x",
                    s->polls.e[i].addr, s->polls.e[i].count,
                    s->polls.e[i].last);
    }
    if (dir && *dir) {
        cdj_dsp_dump_mem(dir, "iram.bin", s->iram, IRAM_SIZE);
        cdj_dsp_dump_mem(dir, "sdram.bin", s->sdram, MiB);
    }
    cdj_dsp_host_report(&s->host);
}

const CdjDspWires *cdj_c6727_init(MemoryRegion *sysmem, hwaddr hpi_base)
{
    CdjC6727 *s = &c6727;

    s->iram = g_malloc0(IRAM_SIZE);
    s->sdram = g_malloc0(SDRAM_SIZE);
    /* The ROM loader leaves the page on internal RAM, where MAIN writes the
     * first image before the DSP has run anything of it. */
    s->cfg[CFG_HPIAMSB / 4] = IRAM_BASE >> 24;
    cdj_dsp_host_init(&s->host, "c6727", "C6727", 300, set_command_pins, s);
    s->mcasps = (DspMcasps){ MCASP_BASE, MCASP_STRIDE };

    memory_region_init_io(&s->iomem, NULL, &hpi_ops, s, "c6727.hpi", 0x100000);
    memory_region_add_subregion(sysmem, hpi_base, &s->iomem);
    s->exit.notify = c6727_exit_report;
    qemu_add_exit_notifier(&s->exit);
    return &s->host.wires;
}
