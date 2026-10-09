/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dsp_audio_out.h"
#include "cdj_host_audio.h"
/*
 * Host playback of the older decks' DSP output.
 *
 * CDJ_DSP_AUDIO=1[:<ring_ms>][:<prefill_ms>][:<maxlat_ms>]
 *   The DSP sends 44.1 kHz stereo to the DAC over McASP1 as I2S, two 32-bit
 *   slots per frame, each word a left-aligned 24-bit sample. The sink and its
 *   rate control are the NXS2's, in cdj_host_audio.c.
 */
#define DAC_MCASP       1

static CdjHostAudio *sink;
static uint32_t left_word;
static bool have_left;

void cdj_dsp_audio_arm(void)
{
    static const CdjHostAudioCfg cfg = {
        .env = "CDJ_DSP_AUDIO", .card = "cdj-dsp", .voice = "cdj-dsp-mcasp1",
        .label = "dsp", .source = "McASP1",
        /* Paused, the DSP program keeps sending the McASP buffer it last
         * filled: 15288 frames (two 7644-frame halves) at deck speed 1.0, bit
         * for bit, on both chips. Right after the stop it first sends the
         * buffer from 17880 frames back. */
        .loop = { .period = 15288, .replay = 17880,
                  .lag_min = 12000, .lag_max = 24000 },
    };

    sink = cdj_host_audio_open(&cfg);
}

void cdj_dsp_audio_frame(unsigned mcasp, uint32_t left, uint32_t right)
{
    if (sink && mcasp == DAC_MCASP) {
        cdj_host_audio_put(sink, (int32_t)left >> 16, (int32_t)right >> 16);
    }
}

void cdj_dsp_audio_word(unsigned mcasp, uint32_t word)
{
    if (!sink || mcasp != DAC_MCASP) {
        return;
    }
    if (!have_left) {
        left_word = word;
        have_left = true;
        return;
    }
    have_left = false;
    cdj_dsp_audio_frame(mcasp, left_word, word);
}
