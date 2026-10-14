/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ850_DSP56371_H
#define CDJ850_DSP56371_H
#include "cdj_common.h"

/* CDJ-850 audio DSP (IC301, DSPC56371AF180): bootstrap READY + upload
 * record peer for MAIN's Port-E bit-bang driver. See dsp56371.c. */
void cdj850_dsp_init(void);
/* MAIN write snoop (bring-up tracker + record). pc = guest PC, ms = vms. */
void cdj850_dsp_snoop_write(hwaddr off, uint64_t val, unsigned size,
                            uint32_t pc, uint64_t ms);
/* DSP-driven input levels ORed over the stored latch on Port-E reads.
 * pc = guest PC performing the read (lets the experimental reply drive
 * answer only the sampler's two phase reads). */
uint64_t cdj850_dsp_adjust_read(hwaddr off, unsigned size, uint64_t v,
                                uint32_t pc);
#endif
