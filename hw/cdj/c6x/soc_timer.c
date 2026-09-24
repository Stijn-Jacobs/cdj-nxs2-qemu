/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Timer64P (SPRUGV5A). The program runs Timer0, Timer3 and Timer4 as dual
 * 32-bit unchained timers with Timer Plus enabled (TGCR |= 4 then bit 4, app
 * 0x8007E820), continuous-with-reload on the low half, PRDINTEN_LO set.
 *
 * The input clock is not in any document we have. 1 GHz / 6 is inferred from
 * the program: Timer3's period 166,666 and Timer4's 500,000 then tick at
 * exactly 1 ms and 3 ms.
 */
#include "soc_internal.h"

enum { TCR_ENAMODE_LO = 6, TCR_ENAMODE_HI = 22 };
enum { TGCR_TIMLORS = 1, TGCR_TIMHIRS = 2, TGCR_PLUSEN = 0x10 };
enum { INT_PRDINTEN_LO = 1, INT_PRDINTSTAT_LO = 2 };

static void clock_ratio(const c6655_soc *s, uint64_t *num, uint64_t *den)
{
    if (s->cfg.timer_clk_hz) {
        *num = s->cfg.timer_clk_hz;
        *den = 1;
    } else {
        *num = NS_PER_S;
        *den = 6;
    }
}

static uint64_t ticks_in(const c6655_soc *s, uint64_t ns)
{
    uint64_t num, den;
    clock_ratio(s, &num, &den);
    return (unsigned __int128)ns * num / ((unsigned __int128)den * NS_PER_S);
}

static uint64_t ns_for(const c6655_soc *s, uint64_t ticks)
{
    uint64_t num, den;
    clock_ratio(s, &num, &den);
    unsigned __int128 x = (unsigned __int128)ticks * den * NS_PER_S;
    return (uint64_t)((x + num - 1) / num);
}

static unsigned timmode(const soc_timer *t)
{
    return (t->tgcr >> 2) & 3;
}

/* Unchained mode has two 32-bit halves; every other mode is one 64-bit timer
 * in half 0 (chained prescaling and the watchdog are not modelled). */
static int wide(const soc_timer *t)
{
    return timmode(t) != 1;
}

static uint64_t half_prd(const soc_timer *t, int h)
{
    return wide(t) ? ((uint64_t)t->prd[1] << 32 | t->prd[0]) : t->prd[h];
}

static uint64_t half_cnt_now(const c6655_soc *s, const soc_timer *t, int h)
{
    const soc_timer_half *x = &t->half[h];
    /* base_ns sits one tick after the last match, so an ISR ack inside that
     * tick would otherwise underflow the count and push it past PRD forever. */
    uint64_t c = x->running && s->now_ns > x->base_ns
               ? x->base_cnt + ticks_in(s, s->now_ns - x->base_ns) : x->base_cnt;
    return wide(t) ? c : (uint32_t)c;
}

static int half_should_run(const soc_timer *t, int h)
{
    unsigned ena = (t->tcr >> (h ? TCR_ENAMODE_HI : TCR_ENAMODE_LO)) & 3;
    if (wide(t))
        return h == 0 && ena && (t->tgcr & (TGCR_TIMLORS | TGCR_TIMHIRS)) == 3;
    return ena && (t->tgcr & (h ? TGCR_TIMHIRS : TGCR_TIMLORS));
}

static void half_schedule(const c6655_soc *s, soc_timer *t, int h)
{
    soc_timer_half *x = &t->half[h];
    if (!x->running) {
        x->match_ns = UINT64_MAX;
        return;
    }
    uint64_t prd = half_prd(t, h), cnt = x->base_cnt;
    uint64_t ticks;
    if (cnt <= prd)
        ticks = prd - cnt;
    else
        ticks = (wide(t) ? 0 : (1ull << 32)) - cnt + prd;   /* wraps first */
    x->match_ns = x->base_ns + ns_for(s, ticks);
}

/* Re-derive run state after any register write, freezing or rebasing the count. */
static void timer_sync(c6655_soc *s, soc_timer *t)
{
    for (int h = 0; h < 2; h++) {
        soc_timer_half *x = &t->half[h];
        uint64_t cnt = half_cnt_now(s, t, h);
        x->running = half_should_run(t, h);
        x->base_cnt = cnt;
        x->base_ns = s->now_ns;
        half_schedule(s, t, h);
    }
}

void timer_reset(c6655_soc *s, soc_timer *t, unsigned index)
{
    *t = (soc_timer){ .index = index };
    t->half[0].match_ns = t->half[1].match_ns = UINT64_MAX;
}

uint64_t timer_next_event(const soc_timer *t)
{
    return t->half[0].match_ns < t->half[1].match_ns ? t->half[0].match_ns : t->half[1].match_ns;
}

void timer_fire(c6655_soc *s, soc_timer *t)
{
    for (int h = 0; h < 2; h++) {
        soc_timer_half *x = &t->half[h];
        if (x->match_ns > s->now_ns)
            continue;
        uint64_t at = x->match_ns;
        unsigned sh = h ? 16 : 0;
        t->intctlstat |= INT_PRDINTSTAT_LO << sh;
        if (!(t->tgcr & TGCR_PLUSEN) || (t->intctlstat & (INT_PRDINTEN_LO << sh)))
            soc_pulse(s, SIG_TINT_L0 + 2 * t->index + h);

        unsigned ena = (t->tcr >> (h ? TCR_ENAMODE_HI : TCR_ENAMODE_LO)) & 3;
        if (ena == 1) {                      /* one shot: stop at the period */
            x->running = 0;
            x->base_cnt = half_prd(t, h);
            x->match_ns = UINT64_MAX;
            continue;
        }
        if (ena == 3) {                      /* reload the period */
            t->prd[h] = t->rel[h];
            if (wide(t))
                t->prd[1] = t->rel[1];
        }
        x->base_cnt = 0;
        x->base_ns = at + ns_for(s, 1);
        half_schedule(s, t, h);
        if (x->match_ns <= at) {             /* zero period: avoid a stuck loop */
            soc_log(s, "Timer%u: zero period, stopping", t->index);
            x->running = 0;
            x->match_ns = UINT64_MAX;
        }
    }
}

uint32_t timer_read(c6655_soc *s, soc_timer *t, uint32_t off, unsigned size)
{
    uint32_t v;
    switch (off & ~3u) {
    case 0x00: v = 0x44431100; break;    /* PID12; value unverified */
    case 0x04: v = t->emumgt; break;
    case 0x10: v = (uint32_t)half_cnt_now(s, t, 0); break;
    case 0x14: v = wide(t) ? (uint32_t)(half_cnt_now(s, t, 0) >> 32)
                           : (uint32_t)half_cnt_now(s, t, 1); break;
    case 0x18: v = t->prd[0]; break;
    case 0x1C: v = t->prd[1]; break;
    case 0x20: v = t->tcr; break;
    case 0x24: v = t->tgcr; break;
    case 0x28: v = t->wdtcr; break;
    case 0x34: v = t->rel[0]; break;
    case 0x38: v = t->rel[1]; break;
    case 0x3C: v = t->cap[0]; break;
    case 0x40: v = t->cap[1]; break;
    case 0x44: v = t->intctlstat; break;
    default:
        soc_log_unimp(s, "Timer", 0x02200000 + (t->index << 16) + off, 0, 0);
        v = 0;
    }
    return reg_get(v, off, size);
}

void timer_write(c6655_soc *s, soc_timer *t, uint32_t off, uint32_t val, unsigned size)
{
    /* Latch the running counts first so a mode change does not lose time. */
    for (int h = 0; h < 2; h++) {
        t->half[h].base_cnt = half_cnt_now(s, t, h);
        t->half[h].base_ns = s->now_ns;
    }
    switch (off & ~3u) {
    case 0x04: t->emumgt = reg_put(t->emumgt, off, val, size); break;
    case 0x10:
        t->cnt[0] = reg_put((uint32_t)t->half[0].base_cnt, off, val, size);
        t->half[0].base_cnt = wide(t) ? ((t->half[0].base_cnt >> 32) << 32 | t->cnt[0]) : t->cnt[0];
        break;
    case 0x14:
        t->cnt[1] = reg_put(0, off, val, size);
        if (wide(t))
            t->half[0].base_cnt = (uint64_t)t->cnt[1] << 32 | (uint32_t)t->half[0].base_cnt;
        else
            t->half[1].base_cnt = t->cnt[1];
        break;
    case 0x18: t->prd[0] = reg_put(t->prd[0], off, val, size); break;
    case 0x1C: t->prd[1] = reg_put(t->prd[1], off, val, size); break;
    case 0x20: t->tcr = reg_put(t->tcr, off, val, size); break;
    case 0x24:
        t->tgcr = reg_put(t->tgcr, off, val, size);
        if (timmode(t) == 2)
            soc_log(s, "Timer%u: watchdog mode is not modelled", t->index);
        break;
    case 0x28: t->wdtcr = reg_put(t->wdtcr, off, val, size); break;
    case 0x34: t->rel[0] = reg_put(t->rel[0], off, val, size); break;
    case 0x38: t->rel[1] = reg_put(t->rel[1], off, val, size); break;
    case 0x44: {
        uint32_t v = reg_put(0, off, val, size);
        uint32_t w1c = 0x000A000A;           /* the four *STAT bits */
        t->intctlstat = ((t->intctlstat & ~0x00050005u) | (v & 0x00050005u)) & ~(v & w1c);
        break;
    }
    default:
        soc_log_unimp(s, "Timer", 0x02200000 + (t->index << 16) + off, 1, val);
        return;
    }
    timer_sync(s, t);
}
