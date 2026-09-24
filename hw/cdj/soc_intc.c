/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/*
 * Interrupt controller, TMU, SCIF and CCN glue for the SH7724.
 *
 * Peripheral clock: the firmware programs TCOR0 = 10415 with TPSC = P0/4,
 * which is exactly a 1 kHz RTOS tick at 41.666667 MHz. The GUI link times out
 * if MAIN's tick runs slow. CDJ_PERIPH_HZ overrides it.
 */
#define CDJ_PERIPH_FREQ  41666667

/* ---------------------------------------------------------------------------
 * INTC-A (0xA4080000).
 *
 * The firmware masks everything early (IMR0-IMR12 = 0xFF), then sets
 * priorities and clears mask bits as each subsystem comes up:
 *
 *   IPRA = 0x8000                 TMU0 TUNI0 priority 8
 *   IMCR4 bit 4  (0x0D0 = 0x10)   unmask TMU0 TUNI0, vector H'400
 *   IMCR0 bits 5,6                unmask TMU1 TUNI1 / TUNI2
 *   IMCR2/3/5/6/10                Ether, DMAC1A, SCIF0/2, MSIOF0, DMAC1B
 *
 * Note (manual table 13.5): IMR0/IMCR0 covers TMU1 and IMR4/IMCR4 covers
 * TMU0. TMU0 TUNI0 is the RTOS tick.
 *
 * sh_intc treats a set bit as enabled, while an IMR bit masks. So IMCR (mask
 * clear) is wired as sh_intc's set_reg and IMR as its clr_reg. enum_ids[0]
 * is the MSB, matching the manual's bit-7-first order.
 */
#define CDJ_INTCA_BASE  0xA4080000
#define CDJ_INTCB_BASE  0xA4140000
#define IPR(n)          (CDJ_INTCA_BASE + 0x00 + (n) * 4)
#define IMR(n)          (CDJ_INTCA_BASE + 0x80 + (n) * 4)
#define IMCR(n)         (CDJ_INTCA_BASE + 0xC0 + (n) * 4)

static struct intc_vect cdj_intc_vectors[] = {
    INTC_VECT(CDJ_TMU0_TUNI0, 0x400),
    INTC_VECT(CDJ_TMU0_TUNI1, 0x420),
    INTC_VECT(CDJ_TMU0_TUNI2, 0x440),
    INTC_VECT(CDJ_TMU1_TUNI0, 0x920),
    INTC_VECT(CDJ_TMU1_TUNI1, 0x940),
    INTC_VECT(CDJ_TMU1_TUNI2, 0x960),
    /* External IRQ pins, manual table 13.4: IRQ0 H'600 .. IRQ7 H'6E0. */
    INTC_VECT(CDJ_IRQ0, 0x600), INTC_VECT(CDJ_IRQ1, 0x620),
    INTC_VECT(CDJ_IRQ2, 0x640), INTC_VECT(CDJ_IRQ3, 0x660),
    INTC_VECT(CDJ_IRQ4, 0x680), INTC_VECT(CDJ_IRQ5, 0x6A0),
    INTC_VECT(CDJ_IRQ6, 0x6C0), INTC_VECT(CDJ_IRQ7, 0x6E0),
    /* USB0 (USI0), manual table 13.4: vector H'A20, IPRF [7:4], IMR9 bit 1. */
    INTC_VECT(CDJ_USB0, 0xA20),
    INTC_VECT(CDJ_ETHI, 0xD60),
    /* DMAC0A DEI0..DEI3, manual table 13.4: H'800..H'860, IPRE [15:12],
     * IMR1 bits 3..0. */
    INTC_VECT(CDJ_DMAC0A_DEI0, 0x800), INTC_VECT(CDJ_DMAC0A_DEI1, 0x820),
    INTC_VECT(CDJ_DMAC0A_DEI2, 0x840), INTC_VECT(CDJ_DMAC0A_DEI3, 0x860),
    /* DMAC1A DEI0..DEI3, H'700..H'760: the SPI link to the GUI board. The
     * firmware registers intno 0x38/0x39 for them (the kernel's intno is
     * INTEVT / 32). Their ISR wakes the SPI1_DMA_END task. */
    INTC_VECT(CDJ_DMAC1A_DEI0, 0x700), INTC_VECT(CDJ_DMAC1A_DEI1, 0x720),
    INTC_VECT(CDJ_DMAC1A_DEI2, 0x740), INTC_VECT(CDJ_DMAC1A_DEI3, 0x760),
    /* IIC0 and IIC1 AL/TACK/WAIT/DTE, manual table 13.4: H'E00..H'E60 and
     * H'D80..H'DE0. Registered by the T_CISR array at 0x080E3840 (intno
     * 0x70..0x73 and 0x6C..0x6F). The DSP transaction waits on DTE1I. */
    INTC_VECT(CDJ_IIC0_AL, 0xE00), INTC_VECT(CDJ_IIC0_TACK, 0xE20),
    INTC_VECT(CDJ_IIC0_WAIT, 0xE40), INTC_VECT(CDJ_IIC0_DTE, 0xE60),
    INTC_VECT(CDJ_IIC1_AL, 0xD80), INTC_VECT(CDJ_IIC1_TACK, 0xDA0),
    INTC_VECT(CDJ_IIC1_WAIT, 0xDC0), INTC_VECT(CDJ_IIC1_DTE, 0xDE0),
    /* The DSP link: DMAC1B DEI4/DEI5 (H'B00, H'B20; IPRK [11:8]; IMR10 bits
     * 4 and 5) and MSIOFI0 (H'C80; IPRH [15:12]; IMR6 bit 0). The T_CISRs at
     * 0x080AE040/54/68 register intno 0x58, 0x59, 0x64 with handlers
     * 0x08326E50, 0x08326E62 and 0x08326EAE. */
    INTC_VECT(CDJ_DMAC1B_DEI4, 0xB00), INTC_VECT(CDJ_DMAC1B_DEI5, 0xB20),
    INTC_VECT(CDJ_MSIOFI0, 0xC80),
};

/* Sources sharing one priority field are an sh_intc group: the group
 * carries the priority, each source its mask bit. */
static struct intc_group cdj_intc_groups[] = {
    INTC_GROUP(CDJ_DMAC0A, CDJ_DMAC0A_DEI0, CDJ_DMAC0A_DEI1,
               CDJ_DMAC0A_DEI2, CDJ_DMAC0A_DEI3),
    INTC_GROUP(CDJ_DMAC1A, CDJ_DMAC1A_DEI0, CDJ_DMAC1A_DEI1,
               CDJ_DMAC1A_DEI2, CDJ_DMAC1A_DEI3),
    INTC_GROUP(CDJ_IIC0, CDJ_IIC0_AL, CDJ_IIC0_TACK,
               CDJ_IIC0_WAIT, CDJ_IIC0_DTE),
    INTC_GROUP(CDJ_IIC1, CDJ_IIC1_AL, CDJ_IIC1_TACK,
               CDJ_IIC1_WAIT, CDJ_IIC1_DTE),
    INTC_GROUP(CDJ_DMAC1B, CDJ_DMAC1B_DEI4, CDJ_DMAC1B_DEI5),
};

/* All 13 IMR/IMCR pairs are declared so undecoded ones still hold their
 * values for the firmware's read-modify-writes. */
static struct intc_mask_reg cdj_intc_mask_registers[] = {
    /* IMR0/IMCR0: -, TUNI2, TUNI1, TUNI0 (TMU1), SDHII3..0 */
    { IMCR(0), IMR(0), 8, { 0, CDJ_TMU1_TUNI2, CDJ_TMU1_TUNI1, CDJ_TMU1_TUNI0 } },
    /* IMR1/IMCR1: VOUI, VEU1I, BEU0I, CEU0I (VIO), then DEI3..DEI0 (DMAC0A) */
    { IMCR(1), IMR(1), 8, { 0, 0, 0, 0, CDJ_DMAC0A_DEI3, CDJ_DMAC0A_DEI2,
                            CDJ_DMAC0A_DEI1, CDJ_DMAC0A_DEI0 } },
    /* IMR2/IMCR2, manual table 13.5: -, -, -, VPUI, ATAPI, EtherMAC, -,
     * SCIFA0. Index 0 is the MSB, so EtherMAC's bit 2 is index 5. */
    { IMCR(2), IMR(2), 8, { 0, 0, 0, 0, 0, CDJ_ETHI, 0, 0 } },
    /* IMR3/IMCR3: DEI3..DEI0 (DMAC1A) in bits 7..4. The SPI-link driver
     * writes 0x10 for channel 0 and 0x20 for channel 1. */
    { IMCR(3), IMR(3), 8, { CDJ_DMAC1A_DEI3, CDJ_DMAC1A_DEI2,
                            CDJ_DMAC1A_DEI1, CDJ_DMAC1A_DEI0 } },
    /* IMR4/IMCR4: -, TUNI2, TUNI1, TUNI0 (TMU0), JPUI, -, -, LCDCI */
    { IMCR(4), IMR(4), 8, { 0, CDJ_TMU0_TUNI2, CDJ_TMU0_TUNI1, CDJ_TMU0_TUNI0 } },
    { IMCR(5),  IMR(5),  8, { 0 } },
    /* IMR6/IMCR6: -, -, ICBI, SCIFA4, CEU1I, -, MSIOFI1, MSIOFI0. The
     * firmware writes IMCR6 = 0x01 (MSIOFI0). */
    { IMCR(6),  IMR(6),  8, { 0, 0, 0, 0, 0, 0, 0, CDJ_MSIOFI0 } },
    /* IMR7/IMCR7, manual table 13.5: DTE0I, WAIT0I, TACK0I, AL0I (I2C0) in
     * bits 7..4, then DTE1I, WAIT1I, TACK1I, AL1I (I2C1) in bits 3..0. The IIC
     * driver writes IMCR7 itself, 0x0F for channel 1 and 0xF0 for channel 0,
     * from a computed base (0x083AF986 / 0x083AF9D2). */
    { IMCR(7),  IMR(7),  8, { CDJ_IIC0_DTE, CDJ_IIC0_WAIT, CDJ_IIC0_TACK,
                              CDJ_IIC0_AL, CDJ_IIC1_DTE, CDJ_IIC1_WAIT,
                              CDJ_IIC1_TACK, CDJ_IIC1_AL } },
    { IMCR(8),  IMR(8),  8, { 0 } },
    /* IMR9/IMCR9: -, -, -, CMTI, -, USI1, USI0, - */
    { IMCR(9),  IMR(9),  8, { 0, 0, 0, 0, 0, 0, CDJ_USB0, 0 } },
    /* IMR10/IMCR10: -, DADERR, DEI5, DEI4 (DMAC1B) in bits 6..4, then the RTC
     * in bits 2..0. DEI4 and DEI5 are the DSP link's two DMA channels. */
    { IMCR(10), IMR(10), 8, { 0, 0, CDJ_DMAC1B_DEI5, CDJ_DMAC1B_DEI4 } },
    { IMCR(11), IMR(11), 8, { 0 } },
    { IMCR(12), IMR(12), 8, { 0 } },
    /* INTC-B INTMSK00 / INTMSKCLR00. Manual 13.3.6: bit 7 is IRQ0 down to
     * bit 0 = IRQ7. The firmware's INTMSKCLR00 = 0x04 unmasks IRQ5. */
    { CDJ_INTCB_BASE + 0x64, CDJ_INTCB_BASE + 0x44, 8,
      { CDJ_IRQ0, CDJ_IRQ1, CDJ_IRQ2, CDJ_IRQ3,
        CDJ_IRQ4, CDJ_IRQ5, CDJ_IRQ6, CDJ_IRQ7 } },
};

static struct intc_prio_reg cdj_intc_prio_registers[] = {
    /* IPRA: TMU0 TUNI0 [15:12], TUNI1 [11:8], TUNI2 [7:4], IrDA [3:0] */
    { IPR(0), 0, 16, 4, { CDJ_TMU0_TUNI0, CDJ_TMU0_TUNI1, CDJ_TMU0_TUNI2, 0 } },
    /* IPRB: JPU [15:12], LCDC [11:8], DMAC1A [7:4], BEU0 [3:0]. Reads 0x8040
     * once booted; 4 is the isrpri of both SPI-link T_CISRs. */
    { IPR(1), 0, 16, 4, { 0, 0, CDJ_DMAC1A, 0 } },
    /* IPRC: TMU1 TUNI0 [15:12], TUNI1 [11:8], TUNI2 [7:4] */
    { IPR(2), 0, 16, 4, { CDJ_TMU1_TUNI0, CDJ_TMU1_TUNI1, CDJ_TMU1_TUNI2, 0 } },
    { IPR(3),  0, 16, 4, { 0 } },
    /* IPRE: DMAC0A [15:12], VIO [11:8], SCIFA3 [7:4], VPU5F [3:0] */
    { IPR(4),  0, 16, 4, { CDJ_DMAC0A, 0, 0, 0 } },
    /* IPRF: KEYSC [15:12], DMAC0B [11:8], USB0/USB1 [7:4], CMT [3:0] */
    { IPR(5),  0, 16, 4, { 0, 0, CDJ_USB0, 0 } },
    { IPR(6),  0, 16, 4, { 0 } },
    /* IPRH: MSIOFI0 [15:12], MSIOFI1 [11:8], I2C1 [7:4], I2C0 [3:0]. The IIC
     * channels set their nibbles at 0x083AF952 and 0x083AF9A8. */
    { IPR(7),  0, 16, 4, { CDJ_MSIOFI0, 0, CDJ_IIC1, CDJ_IIC0 } },
    { IPR(8),  0, 16, 4, { 0 } },
    /* IPRJ: CEU2_1 [15:12], EtherMAC [11:8], FSI [7:4], SDHI1 [3:0]. */
    { IPR(9),  0, 16, 4, { 0, CDJ_ETHI, 0, 0 } },
    /* IPRK: RTC [15:12], DMAC1B [11:8]. */
    { IPR(10), 0, 16, 4, { 0, CDJ_DMAC1B, 0, 0 } },
    { IPR(11), 0, 16, 4, { 0 } },
    /* INTC-B INTPRI00: 4-bit fields, IRQ0 at [31:28] down to IRQ7 at [3:0].
     * The firmware writes 0x400: IRQ5 priority 4. */
    { CDJ_INTCB_BASE + 0x10, 0, 32, 4,
      { CDJ_IRQ0, CDJ_IRQ1, CDJ_IRQ2, CDJ_IRQ3,
        CDJ_IRQ4, CDJ_IRQ5, CDJ_IRQ6, CDJ_IRQ7 } },
};

struct intc_desc cdj_intc;

/* Return the INTC to its power-on state on machine reset. sh_intc keeps its
 * register values in the static arrays and nothing else resets them; a stale
 * timer enable would fire before the firmware refills its dispatch table
 * (0x0AC94F14, in bss) and reset the machine again. */
static void cdj_intc_reset(void *opaque)
{
    struct intc_desc *desc = &cdj_intc;
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(cdj_intc_mask_registers); i++) {
        cdj_intc_mask_registers[i].value = 0;
    }
    for (i = 0; i < ARRAY_SIZE(cdj_intc_prio_registers); i++) {
        cdj_intc_prio_registers[i].value = 0;
    }
    for (i = 0; i < (unsigned)desc->nr_sources; i++) {
        desc->sources[i].asserted = 0;
        desc->sources[i].enable_count = 0;
        desc->sources[i].pending = 0;
    }
    desc->pending = 0;
}

void cdj_intc_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    sh_intc_init(sysmem, &cdj_intc, CDJ_INTC_NR_SOURCES,
                 _INTC_ARRAY(cdj_intc_mask_registers),
                 _INTC_ARRAY(cdj_intc_prio_registers));
    sh_intc_register_sources(&cdj_intc,
                             _INTC_ARRAY(cdj_intc_vectors),
                             _INTC_ARRAY(cdj_intc_groups));
    cpu->env.intc_handle = &cdj_intc;
    qemu_register_reset(cdj_intc_reset, NULL);
}

/* CDJ_IRQ5_PROBE_MS=<period>: toggle IRQ5 on a timer. The firmware enables
 * IRQ5 but nothing on this board drives it; probably the SCIFA4 peer's
 * signal line. A probe, not a model. */
static QEMUTimer *cdj_irq5_timer;
static int64_t cdj_irq5_period_ms;
static unsigned cdj_irq5_pulses;
static int cdj_irq5_level;

static void cdj_irq5_fire(void *opaque)
{
    /* Level, not qemu_irq_pulse(): a pulse is lost if SR.IMASK blocks it
     * at that instant. */
    cdj_irq5_level = !cdj_irq5_level;
    qemu_set_irq(cdj_intc.irqs[CDJ_IRQ5], cdj_irq5_level);
    if (++cdj_irq5_pulses <= 6) {
        qemu_log("irq5-probe: line -> %d\n", cdj_irq5_level);
    }
    timer_mod(cdj_irq5_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + cdj_irq5_period_ms);
}

void cdj_irq5_probe_init(void)
{
    const char *env = getenv("CDJ_IRQ5_PROBE_MS");

    if (!env || !*env) {
        return;
    }
    cdj_irq5_period_ms = strtoll(env, NULL, 0);
    if (cdj_irq5_period_ms <= 0) {
        return;
    }
    info_report("cdj2000nxs2: IRQ5 probe active, pulsing every %" PRId64 " ms",
                cdj_irq5_period_ms);
    cdj_irq5_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, cdj_irq5_fire, NULL);
    timer_mod(cdj_irq5_timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + cdj_irq5_period_ms);
}

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

    /* sh_intc forwards a source only when enable_count reaches enable_max,
     * and the CPU accepts it only above SR.IMASK; print both. */
    for (i = 1; i < CDJ_INTC_NR_SOURCES && i < (unsigned)cdj_intc.nr_sources;
         i++) {
        struct intc_source *src = &cdj_intc.sources[i];

        /* The DMAC0A DEIs are always printed. */
        if (src->asserted || src->pending ||
            (src->vect >= 0x800 && src->vect <= 0x860)) {
            info_report("intc: src=%u vect=0x%03x asserted=%d pending=%d "
                        "enable_count=%d enable_max=%d prio=%d",
                        i, src->vect, src->asserted, src->pending,
                        src->enable_count, src->enable_max,
                        sh_intc_vector_priority(&cdj_intc, src->vect));
        }
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
                                  : CDJ_PERIPH_FREQ;

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
 * CCN / exception registers (0xFF000000).
 *
 * The interrupt entry at VBR+0x600 dispatches on INTEVT (0xFF000028), so
 * EXPEVT and INTEVT expose QEMU's env->expevt/intevt. The rest of the block
 * is a plain register file.
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

    switch (off) {
    case CCN_EXPEVT:
        return s->cpu->env.expevt;
    case CCN_INTEVT:
        /* With CDJ_DMAC_DEBUG, log the first few DMAC0A DEI dispatches. */
        if (s->cpu->env.intevt >= 0x800 && s->cpu->env.intevt <= 0x860 &&
            cdj_ccn_dei_seen < 6 && getenv("CDJ_DMAC_DEBUG")) {
            cdj_ccn_dei_seen++;
            info_report("ccn: INTEVT 0x%03x read at pc=0x%08x (spc=0x%08x "
                        "sr=0x%08x) #%u", s->cpu->env.intevt, s->cpu->env.pc,
                        s->cpu->env.spc, s->cpu->env.sr, cdj_ccn_dei_seen);
        }
        return s->cpu->env.intevt;
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


