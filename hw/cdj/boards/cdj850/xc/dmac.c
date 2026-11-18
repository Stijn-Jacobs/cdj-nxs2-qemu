/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "qemu/timer.h"
#include "cmt.h"
#include "dmac.h"
#include "intc.h"
#include "ssi.h"
#include "trace.h"
#include "usb.h"

/*
 * SH7263 DMAC at 0xFFFE1000 (SH7263 Hardware Manual section 10), modelled
 * for the one transfer the MAIN firmware needs it for: USB mass-storage
 * sector data out of the USB module's D0FIFO/D1FIFO.
 *
 * Registers (Table 10.2): channel n (0-7) SAR +0x10n, DAR +0x10n+4, DMATCR
 * +0x10n+8, CHCR +0x10n+0xC; reload set at +0x100+0x10n; DMAOR +0x200;
 * DMARS0-3 +0x300/+0x304/+0x308/+0x30C (DMARSk: channel 2k+1 in the high
 * byte, 2k in the low byte, each MID[5:0]:RID[1:0]). All of it is storage;
 * CHCR TE (bit 1) / HE (bit 19) and DMAOR AE (bit 2) / NMIF (bit 1) only
 * clear when written 0 (10.3.4, 10.3.8).
 *
 * Established from the firmware (USB read, CDJ850_TRACE): channel 7,
 * SAR = 0xFFFC1C14 (D0FIFO), DAR = buffer, DMATCR = 0x80 (512 bytes),
 * CHCR = 0x4815 (DM = 01 increment, SM = 00 fixed, RS = 1000 DMARS, TS =
 * 10 longword, IE, DE), DMARS3 = 0x0300 (channel 7 = MID 000000 RID 11 =
 * USB_DMA0 "receive FIFO full", Table 10.4), DMAOR = 0x0001 (DME), IPR07 =
 * 0x7007 (DEI7 priority 7). DEIn is vector 108 + 4n, priority IPR06 (ch0-3)
 * / IPR07 (ch4-7) nibbles, high nibble first (Table 6.4).
 *
 * Model: a channel with DE and DME set, TE clear, RS = 1000 and a USB
 * request byte in DMARS moves one TS unit per request while the USB module
 * holds its request (xc/usb.c cdj850_usb_dma_request(): receive data in the
 * pipe's buffer), SAR/DAR stepped per SM/DM; at DMATCR = 0 it sets TE and,
 * with IE, holds DEIn until TE or IE is cleared. Receive transfers run from
 * a 10 us virtual-time timer after the request, not inside the register
 * access that raised it; transmit transfers run inside the USB PIPEnCTR
 * write that starts the pipe (xc/usb.c usb_tx_dma(), why there). Seen in
 * the firmware's write path: channel 7, SAR = buffer, DAR = 0xFFFC1C14,
 * CHCR = 0x1815 (SM = 01, DM = 00), DMATCR = 0x80.
 *
 * Not modelled: every other request source (auto request, SSI, SCIF, ...),
 * reload (RLDSAR/RLDDAR), half-end, burst and
 * intermittent timing, address errors. Such channels are stored, not run.
 */
#define CDJ850_DMAC_BASE    0xFFFE1000
#define CDJ850_DMAC_SIZE    0x400
#define CDJ850_DMAC_CHANS   8
#define DMAC_DMAOR          0x200
#define DMAC_DMARS0         0x300
#define CHCR_DE             0x00000001u
#define CHCR_TE             0x00000002u
#define CHCR_IE             0x00000004u
#define CHCR_HE             0x00080000u
#define DMAOR_DME           0x0001
#define DMAOR_FLAGS         0x0006            /* AE, NMIF */
#define CDJ850_DEI0_VECTOR  108
#define CDJ850_INTC_IPR06   0x400             /* 0xFFFE0C00 */
#define CDJ850_DMAC_DELAY_NS 10000
#define CDJ850_DMAC_IRQ_POLL_NS 100000

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    QEMUTimer *run;
    QEMUTimer *irq_poll;
    uint8_t reg[CDJ850_DMAC_SIZE];
    bool dei[CDJ850_DMAC_CHANS];
    unsigned units[CDJ850_DMAC_CHANS];        /* moved, for the trace */
} Cdj850Dmac;

static Cdj850Dmac *cdj850_dmac;

static const unsigned dmac_unit[4] = { 1, 2, 4, 16 };

static uint32_t rd32(Cdj850Dmac *s, unsigned off)
{
    return ldl_be_p(&s->reg[off]);
}

static void wr32(Cdj850Dmac *s, unsigned off, uint32_t v)
{
    stl_be_p(&s->reg[off], v);
}

static uint16_t rd16(Cdj850Dmac *s, unsigned off)
{
    return lduw_be_p(&s->reg[off]);
}

static uint8_t dmac_dmars(Cdj850Dmac *s, unsigned ch)
{
    uint16_t v = rd16(s, DMAC_DMARS0 + 4 * (ch / 2));

    return (ch & 1) ? v >> 8 : v & 0xff;
}

static unsigned dmac_prio(unsigned ch)
{
    uint16_t ipr = cdj850_intc_reg16(CDJ850_INTC_IPR06 + 2 * (ch / 4));

    return (ipr >> (12 - 4 * (ch % 4))) & 0xf;
}

/* DEIn is held while TE and IE are both set. */
static void dmac_update_dei(Cdj850Dmac *s, unsigned ch)
{
    uint32_t chcr = rd32(s, 0x10 * ch + 0xC);
    bool on = (chcr & CHCR_TE) && (chcr & CHCR_IE);
    uint16_t vec = CDJ850_DEI0_VECTOR + 4 * ch;

    s->dei[ch] = on;
    if (on) {
        unsigned prio = dmac_prio(ch);

        if (prio && !cdj850_irq_pending(vec)) {
            cdj850_irq_raise(s->cpu, vec, prio);
        } else if (!prio) {
            cdj850_irq_withdraw(s->cpu, vec);
        }
        /* Re-present an uncleared level after acceptance and pick up an
         * IPR priority enabled after the transfer completed. */
        if (!timer_pending(s->irq_poll)) {
            timer_mod(s->irq_poll,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      CDJ850_DMAC_IRQ_POLL_NS);
        }
    } else {
        cdj850_irq_withdraw(s->cpu, vec);
    }
}

static void dmac_irq_poll(void *opaque)
{
    Cdj850Dmac *s = opaque;
    unsigned ch;

    for (ch = 0; ch < CDJ850_DMAC_CHANS; ch++) {
        dmac_update_dei(s, ch);
    }
}

/* Is the request this channel waits for active? */
static bool dmac_source_active(uint8_t dmars)
{
    switch (dmars) {
    case CDJ850_DMARS_USB_DMA0:
        return cdj850_usb_dma_request(1);
    case CDJ850_DMARS_USB_DMA1:
        return cdj850_usb_dma_request(2);
    case CDJ850_DMARS_SSI0_TX:
        return cdj850_ssi_tx_dma_request(0);
    case CDJ850_DMARS_SSI1_TX:
        return cdj850_ssi_tx_dma_request(1);
    default:
        return false;
    }
}

static bool dmac_armed(Cdj850Dmac *s, unsigned ch)
{
    uint32_t chcr = rd32(s, 0x10 * ch + 0xC);

    return (rd16(s, DMAC_DMAOR) & DMAOR_DME) &&
           !(rd16(s, DMAC_DMAOR) & DMAOR_FLAGS) &&
           (chcr & CHCR_DE) && !(chcr & CHCR_TE) &&
           ((chcr >> 8) & 0xf) == 0x8;
}

static void dmac_run_channel(Cdj850Dmac *s, unsigned ch)
{
    unsigned base = 0x10 * ch;
    uint32_t chcr = rd32(s, base + 0xC);
    unsigned unit = dmac_unit[(chcr >> 3) & 3];
    unsigned sm = (chcr >> 12) & 3, dm = (chcr >> 14) & 3;
    uint8_t dmars = dmac_dmars(s, ch);
    uint32_t sar = rd32(s, base), dar = rd32(s, base + 4);
    uint32_t tcr = rd32(s, base + 8);
    unsigned moved = 0;
    uint8_t buf[16];

    while (tcr && dmac_source_active(dmars)) {
        address_space_read(&address_space_memory, sar, MEMTXATTRS_UNSPECIFIED,
                           buf, unit);
        address_space_write(&address_space_memory, dar,
                            MEMTXATTRS_UNSPECIFIED, buf, unit);
        sar += sm == 1 ? unit : sm == 2 ? -unit : 0;
        dar += dm == 1 ? unit : dm == 2 ? -unit : 0;
        tcr--;
        moved++;
    }
    wr32(s, base, sar);
    wr32(s, base + 4, dar);
    wr32(s, base + 8, tcr);
    s->units[ch] += moved;
    if (!tcr) {
        wr32(s, base + 0xC, chcr | CHCR_TE);
    }
    if (moved && (cdj850_trace() || getenv("CDJ850_DMAC_DEBUG"))) {
        info_report("cdj850: dmac ch%u moved %u x %u bytes (DMARS 0x%02x), "
                    "DMATCR now %u%s at %" PRIu64 " ms", ch, moved, unit,
                    dmars, tcr, tcr ? "" : ", TE", cdj850_vms());
    }
    dmac_update_dei(s, ch);
}

static void dmac_run(void *opaque)
{
    Cdj850Dmac *s = opaque;
    unsigned ch;

    for (ch = 0; ch < CDJ850_DMAC_CHANS; ch++) {
        if (dmac_armed(s, ch)) {
            dmac_run_channel(s, ch);
        }
    }
}

void cdj850_dmac_request(uint8_t dmars)
{
    Cdj850Dmac *s = cdj850_dmac;
    unsigned ch;

    if (!s) {
        return;
    }
    for (ch = 0; ch < CDJ850_DMAC_CHANS; ch++) {
        if (dmac_armed(s, ch) && dmac_dmars(s, ch) == dmars) {
            if (!timer_pending(s->run)) {
                timer_mod(s->run, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          CDJ850_DMAC_DELAY_NS);
            }
            return;
        }
    }
}

void cdj850_dmac_serve(uint8_t dmars)
{
    Cdj850Dmac *s = cdj850_dmac;
    unsigned ch;

    if (!s) {
        return;
    }
    for (ch = 0; ch < CDJ850_DMAC_CHANS; ch++) {
        if (dmac_armed(s, ch) && dmac_dmars(s, ch) == dmars) {
            dmac_run_channel(s, ch);
        }
    }
}

static uint64_t cdj850_dmac_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Dmac *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) & (CDJ850_DMAC_SIZE - 1)];
    }
    if (cdj850_trace()) {
        info_report("cdj850: dmac read +0x%03x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)v,
                    s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_dmac_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    Cdj850Dmac *s = opaque;
    unsigned i, ch;
    uint8_t old[4];

    if (cdj850_trace()) {
        info_report("cdj850: dmac write +0x%03x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
    for (i = 0; i < size; i++) {
        hwaddr a = (off + i) & (CDJ850_DMAC_SIZE - 1);

        old[i] = s->reg[a];
        s->reg[a] = val >> (8 * (size - 1 - i));
    }
    /* Write-0-to-clear flags: a 1 written leaves the old value. */
    for (i = 0; i < size; i++) {
        hwaddr a = (off + i) & (CDJ850_DMAC_SIZE - 1);
        uint8_t keep = 0;

        if (a < 0x10 * CDJ850_DMAC_CHANS && (a & 0xF) == 0xF) {
            keep = CHCR_TE;                       /* CHCR bits 7:0 */
        } else if (a < 0x10 * CDJ850_DMAC_CHANS && (a & 0xF) == 0xD) {
            keep = CHCR_HE >> 16;                 /* CHCR bits 23:16 */
        } else if (a == DMAC_DMAOR + 1) {
            keep = DMAOR_FLAGS;
        }
        if (keep) {
            s->reg[a] = (s->reg[a] & ~keep) | (old[i] & s->reg[a] & keep);
        }
    }
    if (off < 0x10 * CDJ850_DMAC_CHANS) {
        dmac_update_dei(s, off / 0x10);
    }
    /* Arming a channel (or DME) may meet a request already standing. */
    for (ch = 0; ch < CDJ850_DMAC_CHANS; ch++) {
        if (dmac_armed(s, ch) && dmac_source_active(dmac_dmars(s, ch)) &&
            !timer_pending(s->run)) {
            timer_mod(s->run, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      CDJ850_DMAC_DELAY_NS);
        }
    }
}

static const MemoryRegionOps cdj850_dmac_ops = {
    .read = cdj850_dmac_read,
    .write = cdj850_dmac_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_dmac = {
    .name = "cdj850-dmac",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Dmac, CDJ850_DMAC_SIZE),
        VMSTATE_BOOL_ARRAY(dei, Cdj850Dmac, CDJ850_DMAC_CHANS),
        VMSTATE_TIMER_PTR(run, Cdj850Dmac),
        VMSTATE_TIMER_PTR(irq_poll, Cdj850Dmac),
        VMSTATE_END_OF_LIST()
    }
};

static void cdj850_dmac_reset(void *opaque)
{
    Cdj850Dmac *s = opaque;

    timer_del(s->run);
    timer_del(s->irq_poll);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->dei, 0, sizeof(s->dei));
    memset(s->units, 0, sizeof(s->units));
}

void cdj850_dmac_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Dmac *s = g_new0(Cdj850Dmac, 1);

    s->cpu = cpu;
    s->run = timer_new_ns(QEMU_CLOCK_VIRTUAL, dmac_run, s);
    s->irq_poll = timer_new_ns(QEMU_CLOCK_VIRTUAL, dmac_irq_poll, s);
    cdj850_dmac = s;
    qemu_register_reset(cdj850_dmac_reset, s);
    vmstate_register_any(NULL, &vmstate_cdj850_dmac, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_dmac_ops, s,
                          "cdj850.dmac", CDJ850_DMAC_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, CDJ850_DMAC_BASE, &s->iomem,
                                        1);
}
