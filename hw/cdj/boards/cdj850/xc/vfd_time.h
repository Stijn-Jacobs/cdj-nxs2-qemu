/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ850_VFD_TIME_H
#define CDJ850_VFD_TIME_H

#include <stdbool.h>
#include <stdint.h>

typedef struct Cdj850Time {
    uint8_t minutes, seconds, frames; /* 75 frames per second */
    bool known;
    bool remain_known;
    bool remain;
} Cdj850Time;

static inline bool cdj850_time_in_range(const Cdj850Time *time)
{
    return time->minutes < 100 && time->seconds < 60 && time->frames < 75;
}

#endif
