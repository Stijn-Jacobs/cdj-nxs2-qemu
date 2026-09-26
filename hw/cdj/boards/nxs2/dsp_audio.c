/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
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
static struct {
    bool on, failed;
    QEMUSoundCard card;
    SWVoiceOut *voice;
    QemuMutex lock;
    int16_t *ring;              /* stereo frames                             */
    uint32_t cap, rd, wr, fill, prefill, maxlat;
    uint32_t trim;              /* max frames dropped per callback, 0 = all  */
    bool priming;
    int16_t left;               /* the first word of the frame in progress   */
    bool have_left;
    uint64_t frames_in, frames_out, underruns, dropped, trims;
    /*
     * Asynchronous sample-rate conversion. The host sink does not drain at
     * exactly 44.1 kHz, and a rate error cannot be fixed by dropping frames,
     * so the ring is read with a fractional step chosen by a slow control
     * loop on the fill level.
     */
    bool asrc;
    uint32_t asrc_frac;         /* 16.16 position inside the current frame */
    uint32_t asrc_step;         /* 16.16 step, 0x10000 = 1:1               */
    int16_t asrc_prev[2];       /* the frame the position has just passed  */
    int64_t last_report_ms;
} cdj_dspau;

/* Trim target floor above prefill (10 ms), to stay clear of the underrun edge. */
#define CDJ_DSPAU_TRIM_FLOOR (441u)

/* +/-4 % of 0x10000: the widest the ASRC may bend the rate, and so the pitch. */
#define CDJ_DSPAU_ASRC_MAX  2621

#define CDJ_DSPAU_HIST 16384            /* power of two, > the loop length */
#define CDJ_DSPAU_REPLAY 8820           /* the pause's first pass, see below */

static struct {
    uint32_t period, run, miss, pos;
    bool muting;
    uint32_t hist[CDJ_DSPAU_HIST];      /* L<<16 | R of the last frames */
    uint64_t muted;
} cdj_dspau_loop;

static void cdj_dspau_cb(void *opaque, int avail)
{
    int16_t out[512];
    int n;

    qemu_mutex_lock(&cdj_dspau.lock);
    {
        int64_t t = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

        if (t - cdj_dspau.last_report_ms >= 5000) {
            cdj_dspau.last_report_ms = t;
            info_report("c6x audio: %" PRIu64 " frames in, %" PRIu64 " out, "
                        "%" PRIu64 " underruns, %" PRIu64 " dropped in %"
                        PRIu64 " trims, fill %u (prefill %u, cap %u), "
                        "asrc %+.2f %%",
                        cdj_dspau.frames_in, cdj_dspau.frames_out,
                        cdj_dspau.underruns, cdj_dspau.dropped,
                        cdj_dspau.trims, cdj_dspau.fill,
                        cdj_dspau.prefill, cdj_dspau.maxlat,
                        cdj_dspau.asrc
                        ? 100.0 * ((double)cdj_dspau.asrc_step - 65536.0) / 65536.0
                        : 0.0);
        }
    }
    /*
     * ASRC control loop, one step per callback: the fill error moves far
     * slower than a buffer. Clamped to CDJ_DSPAU_ASRC_MAX.
     */
    if (cdj_dspau.asrc) {
        int32_t target = (int32_t)(cdj_dspau.maxlat
                                   ? (cdj_dspau.prefill + cdj_dspau.maxlat) / 2
                                   : cdj_dspau.prefill * 2);
        int32_t half = target / 2 > 0 ? target / 2 : 1;
        int64_t err = (int64_t)cdj_dspau.fill - target;
        int64_t adj = err * CDJ_DSPAU_ASRC_MAX / half;

        if (adj > CDJ_DSPAU_ASRC_MAX) {
            adj = CDJ_DSPAU_ASRC_MAX;
        } else if (adj < -CDJ_DSPAU_ASRC_MAX) {
            adj = -CDJ_DSPAU_ASRC_MAX;
        }
        cdj_dspau.asrc_step = (uint32_t)(0x10000 + adj);
    }
    if (cdj_dspau.maxlat && cdj_dspau.fill > cdj_dspau.maxlat) {
        uint32_t floor_ = cdj_dspau.prefill + CDJ_DSPAU_TRIM_FLOOR;
        uint32_t target = cdj_dspau.maxlat - cdj_dspau.maxlat / 8;
        uint32_t skip;

        /* Stay above prefill; the cap itself is the backstop. */
        if (target < floor_) {
            target = floor_;
        }
        if (target > cdj_dspau.maxlat) {
            target = cdj_dspau.maxlat;
        }
        skip = cdj_dspau.fill > target ? cdj_dspau.fill - target : 0;
        if (cdj_dspau.trim && skip > cdj_dspau.trim) {
            skip = cdj_dspau.trim;
        }
        if (skip) {
            cdj_dspau.rd = (cdj_dspau.rd + skip) % cdj_dspau.cap;
            cdj_dspau.fill -= skip;
            cdj_dspau.dropped += skip;
            cdj_dspau.trims++;
        }
    }
    while (avail >= 4) {
        bool silent = cdj_dspau.priming && cdj_dspau.fill < cdj_dspau.prefill;

        if (!silent) {
            cdj_dspau.priming = false;
        }
        n = 0;
        while (!silent && n + 2 <= (int)ARRAY_SIZE(out) && avail >= 4
               && cdj_dspau.fill) {
            if (!cdj_dspau.asrc) {
                out[n++] = cdj_dspau.ring[cdj_dspau.rd * 2];
                out[n++] = cdj_dspau.ring[cdj_dspau.rd * 2 + 1];
                cdj_dspau.rd = (cdj_dspau.rd + 1) % cdj_dspau.cap;
                cdj_dspau.fill--;
                avail -= 4;
                continue;
            }
            /* Consume input frames the fractional position has passed, then
             * interpolate between the last one and the next. */
            while (cdj_dspau.asrc_frac >= 0x10000u && cdj_dspau.fill) {
                cdj_dspau.asrc_prev[0] = cdj_dspau.ring[cdj_dspau.rd * 2];
                cdj_dspau.asrc_prev[1] = cdj_dspau.ring[cdj_dspau.rd * 2 + 1];
                cdj_dspau.rd = (cdj_dspau.rd + 1) % cdj_dspau.cap;
                cdj_dspau.fill--;
                cdj_dspau.asrc_frac -= 0x10000u;
            }
            if (!cdj_dspau.fill) {
                break;                  /* nothing to interpolate towards */
            }
            {
                int32_t f = (int32_t)cdj_dspau.asrc_frac;
                int32_t a0 = cdj_dspau.asrc_prev[0];
                int32_t a1 = cdj_dspau.asrc_prev[1];
                int32_t b0 = cdj_dspau.ring[cdj_dspau.rd * 2];
                int32_t b1 = cdj_dspau.ring[cdj_dspau.rd * 2 + 1];

                out[n++] = (int16_t)(a0 + (((b0 - a0) * f) >> 16));
                out[n++] = (int16_t)(a1 + (((b1 - a1) * f) >> 16));
            }
            cdj_dspau.asrc_frac += cdj_dspau.asrc_step;
            avail -= 4;
        }
        if (n) {
            cdj_dspau.frames_out += n / 2;
            AUD_write(cdj_dspau.voice, (uint8_t *)out, n * 2);
            continue;
        }
        if (!silent) {
            cdj_dspau.underruns++;
            cdj_dspau.priming = true;
        }
        n = MIN((int)ARRAY_SIZE(out), avail / 4 * 2);
        memset(out, 0, n * sizeof(out[0]));
        AUD_write(cdj_dspau.voice, (uint8_t *)out, n * 2);
        break;
    }
    qemu_mutex_unlock(&cdj_dspau.lock);
}

/* At machine init, for the reason cdj_audio_live_arm() gives. */
void cdj_dspau_arm(void)
{
    const char *e = getenv("CDJ_C6X_AUDIO");
    struct audsettings as = {
        .freq = 44100, .nchannels = 2, .fmt = AUDIO_FORMAT_S16, .endianness = 0,
    };
    unsigned ring_ms = 4000, prefill_ms = 500, maxlat_ms = 0;
    Error *err = NULL;

    if (!e || atoi(e) <= 0) {
        return;
    }
    sscanf(e, "%*d:%u:%u:%u", &ring_ms, &prefill_ms, &maxlat_ms);
    cdj_dspau.cap = MAX(ring_ms, 100u) * 44100 / 1000;
    cdj_dspau.prefill = MIN(prefill_ms * 44100 / 1000, cdj_dspau.cap / 2);
    cdj_dspau.maxlat = maxlat_ms ? MAX(MIN(maxlat_ms * 44100 / 1000, cdj_dspau.cap),
                                       cdj_dspau.prefill + 441) : 0;
    /* CDJ_C6X_AUDIO_ASRC=0 disables the resampler (plain 1:1 read). */
    {
        const char *a = getenv("CDJ_C6X_AUDIO_ASRC");

        cdj_dspau.asrc = !(a && *a && strcmp(a, "0") == 0);
        cdj_dspau.asrc_step = 0x10000;
    }
    /*
     * CDJ_C6X_AUDIO_TRIM_MS: maximum drop per callback for the latency trim,
     * 0 (default) = one step. Small steady trims against a rate mismatch
     * sound like continuous distortion; an occasional single skip is less
     * audible. The trim stops a little below the cap, not at prefill.
     */
    {
        const char *t = getenv("CDJ_C6X_AUDIO_TRIM_MS");

        cdj_dspau.trim = (t ? (unsigned)MAX(atoi(t), 0) : 0u) * 44100 / 1000;
    }
    cdj_dspau.ring = g_new0(int16_t, cdj_dspau.cap * 2);
    cdj_dspau.priming = true;
    qemu_mutex_init(&cdj_dspau.lock);
    AUD_register_card("cdj2000nxs2-dsp", &cdj_dspau.card, &err);
    if (err) {
        warn_report_err(err);
        warn_report("c6x audio: no audio backend (-audio pa,... or wav)");
        cdj_dspau.failed = true;
        return;
    }
    cdj_dspau.voice = AUD_open_out(&cdj_dspau.card, NULL, "cdj-c6x-mcbsp",
                                   NULL, cdj_dspau_cb, &as);
    if (!cdj_dspau.voice) {
        warn_report("c6x audio: the backend refused a 44100 Hz stereo voice");
        cdj_dspau.failed = true;
        return;
    }
    AUD_set_active_out(cdj_dspau.voice, 1);
    {
        const char *lp = getenv("CDJ_C6X_AUDIO_LOOP");

        cdj_dspau_loop.period = MIN(lp ? (uint32_t)strtoul(lp, NULL, 0) : 7644,
                                    CDJ_DSPAU_HIST - 1);
    }
    cdj_dspau.on = true;
    info_report("c6x audio: IC301's McBSP0 PCM -> host voice, ring %u ms, "
                "prefill %u ms, latency cap %u ms", ring_ms, prefill_ms, maxlat_ms);
}

/*
 * Loop mute. While paused the DSP stops refilling its McBSP buffer but the DMA
 * keeps sending it, so the same buffer repeats. Decoded music never repeats bit
 * for bit across a whole buffer, so a non-silent stream that matches itself one
 * buffer back for 10 ms is muted until it stops matching.
 * The loop does not start at once: after ~20 ms of silence the DSP first sends
 * the buffer it played 8820 frames (200 ms) earlier, sample for sample, and
 * that pass cannot match one buffer back. Compared at the buffer length alone,
 * 173 ms of stale audio reached the speakers at every stop.
 * CDJ_C6X_AUDIO_LOOP=<frames> sets the buffer length (default 7644), 0 disables.
 */

static bool cdj_dspau_loop_frame(int16_t l, int16_t r)
{
    uint32_t f = ((uint32_t)(uint16_t)l << 16) | (uint16_t)r;
    uint32_t p = cdj_dspau_loop.period;
    bool same;

    if (!p) {
        return false;
    }
    same = cdj_dspau_loop.hist[(cdj_dspau_loop.pos - p) &
                               (CDJ_DSPAU_HIST - 1)] == f ||
           cdj_dspau_loop.hist[(cdj_dspau_loop.pos - CDJ_DSPAU_REPLAY) &
                               (CDJ_DSPAU_HIST - 1)] == f;
    cdj_dspau_loop.hist[cdj_dspau_loop.pos++ & (CDJ_DSPAU_HIST - 1)] = f;
    /* Digital silence occurs inside the looped buffer too, so a zero frame
     * neither ends a match run nor unmutes. */
    if (f) {
        cdj_dspau_loop.run = same ? cdj_dspau_loop.run + 1 : 0;
        cdj_dspau_loop.miss = same ? 0 : cdj_dspau_loop.miss + 1;
    }
    if (cdj_dspau_loop.run >= 441 && !cdj_dspau_loop.muting) {
        cdj_dspau_loop.muting = true;
        info_report("c6x audio: output buffer is looping (paused deck), muting");
    } else if (cdj_dspau_loop.miss >= 64 && cdj_dspau_loop.muting) {
        cdj_dspau_loop.muting = false;
        info_report("c6x audio: fresh audio, unmuted after %" PRIu64 " frames",
                    cdj_dspau_loop.muted);
    }
    if (cdj_dspau_loop.muting) {
        cdj_dspau_loop.muted++;
    }
    return cdj_dspau_loop.muting;
}

static void cdj_dspau_word(uint32_t word)
{
    int16_t s = (int16_t)((int32_t)word >> 16);

    if (!cdj_dspau.have_left) {
        cdj_dspau.left = s;
        cdj_dspau.have_left = true;
        return;
    }
    cdj_dspau.have_left = false;
    if (cdj_dspau_loop_frame(cdj_dspau.left, s)) {
        cdj_dspau.left = s = 0;
    }
    qemu_mutex_lock(&cdj_dspau.lock);
    if (cdj_dspau.fill == cdj_dspau.cap) {
        cdj_dspau.rd = (cdj_dspau.rd + 1) % cdj_dspau.cap;
        cdj_dspau.fill--;
        cdj_dspau.dropped++;
    }
    cdj_dspau.ring[cdj_dspau.wr * 2] = cdj_dspau.left;
    cdj_dspau.ring[cdj_dspau.wr * 2 + 1] = s;
    cdj_dspau.wr = (cdj_dspau.wr + 1) % cdj_dspau.cap;
    cdj_dspau.fill++;
    cdj_dspau.frames_in++;
    qemu_mutex_unlock(&cdj_dspau.lock);
}

void cdj_c6x_mcbsp_tx(void *opaque, unsigned port, uint32_t word,
                             unsigned bits)
{
    CdjC6x *c = &cdj_c6x;
    int64_t p0 = c->prof ? get_clock() : 0;

    if (cdj_dspau.on && port == 0) {
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

