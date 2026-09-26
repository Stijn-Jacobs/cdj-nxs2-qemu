/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "cdj_getenv.h"
/*
 * The SH-4 core blocks every CDJ MAIN board has: IRQ line counting, the TMU
 * and SCIF glue, and the CCN exception registers. Board specifics (the TMU
 * clock, the board's own exit lines) come from the CdjBoardDesc.
 */

/* ---------------------------------------------------------------------------
 * Counting passthrough on an IRQ line, reported at exit. Lines are level
 * driven, so "rises=1" means it went high and stayed high, not one tick.
 */
typedef struct CdjIrqCount {
    qemu_irq target;
    const char *name;
    uint64_t rises;
    uint64_t falls;
} CdjIrqCount;

/* Lines past this limit are silently left out of the summary. */
static CdjIrqCount *cdj_irq_counters[32];
static unsigned cdj_irq_counter_n;
Notifier cdj_irqcount_exit;

static void cdj_irq_count(void *opaque, int n, int level)
{
    CdjIrqCount *c = opaque;

    if (level) {
        c->rises++;
    } else {
        c->falls++;
    }
    qemu_set_irq(c->target, level);
}

qemu_irq cdj_count_irq(qemu_irq target, const char *name)
{
    CdjIrqCount *c = g_new0(CdjIrqCount, 1);

    c->target = target;
    c->name = name;
    if (cdj_irq_counter_n < ARRAY_SIZE(cdj_irq_counters)) {
        cdj_irq_counters[cdj_irq_counter_n++] = c;
    }
    return qemu_allocate_irq(cdj_irq_count, c, 0);
}

void cdj_irqcount_dump(Notifier *n, void *opaque)
{
    unsigned i;

    /* A machine reset invalidates every count below; say so. */
    if (cdj_reset_count > 1) {
        warn_report("MACHINE RESET happened %u times -- every count below is "
                    "from a boot that did not run to completion",
                    cdj_reset_count - 1);
    }

    for (i = 0; i < cdj_irq_counter_n; i++) {
        info_report("irqcount: %-12s rises=%" PRIu64 " falls=%" PRIu64,
                    cdj_irq_counters[i]->name, cdj_irq_counters[i]->rises,
                    cdj_irq_counters[i]->falls);
    }
    if (cdj_board->exit_report) {
        cdj_board->exit_report();
    }
}

void cdj_irq_sink(void *opaque, int n, int level)
{
}

void cdj_tmu_init(MemoryRegion *sysmem, hwaddr base,
                         qemu_irq ch0, qemu_irq ch1, qemu_irq ch2)
{
    const char *fenv = getenv("CDJ_PERIPH_HZ");
    uint32_t freq = fenv && *fenv ? (uint32_t)strtoul(fenv, NULL, 0)
                                  : cdj_board->periph_hz;

    tmu012_init(sysmem, base, TMU012_FEAT_3CHAN, freq,
                ch0, ch1, ch2, NULL);
}

void cdj_scif(MemoryRegion *sysmem, const char *id,
                     hwaddr addr, Chardev *chr)
{
    DeviceState *dev;
    SysBusDevice *sb;
    MemoryRegion *mr, *alias;

    dev = qdev_new(TYPE_SH_SERIAL);
    dev->id = g_strdup(id);
    qdev_prop_set_chr(dev, "chardev", chr);
    qdev_prop_set_uint8(dev, "features", SH_SERIAL_FEAT_SCIF);
    sb = SYS_BUS_DEVICE(dev);
    sysbus_realize_and_unref(sb, &error_fatal);
    sysbus_mmio_map(sb, 0, addr);

    /* eri/rxi/txi/bri are not connected; the firmware polls. */
    mr = sysbus_mmio_get_region(sb, 0);
    alias = g_malloc(sizeof(*alias));
    memory_region_init_alias(alias, OBJECT(dev), id, mr, 0,
                             memory_region_size(mr));
    memory_region_add_subregion(sysmem, A7ADDR(addr), alias);
}


/* ---------------------------------------------------------------------------
 * An unpopulated part of a chip select: nothing answers, and the bus's
 * pull-ups make every read all ones. QEMU's own unassigned memory reads 0,
 * which a firmware probing for erased (0xFFFF) words takes for data and scans
 * on. The first reads and every write are logged.
 */
typedef struct CdjOpenBus {
    MemoryRegion iomem;
    const char *name;
    uint64_t reads, writes;
} CdjOpenBus;

static uint64_t cdj_open_bus_read(void *opaque, hwaddr off, unsigned size)
{
    CdjOpenBus *s = opaque;

    if (++s->reads <= 4) {
        qemu_log_mask(LOG_UNIMP, "%s: open-bus read at +0x%" HWADDR_PRIx
                      " (size %u)\n", s->name, off, size);
    }
    return size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1;
}

static void cdj_open_bus_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjOpenBus *s = opaque;

    s->writes++;
    qemu_log_mask(LOG_UNIMP, "%s: write to open bus at +0x%" HWADDR_PRIx
                  " = 0x%" PRIx64 " (size %u)\n", s->name, off, val, size);
}

static const MemoryRegionOps cdj_open_bus_ops = {
    .read = cdj_open_bus_read,
    .write = cdj_open_bus_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void cdj_open_bus(MemoryRegion *sysmem, const char *name, hwaddr base, hwaddr size)
{
    CdjOpenBus *s = g_new0(CdjOpenBus, 1);

    s->name = name;
    memory_region_init_io(&s->iomem, NULL, &cdj_open_bus_ops, s, name, size);
    memory_region_add_subregion(sysmem, base, &s->iomem);
}


/* ---------------------------------------------------------------------------
 * A plain register block: writes are stored, reads return them, and listed
 * registers start at a power-on value. For SoC blocks whose only job during
 * bring-up is to read back sensibly (a clock divider, a mode register).
 */
typedef struct CdjRegsState {
    MemoryRegion iomem;
    uint32_t *reg;
} CdjRegsState;

static uint64_t cdj_regs_read(void *opaque, hwaddr off, unsigned size)
{
    CdjRegsState *s = opaque;
    uint32_t v = s->reg[off / 4] >> (8 * (off & 3));

    return size == 4 ? v : v & ((1u << (8 * size)) - 1);
}

static void cdj_regs_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjRegsState *s = opaque;
    unsigned shift = 8 * (off & 3);
    uint32_t mask = (size == 4 ? 0xFFFFFFFFu : (1u << (8 * size)) - 1) << shift;

    s->reg[off / 4] = (s->reg[off / 4] & ~mask) | (((uint32_t)val << shift) & mask);
}

static const MemoryRegionOps cdj_regs_ops = {
    .read = cdj_regs_read,
    .write = cdj_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void cdj_regs(MemoryRegion *sysmem, const char *name, hwaddr base,
              hwaddr size, const CdjRegInit *init)
{
    CdjRegsState *s = g_new0(CdjRegsState, 1);

    s->reg = g_new0(uint32_t, size / 4);
    for (; init && init->value; init++) {
        s->reg[init->off / 4] = init->value;
    }
    memory_region_init_io(&s->iomem, NULL, &cdj_regs_ops, s, name, size);
    memory_region_add_subregion(sysmem, base, &s->iomem);
}


/* ---------------------------------------------------------------------------
 * CCN / exception registers (0xFF000000).
 *
 * The interrupt entry dispatches on INTEVT (0xFF000028), so EXPEVT and INTEVT
 * expose QEMU's env->expevt/intevt. The rest of the block is a plain register
 * file.
 */
#define CDJ_CCN_BASE    0xFF000000
#define CDJ_CCN_SIZE    0x1000
#define CCN_EXPEVT      0x24
#define CCN_INTEVT      0x28

typedef struct CdjCcnState {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint32_t reg[CDJ_CCN_SIZE / 4];
} CdjCcnState;

static unsigned cdj_ccn_dei_seen;

static uint64_t cdj_ccn_read(void *opaque, hwaddr off, unsigned size)
{
    CdjCcnState *s = opaque;
    uint32_t intevt;

    switch (off) {
    case CCN_EXPEVT:
        return s->cpu->env.expevt;
    case CCN_INTEVT:
        /* With CDJ_DMAC_DEBUG, log the first few dispatches in the board's
         * trace window (its DMA completion vectors). */
        intevt = s->cpu->env.intevt;
        if (intevt >= cdj_board->ccn_trace_lo &&
            intevt <= cdj_board->ccn_trace_hi &&
            cdj_ccn_dei_seen < 6 && getenv("CDJ_DMAC_DEBUG")) {
            cdj_ccn_dei_seen++;
            info_report("ccn: INTEVT 0x%03x read at pc=0x%08x (spc=0x%08x "
                        "sr=0x%08x) #%u", intevt, s->cpu->env.pc,
                        s->cpu->env.spc, s->cpu->env.sr, cdj_ccn_dei_seen);
        }
        return intevt;
    default:
        return s->reg[off / 4];
    }
}

static void cdj_ccn_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjCcnState *s = opaque;

    switch (off) {
    case CCN_EXPEVT:
        s->cpu->env.expevt = val;
        return;
    case CCN_INTEVT:
        s->cpu->env.intevt = val;
        return;
    default:
        s->reg[off / 4] = val;
        return;
    }
}

static const MemoryRegionOps cdj_ccn_ops = {
    .read = cdj_ccn_read,
    .write = cdj_ccn_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void cdj_ccn_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    CdjCcnState *s = g_new0(CdjCcnState, 1);

    s->cpu = cpu;
    memory_region_init_io(&s->iomem, NULL, &cdj_ccn_ops, s,
                          "sh4.ccn", CDJ_CCN_SIZE);
    memory_region_add_subregion(sysmem, CDJ_CCN_BASE, &s->iomem);
}
