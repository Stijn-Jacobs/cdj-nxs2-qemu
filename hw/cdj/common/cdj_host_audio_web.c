/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * cdj_host_audio.h for the browser build. A browser has no QEMU audio
 * backend; the page's audio worklet reads a ring in this module's shared heap
 * (the layout of web/audio-ring.js) and holds its own fill level and rate
 * loop, so this side only keeps the board's mutes and writes the frames in.
 * The frames come from the DSP thread, a worker with no page objects, which
 * is why the ring is plain memory rather than a JS writer. They go in 128 at
 * a time (2.9 ms, one worklet quantum).
 *
 * Only <env>=1 and <env>_LOOP mean anything here; the ring, prefill, latency
 * cap and ASRC knobs belong to the worklet.
 */
#include "cdj_host_audio.h"
#include "qemu/atomic.h"
#include <emscripten.h>

#define BATCH_FRAMES 128
/* 10 ms of frames between two cfg.held() polls. */
#define HELD_POLL_FRAMES 441u
/* A power of two over audio-ring.js's one second. */
#define RING_FRAMES 65536u

enum { RING_WR, RING_RD, RING_DROPPED = 4, RING_FRAMES_SLOT = 7, RING_HEADER };

static struct {
    uint32_t hdr[RING_HEADER];
    int16_t pcm[RING_FRAMES * 2];
} ring = { .hdr[RING_FRAMES_SLOT] = RING_FRAMES };

struct CdjHostAudio {
    CdjHostAudioCfg cfg;
    CdjLoopMute *loop;
    uint32_t held_poll;
    bool held;
    uint32_t batched;
    int16_t batch[BATCH_FRAMES * 2];
};

EMSCRIPTEN_KEEPALIVE void *cdj_web_audio_ring(void)
{
    return &ring;
}

/* A full ring drops what does not fit, and one frame stays free: the
 * worklet's cubic reads one frame behind RD. */
static void ring_write(const int16_t *frames, uint32_t count)
{
    uint32_t wr = qatomic_read(&ring.hdr[RING_WR]);
    uint32_t fill = wr - qatomic_load_acquire(&ring.hdr[RING_RD]);
    uint32_t take = MIN(count, fill < RING_FRAMES ? RING_FRAMES - 1 - fill : 0);

    for (uint32_t i = 0; i < take; i++) {
        memcpy(&ring.pcm[((wr + i) & (RING_FRAMES - 1)) * 2], &frames[i * 2], 4);
    }
    if (take < count) {
        qatomic_add(&ring.hdr[RING_DROPPED], count - take);
    }
    qatomic_store_release(&ring.hdr[RING_WR], wr + take);
}

CdjHostAudio *cdj_host_audio_open(const CdjHostAudioCfg *cfg)
{
    const char *e = getenv(cfg->env);
    char name[64];
    const char *opt;
    CdjLoopMuteCfg loop;
    CdjHostAudio *a;

    if (!e || atoi(e) <= 0) {
        return NULL;
    }
    a = g_new0(CdjHostAudio, 1);
    a->cfg = *cfg;
    snprintf(name, sizeof(name), "%s_LOOP", cfg->env);
    opt = getenv(name);
    loop = cfg->loop;
    if (opt && *opt) {
        loop.period = strtoul(opt, NULL, 0);
    }
    if (loop.period) {
        a->loop = cdj_loop_mute_new(&loop, cfg->label);
    }
    return a;
}

void cdj_host_audio_put(CdjHostAudio *a, int16_t left, int16_t right)
{
    if (a->cfg.held && ++a->held_poll >= HELD_POLL_FRAMES) {
        a->held_poll = 0;
        a->held = a->cfg.held();
    }
    if ((a->loop && cdj_loop_mute_frame(a->loop, left, right)) || a->held) {
        left = right = 0;
    }
    a->batch[a->batched * 2] = left;
    a->batch[a->batched * 2 + 1] = right;
    if (++a->batched == BATCH_FRAMES) {
        ring_write(a->batch, a->batched);
        a->batched = 0;
    }
}
