/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 MTU2 (SH7263 multi-function timer pulse unit 2) register capture. */
#ifndef CDJ850_MTU2_H
#define CDJ850_MTU2_H
#include "cdj_common.h"

void cdj850_mtu2_init(MemoryRegion *sysmem, SuperHCPU *cpu);

/*
 * Channel 3 as the CD loader drives it: true while TSTR.CST3 is set and
 * TMDR_3 selects PWM mode 1, in which case the values the firmware
 * programmed are returned. False while the channel is stopped or in
 * another mode (the loader stops it while it reprograms the duty).
 */
bool cdj850_mtu2_ch3_pwm(uint8_t *tiorl, uint16_t *tgrc, uint16_t *tgrd);
#endif
