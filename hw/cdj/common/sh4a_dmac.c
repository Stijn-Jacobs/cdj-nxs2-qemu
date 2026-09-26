/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "cdj_getenv.h"
/* ---------------------------------------------------------------------------
 * The SH-4A DMAC, shared by the SH7724 (DMAC0 at 0xFE008000) and the SH7763
 * (0xFF608000): the same register block at a different base.
 *
 * Per channel: +0x00 SAR, +0x04 DAR, +0x08 TCR, +0x0C CHCR.
 * CHCR bit 0 = DE (start), bit 1 = TE (transfer end), bit 2 = IE.
 *
 * The firmware uses DMA as its memcpy and polls TE, so transfers run
 * synchronously. Channels with an end fixed on the board's DREQ window (its
 * USB FIFO) wait for the peripheral's request instead.
 */
#define CDJ_DMAC_SIZE   0x2000
#define DMAC_UNIT       16

typedef struct CdjDmacState {
    MemoryRegion iomem;
    /* DEI lines for channels 0..3 (DMAC0A); NULL for the rest. */
    qemu_irq dei[4];
    QEMUBH *dei_bh[4];
    QEMUTimer *dei_timer[4];
    /* Raises vs acknowledgements per channel; a big gap means a storm. */
    unsigned dei_raised[4];
    unsigned dei_acked[4];
    /* Cap on raises per channel, 0 = unlimited. A storming source is
     * silenced with one warning instead of starving the boot. */
    unsigned dei_max[4];
    bool dei_capped[4];
    /* Channels armed against a peripheral FIFO and waiting for its DREQ,
     * one bit per register block. */
    uint32_t dreq_pending;
    QEMUBH *dreq_bh;
    hwaddr dreq_base, dreq_size;    /* the FIFO window, size 0 = none */
    Notifier exit;
    uint32_t nread[CDJ_DMAC_SIZE / 4];
    uint32_t reg[CDJ_DMAC_SIZE / 4];
} CdjDmacState;

/* For cdj_dmac_dreq(), which peripherals call without a handle. */
static CdjDmacState *cdj_dmac;

/*
 * DMA0_SAR_0 is at 0xFE008020: channel n (0-3) is at +0x20 + 0x10 * n,
 * DMAOR at +0x60, channels 4 and 5 at +0x70 and +0x80. So offset / 16 is not
 * the channel number. Returns the channel, or -1 outside a channel.
 */
static int cdj_dmac_chan_num(hwaddr chan)
{
    if (chan >= 0x20 && chan < 0x60) {
        return (chan - 0x20) / 16;
    }
    if (chan >= 0x70 && chan < 0x90) {
        return 4 + (chan - 0x70) / 16;
    }
    return -1;
}

/* Set TE and, with CHCR.IE, raise the channel's DEI.
 *
 * A channel started by the guest's own CHCR store raises DEI from a bottom
 * half, since raising it inline would re-enter the driver before that store
 * retires. A DREQ-driven channel raises it inline at the end of the transfer,
 * as hardware does; deferring it there loses a race in the firmware's
 * udp_vcre_cep().
 */
static void cdj_dmac_complete(CdjDmacState *s, hwaddr chan, bool from_dreq)
{
    int chn = cdj_dmac_chan_num(chan);
    unsigned ch = chn < 0 ? ARRAY_SIZE(s->dei) : (unsigned)chn;

    s->reg[(chan + 0x0C) / 4] |= 0x02;          /* TE: transfer end */
    if (ch < ARRAY_SIZE(s->dei) && s->dei_bh[ch] &&
        (s->reg[(chan + 0x0C) / 4] & 0x04)) {
        const char *us = getenv("CDJ_DMAC_DEI_DELAY_US");

        if (s->dei_max[ch] && s->dei_raised[ch] >= s->dei_max[ch]) {
            if (!s->dei_capped[ch]) {
                s->dei_capped[ch] = true;
                warn_report("dmac: DEI%u hit its %u-raise cap and is now "
                            "silent -- this source is storming", ch,
                            s->dei_max[ch]);
            }
            return;
        }
        s->dei_raised[ch]++;
        if (us && s->dei_timer[ch]) {
            /* CDJ_DMAC_DEI_DELAY_US: delay the interrupt by a plausible
             * transfer time. */
            timer_mod(s->dei_timer[ch],
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      (int64_t)atoi(us) * 1000);
        } else if (from_dreq && !getenv("CDJ_DMAC_DEI_BH")) {
            qemu_set_irq(s->dei[ch], 1);
        } else {
            qemu_bh_schedule(s->dei_bh[ch]);
        }
    }
}

/* Transfer unit from CHCR TS[3:2] (bits 21:20) and TS[1:0] (bits 4:3),
 * manual 16.3.4. CDJ_DMAC_LEGACY_UNIT=1 forces 16 bytes. */
static unsigned cdj_dmac_unit(uint32_t chcr)
{
    unsigned ts = (((chcr >> 20) & 0x3) << 2) | ((chcr >> 3) & 0x3);

    switch (ts) {
    case 0x0: return 1;
    case 0x1: return 2;
    case 0x2: return 4;
    case 0x3: return 16;
    case 0x4: return 32;
    case 0x7: return 8;
    case 0xb: return 16;      /* 16-byte, 2-division */
    case 0xc: return 32;      /* 32-byte, 2-division */
    default:  return DMAC_UNIT;
    }
}

/* A channel with either end fixed on the DREQ window is DREQ-driven: it
 * runs when the peripheral requests it, not when CHCR.DE is written. The USB
 * driver arms DE before selecting the pipe (D0FIFOSEL), so running it early
 * would move data through the wrong pipe. */
static bool cdj_dmac_waits_for_dreq(CdjDmacState *s, uint32_t sar, unsigned sm,
                                    uint32_t dar, unsigned dm)
{
    hwaddr mask = ~(s->dreq_size - 1);
    bool src = sm == 0 && (A7ADDR(sar) & mask) == s->dreq_base;
    bool dst = dm == 0 && (A7ADDR(dar) & mask) == s->dreq_base;

    return s->dreq_size && (src || dst);
}

static void cdj_dmac_run(CdjDmacState *s, hwaddr chan, bool from_dreq)
{
    uint32_t sar = s->reg[(chan + 0x00) / 4];
    uint32_t dar = s->reg[(chan + 0x04) / 4];
    uint32_t tcr = s->reg[(chan + 0x08) / 4];
    uint32_t chcr = s->reg[(chan + 0x0C) / 4];
    /* SM/DM (bits 13:12 / 15:14): 00 fixed, 01 increment, 10 decrement. */
    unsigned sm = (chcr >> 12) & 0x3;
    unsigned dm = (chcr >> 14) & 0x3;
    unsigned unit = getenv("CDJ_DMAC_LEGACY_UNIT") ? DMAC_UNIT
                                                   : cdj_dmac_unit(chcr);
    uint64_t len = (uint64_t)tcr * unit;
    g_autofree uint8_t *buf = NULL;

    if (getenv("CDJ_DMAC_DEBUG")) {
        uint32_t chcr = s->reg[(chan + 0x0C) / 4];

        info_report("dmac: ch %d (+0x%03x) sar=0x%08x dar=0x%08x tcr=%u "
                    "chcr=0x%08x (TS=%u SM=%u DM=%u)",
                    cdj_dmac_chan_num(chan), (unsigned)chan, sar, dar, tcr,
                    chcr, (((chcr >> 20) & 3) << 2) | ((chcr >> 3) & 3),
                    (chcr >> 12) & 3, (chcr >> 14) & 3);
    }
    if (!from_dreq && cdj_dmac_waits_for_dreq(s, sar, sm, dar, dm)) {
        s->dreq_pending |= 1u << (chan / 16);
        return;                                 /* TE stays clear: not done yet */
    }

    /* Refuse a nonsense TCR from an uninitialised channel. */
    if (len == 0 || len > 64 * MiB || tcr > 0x10000) {
        cdj_dmac_complete(s, chan, from_dreq);             /* claim TE anyway */
        return;
    }

    if (sm == 1 && dm == 1) {
        /* Plain memcpy, the hot path: one bulk copy. */
        buf = g_malloc(len);
        cpu_physical_memory_read(A7ADDR(sar), buf, len);
        cpu_physical_memory_write(A7ADDR(dar), buf, len);
        s->reg[(chan + 0x00) / 4] = sar + len;
        s->reg[(chan + 0x04) / 4] = dar + len;
        s->reg[(chan + 0x08) / 4] = 0;
        cdj_dmac_complete(s, chan, from_dreq);
        return;
    }

    buf = g_malloc(unit);
    /* One unit at a time, so a fixed end stays put. */
    while (tcr--) {
        cpu_physical_memory_read(A7ADDR(sar), buf, unit);
        cpu_physical_memory_write(A7ADDR(dar), buf, unit);
        if (sm == 1) {
            sar += unit;
        } else if (sm == 2) {
            sar -= unit;
        }
        if (dm == 1) {
            dar += unit;
        } else if (dm == 2) {
            dar -= unit;
        }
    }
    if (from_dreq && getenv("CDJ_DMAC_DEBUG")) {
        uint8_t head[16];
        char hex[3 * sizeof(head) + 1];
        unsigned k;

        cpu_physical_memory_read(A7ADDR(s->reg[(chan + 0x04) / 4]), head,
                                 sizeof(head));
        for (k = 0; k < sizeof(head); k++) {
            snprintf(hex + 3 * k, 4, "%02x ", head[k]);
        }
        info_report("dmac: ch +0x%03x delivered %" PRIu64 " bytes to 0x%08x, "
                    "head: %s", (unsigned)chan, len,
                    s->reg[(chan + 0x04) / 4], hex);
        /* The tail shows whether a sector arrived (an MBR ends 55 aa). */
        cpu_physical_memory_read(A7ADDR(s->reg[(chan + 0x04) / 4]) + len - 16,
                                 head, sizeof(head));
        for (k = 0; k < sizeof(head); k++) {
            snprintf(hex + 3 * k, 4, "%02x ", head[k]);
        }
        info_report("dmac:   tail: %s", hex);
    }

    /* Hardware leaves the registers where the transfer finished. */
    s->reg[(chan + 0x00) / 4] = sar;
    s->reg[(chan + 0x04) / 4] = dar;
    s->reg[(chan + 0x08) / 4] = 0;
    cdj_dmac_complete(s, chan, from_dreq);
}

static uint64_t cdj_dmac_read(void *opaque, hwaddr off, unsigned size)
{
    CdjDmacState *s = opaque;
    uint32_t v = s->reg[off / 4];

    /* Count reads per register; log the first few with CDJ_DMAC_DEBUG. */
    if ((off / 4) < ARRAY_SIZE(s->nread) && ++s->nread[off / 4] <= 8 &&
        getenv("CDJ_DMAC_DEBUG")) {
        info_report("dmac: read +0x%03x = 0x%08x  #%u (pc=0x%08x)",
                    (unsigned)off, v, s->nread[off / 4],
                    current_cpu ? SUPERH_CPU(current_cpu)->env.pc : 0);
    }
    return v;
}

static void cdj_dmac_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjDmacState *s = opaque;
    uint32_t was = s->reg[off / 4];

    s->reg[off / 4] = val;
    if ((off & 0x0F) == 0x0C) {
        hwaddr chan = off & ~0x0FULL;
        /* DEI is indexed by channel number (cdj_dmac_chan_num()),
         * dreq_pending by register block index (cdj_dmac_dreq_run()). */
        int chn = cdj_dmac_chan_num(chan);
        unsigned ch = chn < 0 ? ARRAY_SIZE(s->dei) : (unsigned)chn;
        unsigned slot = chan / 16;

        /* DEI is held while TE is set; the ack is a write clearing a TE
         * that was set (not the arming write). */
        if ((was & 0x02) && !(val & 0x02) &&
            ch < ARRAY_SIZE(s->dei) && s->dei[ch]) {
            if (s->dei_bh[ch]) {
                qemu_bh_cancel(s->dei_bh[ch]);
            }
            s->dei_acked[ch]++;
            qemu_set_irq(s->dei[ch], 0);
            if (getenv("CDJ_DMAC_DEBUG") && current_cpu) {
                CPUSH4State *e = &SUPERH_CPU(current_cpu)->env;

                info_report("dmac: ch %u DEI acked from pc=0x%08x pr=0x%08x "
                            "sr=0x%08x r4=0x%08x", ch, e->pc, e->pr, e->sr,
                            e->gregs[4]);
            }
        }
        /* Writing DE=1 (and not TE) to a CHCR starts that channel. */
        if ((val & 0x01) && !(val & 0x02)) {
            cdj_dmac_run(s, chan, false);
        } else if (!(val & 0x01)) {
            s->dreq_pending &= ~(1u << slot);    /* disarmed before it ran */
        }
    }
}

static const MemoryRegionOps cdj_dmac_ops = {
    .read = cdj_dmac_read,
    .write = cdj_dmac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_dmac_dreq_run(void *opaque)
{
    CdjDmacState *s = opaque;
    unsigned ch;

    for (ch = 0; ch < CDJ_DMAC_SIZE / 16 && s->dreq_pending; ch++) {
        if (s->dreq_pending & (1u << ch)) {
            s->dreq_pending &= ~(1u << ch);
            cdj_dmac_run(s, ch * 16, true);
        }
    }
}

/* Called by a peripheral once its FIFO holds data; runs every channel armed
 * against it. Runs inline in the peripheral's MMIO handler, while CURPIPE
 * still names the requesting pipe; a bottom half runs too late, after the
 * driver has switched pipes. The USB region therefore opts out of QEMU's
 * re-entrancy guard.
 */
void cdj_dmac_dreq(void)
{
    if (cdj_dmac) {
        cdj_dmac_dreq_run(cdj_dmac);
    }
}

static void cdj_dmac_dei_raise(void *opaque)
{
    qemu_set_irq(*(qemu_irq *)opaque, 1);
}

static void cdj_dmac_dump(Notifier *n, void *unused)
{
    CdjDmacState *s = container_of(n, CdjDmacState, exit);
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(s->dei_raised); i++) {
        if (s->dei_raised[i] || s->dei_acked[i]) {
            info_report("dmac: DEI%u raised=%u acked=%u%s", i,
                        s->dei_raised[i], s->dei_acked[i],
                        s->dei_raised[i] && !s->dei_acked[i]
                        ? "  <- raised and NEVER acknowledged" : "");
        }
    }
    for (i = 0; i < CDJ_DMAC_SIZE / 4; i++) {
        if (s->nread[i]) {
            info_report("dmac: read +0x%03x %u times = 0x%08x", 4 * i,
                        s->nread[i], s->reg[i]);
        }
    }
}

/* dei[] gives the first four channels' DEI lines, a NULL line leaving that
 * channel without an interrupt, and a raise cap per line (0 = unlimited).
 * Which lines a board connects is its own policy. */
void cdj_dmac_init(MemoryRegion *sysmem, const char *name, hwaddr base,
                   hwaddr dreq_base, hwaddr dreq_size, const CdjDmacDei *dei)
{
    CdjDmacState *s = g_new0(CdjDmacState, 1);
    unsigned i;

    s->dreq_base = dreq_base;
    s->dreq_size = dreq_size;
    for (i = 0; i < ARRAY_SIZE(s->dei); i++) {
        s->dei[i] = dei ? dei[i].irq : NULL;
        s->dei_max[i] = dei ? dei[i].max : 0;
        if (s->dei[i]) {
            s->dei_bh[i] = qemu_bh_new(cdj_dmac_dei_raise, &s->dei[i]);
            s->dei_timer[i] = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                           cdj_dmac_dei_raise, &s->dei[i]);
        }
    }
    memory_region_init_io(&s->iomem, NULL, &cdj_dmac_ops, s,
                          name, CDJ_DMAC_SIZE);
    memory_region_add_subregion(sysmem, base, &s->iomem);
    s->dreq_bh = qemu_bh_new(cdj_dmac_dreq_run, s);
    s->exit.notify = cdj_dmac_dump;
    qemu_add_exit_notifier(&s->exit);
    cdj_dmac = s;
}
