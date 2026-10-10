/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ2000_DSP_DMAX_H
#define CDJ2000_DSP_DMAX_H
#include "qemu/osdep.h"

#define DMAX_BASE       0x60000000u
#define DMAX_SIZE       0x03000000u     /* control, then MAX0 and MAX1 */

typedef struct CdjDmax {
    uint32_t ctl[0x40];
    uint32_t max[2][0x4000];            /* microcode, then parameter RAM at +0x2000 words */
    uint32_t enabled;                   /* set through DEER, cleared through DEDR */
    uint32_t high_priority;             /* set through DEHPR, cleared through DELPR */
    uint32_t detr;
    int64_t audio_deadline_ns;          /* DSP clock; 0 = not started */
    bool audio_tx_up;                   /* McASP1 and McASP2 transmit sections released */
    uint64_t audio_halves, audio_overruns;
    /* Called for each frame the McASPs send; NULL discards the audio. */
    void (*audio_frame)(void *chip, unsigned mcasp, uint32_t left,
                        uint32_t right);
    /* The chip's side: DSP memory by address, and the CPU interrupt lines. */
    uint8_t *(*ram)(void *chip, uint32_t addr);
    void (*set_irq)(void *chip, int line, int level);
    /* A transfer has written DSP memory [addr, addr + len). */
    void (*stored)(void *chip, uint32_t addr, uint32_t len);
    void *chip;
} CdjDmax;

uint32_t cdj_dmax_read(CdjDmax *d, uint32_t addr);
void cdj_dmax_write(CdjDmax *d, uint32_t addr, uint32_t val);
/* The CPU's write to the event trigger register (DETR, control register 9). */
void cdj_dmax_detr(CdjDmax *d, uint32_t val);

/* The McASPs' transmitters are running or not; the audio events need both it
 * and the event enable bits. */
void cdj_dmax_audio_tx(CdjDmax *d, bool up);
/* The DSP clock has reached @dsp_ns: send the McASP halves that are due. */
void cdj_dmax_audio_run(CdjDmax *d, int64_t dsp_ns);
void cdj_dmax_report(CdjDmax *d);
#endif
