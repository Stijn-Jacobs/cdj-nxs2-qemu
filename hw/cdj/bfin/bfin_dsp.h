/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The DSP32 groups and the arithmetic flag helpers they share with
 * bfin_exec.c. */
#ifndef BFIN_DSP_H
#define BFIN_DSP_H

#include "bfin_priv.h"

void     bfin_flags_nz(bfin_core *c, uint32_t v);
void     bfin_flags_v(bfin_core *c, int v);
void     bfin_flags_ac0(bfin_core *c, int ac0);
uint32_t bfin_add32(bfin_core *c, uint32_t a, uint32_t b, int sub, int sat);

void bfin_divs(bfin_core *c, unsigned dst, unsigned src);
void bfin_divq(bfin_core *c, unsigned dst, unsigned src);

void bfin_dsp32mac(bfin_core *c, uint16_t iw0, uint16_t iw1, int mult);
void bfin_dsp32alu(bfin_core *c, uint16_t iw0, uint16_t iw1);
void bfin_dsp32shift(bfin_core *c, uint16_t iw0, uint16_t iw1);
void bfin_dsp32shiftimm(bfin_core *c, uint16_t iw0, uint16_t iw1);

#endif
