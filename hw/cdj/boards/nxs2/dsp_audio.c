/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
#include "cdj_host_audio.h"
#include <math.h>
/*
 * Host playback of the C6655's McBSP0 output (IC301).
 *
 * CDJ_C6X_AUDIO=1[:<ring_ms>][:<prefill_ms>][:<maxlat_ms>]
 *   Nothing is decoded here: the samples are the PCM words the DSP program
 *   produced, left-aligned int24, L/R interleaved, 44.1 kHz. The DSP runs at
 *   virtual speed and the backend pulls at wall speed, so after an underrun
 *   playback waits for <prefill_ms> of audio (silence is written meanwhile).
 *   A full ring drops its oldest frames. <maxlat_ms> (default 0 = off) caps
 *   the ring fill so controls are not heard seconds late; keep it well above
 *   <prefill_ms> or jitter hits one limit or the other constantly.
 */
static CdjHostAudio *cdj_dspau;
static int16_t cdj_dspau_left;      /* the first word of the frame in progress */
static bool cdj_dspau_have_left;

#define CDJ_DSPAU_REPLAY 8820            /* the pause's first pass, see below */

/* The loop period scales with deck speed (fader or MASTER TEMPO), so the lag
 * to match against is not always CDJ_C6X_AUDIO_LOOP/CDJ_DSPAU_REPLAY: search
 * this range (frames) when neither of those matches. */
#define CDJ_DSPAU_LAG_MIN 7000u
#define CDJ_DSPAU_LAG_MAX 20000u

/* A silence run this long (10 ms) is what opens every pause and is long
 * enough that a brief digital-silence gap inside real playing music rarely
 * reaches it; short enough that a genuine pause always does. Arming a search
 * on a shorter, coincidental gap just costs a search, never a false mute. */
#define CDJ_DSPAU_ZERO_MIN 441u

/* The correlation window (50 ms): short and rhythmic, heavily-looped dance
 * music can correlate above 0.99 against an unrelated lag for a block or two
 * before the pattern actually varies, but not for this long -- a real frozen
 * buffer stays flat near 1.0 at any window length, so this is what tells the
 * two apart. Off a real repeat the DSP's own resampler is not bit-exact
 * either (0.988-0.996 observed against an accept-or-reject gap this window
 * keeps unrelated music comfortably below), hence a correlation accept
 * rather than an equality compare. Re-checked at CDJ_DSPAU_RECHECK cadence
 * so a resume is not heard for the whole window before it is caught. */
#define CDJ_DSPAU_CONFIRM 2205u
#define CDJ_DSPAU_RECHECK 441u
#define CDJ_DSPAU_CORR_ACCEPT 0.97

/* A speed-scaled loop is not one steady lag throughout: between the DSP's
 * opening replay and the loop it settles into there is a stretch of a few
 * RECHECK periods where no single lag covers the whole CDJ_DSPAU_CONFIRM
 * window, so cdj_dspau_find_lag() legitimately comes up empty there too.
 * Losing the lock for a handful of consecutive rechecks is that transition,
 * not a resume -- only this many misses in a row is read as fresh audio. */
#define CDJ_DSPAU_PATIENCE 6u

#define CDJ_DSPAU_HIST 32768             /* power of two, > CDJ_DSPAU_LAG_MAX + CDJ_DSPAU_CONFIRM */

static struct {
    uint32_t period;                    /* CDJ_C6X_AUDIO_LOOP, 0 disables   */
    uint32_t run, miss;                 /* exact-match streaks, see below    */
    uint32_t lag;                       /* correlation fallback: locked lag  */
    uint32_t zero_run;                  /* consecutive exact-zero frames     */
    uint32_t discover_at;               /* pos to try a lag, 0 = none armed  */
    uint32_t misses;                    /* consecutive rechecks with no lag  */
    uint32_t pos;
    bool muting, exact;                 /* exact: which detector is muting   */
    uint32_t hist[CDJ_DSPAU_HIST];      /* L<<16 | R of the last frames */
    uint64_t muted;
} cdj_dspau_loop;

/* At machine init, for the reason cdj_host_audio_open() gives. */
void cdj_dspau_arm(void)
{
    static const CdjHostAudioCfg cfg = {
        .env = "CDJ_C6X_AUDIO", .card = "cdj2000nxs2-dsp",
        .voice = "cdj-c6x-mcbsp", .label = "c6x",
        .source = "IC301's McBSP0 PCM",
    };
    const char *lp = getenv("CDJ_C6X_AUDIO_LOOP");

    cdj_dspau = cdj_host_audio_open(&cfg);
    if (!cdj_dspau) {
        return;
    }
    cdj_dspau_loop.period = MIN(lp ? (uint32_t)strtoul(lp, NULL, 0) : 7644,
                                CDJ_DSPAU_HIST - 1);
}

/*
 * Loop mute. While paused the DSP stops refilling its McBSP buffer but the DMA
 * keeps sending it, so the same buffer repeats. Decoded music never repeats
 * across a whole buffer, so a non-silent stream that matches itself one
 * buffer back is muted until it stops matching.
 * The loop does not start at once: after ~20 ms of silence the DSP first sends
 * the buffer it played 8820 frames (200 ms) earlier, then loops it -- at deck
 * speed 1.0 (centre fader, no MASTER TEMPO) that pass and the loop it settles
 * into are both exact, sample for sample, so an equality compare catches
 * either lag within CDJ_DSPAU_ZERO_MIN of the silence (a 0-20 ms tail).
 * At any other speed (the pitch fader, or MASTER TEMPO) the DSP's own
 * resampler stretches that buffer, so the repeat lands well away from the
 * 1.0x lag and is not bit-exact with it either: the equality compare never
 * fires, so cdj_dspau_find_lag()'s correlation search is the fallback,
 * costing CDJ_DSPAU_CONFIRM of extra tail (~50 ms) to tell a real loop apart
 * from a rhythmic passage that briefly correlates with itself by chance.
 * CDJ_C6X_AUDIO_LOOP=<frames> sets the 1.0x buffer length (default 7644), 0
 * disables the whole detector.
 */

/* Cosine similarity of two CDJ_DSPAU_HIST-relative windows, n frames each
 * ending just before "at": 1.0 for an exact repeat, ~0 for unrelated audio.
 * Silence in either window (no signal to correlate against) reads as 0. */
static double cdj_dspau_corr(uint32_t at, uint32_t lag, uint32_t n)
{
    int64_t dot = 0, na = 0, nb = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        uint32_t fa = cdj_dspau_loop.hist[(at - n + i) & (CDJ_DSPAU_HIST - 1)];
        uint32_t fb = cdj_dspau_loop.hist[(at - n + i - lag) & (CDJ_DSPAU_HIST - 1)];
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

/* Which lag, if any, the CDJ_DSPAU_CONFIRM frames ending at "at" repeat: the
 * currently locked lag first when re-checking an ongoing mute ("prefer", 0 if
 * there is none yet), then the 1.0x candidates (cheap, and the common case),
 * then a search of the range a speed-scaled buffer can land in. 0 if nothing
 * correlates. The DSP's own pause opens with one pass of CDJ_DSPAU_REPLAY
 * before it settles into the real loop, so a mute started on that pass must
 * hand off to the loop's own lag rather than lose the match and unmute. */
static uint32_t cdj_dspau_find_lag(uint32_t at, uint32_t prefer)
{
    uint32_t best_lag = 0, lag;
    double best = CDJ_DSPAU_CORR_ACCEPT;
    double c1x, c8820;

    if (prefer && cdj_dspau_corr(at, prefer, CDJ_DSPAU_CONFIRM) >= best) {
        return prefer;
    }
    c1x = cdj_dspau_corr(at, cdj_dspau_loop.period, CDJ_DSPAU_CONFIRM);
    c8820 = cdj_dspau_corr(at, CDJ_DSPAU_REPLAY, CDJ_DSPAU_CONFIRM);
    if (c1x >= best) {
        best = c1x;
        best_lag = cdj_dspau_loop.period;
    }
    if (c8820 >= best) {
        best = c8820;
        best_lag = CDJ_DSPAU_REPLAY;
    }
    if (best_lag) {
        return best_lag;
    }
    for (lag = CDJ_DSPAU_LAG_MIN; lag <= CDJ_DSPAU_LAG_MAX; lag++) {
        double c = cdj_dspau_corr(at, lag, CDJ_DSPAU_CONFIRM);

        if (c >= best) {
            best = c;
            best_lag = lag;
        }
    }
    return best_lag;
}

static bool cdj_dspau_loop_frame(int16_t l, int16_t r)
{
    uint32_t f = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
    bool same;

    if (!cdj_dspau_loop.period) {
        return false;
    }
    same = cdj_dspau_loop.hist[(cdj_dspau_loop.pos - cdj_dspau_loop.period) &
                               (CDJ_DSPAU_HIST - 1)] == f ||
           cdj_dspau_loop.hist[(cdj_dspau_loop.pos - CDJ_DSPAU_REPLAY) &
                               (CDJ_DSPAU_HIST - 1)] == f;
    cdj_dspau_loop.hist[cdj_dspau_loop.pos & (CDJ_DSPAU_HIST - 1)] = f;
    cdj_dspau_loop.pos++;
    cdj_dspau_loop.zero_run = f ? 0 : cdj_dspau_loop.zero_run + 1;
    /* Digital silence occurs inside the looped buffer too, so a zero frame
     * neither ends a match run nor unmutes. */
    if (f) {
        cdj_dspau_loop.run = same ? cdj_dspau_loop.run + 1 : 0;
        cdj_dspau_loop.miss = same ? 0 : cdj_dspau_loop.miss + 1;
    }

    if (cdj_dspau_loop.muting) {
        if (cdj_dspau_loop.exact) {
            if (cdj_dspau_loop.miss >= 64) {
                cdj_dspau_loop.muting = false;
                info_report("c6x audio: fresh audio, unmuted after %" PRIu64 " frames",
                            cdj_dspau_loop.muted);
            }
        } else if (cdj_dspau_loop.pos % CDJ_DSPAU_RECHECK == 0 &&
                   cdj_dspau_corr(cdj_dspau_loop.pos, cdj_dspau_loop.lag,
                                  CDJ_DSPAU_RECHECK) < CDJ_DSPAU_CORR_ACCEPT) {
            /* The short block no longer matches the locked lag -- look for
             * any lag that still fits before giving up. Needed because the
             * pause's opening replay (CDJ_DSPAU_REPLAY) and the loop it
             * settles into are different lags, so a mute that started on the
             * replay must hand off rather than read as "fresh audio". */
            uint32_t lag = cdj_dspau_find_lag(cdj_dspau_loop.pos, cdj_dspau_loop.lag);

            if (lag) {
                cdj_dspau_loop.lag = lag;
                cdj_dspau_loop.misses = 0;
            } else if (++cdj_dspau_loop.misses >= CDJ_DSPAU_PATIENCE) {
                cdj_dspau_loop.muting = false;
                info_report("c6x audio: fresh audio, unmuted after %" PRIu64 " frames",
                            cdj_dspau_loop.muted);
            }
        }
        if (cdj_dspau_loop.muting) {
            cdj_dspau_loop.muted++;
        }
        return cdj_dspau_loop.muting;
    }

    /* The exact-match fast path: 1.0x speed repeats bit for bit, so this
     * alone gives a 0-20 ms tail and is tried on every frame regardless of
     * what the correlation fallback below is doing. */
    if (cdj_dspau_loop.run >= 441) {
        cdj_dspau_loop.muting = true;
        cdj_dspau_loop.exact = true;
        cdj_dspau_loop.muted = 0;
        info_report("c6x audio: output buffer is looping (paused deck), muting");
        return true;
    }

    /* The fallback: arm a correlation search CDJ_DSPAU_CONFIRM frames past
     * the end of a qualifying silence run (CDJ_DSPAU_ZERO_MIN), so the
     * search has a full window of whatever the DSP sends next -- comfortably
     * longer than the exact path needs, so a 1.0x pause is always caught by
     * that path first and never reaches here. A false arm on a coincidental
     * short silence just costs a search: only a lag that correlates over the
     * whole window engages the mute. */
    if (cdj_dspau_loop.zero_run >= CDJ_DSPAU_ZERO_MIN) {
        cdj_dspau_loop.discover_at = cdj_dspau_loop.pos + CDJ_DSPAU_CONFIRM;
    } else if (cdj_dspau_loop.discover_at &&
               cdj_dspau_loop.pos == cdj_dspau_loop.discover_at) {
        uint32_t lag = cdj_dspau_find_lag(cdj_dspau_loop.pos, 0);

        cdj_dspau_loop.discover_at = 0;
        if (lag) {
            cdj_dspau_loop.lag = lag;
            cdj_dspau_loop.muting = true;
            cdj_dspau_loop.exact = false;
            cdj_dspau_loop.muted = 0;
            cdj_dspau_loop.misses = 0;
            info_report("c6x audio: output buffer is looping (paused deck), muting");
        }
    }
    return cdj_dspau_loop.muting;
}

static void cdj_dspau_word(uint32_t word)
{
    int16_t s = (int16_t)((int32_t)word >> 16);

    if (!cdj_dspau_have_left) {
        cdj_dspau_left = s;
        cdj_dspau_have_left = true;
        return;
    }
    cdj_dspau_have_left = false;
    if (cdj_dspau_loop_frame(cdj_dspau_left, s)) {
        cdj_dspau_left = s = 0;
    }
    cdj_host_audio_put(cdj_dspau, cdj_dspau_left, s);
}

void cdj_c6x_mcbsp_tx(void *opaque, unsigned port, uint32_t word,
                             unsigned bits)
{
    CdjC6x *c = &cdj_c6x;
    int64_t p0 = c->prof ? get_clock() : 0;

    if (cdj_dspau && port == 0) {
        cdj_dspau_word(word);
    }
    c->pcm_words[port & 1]++;
    if (word && port == 0) {
        c->pcm_nonzero++;
    }
    if (c->pcm && port == 0) {
        fwrite(&word, sizeof(word), 1, c->pcm);
    }
    if (c->prof) {
        c->prof_pcm_ns += get_clock() - p0;
    }
}

