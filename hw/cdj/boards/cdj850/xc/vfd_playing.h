/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ850_VFD_PLAYING_H
#define CDJ850_VFD_PLAYING_H

#include <stdbool.h>
#include <stdint.h>

#define CDJ850_OVERVIEW_COLUMNS 100
#define CDJ850_OVERVIEW_ROWS 5

typedef struct Cdj850Playing {
    bool artist_known;
    char artist[256];
    bool waveform_known;
    uint8_t waveform[CDJ850_OVERVIEW_COLUMNS];
    bool strips_known;
    uint8_t memory[CDJ850_OVERVIEW_COLUMNS];
    uint8_t position[CDJ850_OVERVIEW_COLUMNS];
    uint8_t cue[CDJ850_OVERVIEW_COLUMNS];
    bool memory_lamp, cue_lamp;
} Cdj850Playing;

#endif
