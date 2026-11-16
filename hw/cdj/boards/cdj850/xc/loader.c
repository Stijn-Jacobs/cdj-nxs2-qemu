/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "loader.h"
#include "mtu2.h"
#include "port.h"
#include "trace.h"

/*
 * CD slot-in loading mechanism: motor drive in, position switches out.
 *
 * Derived from the MAIN firmware (static reverse engineering; addresses are
 * in the MAIN image, load 0x0C000000):
 *   - The loader state machine at 0x0C105E0E (with the insertion detector at
 *     0x0C105DA4, run from the CD task tick 0x0C0FF7D4) tests the switches
 *     as PFDRH bit 9 = LPS1 (PF25, IC103 pin 135) and bit 10 = LPS2 (PF26,
 *     pin 134), each pulled up by 10 k and closed to ground by a mechanism
 *     switch (S1702 and S1701). Low means the switch is closed.
 *   - Loader init 0x0C0FC170 stops the motor, then picks the first state
 *     from the switches: (LPS1,LPS2) = (1,0) disc already loaded; (0,0)
 *     mid-load; (0,1) and (1,1) run the unload/home sequence.
 *   - The motor is a DC motor behind a BD7956FS driver. PD9 (PDDRL bit 9,
 *     DRVMUTE2 -> CTL1) enables it. The drive level is PWM from MTU2
 *     channel 3 on TIOC3C (PD10 = PWMLO), programmed by 0x0C106638(level)
 *     as TCR_3 0xA0, TIORL_3, TGRC_3 (period - 1), TGRD_3 (high time),
 *     TMDR_3 2 (PWM mode 1). PWMLO goes through an inverter and an RC to the
 *     driver's LDIN input, so LDIN is about 3.3 V * (1 - duty) and the
 *     motor runs against the reference at 1.65 V: u = 1 - 2*duty, positive
 *     = direction A, negative = direction B. The five levels the firmware
 *     uses: 0 (TIORL_3 0x65, TGRC_3 1, TGRD_3 0xFFFF: output held low,
 *     u = +1), 1 (TGRC 100, TGRD 25: u = +0.5), 2 (50: u = 0, always with
 *     PD9 = 0), 3 (75: u = -0.5), 4 (99: u = -0.96).
 *   - Home sequence with no disc (states 1, 5, 6, 7, 8, 9, 10, 11; error
 *     path 0x0C106218 = "E90"): drive A at level 1 until LPS2 goes low
 *     (confirmed three SM calls later), drive B at level 3 until LPS2 goes
 *     high again (confirmed likewise), a short A pulse at level 1 as a
 *     brake, then stop with the state byte at 0 and the status byte at 2.
 *     The first LPS2 edge must come within 2000 kernel ticks of the start
 *     of the A drive, the second within the same 2000. LPS2 must then stay
 *     high, or the insertion detector starts a load. LPS1 must be high when
 *     state 6 evaluates it.
 *   - Load (state 0x81, level 0): LPS1 must fall within 500 ticks and rise
 *     again within 3000; loaded is (LPS1,LPS2) = (1,0). Unload from loaded
 *     (level 4) crosses the LPS1 zone the other way. Not exercised here.
 *
 * Assumed (nothing in the firmware or the service manual decides these):
 *   - Which physical direction is A. The model calls A "load" because the
 *     load state uses level 0 and unload uses level 4.
 *   - All geometry: one position variable x in arbitrary units, end stops 0
 *     and 1400, LPS1 low on [200, 1200], LPS2 low from 400 up (so the order
 *     along A is LPS1 closes, LPS2 closes, LPS1 opens, end stop), 4 units
 *     of release hysteresis on LPS2 (a snap switch), start position 150
 *     (both switches open). The service manual p.6 says clamped means LPS1
 *     low, which conflicts with the code's loaded = (1,0); the code wins.
 *   - Speed 0.58 units/ms at |u| = 1 and a first-order motor lag of 17 ms.
 *     The firmware counts kernel ticks, nominally 1 ms; the emulator's CMT0
 *     tick has been measured at about 1.7 ms. Both were checked with a
 *     Python re-implementation of the state machine, for tick lengths
 *     0.7 .. 3 ms. The lag is needed so the 7-call brake pulse cannot push
 *     the cam back over the LPS2 edge. Motor off (PD9 = 0, or |u| < 0.1)
 *     decays the speed with the same lag.
 *   - The model state is not kept across emulator resets.
 *
 * The model is stepped by a QEMU_CLOCK_VIRTUAL timer every 1 ms. A late
 * timer is caught up with one step per missed millisecond. Each step reads
 * PDDRL from the port model and channel 3 from the MTU2 capture; while the
 * firmware has channel 3 stopped to reprogram it, the last duty stays.
 * Disc insertion and eject are not modelled; a later hook would push x
 * (or add an offset in loader_switches()) and nothing else here changes.
 */
#define LOADER_STEP_NS      1000000
#define LOADER_CATCHUP_MAX  64
#define LOADER_PD9          0x0200

/* Fixed point: positions and speeds in 1/65536 unit (speed per ms). */
#define LOADER_Q            65536
#define LOADER_X_END        (1400 * LOADER_Q)
#define LOADER_X_START      (150 * LOADER_Q)
#define LOADER_P1           (200 * LOADER_Q)    /* LPS1 closes */
#define LOADER_P2           (1200 * LOADER_Q)   /* LPS1 opens */
#define LOADER_Q1           (400 * LOADER_Q)    /* LPS2 closes */
#define LOADER_Q1_HYST      (4 * LOADER_Q)      /* LPS2 opens at Q1 - this */
#define LOADER_VMAX         38011               /* 0.58 unit/ms */
#define LOADER_TAU_MS       17
#define LOADER_DEADBAND     (LOADER_Q / 10)     /* |u| below this: stopped */

#define PFDRH_LPS1          0x0200
#define PFDRH_LPS2          0x0400

typedef struct {
    QEMUTimer *timer;
    int64_t next_ns;
    int32_t x;              /* position, +A */
    int32_t v;              /* speed, +A */
    int32_t u;              /* last decoded drive, Q16, +A */
    bool lps1;              /* pin levels: 1 = open (pulled up) */
    bool lps2;
    int32_t drive_pct;      /* last logged drive, percent, 0 = stopped */
    bool lps2_was_low;
    bool homed;             /* info_report for the first low-high cycle done */
    bool fixed;             /* CDJ850_LOADER_SW: constant, no model */
    bool off;               /* CDJ850_LOADER=0 or fixed: no model */
    uint16_t fixed_pins;
} Loader;

static Loader ld;

uint16_t cdj850_loader_pf_hi_pins(void)
{
    if (ld.fixed) {
        return ld.fixed_pins;
    }
    if (ld.off) {
        return PFDRH_LPS1 | PFDRH_LPS2;
    }
    return (ld.lps1 ? PFDRH_LPS1 : 0) | (ld.lps2 ? PFDRH_LPS2 : 0);
}

/* TIOC3C duty as the firmware programs it (0x0C106638); see the header. */
static void loader_decode_pwm(void)
{
    uint8_t tiorl;
    uint16_t tgrc, tgrd;
    uint32_t period, hi;

    if (!cdj850_mtu2_ch3_pwm(&tiorl, &tgrc, &tgrd)) {
        return;                 /* stopped for reprogramming: keep the duty */
    }
    period = (uint32_t)tgrc + 1;
    if (tiorl == 0x65 && tgrd > tgrc) {
        hi = 0;                 /* held low at every TGRC match */
    } else if (tiorl == 0x56) {
        hi = MIN((uint32_t)tgrd, period);
    } else {
        return;                 /* not a configuration the loader uses */
    }
    ld.u = (int32_t)(((int64_t)period - 2 * (int64_t)hi) * LOADER_Q / period);
}

/* Switch levels from the position; the one place a later insert-disc hook
 * would change what the cam position means. */
static void loader_switches(void)
{
    bool lps1 = !(ld.x >= LOADER_P1 && ld.x <= LOADER_P2);
    bool lps2_low = !ld.lps2;

    if (lps2_low) {
        if (ld.x < LOADER_Q1 - LOADER_Q1_HYST) {
            lps2_low = false;
        }
    } else if (ld.x >= LOADER_Q1) {
        lps2_low = true;
    }
    if (lps1 != ld.lps1 && cdj850_trace()) {
        info_report("cdj850: loader LPS1 %d->%d at x=%.1f %" PRIu64 " ms",
                    ld.lps1, lps1, (double)ld.x / LOADER_Q, cdj850_vms());
    }
    ld.lps1 = lps1;
    if (!lps2_low != ld.lps2) {
        if (cdj850_trace()) {
            info_report("cdj850: loader LPS2 %d->%d at x=%.1f %" PRIu64
                        " ms", ld.lps2, !lps2_low,
                        (double)ld.x / LOADER_Q, cdj850_vms());
        }
        if (lps2_low) {
            ld.lps2_was_low = true;
        } else if (ld.lps2_was_low && !ld.homed) {
            ld.homed = true;
            info_report("cdj850: loader: homing observed, LPS2 low then "
                        "high again at x=%.1f (%" PRIu64 " ms)",
                        (double)ld.x / LOADER_Q, cdj850_vms());
        }
    }
    ld.lps2 = !lps2_low;
}

static void loader_step(void)
{
    bool en = cdj850_port_pddrl() & LOADER_PD9;
    int32_t target = 0, pct = 0;

    loader_decode_pwm();
    if (en && abs(ld.u) >= LOADER_DEADBAND) {
        target = (int32_t)((int64_t)ld.u * LOADER_VMAX / LOADER_Q);
        pct = (ld.u * 100 + (ld.u > 0 ? LOADER_Q / 2 : -LOADER_Q / 2)) /
              LOADER_Q;
    }
    if (pct != ld.drive_pct) {
        if (cdj850_trace()) {
            info_report("cdj850: loader motor %s (u=%+d%%, PD9=%d) at "
                        "x=%.1f %" PRIu64 " ms",
                        pct > 0 ? "A" : pct < 0 ? "B" : "stop", pct,
                        en, (double)ld.x / LOADER_Q, cdj850_vms());
        }
        ld.drive_pct = pct;
    }
    ld.v += (target - ld.v) / LOADER_TAU_MS;
    if (target == 0 && abs(ld.v) < 65) {
        ld.v = 0;               /* integer division stops short of zero */
    }
    ld.x += ld.v;
    if (ld.x <= 0) {
        ld.x = 0;
        ld.v = 0;
    } else if (ld.x >= LOADER_X_END) {
        ld.x = LOADER_X_END;
        ld.v = 0;
    }
    loader_switches();
}

static void loader_tick(void *opaque)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned n = 0;

    (void)opaque;
    while (ld.next_ns <= now && n < LOADER_CATCHUP_MAX) {
        loader_step();
        ld.next_ns += LOADER_STEP_NS;
        n++;
    }
    if (ld.next_ns <= now) {
        ld.next_ns = now + LOADER_STEP_NS;      /* far behind: resync */
    }
    timer_mod(ld.timer, ld.next_ns);
}

static const VMStateDescription vmstate_cdj850_loader = {
    .name = "cdj850-loader",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, typeof(ld)),
        VMSTATE_INT64(next_ns, typeof(ld)),
        VMSTATE_INT32(x, typeof(ld)),
        VMSTATE_INT32(v, typeof(ld)),
        VMSTATE_INT32(u, typeof(ld)),
        VMSTATE_BOOL(lps1, typeof(ld)),
        VMSTATE_BOOL(lps2, typeof(ld)),
        VMSTATE_INT32(drive_pct, typeof(ld)),
        VMSTATE_BOOL(lps2_was_low, typeof(ld)),
        VMSTATE_BOOL(homed, typeof(ld)),
        VMSTATE_END_OF_LIST()
    }
};

void cdj850_loader_init(void)
{
    const char *sw = getenv("CDJ850_LOADER_SW");
    const char *e = getenv("CDJ850_LOADER");

    ld.lps1 = ld.lps2 = true;
    if (sw) {
        ld.fixed = ld.off = true;
        ld.fixed_pins = strtoul(sw, NULL, 0) & 0x0700;
        info_report("cdj850: loader model off, switches fixed at 0x%04x "
                    "(CDJ850_LOADER_SW)", ld.fixed_pins);
        return;
    }
    if (e && *e == '0') {
        ld.off = true;
        info_report("cdj850: loader model disabled (CDJ850_LOADER=0), "
                    "switches read open");
        return;
    }
    ld.x = LOADER_X_START;
    ld.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, loader_tick, NULL);
    vmstate_register_any(NULL, &vmstate_cdj850_loader, &ld);
    loader_switches();
    ld.next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + LOADER_STEP_NS;
    timer_mod(ld.timer, ld.next_ns);
}
