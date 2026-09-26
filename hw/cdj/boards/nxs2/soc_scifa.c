/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/* ---------------------------------------------------------------------------
 * SCIFA3-5 (0xA4E30000 / 0xA4E40000 / 0xA4E50000).
 *
 * SCIFA has a different register map from SCIF, so TYPE_SH_SERIAL does not
 * fit. SH7724 manual table 27.2:
 *
 *     +0x00 SCASMR   16  serial mode
 *     +0x04 SCABRR    8  bit rate
 *     +0x08 SCASCR   16  serial control
 *     +0x0C SCATDSR   8  transmit data stop    <-- SCIF keeps TX data here
 *     +0x10 SCAFER   16  FIFO error count      <-- SCIF keeps status here
 *     +0x14 SCASSR   16  serial status
 *     +0x18 SCAFCR   16  FIFO control
 *     +0x1C SCAFDR   16  FIFO data count
 *     +0x20 SCAFTDR   8  transmit FIFO data    <-- the console byte
 *     +0x24 SCAFRDR   8  receive FIFO data
 *
 * SCASSR bit 6 = TEND, bit 5 = TDFE (manual 27.3.8), held set so transmit
 * never blocks. Receive is not modelled: RDF stays clear, SCAFRDR reads 0.
 */
static void cdj_scifa_trace(CdjScifaState *s, const char *op,
                            hwaddr off, uint64_t val)
{
    if (s->traced < SCIFA_TRACE_MAX) {
        s->traced++;
        qemu_log("%s: %s off 0x%02" HWADDR_PRIx " val 0x%04" PRIx64 "%s\n",
                 s->name, op, off, val,
                 s->traced == SCIFA_TRACE_MAX ? "  [further accesses muted]" : "");
    }
}

/* CDJ_SCIFA_RD_DEBUG: log each distinct (channel, offset, guest PC) read
 * once, to locate receive-wait loops. */
static struct { const char *name; hwaddr off; uint32_t pc; } cdj_scifa_rd_seen[256];
static unsigned cdj_scifa_rd_seen_n;
static int cdj_scifa_rd_dbg = -1;

static uint64_t cdj_scifa_read(void *opaque, hwaddr off, unsigned size)
{
    CdjScifaState *s = opaque;

    s->reads++;
    cdj_scifa_trace(s, "rd", off, 0);

    if (cdj_scifa_rd_dbg < 0) {
        cdj_scifa_rd_dbg = getenv("CDJ_SCIFA_RD_DEBUG") ? 1 : 0;
    }
    if (cdj_scifa_rd_dbg && current_cpu) {
        uint32_t pc = SUPERH_CPU(current_cpu)->env.pc;
        unsigned j;
        for (j = 0; j < cdj_scifa_rd_seen_n; j++) {
            if (cdj_scifa_rd_seen[j].name == s->name &&
                cdj_scifa_rd_seen[j].off == off &&
                cdj_scifa_rd_seen[j].pc == pc) {
                break;
            }
        }
        if (j == cdj_scifa_rd_seen_n &&
            cdj_scifa_rd_seen_n < ARRAY_SIZE(cdj_scifa_rd_seen)) {
            cdj_scifa_rd_seen[cdj_scifa_rd_seen_n].name = s->name;
            cdj_scifa_rd_seen[cdj_scifa_rd_seen_n].off = off;
            cdj_scifa_rd_seen[cdj_scifa_rd_seen_n].pc = pc;
            cdj_scifa_rd_seen_n++;
            qemu_log("scifa-rd: %s +0x%02x pc 0x%08x\n", s->name, (unsigned)off, pc);
        }
    }

    switch (off) {
    case SCIFA_SCASSR:
        /* Transmitter permanently ready; no receive/error flags. */
        return SCIFA_TX_READY;
    case SCIFA_SCAFDR:
        return 0;               /* both FIFOs empty */
    case SCIFA_SCAFRDR:
        return 0;               /* idle line */
    default:
        return s->reg[off / 2];
    }
}

static void cdj_scifa_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjScifaState *s = opaque;

    s->writes++;
    cdj_scifa_trace(s, "wr", off, val);

    if (off == SCIFA_SCAFTDR) {
        uint8_t ch = val & 0xFF;

        /* CDJ_CAPTURE: one file per channel. SCIFA4 carries ~54 KB of SH
         * code for a Renesas peer. */
        if (getenv("CDJ_CAPTURE")) {
            char suffix[64];

            snprintf(suffix, sizeof(suffix), "scifa-%s", s->name);
            cdj_capture_write(&s->cap, suffix, &ch, 1);
        }
        s->txbytes++;
        /* Log the transmitting PC for the first bytes and every 4096th. */
        if (s->txbytes <= 24 || (s->txbytes % 4096) == 0) {
            qemu_log("%s: tx[%" PRIu64 "] = 0x%02x from pc 0x%08x\n",
                     s->name, s->txbytes, ch,
                     current_cpu ? SUPERH_CPU(current_cpu)->env.pc : 0);
        }
        /* Blocking write so console output is never dropped. */
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        return;
    }
    s->reg[off / 2] = val;
}

static const MemoryRegionOps cdj_scifa_ops = {
    .read = cdj_scifa_read,
    .write = cdj_scifa_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* The panel reads this to show link progress; set for the SCIFA4 channel. */
CdjScifaState *cdj_scifa4;

uint64_t cdj_peer_tx_bytes(void)
{
    return cdj_scifa4 ? cdj_scifa4->txbytes : 0;
}

static void cdj_scifa_summary(Notifier *n, void *unused)
{
    CdjScifaState *s = container_of(n, CdjScifaState, exit);

    qemu_log("%s: %" PRIu64 " reads, %" PRIu64 " writes, %" PRIu64 " tx bytes\n",
             s->name, s->reads, s->writes, s->txbytes);
}

CdjScifaState *cdj_scifa_last;

void cdj_scifa_init(MemoryRegion *sysmem, const char *name,
                           hwaddr addr, Chardev *chr)
{
    CdjScifaState *s = g_new0(CdjScifaState, 1);

    cdj_scifa_last = s;
    MemoryRegion *alias = g_new(MemoryRegion, 1);
    g_autofree char *a7name = g_strdup_printf("%s-a7", name);

    s->name = g_strdup(name);
    qemu_chr_fe_init(&s->chr, chr, &error_abort);
    s->exit.notify = cdj_scifa_summary;
    qemu_add_exit_notifier(&s->exit);
    memory_region_init_io(&s->iomem, NULL, &cdj_scifa_ops, s,
                          name, CDJ_SCIFA_SIZE);
    memory_region_add_subregion(sysmem, addr, &s->iomem);

    memory_region_init_alias(alias, NULL, a7name, &s->iomem,
                             0, CDJ_SCIFA_SIZE);
    memory_region_add_subregion(sysmem, A7ADDR(addr), alias);
}

/*
 * CDJ_CAPTURE=<path prefix>: dump the raw bytes MAIN sends on its outgoing
 * links to <prefix>-<suffix>.bin (e.g. -i2c, -msiof, -scifa-<name>).
 * Appended and flushed on every write, since runs are usually killed.
 */
static FILE *cdj_capture_open(const char *suffix)
{
    static char path[512];
    const char *pfx = getenv("CDJ_CAPTURE");

    if (!pfx || !*pfx) {
        return NULL;
    }
    snprintf(path, sizeof(path), "%s-%s.bin", pfx, suffix);
    return fopen(path, "ab");
}

void cdj_capture_write(FILE **fp, const char *suffix,
                              const void *buf, size_t len)
{
    if (!getenv("CDJ_CAPTURE")) {
        return;
    }
    if (!*fp) {
        *fp = cdj_capture_open(suffix);
        if (!*fp) {
            return;
        }
    }
    fwrite(buf, 1, len, *fp);
    fflush(*fp);
}

