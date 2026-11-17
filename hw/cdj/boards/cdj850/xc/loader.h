/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 CD slot-in loading mechanism model (xc/loader.c). */
#ifndef CDJ850_LOADER_H
#define CDJ850_LOADER_H
#include "cdj_common.h"

/*
 * Env: CDJ850_LOADER=0 turns the model off (switches read the constant
 * 0x0600, both open); CDJ850_LOADER_SW=0xNNNN pins PFDRH bits 8-10
 * (INSIDE, LPS1, LPS2) to a fixed value and turns the model off.
 */
void cdj850_loader_init(void);

/* PFDRH bits 8-10 as the board drives them: bit 8 INSIDE (PF24), bit 9
 * LPS1 (PF25), bit 10 LPS2 (PF26). The model, or the fixed value when it
 * is off. */
uint16_t cdj850_loader_pf_hi_pins(void);
#endif
