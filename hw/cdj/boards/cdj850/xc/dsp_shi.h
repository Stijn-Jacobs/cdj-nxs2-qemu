/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 audio DSP (DSP56371) SHI bootstrap observer (xc/dsp_shi.c). */
#ifndef CDJ850_DSP_SHI_H
#define CDJ850_DSP_SHI_H
#include "cdj_common.h"

/* Register VMState for the SHI bootstrap observer. */
void cdj850_dsp_shi_init(void);
/* One byte MAIN clocked out on SSU0 (the DSP's SHI, SPI slave). */
void cdj850_dsp_shi_tx(uint8_t b);
/* One byte MAIN clocked in from the DSP's SHI on SSU0. */
uint8_t cdj850_dsp_shi_rx(void);
#endif
