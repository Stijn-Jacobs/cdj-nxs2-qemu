/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_loop_mute.h"
#include "qemu/error-report.h"
#include <math.h>
/*
 * The loop does not start at once: after ~20 ms of silence the DSP first sends
 * the buffer it played <replay> frames earlier, then loops it -- at deck speed
 * 1.0 (centre fader, no MASTER TEMPO) that pass and the loop it settles into
 * are both exact, sample for sample, so an equality compare catches either lag
 * within CDJ_LOOP_ZERO_MIN of the silence (a 0-20 ms tail).
 * At any other speed (the pitch fader, or MASTER TEMPO) the DSP's own
 * resampler stretches that buffer, so the repeat lands well away from the
 * 1.0x lag and is not bit-exact with it either: the equality compare never
 * fires, so find_lag()'s correlation search is the fallback, costing
 * CDJ_LOOP_CONFIRM of extra tail (~50 ms) to tell a real loop apart from a
 * rhythmic passage that briefly correlates with itself by chance.
 */

/* A silence run this long (10 ms) is what opens every pause and is long
 * enough that a brief digital-silence gap inside real playing music rarely
 * reaches it; short enough that a genuine pause always does. Arming a search
 * on a shorter, coincidental gap just costs a search, never a false mute. */
#define CDJ_LOOP_ZERO_MIN 441u

/* The correlation window (50 ms): short and rhythmic, heavily-looped dance
 * music can correlate above 0.99 against an unrelated lag for a block or two
 * before the pattern actually varies, but not for this long -- a real frozen
 * buffer stays flat near 1.0 at any window length, so this is what tells the
 * two apart. Off a real repeat the DSP's own resampler is not bit-exact
 * either (0.988-0.996 observed against an accept-or-reject gap this window
 * keeps unrelated music comfortably below), hence a correlation accept
 * rather than an equality compare. Re-checked at CDJ_LOOP_RECHECK cadence
 * so a resume is not heard for the whole window before it is caught. */
#define CDJ_LOOP_CONFIRM 2205u
#define CDJ_LOOP_RECHECK 441u
#define CDJ_LOOP_CORR_ACCEPT 0.97

/* A speed-scaled loop is not one steady lag throughout: between the DSP's
 * opening replay and the loop it settles into there is a stretch of a few
 * RECHECK periods where no single lag covers the whole CDJ_LOOP_CONFIRM
 * window, so find_lag() legitimately comes up empty there too. Losing the
 * lock for a handful of consecutive rechecks is that transition, not a
 * resume -- only this many misses in a row is read as fresh audio. */
#define CDJ_LOOP_PATIENCE 6u

#define CDJ_LOOP_HIST 32768             /* power of two, > lag_max + CDJ_LOOP_CONFIRM */

struct CdjLoopMute {
    CdjLoopMuteCfg cfg;
    const char *label;
    uint32_t run, miss;                 /* exact-match streaks               */
    uint32_t lag;                       /* correlation fallback: locked lag  */
    uint32_t zero_run;                  /* consecutive exact-zero frames     */
    uint32_t discover_at;               /* pos to try a lag, 0 = none armed  */
    uint32_t misses;                    /* consecutive rechecks with no lag  */
    uint32_t pos;
    bool muting, exact;                 /* exact: which detector is muting   */
    uint64_t muted;
    uint32_t hist[CDJ_LOOP_HIST];       /* L<<16 | R of the last frames      */
};

CdjLoopMute *cdj_loop_mute_new(const CdjLoopMuteCfg *cfg, const char *label)
{
    CdjLoopMute *m = g_new0(CdjLoopMute, 1);

    m->cfg = *cfg;
    m->cfg.period = MIN(cfg->period, CDJ_LOOP_HIST - 1);
    m->label = label;
    return m;
}

/* Cosine similarity of two CDJ_LOOP_HIST-relative windows, n frames each
 * ending just before "at": 1.0 for an exact repeat, ~0 for unrelated audio.
 * Silence in either window (no signal to correlate against) reads as 0. */
static double corr(const CdjLoopMute *m, uint32_t at, uint32_t lag, uint32_t n)
{
    int64_t dot = 0, na = 0, nb = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t fa = m->hist[(at - n + i) & (CDJ_LOOP_HIST - 1)];
        uint32_t fb = m->hist[(at - n + i - lag) & (CDJ_LOOP_HIST - 1)];
        int32_t al = (int16_t)(fa >> 16), ar = (int16_t)fa;
        int32_t bl = (int16_t)(fb >> 16), br = (int16_t)fb;

        dot += (int64_t)al * bl + (int64_t)ar * br;
        na  += (int64_t)al * al + (int64_t)ar * ar;
        nb  += (int64_t)bl * bl + (int64_t)br * br;
    }
    if (na == 0 || nb == 0) {
        return 0.0;
    }
    return (double)dot / (sqrt((double)na) * sqrt((double)nb));
}

/* Which lag, if any, the CDJ_LOOP_CONFIRM frames ending at "at" repeat: the
 * currently locked lag first when re-checking an ongoing mute ("prefer", 0 if
 * there is none yet), then the 1.0x candidates (cheap, and the common case),
 * then a search of the range a speed-scaled buffer can land in. 0 if nothing
 * correlates. The DSP's own pause opens with one pass of cfg.replay before it
 * settles into the real loop, so a mute started on that pass must hand off to
 * the loop's own lag rather than lose the match and unmute. */
static uint32_t find_lag(const CdjLoopMute *m, uint32_t at, uint32_t prefer)
{
    uint32_t best_lag = 0, lag;
    double best = CDJ_LOOP_CORR_ACCEPT;
    double c1x, creplay;

    if (prefer && corr(m, at, prefer, CDJ_LOOP_CONFIRM) >= best) {
        return prefer;
    }
    c1x = corr(m, at, m->cfg.period, CDJ_LOOP_CONFIRM);
    creplay = m->cfg.replay ? corr(m, at, m->cfg.replay, CDJ_LOOP_CONFIRM) : 0.0;
    if (c1x >= best) {
        best = c1x;
        best_lag = m->cfg.period;
    }
    if (creplay >= best) {
        best = creplay;
        best_lag = m->cfg.replay;
    }
    if (best_lag) {
        return best_lag;
    }
    for (lag = m->cfg.lag_min; lag <= m->cfg.lag_max; lag++) {
        double c = corr(m, at, lag, CDJ_LOOP_CONFIRM);

        if (c >= best) {
            best = c;
            best_lag = lag;
        }
    }
    return best_lag;
}

static void unmute(CdjLoopMute *m)
{
    m->muting = false;
    info_report("%s audio: fresh audio, unmuted after %" PRIu64 " frames",
                m->label, m->muted);
}

bool cdj_loop_mute_frame(CdjLoopMute *m, int16_t l, int16_t r)
{
    uint32_t f = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
    bool same;

    if (!m->cfg.period) {
        return false;
    }
    same = m->hist[(m->pos - m->cfg.period) & (CDJ_LOOP_HIST - 1)] == f ||
           (m->cfg.replay &&
            m->hist[(m->pos - m->cfg.replay) & (CDJ_LOOP_HIST - 1)] == f);
    m->hist[m->pos & (CDJ_LOOP_HIST - 1)] = f;
    m->pos++;
    m->zero_run = f ? 0 : m->zero_run + 1;
    /* Digital silence occurs inside the looped buffer too, so a zero frame
     * neither ends a match run nor unmutes. */
    if (f) {
        m->run = same ? m->run + 1 : 0;
        m->miss = same ? 0 : m->miss + 1;
    }

    if (m->muting) {
        if (m->exact) {
            if (m->miss >= 64) {
                unmute(m);
            }
        } else if (m->pos % CDJ_LOOP_RECHECK == 0 &&
                   corr(m, m->pos, m->lag, CDJ_LOOP_RECHECK) < CDJ_LOOP_CORR_ACCEPT) {
            /* The short block no longer matches the locked lag -- look for
             * any lag that still fits before giving up. Needed because the
             * pause's opening replay and the loop it settles into are
             * different lags, so a mute that started on the replay must hand
             * off rather than read as "fresh audio". */
            uint32_t lag = find_lag(m, m->pos, m->lag);

            if (lag) {
                m->lag = lag;
                m->misses = 0;
            } else if (++m->misses >= CDJ_LOOP_PATIENCE) {
                unmute(m);
            }
        }
        if (m->muting) {
            m->muted++;
        }
        return m->muting;
    }

    /* The exact-match fast path: 1.0x speed repeats bit for bit, so this
     * alone gives a 0-20 ms tail and is tried on every frame regardless of
     * what the correlation fallback below is doing. */
    if (m->run >= 441) {
        m->muting = true;
        m->exact = true;
        m->muted = 0;
        info_report("%s audio: output buffer is looping (paused deck), muting",
                    m->label);
        return true;
    }

    /* The fallback: arm a correlation search CDJ_LOOP_CONFIRM frames past the
     * end of a qualifying silence run (CDJ_LOOP_ZERO_MIN), so the search has a
     * full window of whatever the DSP sends next -- comfortably longer than
     * the exact path needs, so a 1.0x pause is always caught by that path
     * first and never reaches here. A false arm on a coincidental short
     * silence just costs a search: only a lag that correlates over the whole
     * window engages the mute. */
    if (m->zero_run >= CDJ_LOOP_ZERO_MIN) {
        m->discover_at = m->pos + CDJ_LOOP_CONFIRM;
    } else if (m->discover_at && m->pos == m->discover_at) {
        uint32_t lag = find_lag(m, m->pos, 0);

        m->discover_at = 0;
        if (lag) {
            m->lag = lag;
            m->muting = true;
            m->exact = false;
            m->muted = 0;
            m->misses = 0;
            info_report("%s audio: output buffer is looping (paused deck), muting",
                        m->label);
        }
    }
    return m->muting;
}
