/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dsp_dmax.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
/*
 * The C6727's dMAX, as far as the CDJ-2000's DSP uses it (TI SPRU795D). The
 * firmware loads TI's microcode into both engines and then only programs
 * event entries and general-purpose transfer entries, so the transfers are
 * done here from those, not by running the microcode.
 *
 *   0x60000000       control: +0x08 DEPR, +0x0C DEER, +0x10 DEDR, +0x14 DEHPR,
 *                    +0x18 DELPR, +0x80 DTCR0, +0xA0 DTCR1 (SPRU795D 2.3)
 *   0x61000000       MAX0, 0x62000000 MAX1: microcode, and at +0x8000 the
 *                    parameter RAM, event entries first (one word per event)
 *
 * High-priority events (DEHPR) are served by MAX0 from its parameter RAM,
 * low-priority ones (DELPR) by MAX1. The firmware puts the software events
 * 0 and 1 on MAX1 and the McASP transmit events 6 and 8 on MAX0.
 *
 * Event entry (SPRU795D 2.1.1.1): SYNC 29, TCC 27:24, ATCINT 23, TCINT 22,
 * RLOAD 20, CC 19:18, ESIZE 17:16, PTE 14:8 (the transfer entry's word
 * offset), ETYPE 4:0 (3 = general purpose). Transfer entry (2.1.2.1): active
 * source and destination, PP and the active counts, then destination (31:16)
 * and source (15:0) index pairs for the three dimensions, in elements, the
 * reference counts and the two reload address sets. CC splits the count
 * words: 00 COUNT2 15 bits, COUNT1 8, COUNT0 8; 01 7/8/16; 10 7/16/8.
 *
 * The firmware's entries:
 *   event 0 0xE0462803: whole transfer per event, 32-bit elements, TCC 0. It
 *     interleaves two 588-word sector buffers 0x2930 bytes apart into one
 *     4704-byte block of the SDRAM pool (0x80060000, blocks of 4800 bytes):
 *     COUNT1 2, COUNT0 588, source index 1/2049, destination index 2/-1173.
 *   event 1 0xC1463303: 1-D, 588 words, TCC 1: a pool half-block into the
 *     sector ring at 0x10017810 (stride 2352).
 *   events 6 and 8 0x82522803/0x84523303: one element per McASP request,
 *     reload with ping-pong, TCC 2 and 4; see below.
 * The firmware rewrites the active words before every software event, so
 * those always start from fresh state.
 */

#define CTL_DEPR                (0x08 / 4)
#define CTL_DEER                (0x0C / 4)
#define CTL_DEDR                (0x10 / 4)
#define CTL_DEHPR               (0x14 / 4)
#define CTL_DELPR               (0x18 / 4)
#define CTL_DTCR0               (0x80 / 4)
#define CTL_DTCR1               (0xA0 / 4)
#define PARAM_WORDS             0x2000

#define EV_SYNC                 (1u << 29)
#define EV_ATCINT               (1u << 23)
#define EV_TCINT                (1u << 22)
#define EV_RLOAD                (1u << 20)
#define EV_TCC(e)               ((e) >> 24 & 0xF)
#define EV_CC(e)                ((e) >> 18 & 3)
#define EV_ESIZE(e)             ((e) >> 16 & 3)
#define EV_PTE(e)               ((e) >> 8 & 0x7F)
#define EV_ETYPE(e)             ((e) & 0x1F)
#define ETYPE_GENERAL           3

#define TE_PP                   (1u << 31)

#define DMAX_INT                8

/* DETR bits to events, SPRU795D Table 2-27. */
static const struct {
    unsigned bit, event;
} detr_events[] = {
    { 0, 0 }, { 1, 10 }, { 2, 17 }, { 3, 23 },
    { 16, 1 }, { 17, 11 }, { 18, 18 }, { 19, 24 }, { 20, 30 }, { 21, 31 },
};

/* Bits of COUNT0 and COUNT1 in a count word, per CC; COUNT2 takes the rest
 * below PP. */
static const unsigned count_bits[4][2] = { { 8, 8 }, { 16, 8 }, { 8, 16 } };

/*
 * The two McASP transmit events of MAX0. Each moves one half of a 256-byte
 * ping-pong buffer (16 stereo frames, planar: left words at +0, right words
 * at +0x40) to the data port of its McASP: 16 blocks of two frames of one
 * element, source index 16 between the left and right word and -15 back to
 * the next left one. RLOAD with PP switches halves: the firmware's ISR
 * (0x80048970, 0x80049CC8) picks the half to refill from PP (clear: refill
 * the second half) and runs its sample renderer once both TCCs have been
 * seen. The events are paced by the DSP's own clock at the 44.1 kHz frame
 * rate, one half buffer per tick, so the transmitters never outrun a core
 * that is slower than the chip.
 */
#define MAX_AUDIO               0
#define AUDIO_FRAMES            16
#define AUDIO_NS_PER_HALF       (AUDIO_FRAMES * 1000000000ull / 44100)

static const unsigned audio_events[] = { 6, 8 };

static uint32_t *dmax_word(CdjDmax *d, uint32_t addr)
{
    uint32_t off = (addr & 0xFFFF) >> 2;

    switch (addr >> 24) {
    case 0x60:
        return off < ARRAY_SIZE(d->ctl) ? &d->ctl[off] : NULL;
    case 0x61:
    case 0x62:
        return &d->max[(addr >> 24) - 0x61][off];
    }
    return NULL;
}

uint32_t cdj_dmax_read(CdjDmax *d, uint32_t addr)
{
    uint32_t *w = dmax_word(d, addr);

    return w ? *w : 0;
}

/* The core takes the interrupt on the line's rising edge. Its ISR serves
 * one device per entry (0x80049CA4: the McASP halves before the copy
 * completion) and returns with the line still high when more is pending, so
 * every raise and every acknowledgement that leaves a TCC behind pulses it. */
static void dmax_update_irq(CdjDmax *d)
{
    bool pending = d->ctl[CTL_DTCR0] || d->ctl[CTL_DTCR1];

    d->set_irq(d->chip, DMAX_INT, 0);
    if (pending) {
        d->set_irq(d->chip, DMAX_INT, 1);
    }
}

static unsigned event_max(CdjDmax *d, unsigned event)
{
    return d->high_priority >> event & 1 ? 0 : 1;
}

static uint32_t event_entry(CdjDmax *d, unsigned max, unsigned event)
{
    return d->max[max][PARAM_WORDS + event];
}

static uint32_t *tcc_reg(CdjDmax *d, uint32_t entry, uint32_t *bit)
{
    *bit = 1u << (EV_TCC(entry) & 7);
    return &d->ctl[EV_TCC(entry) < 8 ? CTL_DTCR0 : CTL_DTCR1];
}

static void dmax_store(CdjDmax *d, uint32_t dst, const uint8_t *s,
                       unsigned size, uint32_t *port, unsigned *nport,
                       unsigned port_cap, uint32_t *lo, uint32_t *hi)
{
    uint8_t *t = d->ram(d->chip, dst);

    if (t) {
        memcpy(t, s, size);
        *lo = MIN(*lo, dst);
        *hi = MAX(*hi, dst + size);
    } else if (port && *nport < port_cap) {
        port[(*nport)++] = size == 4 ? ldl_le_p(s) : 0;
    }
}

/*
 * One synchronisation event on general-purpose transfer @event of @max. A
 * frame-synchronised entry (SYNC 0) moves one frame of COUNT0 elements, a
 * 1-D transfer or one with SYNC 1 the whole transfer (SPRU795D 1.5). The
 * active addresses and counts are written back, so the next event continues
 * where this one stopped. Elements whose destination is not memory, a
 * McASP data port, are handed out through @port. Sets the TCC per TCINT and
 * ATCINT; the caller updates the interrupt line. Returns true when the
 * transfer completed.
 */
static bool dmax_sync(CdjDmax *d, unsigned max, unsigned event,
                      uint32_t *port, unsigned *nport, unsigned port_cap)
{
    uint32_t entry = event_entry(d, max, event);
    uint32_t *te = &d->max[max][PARAM_WORDS + EV_PTE(entry)];
    unsigned b0 = count_bits[EV_CC(entry)][0];
    unsigned b1 = count_bits[EV_CC(entry)][1];
    unsigned size = 1u << EV_ESIZE(entry);
    uint32_t m0 = (1u << b0) - 1, m1 = (1u << b1) - 1;
    uint32_t m2 = (1u << (31 - b0 - b1)) - 1;
    uint32_t a0 = te[2] & m0, a1 = te[2] >> b0 & m1, a2 = te[2] >> (b0 + b1) & m2;
    uint32_t src = te[0], dst = te[1], pp = te[2] & TE_PP;
    bool whole = (entry & EV_SYNC) || (a2 == 0 && a1 <= 1);
    bool done = false;
    uint32_t tcc, *dtcr, lo = UINT32_MAX, hi = 0;

    if (EV_ETYPE(entry) != ETYPE_GENERAL || !a0) {
        return false;
    }
    for (;;) {
        uint8_t *s = d->ram(d->chip, src);

        if (s) {
            dmax_store(d, dst, s, size, port, nport, port_cap, &lo, &hi);
        }
        if (--a0) {
            src += (int16_t)te[3] * size;
            dst += (int16_t)(te[3] >> 16) * size;
            continue;
        }
        if (a1 > 1) {
            a1--;
            a0 = te[6] & m0;
            src += (int16_t)te[4] * size;
            dst += (int16_t)(te[4] >> 16) * size;
        } else if (a2 > 1) {
            a2--;
            a1 = te[6] >> b0 & m1;
            a0 = te[6] & m0;
            src += (int16_t)te[5] * size;
            dst += (int16_t)(te[5] >> 16) * size;
        } else {
            a1 = a2 = 0;
            done = true;
            break;
        }
        if (!whole) {
            break;
        }
    }
    if (hi > lo) {
        d->stored(d->chip, lo, hi - lo);
    }
    if (done && (entry & EV_RLOAD)) {
        a0 = te[6] & m0;
        a1 = te[6] >> b0 & m1;
        a2 = te[6] >> (b0 + b1) & m2;
        src = te[pp ? 7 : 9];
        dst = te[pp ? 8 : 10];
        pp ^= TE_PP;
    }
    te[0] = src;
    te[1] = dst;
    te[2] = pp | a2 << (b0 + b1) | a1 << b0 | a0;
    if ((done && (entry & EV_TCINT)) || (!done && (entry & EV_ATCINT))) {
        dtcr = tcc_reg(d, entry, &tcc);
        *dtcr |= tcc;
    }
    return done;
}

static bool audio_enabled(CdjDmax *d)
{
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(audio_events); i++) {
        if (!(d->enabled >> audio_events[i] & 1)) {
            return false;
        }
    }
    return true;
}

static bool audio_half_done(CdjDmax *d)
{
    uint32_t words[2 * AUDIO_FRAMES];
    unsigned i, k, n;

    /* The core has not served the last half yet: the sample clock waits for
     * it, like the transmitter does when its buffer is not refilled. */
    for (i = 0; i < ARRAY_SIZE(audio_events); i++) {
        uint32_t tcc;

        if (*tcc_reg(d, event_entry(d, MAX_AUDIO, audio_events[i]), &tcc) &
            tcc) {
            d->audio_overruns++;
            return false;
        }
    }
    for (i = 0; i < ARRAY_SIZE(audio_events); i++) {
        n = 0;
        for (k = 0; k < ARRAY_SIZE(words); k++) {
            if (dmax_sync(d, MAX_AUDIO, audio_events[i], words, &n,
                          ARRAY_SIZE(words))) {
                break;
            }
        }
        if (d->audio_frame) {
            for (k = 0; k + 1 < n; k += 2) {
                d->audio_frame(d->chip, i, words[k], words[k + 1]);
            }
        }
    }
    d->audio_halves++;
    dmax_update_irq(d);
    return true;
}

void cdj_dmax_audio_tx(CdjDmax *d, bool up)
{
    if (up != d->audio_tx_up) {
        d->audio_tx_up = up;
        d->audio_deadline_ns = 0;
    }
}

void cdj_dmax_audio_run(CdjDmax *d, int64_t dsp_ns)
{
    if (!d->audio_tx_up || !audio_enabled(d)) {
        return;
    }
    if (!d->audio_deadline_ns) {
        d->audio_deadline_ns = dsp_ns + AUDIO_NS_PER_HALF;
    }
    if (dsp_ns >= d->audio_deadline_ns && audio_half_done(d)) {
        d->audio_deadline_ns = dsp_ns + AUDIO_NS_PER_HALF;
    }
}

void cdj_dmax_write(CdjDmax *d, uint32_t addr, uint32_t val)
{
    uint32_t *w = dmax_word(d, addr);

    if (!w) {
        return;
    }
    if (w == &d->ctl[CTL_DTCR0] || w == &d->ctl[CTL_DTCR1]) {
        *w &= ~val;
        dmax_update_irq(d);
        return;
    }
    if (w == &d->ctl[CTL_DEER] || w == &d->ctl[CTL_DEDR]) {
        if (w == &d->ctl[CTL_DEER]) {
            d->enabled |= val;
        } else {
            d->enabled &= ~val;
        }
        d->audio_deadline_ns = 0;
        return;
    }
    if (w == &d->ctl[CTL_DEHPR]) {
        d->high_priority |= val;
    } else if (w == &d->ctl[CTL_DELPR]) {
        d->high_priority &= ~val;
    }
    *w = val;
}

static void dmax_event(CdjDmax *d, unsigned event)
{
    if (!(d->enabled >> event & 1)) {
        return;
    }
    dmax_sync(d, event_max(d, event), event, NULL, NULL, 0);
    dmax_update_irq(d);
}

/* An edge on a DETR bit raises its event: rising when the event's DEPR bit
 * is set, falling otherwise. The firmware sets DEPR for events 0 and 1 and
 * triggers them with 1 << 0 (0x8002C8B8) and 1 << 16 (0x800490B4). */
void cdj_dmax_detr(CdjDmax *d, uint32_t val)
{
    uint32_t rising = val & ~d->detr, falling = d->detr & ~val;
    unsigned i;

    d->detr = val;
    for (i = 0; i < ARRAY_SIZE(detr_events); i++) {
        unsigned ev = detr_events[i].event;
        uint32_t edge = d->ctl[CTL_DEPR] >> ev & 1 ? rising : falling;

        if (edge >> detr_events[i].bit & 1) {
            dmax_event(d, ev);
        }
    }
}

void cdj_dmax_report(CdjDmax *d)
{
    info_report("dmax: %" PRIu64 " McASP half buffers sent, %" PRIu64
                " ticks found the previous interrupt unacknowledged",
                d->audio_halves, d->audio_overruns);
}
