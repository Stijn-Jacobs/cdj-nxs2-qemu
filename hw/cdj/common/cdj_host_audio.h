/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The host playback sink the DSP boards share: a ring of 44.1 kHz S16 stereo
 * frames in front of one QEMU output voice. The DSP runs at virtual speed and
 * the backend pulls at wall speed, so after an underrun playback waits for
 * the prefill, and a slow control loop on the fill level bends the read rate
 * by up to 4 % (ASRC) because a host sink never drains at exactly 44.1 kHz.
 *
 * Each board names its knob <env> and gets
 *   <env>=1[:<ring_ms>][:<prefill_ms>][:<maxlat_ms>]
 *   <env>_ASRC=0       plain 1:1 read
 *   <env>_TRIM_MS=<n>  most the latency cap drops per callback, 0 = one step
 * <maxlat_ms> (default 0 = off) caps the ring fill so controls are not heard
 * seconds late; keep it well above the prefill.
 */
#ifndef CDJ_HOST_AUDIO_H
#define CDJ_HOST_AUDIO_H
#include "qemu/osdep.h"

typedef struct CdjHostAudio CdjHostAudio;

typedef struct CdjHostAudioCfg {
    const char *env;        /* knob name, e.g. "CDJ_C6X_AUDIO" */
    const char *card;       /* QEMU sound card name */
    const char *voice;      /* QEMU voice name */
    const char *label;      /* log prefix, e.g. "c6x" */
    const char *source;     /* what feeds it, for the start-up line */
} CdjHostAudioCfg;

/* At machine init, for the reason cdj_audio_live_arm() gives: a voice opened
 * after the VM runs is never pulled. NULL when the knob is off or the backend
 * refuses the voice. */
CdjHostAudio *cdj_host_audio_open(const CdjHostAudioCfg *cfg);

void cdj_host_audio_put(CdjHostAudio *a, int16_t left, int16_t right);
#endif
