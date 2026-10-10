/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Loop mute for the DSP boards' host audio. While paused a DSP stops refilling
 * its output buffer but the DMA keeps sending it, so the same buffer repeats.
 * Decoded music never repeats across a whole buffer, so a non-silent stream
 * that matches itself one buffer back is muted until it stops matching.
 */
#ifndef CDJ_LOOP_MUTE_H
#define CDJ_LOOP_MUTE_H
#include "qemu/osdep.h"

typedef struct CdjLoopMuteCfg {
    uint32_t period;        /* the buffer length at deck speed 1.0, frames; 0 = off */
    uint32_t replay;        /* the pause's first pass, frames; 0 = the DSP has none */
    uint32_t lag_min;       /* the range a speed-scaled loop can land in, frames */
    uint32_t lag_max;
} CdjLoopMuteCfg;

typedef struct CdjLoopMute CdjLoopMute;

CdjLoopMute *cdj_loop_mute_new(const CdjLoopMuteCfg *cfg, const char *label);

/* True when this frame belongs to a looping buffer and must be played as silence. */
bool cdj_loop_mute_frame(CdjLoopMute *m, int16_t left, int16_t right);
#endif
