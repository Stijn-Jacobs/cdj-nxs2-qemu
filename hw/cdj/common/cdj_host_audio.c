/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_host_audio.h"
#include "cdj_common.h"
#include "audio/audio.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/thread.h"
#include "qemu/timer.h"

struct CdjHostAudio {
    CdjHostAudioCfg cfg;
    QEMUSoundCard card;
    SWVoiceOut *voice;
    QemuMutex lock;
    int16_t *ring;              /* stereo frames */
    uint32_t cap, rd, wr, fill, prefill, maxlat;
    uint32_t trim;              /* max frames dropped per callback, 0 = all */
    bool priming;
    uint64_t frames_in, frames_out, underruns, dropped, trims;
    /*
     * Asynchronous sample-rate conversion. A rate error cannot be fixed by
     * dropping frames, so the ring is read with a fractional step chosen by
     * a slow control loop on the fill level.
     */
    bool asrc;
    uint32_t asrc_frac;         /* 16.16 position inside the current frame */
    uint32_t asrc_step;         /* 16.16 step, 0x10000 = 1:1 */
    int16_t asrc_prev[2];       /* the frame the position has just passed */
    int64_t last_report_ms;
    CdjLoopMute *loop;          /* NULL when the board has no pause loop */
    uint32_t held_poll;         /* frames since cfg.held was asked */
    bool held;
};

/* 10 ms of frames between two cfg.held() polls. */
#define HELD_POLL_FRAMES 441u

/* Trim target floor above prefill (10 ms), to stay clear of the underrun edge. */
#define TRIM_FLOOR 441u

/* +/-4 % of 0x10000: the widest the ASRC may bend the rate, and so the pitch. */
#define ASRC_MAX 2621

static void audio_cb(void *opaque, int avail)
{
    CdjHostAudio *a = opaque;
    int16_t out[512];
    int n;

    qemu_mutex_lock(&a->lock);
    {
        int64_t t = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

        if (t - a->last_report_ms >= 5000) {
            a->last_report_ms = t;
            info_report("%s audio: %" PRIu64 " frames in, %" PRIu64 " out, "
                        "%" PRIu64 " underruns, %" PRIu64 " dropped in %"
                        PRIu64 " trims, fill %u (prefill %u, cap %u), "
                        "asrc %+.2f %%",
                        a->cfg.label, a->frames_in, a->frames_out,
                        a->underruns, a->dropped, a->trims, a->fill,
                        a->prefill, a->maxlat,
                        a->asrc
                        ? 100.0 * ((double)a->asrc_step - 65536.0) / 65536.0
                        : 0.0);
        }
    }
    /*
     * ASRC control loop, one step per callback: the fill error moves far
     * slower than a buffer. Clamped to ASRC_MAX.
     */
    if (a->asrc) {
        int32_t target = (int32_t)(a->maxlat ? (a->prefill + a->maxlat) / 2
                                             : a->prefill * 2);
        int32_t half = target / 2 > 0 ? target / 2 : 1;
        int64_t err = (int64_t)a->fill - target;
        int64_t adj = err * ASRC_MAX / half;

        if (adj > ASRC_MAX) {
            adj = ASRC_MAX;
        } else if (adj < -ASRC_MAX) {
            adj = -ASRC_MAX;
        }
        a->asrc_step = (uint32_t)(0x10000 + adj);
    }
    if (a->maxlat && a->fill > a->maxlat) {
        uint32_t floor_ = a->prefill + TRIM_FLOOR;
        uint32_t target = a->maxlat - a->maxlat / 8;
        uint32_t skip;

        /* Stay above prefill; the cap itself is the backstop. */
        if (target < floor_) {
            target = floor_;
        }
        if (target > a->maxlat) {
            target = a->maxlat;
        }
        skip = a->fill > target ? a->fill - target : 0;
        if (a->trim && skip > a->trim) {
            skip = a->trim;
        }
        if (skip) {
            a->rd = (a->rd + skip) % a->cap;
            a->fill -= skip;
            a->dropped += skip;
            a->trims++;
        }
    }
    while (avail >= 4) {
        bool silent = a->priming && a->fill < a->prefill;

        if (!silent) {
            a->priming = false;
        }
        n = 0;
        while (!silent && n + 2 <= (int)ARRAY_SIZE(out) && avail >= 4
               && a->fill) {
            if (!a->asrc) {
                out[n++] = a->ring[a->rd * 2];
                out[n++] = a->ring[a->rd * 2 + 1];
                a->rd = (a->rd + 1) % a->cap;
                a->fill--;
                avail -= 4;
                continue;
            }
            /* Consume input frames the fractional position has passed, then
             * interpolate between the last one and the next. */
            while (a->asrc_frac >= 0x10000u && a->fill) {
                a->asrc_prev[0] = a->ring[a->rd * 2];
                a->asrc_prev[1] = a->ring[a->rd * 2 + 1];
                a->rd = (a->rd + 1) % a->cap;
                a->fill--;
                a->asrc_frac -= 0x10000u;
            }
            if (!a->fill) {
                break;                  /* nothing to interpolate towards */
            }
            {
                int32_t f = (int32_t)a->asrc_frac;
                int32_t a0 = a->asrc_prev[0];
                int32_t a1 = a->asrc_prev[1];
                int32_t b0 = a->ring[a->rd * 2];
                int32_t b1 = a->ring[a->rd * 2 + 1];

                out[n++] = (int16_t)(a0 + (((b0 - a0) * f) >> 16));
                out[n++] = (int16_t)(a1 + (((b1 - a1) * f) >> 16));
            }
            a->asrc_frac += a->asrc_step;
            avail -= 4;
        }
        if (n) {
            a->frames_out += n / 2;
            AUD_write(a->voice, (uint8_t *)out, n * 2);
            continue;
        }
        if (!silent) {
            a->underruns++;
            a->priming = true;
        }
        n = MIN((int)ARRAY_SIZE(out), avail / 4 * 2);
        memset(out, 0, n * sizeof(out[0]));
        AUD_write(a->voice, (uint8_t *)out, n * 2);
        break;
    }
    qemu_mutex_unlock(&a->lock);
}

CdjHostAudio *cdj_host_audio_open(const CdjHostAudioCfg *cfg)
{
    const char *e = getenv(cfg->env);
    struct audsettings as = {
        .freq = 44100, .nchannels = 2, .fmt = AUDIO_FORMAT_S16, .endianness = 0,
    };
    unsigned ring_ms = 4000, prefill_ms = 500, maxlat_ms = 0;
    char name[64];
    const char *opt;
    CdjHostAudio *a;
    CdjLoopMuteCfg loop;
    Error *err = NULL;

    if (!e || atoi(e) <= 0) {
        return NULL;
    }
    a = g_new0(CdjHostAudio, 1);
    a->cfg = *cfg;
    sscanf(e, "%*d:%u:%u:%u", &ring_ms, &prefill_ms, &maxlat_ms);
    a->cap = MAX(ring_ms, 100u) * 44100 / 1000;
    a->prefill = MIN(prefill_ms * 44100 / 1000, a->cap / 2);
    a->maxlat = maxlat_ms ? MAX(MIN(maxlat_ms * 44100 / 1000, a->cap),
                                a->prefill + 441) : 0;
    snprintf(name, sizeof(name), "%s_ASRC", cfg->env);
    opt = getenv(name);
    a->asrc = !(opt && *opt && strcmp(opt, "0") == 0);
    a->asrc_step = 0x10000;
    /* Small steady trims against a rate mismatch sound like continuous
     * distortion; an occasional single skip is less audible. The trim stops
     * a little below the cap, not at prefill. */
    snprintf(name, sizeof(name), "%s_TRIM_MS", cfg->env);
    opt = getenv(name);
    a->trim = (opt ? (unsigned)MAX(atoi(opt), 0) : 0u) * 44100 / 1000;
    snprintf(name, sizeof(name), "%s_LOOP", cfg->env);
    opt = getenv(name);
    loop = cfg->loop;
    if (opt && *opt) {
        loop.period = strtoul(opt, NULL, 0);
    }
    if (loop.period) {
        a->loop = cdj_loop_mute_new(&loop, cfg->label);
    }
    a->ring = g_new0(int16_t, a->cap * 2);
    a->priming = true;
    qemu_mutex_init(&a->lock);
    AUD_register_card(cfg->card, &a->card, &err);
    if (err) {
        warn_report_err(err);
        warn_report("%s audio: no audio backend (-audio pa,... or wav)",
                    cfg->label);
        return NULL;
    }
    a->voice = AUD_open_out(&a->card, NULL, cfg->voice, a, audio_cb, &as);
    if (!a->voice) {
        warn_report("%s audio: the backend refused a 44100 Hz stereo voice",
                    cfg->label);
        return NULL;
    }
    AUD_set_active_out(a->voice, 1);
    info_report("%s audio: %s -> host voice, ring %u ms, prefill %u ms, "
                "latency cap %u ms", cfg->label, cfg->source, ring_ms,
                prefill_ms, maxlat_ms);
    return a;
}

void cdj_host_audio_put(CdjHostAudio *a, int16_t left, int16_t right)
{
    if (a->cfg.held && ++a->held_poll >= HELD_POLL_FRAMES) {
        bool held = a->cfg.held();

        a->held_poll = 0;
        if (held != a->held) {
            a->held = held;
            if (cdj_report_enabled()) {
                info_report("%s audio: deck %s at %" PRId64 " ms, %s",
                            a->cfg.label, held ? "paused" : "playing",
                            qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                            held ? "muting" : "unmuted");
            }
        }
    }
    if (a->loop && cdj_loop_mute_frame(a->loop, left, right)) {
        left = right = 0;
    }
    if (a->held) {
        left = right = 0;
    }
    qemu_mutex_lock(&a->lock);
    if (a->fill == a->cap) {
        a->rd = (a->rd + 1) % a->cap;
        a->fill--;
        a->dropped++;
    }
    a->ring[a->wr * 2] = left;
    a->ring[a->wr * 2 + 1] = right;
    a->wr = (a->wr + 1) % a->cap;
    a->fill++;
    a->frames_in++;
    qemu_mutex_unlock(&a->lock);
}
