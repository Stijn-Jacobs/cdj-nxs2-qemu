/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ850_VFD_PITCH_H
#define CDJ850_VFD_PITCH_H

#include <stdint.h>

#define CDJ850_PITCH_WIDE 100

typedef struct Cdj850Pitch {
    int16_t value;       /* Signed units at the transmitted precision. */
    uint8_t decimals;    /* 0 for a blank readout, otherwise 1 or 2. */
    uint8_t range;       /* 0 for an unknown range, otherwise 6/10/16/100. */
} Cdj850Pitch;

#endif
