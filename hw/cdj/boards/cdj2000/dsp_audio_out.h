/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_DSP_AUDIO_OUT_H
#define CDJ_DSP_AUDIO_OUT_H
#include "qemu/osdep.h"

/* At chip init: opens the host voice when CDJ_DSP_AUDIO is set. */
void cdj_dsp_audio_arm(void);

/* One frame for McASP1, the DAC; McASP2's S/PDIF carries the same samples. */
void cdj_dsp_audio_frame(unsigned mcasp, uint32_t left, uint32_t right);

/* The same for a chip whose transfers deliver one word at a time. */
void cdj_dsp_audio_word(unsigned mcasp, uint32_t word);
#endif
