/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/*
 * Interrupt controller for the SH7724. The TMU, SCIF and CCN glue every board
 * shares is in common/sh4_periph.c.
 */

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

void cdj_intc_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    cdj_intc_setup(sysmem, cpu, &cdj_intc, CDJ_INTC_NR_SOURCES,
                   _INTC_ARRAY(cdj_intc_mask_registers),
                   _INTC_ARRAY(cdj_intc_prio_registers),
                   _INTC_ARRAY(cdj_intc_vectors),
                   _INTC_ARRAY(cdj_intc_groups));
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

/* The sh_intc state behind the IRQ counts, printed after them at exit; the
 * DMAC0A DEIs (H'800..H'860) are always printed. */
void cdj_intc_exit_report(void)
{
    cdj_intc_report(&cdj_intc, CDJ_INTC_NR_SOURCES, 0x800, 0x860);
}
