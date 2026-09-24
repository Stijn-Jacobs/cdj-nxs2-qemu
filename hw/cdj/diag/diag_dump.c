/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj.h"
#include "../cdj_getenv.h"
/* ---------------------------------------------------------------------------
 * Exit-time diagnostics: console buffer, PC ring, interrupt table, and a few
 * logging probe regions.
 *
 * The firmware's log output does not go to a UART. 0x08512596 (used by the
 * panic handler) copies each message into a 125-byte buffer at 0x0BFBDB60
 * and hands it to 0x083525DE. Only the last message survives; it is printed
 * at exit.
 */
#define CDJ_CONSOLE_BUF     0x0BFBDB60
#define CDJ_CONSOLE_LEN     128
#define CDJ_CONSOLE_STATE   0x0BFBDBE0

Notifier cdj_console_exit;

/* The RTOS interrupt dispatch table, dumped at exit. The entry at VBR+0x600
 * does:
 *     r1 = INTEVT ; if (!r1) rte ; jmp *(0x0AC94F14 + (r1 >> 3))
 * so a NULL slot sends the CPU to address 0.
 */
#define CDJ_IVT_BASE    0x0AC94F14
#define CDJ_IVT_SLOTS   0x100        /* scan window; the real table is shorter */
#define CDJ_IVT_FIRST_IRQ 14         /* INTEVT 0x1C0 (NMI); lower slots are
                                      * EXPEVT codes and stay empty */

Notifier cdj_ivt_exit;


/* The last blocks executed, from the ring in accel/tcg/cpu-exec.c. A stall
 * shows as a few repeating addresses. */
Notifier cdj_pcring_exit;

void cdj_pcring_dump(Notifier *n, void *opaque)
{
    unsigned i;
    unsigned distinct = 0;
    uint64_t seen[64];
    int last_bl_clear = -1;

    for (i = CDJ_PC_RING_LEN - 256; i < CDJ_PC_RING_LEN; i++) {
        unsigned slot = (cdj_pc_ring_idx + i) & (CDJ_PC_RING_LEN - 1);
        uint64_t pc = cdj_pc_ring[slot].pc;
        unsigned k;

        for (k = 0; k < distinct; k++) {
            if (seen[k] == pc) {
                break;
            }
        }
        if (k == distinct && distinct < ARRAY_SIZE(seen)) {
            seen[distinct++] = pc;
        }
    }

    info_report("pcring: %u distinct blocks in the last 256 executed", distinct);
    for (i = 0; i < distinct && i < 16; i++) {
        info_report("pcring:   0x%08" PRIx64, seen[i]);
    }

    /* Show where SR.BL was last clear, in case the guest ends up spinning
     * with interrupts blocked. */
    for (i = CDJ_PC_RING_LEN; i > 0; i--) {
        unsigned slot = (cdj_pc_ring_idx + i - 1) & (CDJ_PC_RING_LEN - 1);

        if (!(cdj_pc_ring[slot].sr & (1u << CDJ_SR_BL_BIT))) {
            last_bl_clear = (int)i - 1;
            break;
        }
    }

    if (last_bl_clear < 0) {
        info_report("pcring: SR.BL was set for the whole recorded window "
                    "(%d blocks)", CDJ_PC_RING_LEN);
        return;
    }

    info_report("pcring: SR.BL last clear %d blocks before the end; "
                "transition follows",
                CDJ_PC_RING_LEN - 1 - last_bl_clear);
    for (i = (last_bl_clear > 8 ? last_bl_clear - 8 : 0);
         i < CDJ_PC_RING_LEN && i < (unsigned)last_bl_clear + 12; i++) {
        unsigned slot = (cdj_pc_ring_idx + i) & (CDJ_PC_RING_LEN - 1);

        info_report("pcring:   [%+6d] pc=0x%08" PRIx64 " sr=0x%08x%s",
                    (int)i - (CDJ_PC_RING_LEN - 1),
                    cdj_pc_ring[slot].pc, cdj_pc_ring[slot].sr,
                    (cdj_pc_ring[slot].sr & (1u << CDJ_SR_BL_BIT)) ? " BL" : "");
    }
}

/* A slot holds a handler only if it points into code (0x08000000 to
 * 0x0BFFFFFF); the data past the table's end does not. */
static inline bool cdj_ivt_is_handler(uint32_t v)
{
    uint8_t hi = v >> 24;
    return hi >= 0x08 && hi <= 0x0B;
}

void cdj_ivt_dump(Notifier *n, void *unused)
{
    uint32_t slot[CDJ_IVT_SLOTS];
    unsigned i, installed = 0, unhooked = 0, top = CDJ_IVT_FIRST_IRQ;

    cpu_physical_memory_read(CDJ_IVT_BASE, slot, sizeof(slot));

    /* The table's top is the last slot holding a code pointer. */
    for (i = CDJ_IVT_FIRST_IRQ; i < CDJ_IVT_SLOTS; i++) {
        if (cdj_ivt_is_handler(le32_to_cpu(slot[i]))) {
            top = i;
        }
    }
    for (i = CDJ_IVT_FIRST_IRQ; i <= top; i++) {
        uint32_t v = le32_to_cpu(slot[i]);

        if (cdj_ivt_is_handler(v)) {
            installed++;
            if (installed <= 24) {
                /* byte offset i*4 == INTEVT>>3, so INTEVT == i<<5 */
                qemu_log("ivt: INTEVT 0x%03x -> 0x%08x\n", i << 5, v);
            }
        } else {
            /* None in a healthy boot. */
            unhooked++;
            qemu_log("ivt: INTEVT 0x%03x -> UNHOOKED (0x%08x)\n", i << 5, v);
        }
    }
    qemu_log("ivt: %u/%u interrupt vectors installed (INTEVT 0x%03x..0x%03x), "
             "%u unhooked\n", installed, installed + unhooked,
             CDJ_IVT_FIRST_IRQ << 5, top << 5, unhooked);
    {
        /* The vector-0x400 handler lives in DRAM above the image, so it is
         * not in main_unpacked.bin; dump its first bytes. */
        uint8_t code[64];
        uint32_t h = 0;
        unsigned k;

        cpu_physical_memory_read(CDJ_IVT_BASE + (0x400 >> 3), &h, 4);
        h = le32_to_cpu(h);
        if (h) {
            cpu_physical_memory_read(h & 0x1FFFFFFF, code, sizeof(code));
            qemu_log("ivt: handler 0x%08x bytes:", h);
            for (k = 0; k < sizeof(code); k++) {
                qemu_log(" %02x", code[k]);
            }
            qemu_log("\n");
        }
    }
    /* The vectors this board actually raises. */
    for (i = 0; i < 3; i++) {
        /* 0x400 TMU0 TUNI0, 0x940 TMU1 TUNI1, 0xA20 USB0 USI0. */
        static const unsigned vecs[] = { 0x400, 0x940, 0xA20 };
        unsigned vec = vecs[i];
        uint32_t v = 0;

        /* INTEVT >> 3 is a byte offset, so the word index is INTEVT >> 5. */
        if ((vec >> 5) < CDJ_IVT_SLOTS) {
            v = le32_to_cpu(slot[vec >> 5]);
        }
        qemu_log("ivt: vector 0x%03x (slot %u) -> 0x%08x %s\n",
                 vec, vec >> 5, v, v ? "installed" : "<-- NULL, jmp @0");
    }
}

void cdj_console_dump(Notifier *n, void *unused)
{
    uint8_t buf[CDJ_CONSOLE_LEN + 1];
    uint32_t st[4];
    bool printable = false;

    cpu_physical_memory_read(CDJ_CONSOLE_BUF, buf, CDJ_CONSOLE_LEN);
    cpu_physical_memory_read(CDJ_CONSOLE_STATE, st, sizeof(st));
    buf[CDJ_CONSOLE_LEN] = 0;

    for (unsigned i = 0; i < CDJ_CONSOLE_LEN && buf[i]; i++) {
        if (buf[i] >= 0x20 && buf[i] < 0x7F) {
            printable = true;
        }
    }

    qemu_log("console: state busy=0x%08x seq=0x%08x cnt=0x%08x handle=0x%08x\n",
             le32_to_cpu(st[0]), le32_to_cpu(st[1]),
             le32_to_cpu(st[2]), le32_to_cpu(st[3]));
    if (printable) {
        /* Replace non-text so a partially-filled buffer still prints safely. */
        for (unsigned i = 0; i < CDJ_CONSOLE_LEN; i++) {
            if (buf[i] && (buf[i] < 0x20 || buf[i] >= 0x7F) &&
                buf[i] != '\r' && buf[i] != '\n') {
                buf[i] = '.';
            }
        }
        qemu_log("console: last message: \"%s\"\n", (char *)buf);
    } else {
        qemu_log("console: buffer empty (no log line was ever emitted)\n");
    }
}

/* An external board device on the HPB bus at 0x04CE0000 (not an SH7724
 * register), polled by MAIN about twice a second. CDJ_HPB_DEBUG logs the
 * reader PC; CDJ_HPB_READY=<value> is returned by every read. */
typedef struct { MemoryRegion iomem; } CdjHpbState;
static struct { hwaddr off; uint32_t pc; } cdj_hpb_seen[256];
static unsigned cdj_hpb_seen_n;
static int cdj_hpb_dbg = -1;

static uint64_t cdj_hpb_read(void *opaque, hwaddr off, unsigned size)
{
    const char *rv;

    if (cdj_hpb_dbg < 0) {
        cdj_hpb_dbg = getenv("CDJ_HPB_DEBUG") ? 1 : 0;
    }
    if (cdj_hpb_dbg && current_cpu) {
        uint32_t pc = SUPERH_CPU(current_cpu)->env.pc;
        unsigned j;
        for (j = 0; j < cdj_hpb_seen_n; j++) {
            if (cdj_hpb_seen[j].off == off && cdj_hpb_seen[j].pc == pc) {
                break;
            }
        }
        if (j == cdj_hpb_seen_n && cdj_hpb_seen_n < ARRAY_SIZE(cdj_hpb_seen)) {
            cdj_hpb_seen[cdj_hpb_seen_n].off = off;
            cdj_hpb_seen[cdj_hpb_seen_n].pc = pc;
            cdj_hpb_seen_n++;
            qemu_log("hpb-rd: +0x%05x pc 0x%08x\n", (unsigned)off, pc);
        }
    }
    rv = getenv("CDJ_HPB_READY");
    if (rv && *rv) {
        return (uint64_t)strtoull(rv, NULL, 0);
    }
    return 0;
}

static void cdj_hpb_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
}

static const MemoryRegionOps cdj_hpb_ops = {
    .read = cdj_hpb_read,
    .write = cdj_hpb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void cdj_hpb_probe_init(MemoryRegion *sysmem)
{
    CdjHpbState *s = g_new0(CdjHpbState, 1);

    memory_region_init_io(&s->iomem, NULL, &cdj_hpb_ops, s,
                          "cdj.hpb-probe", 0x10000);
    memory_region_add_subregion_overlap(sysmem, A7ADDR(0x04CE0000),
                                        &s->iomem, 2);
}

/* ---------------------------------------------------------------------------
 * PC-logging probe region: like create_unimplemented_device, but logs the
 * guest PC and stores writes. Used for addresses the SCIFA4 peer path touches.
 */
typedef struct CdjProbeState {
    MemoryRegion iomem;
    const char *name;
    hwaddr base;
    unsigned logged;
    uint32_t reg[0x400];
} CdjProbeState;

#define CDJ_PROBE_LOG_MAX 40

static uint64_t cdj_probe_read(void *opaque, hwaddr off, unsigned size)
{
    CdjProbeState *s = opaque;

    if (s->logged < CDJ_PROBE_LOG_MAX) {
        s->logged++;
        qemu_log("%s: rd 0x%08" HWADDR_PRIx " size %u from pc 0x%08x\n",
                 s->name, s->base + off, size,
                 current_cpu ? SUPERH_CPU(current_cpu)->env.pc : 0);
    }
    return s->reg[(off / 4) % ARRAY_SIZE(s->reg)];
}

static void cdj_probe_write(void *opaque, hwaddr off, uint64_t val,
                            unsigned size)
{
    CdjProbeState *s = opaque;

    if (s->logged < CDJ_PROBE_LOG_MAX) {
        s->logged++;
        qemu_log("%s: wr 0x%08" HWADDR_PRIx " = 0x%08x size %u from pc 0x%08x\n",
                 s->name, s->base + off, (uint32_t)val, size,
                 current_cpu ? SUPERH_CPU(current_cpu)->env.pc : 0);
    }
    s->reg[(off / 4) % ARRAY_SIZE(s->reg)] = val;
}

static const MemoryRegionOps cdj_probe_ops = {
    .read = cdj_probe_read,
    .write = cdj_probe_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void cdj_probe(MemoryRegion *sysmem, const char *name,
                      hwaddr base, hwaddr size)
{
    CdjProbeState *s = g_new0(CdjProbeState, 1);

    s->name = name;
    s->base = base;
    memory_region_init_io(&s->iomem, NULL, &cdj_probe_ops, s, name, size);
    /* Low priority: only catches what nothing else claims. */
    memory_region_add_subregion_overlap(sysmem, base, &s->iomem, -900);
}

/* ---------------------------------------------------------------------------
 * IVT slot watcher (CDJ_IVT_WATCH=1): overlays the 4 KB page holding the
 * vector-0x400 slot (0x0AC94F94) with its own buffer and logs every access
 * to that slot with the guest PC.
 */
#define CDJ_IVTW_PAGE   0x0AC94000
#define CDJ_IVTW_SIZE   0x1000
#define CDJ_IVTW_SLOT   (0x0AC94F14 + (0x400 >> 3))   /* 0x0AC94F94 */

typedef struct CdjIvtWatch {
    MemoryRegion iomem;
    uint8_t data[CDJ_IVTW_SIZE];
    unsigned logged;
} CdjIvtWatch;

static uint64_t cdj_ivtw_read(void *opaque, hwaddr off, unsigned size)
{
    CdjIvtWatch *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v |= (uint64_t)s->data[off + i] << (8 * i);
    }
    if (CDJ_IVTW_PAGE + off == CDJ_IVTW_SLOT && s->logged < 40) {
        s->logged++;
        qemu_log("ivtw: RD slot = 0x%08x (size %u) from pc 0x%08x\n",
                 (uint32_t)v, size,
                 current_cpu ? SUPERH_CPU(current_cpu)->env.pc : 0);
    }
    return v;
}

static void cdj_ivtw_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    CdjIvtWatch *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->data[off + i] = (val >> (8 * i)) & 0xFF;
    }
    if (CDJ_IVTW_PAGE + off == CDJ_IVTW_SLOT && s->logged < 40) {
        s->logged++;
        qemu_log("ivtw: WR slot = 0x%08x (size %u) from pc 0x%08x\n",
                 (uint32_t)val, size,
                 current_cpu ? SUPERH_CPU(current_cpu)->env.pc : 0);
    }
}

static const MemoryRegionOps cdj_ivtw_ops = {
    .read = cdj_ivtw_read,
    .write = cdj_ivtw_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void cdj_ivtw_init(MemoryRegion *sysmem)
{
    CdjIvtWatch *s = g_new0(CdjIvtWatch, 1);

    memory_region_init_io(&s->iomem, NULL, &cdj_ivtw_ops, s,
                          "cdj.ivt-watch", CDJ_IVTW_SIZE);
    memory_region_add_subregion_overlap(sysmem, CDJ_IVTW_PAGE, &s->iomem, 10);
    info_report("cdj2000nxs2: IVT watch active on 0x%08x", CDJ_IVTW_SLOT);
}

