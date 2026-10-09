/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
#include "cdj_host_audio.h"
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

/* At machine init, for the reason cdj_host_audio_open() gives. */
void cdj_dspau_arm(void)
{
    /* The pause's first pass replays the buffer sent 8820 frames (200 ms)
     * earlier; the loop it settles into is 7644 frames at deck speed 1.0 and
     * scales with the deck speed (fader or MASTER TEMPO). */
    static const CdjHostAudioCfg cfg = {
        .env = "CDJ_C6X_AUDIO", .card = "cdj2000nxs2-dsp",
        .voice = "cdj-c6x-mcbsp", .label = "c6x",
        .source = "IC301's McBSP0 PCM",
        .loop = { .period = 7644, .replay = 8820,
                  .lag_min = 7000, .lag_max = 20000 },
    };

    cdj_dspau = cdj_host_audio_open(&cfg);
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

