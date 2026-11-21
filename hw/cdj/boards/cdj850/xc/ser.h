/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 serial block at 0xFFFE7000 (TX stub with tracing). */
#ifndef CDJ850_SER_H
#define CDJ850_SER_H
#include "cdj_common.h"

void cdj850_ser_init(MemoryRegion *sysmem, SuperHCPU *cpu);
#endif
