/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 SCIF0/1 serial consoles (TX-ready stub with TX capture). */
#ifndef CDJ850_SCIF_H
#define CDJ850_SCIF_H
#include "cdj_common.h"

void cdj850_scif_init(MemoryRegion *sysmem, SuperHCPU *cpu);

#endif
