/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "cmt.h"
#include "intc.h"
#include "trace.h"

/*
 * SH7263 INTC at 0xFFFE0800: register storage plus the external IRQ0-7
 * inputs (SH7263 Hardware Manual section 6).
 *
 *   +0x02 ICR1   IRQn sense, two bits per pin (IRQ0 in bits 1:0 ... IRQ7 in
 *                15:14): 00 low level, 01 falling edge, 10 rising edge,
 *                11 both edges.
 *   +0x06 IRQRR  IRQnF request flags, bit n. Edge modes latch the flag on
 *                the selected edge; it clears on acceptance or when the
 *                firmware writes 0 to a flag it has read as 1. Level mode
 *                mirrors the pin.
 *   +0x18 IPR01  priorities IRQ0 15:12, IRQ1 11:8, IRQ2 7:4, IRQ3 3:0.
 *   +0x1A IPR02  priorities IRQ4 15:12 ... IRQ7 3:0. Priority 0 masks.
 *
 * IRQn is vector 64+n. The MAIN firmware confirms both the numbering and
 * that it wants these interrupts: its ISR descriptors register
 *   65 IRQ1 -> 0x0C105004   66 IRQ2 -> 0x0C107484   68 IRQ4 -> 0x0C0773EE
 *   69 IRQ5 -> 0x0C0FA05E   71 IRQ7 -> 0x0C076624
 * and its drivers program ICR1 = 0x4924 (IRQ1 falling, IRQ2 rising, IRQ4
 * falling, IRQ5 rising, IRQ7 falling), IPR01 = 0x0770, IPR02 = 0x7907.
 *
 * Pin routing on this board (PFC values the boot block writes, service
 * manual 10.2 MAIN ASSY 1/4 for the nets):
 *   IRQ1 = PB1, net SRVSBSY (servo)     IRQ2 = PB2, net SPDLFG (spindle FG)
 *   IRQ4 = PB4, net ZERO                IRQ5 = PE9, net PNL_XBUSY (panel)
 *   IRQ6 = PE10, net USB_CDETFLG (USB-A over-current flag, PECRL3 =
 *          0x2220; priority 0, polled through PEDRL instead)
 *   IRQ7 = PE11, net DSPDREQ (audio DSP)
 *
 * Everything else in the block stays plain storage so read-modify-writes
 * read back. On-chip peripheral interrupts are raised by their own models
 * (only CMT0 exists so far, xc/cmt.c).
 *
 * Accepted flags are cleared when the request is queued, not when the CPU
 * takes it: the queue in xc/cmt.c has no acceptance callback. A request
 * that arrives while its priority is 0 stays flagged and is delivered when
 * the firmware raises the priority, as on hardware.
 *
 * CDJ850_IRQ_PULSE="n@ms[,n@ms...]" (default unset = no pulses) inverts
 * the IRQn pin for 1 ms at the given virtual time. It is a diagnostic for
 * exercising a handler before the peer that drives the pin is modelled;
 * it invents an event, so nothing may depend on it.
 */
#define CDJ850_INTC_BASE    0xFFFE0800
#define CDJ850_INTC_SIZE    0x800
#define INTC_ICR1           0x02
#define INTC_IRQRR          0x06
#define INTC_IPR01          0x18
#define INTC_IPR02          0x1A
#define CDJ850_IRQ0_VECTOR  64
#define CDJ850_PULSE_MAX    16

typedef struct {
    unsigned irq;
    bool restore;
    QEMUTimer *timer;
} Cdj850IrqPulse;

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_INTC_SIZE];
    uint8_t pins;            /* current level of the IRQ0-7 pins */
    uint8_t flags;           /* IRQRR latched requests */
    uint8_t read_set;        /* flags the firmware has read as 1 */
    QEMUTimer *level_timer;  /* settles low-level requests, see below */
    Cdj850IrqPulse pulse[CDJ850_PULSE_MAX];
} Cdj850Intc;

static Cdj850Intc *cdj850_intc;

static uint16_t intc_reg16(Cdj850Intc *s, unsigned off)
{
    return (s->reg[off] << 8) | s->reg[off + 1];
}

static unsigned intc_sense(Cdj850Intc *s, unsigned irq)
{
    return (intc_reg16(s, INTC_ICR1) >> (irq * 2)) & 3;
}

static unsigned intc_prio(Cdj850Intc *s, unsigned irq)
{
    uint16_t ipr = intc_reg16(s, irq < 4 ? INTC_IPR01 : INTC_IPR02);

    return (ipr >> ((3 - (irq & 3)) * 4)) & 0xf;
}

/*
 * settled = false defers low-level requests by 100 us. The firmware writes
 * a pin's priority one instruction before its edge sense, so for that
 * instant the pin is in the reset-default low-level mode; a request that
 * does not outlive the window is not delivered.
 */
static void intc_update(Cdj850Intc *s, bool settled)
{
    unsigned irq;

    for (irq = 0; irq < 8; irq++) {
        unsigned prio = intc_prio(s, irq);
        bool level_mode = intc_sense(s, irq) == 0;
        bool req = level_mode ? !(s->pins & (1u << irq))
                              : (s->flags & (1u << irq));

        if (!req || !prio) {
            continue;
        }
        if (level_mode && !settled) {
            timer_mod(s->level_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 100000);
            continue;
        }
        if (!level_mode) {
            s->flags &= ~(1u << irq);
            s->read_set &= ~(1u << irq);
        }
        if (cdj850_trace() || getenv("CDJ850_IRQ_DEBUG")) {
            info_report("cdj850: intc IRQ%u -> vector %u level %u at %"
                        PRIu64 " ms", irq, CDJ850_IRQ0_VECTOR + irq, prio,
                        cdj850_vms());
        }
        cdj850_irq_raise(s->cpu, CDJ850_IRQ0_VECTOR + irq, prio);
    }
}

static void intc_level_settle(void *opaque)
{
    intc_update(opaque, true);
}

void cdj850_intc_set_pin(unsigned irq, bool level)
{
    Cdj850Intc *s = cdj850_intc;
    bool old;

    if (!s || irq > 7) {
        return;
    }
    old = s->pins & (1u << irq);
    if (old == level) {
        return;
    }
    s->pins = (s->pins & ~(1u << irq)) | (level ? 1u << irq : 0);

    switch (intc_sense(s, irq)) {
    case 1:
        if (!level) {
            s->flags |= 1u << irq;
        }
        break;
    case 2:
        if (level) {
            s->flags |= 1u << irq;
        }
        break;
    case 3:
        s->flags |= 1u << irq;
        break;
    default:
        break;
    }
    intc_update(s, false);
}

bool cdj850_intc_get_pin(unsigned irq)
{
    return cdj850_intc && irq < 8 && (cdj850_intc->pins & (1u << irq));
}

uint16_t cdj850_intc_reg16(unsigned off)
{
    if (!cdj850_intc || off + 1 >= CDJ850_INTC_SIZE) {
        return 0;
    }
    return intc_reg16(cdj850_intc, off);
}

static uint8_t intc_irqrr(Cdj850Intc *s)
{
    uint8_t v = 0;
    unsigned irq;

    for (irq = 0; irq < 8; irq++) {
        if (intc_sense(s, irq) == 0) {
            v |= (s->pins & (1u << irq)) ? 0 : 1u << irq;
        } else {
            v |= s->flags & (1u << irq);
        }
    }
    return v;
}

static uint64_t cdj850_intc_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Intc *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = (off + i) & (CDJ850_INTC_SIZE - 1);
        uint8_t b = s->reg[a];

        if (a == INTC_IRQRR + 1) {
            b = intc_irqrr(s);
            s->read_set |= b;
        } else if (a == INTC_IRQRR) {
            b = 0;
        }
        v = (v << 8) | b;
    }
    if (cdj850_trace()) {
        info_report("cdj850: intc read +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)v,
                    s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_intc_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    Cdj850Intc *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = (off + size - 1 - i) & (CDJ850_INTC_SIZE - 1);
        uint8_t b = val >> (8 * i);

        if (a == INTC_IRQRR + 1) {
            /* Only a 0 written to a flag already read as 1 clears it. */
            uint8_t clr = ~b & s->read_set;

            s->flags &= ~clr;
            s->read_set &= ~clr;
            continue;
        }
        s->reg[a] = b;
    }
    if (cdj850_trace()) {
        info_report("cdj850: intc write +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
    /* A priority or sense change can release a latched request. */
    intc_update(s, false);
}

static const MemoryRegionOps cdj850_intc_ops = {
    .read = cdj850_intc_read,
    .write = cdj850_intc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_intc = {
    .name = "cdj850-intc",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Intc, CDJ850_INTC_SIZE),
        VMSTATE_UINT8(pins, Cdj850Intc),
        VMSTATE_UINT8(flags, Cdj850Intc),
        VMSTATE_UINT8(read_set, Cdj850Intc),
        VMSTATE_TIMER_PTR(level_timer, Cdj850Intc),
        VMSTATE_END_OF_LIST()
    }
};

static void intc_pulse_fire(void *opaque)
{
    Cdj850IrqPulse *p = opaque;

    cdj850_intc_set_pin(p->irq, !cdj850_intc_get_pin(p->irq));
    if (!p->restore) {
        p->restore = true;
        timer_mod(p->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
}

static void intc_pulse_setup(Cdj850Intc *s)
{
    const char *e = getenv("CDJ850_IRQ_PULSE");
    unsigned n = 0;

    while (e && *e && n < CDJ850_PULSE_MAX) {
        unsigned irq, ms;
        int used = 0;

        if (sscanf(e, "%u@%u%n", &irq, &ms, &used) != 2 || irq > 7) {
            warn_report("cdj850: bad CDJ850_IRQ_PULSE entry at '%s'", e);
            break;
        }
        s->pulse[n].irq = irq;
        s->pulse[n].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, intc_pulse_fire,
                                         &s->pulse[n]);
        timer_mod(s->pulse[n].timer, (int64_t)ms * 1000000);
        warn_report("cdj850: DIAGNOSTIC pulse on IRQ%u at %u ms (invented "
                    "event, CDJ850_IRQ_PULSE)", irq, ms);
        n++;
        e += used;
        if (*e == ',') {
            e++;
        }
    }
}

void cdj850_intc_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Intc *s = g_new0(Cdj850Intc, 1);

    s->cpu = cpu;
    const char *idle = getenv("CDJ850_IRQ_IDLE");

    /*
     * Idle pin levels, bit n = IRQn; 0xC0 = DSPDREQ (IRQ7) and USB_CDETFLG
     * (IRQ6) idle high. DSPDREQ: R513 10 k pull-up, low pulse per
     * transaction (service manual 10.14 waveform 5). USB_CDETFLG (PE10,
     * IC103 pin 71, from the USB-A power switch IC2003 on the CMPX board):
     * service manual 10.15 "H (3.3 V): Normal, L (0 V): Overcurrent
     * detection", and [4-1] step 3 (VBUSON H with CODETFLG L = short). The
     * firmware arms IRQ6 at priority 0 (IPR02 = 0x7907) and instead polls
     * PEDRL bit 10 at 0x0C10DCEA, latching an over-current after 15
     * consecutive lows (counter 0x0C1E1270, flag 0x0C1E1271); before this
     * the pin read 0 and that fault was reported continuously. No
     * over-current is modelled. PNL_XBUSY (IRQ5) idles low and is driven by the panel
     * peer, xc/panel_peer.c. SRVSBSY (IRQ1), SPDLFG (IRQ2) and ZERO (IRQ4)
     * have no peer model and read low. An earlier inference that IRQ1 and
     * IRQ5 idle high is withdrawn: the firmware arms those pins at a nonzero
     * priority inside an interrupt-locked section, while the pin is still
     * in low-level mode, so the brief low-level window proves nothing about
     * the resting level. CDJ850_IRQ_IDLE overrides.
     */
    s->pins = idle ? strtoul(idle, NULL, 0) : 0xC0;
    s->level_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, intc_level_settle, s);
    cdj850_intc = s;
    vmstate_register_any(NULL, &vmstate_cdj850_intc, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_intc_ops, s,
                          "cdj850.intc", CDJ850_INTC_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, CDJ850_INTC_BASE, &s->iomem,
                                        1);
    intc_pulse_setup(s);
}
