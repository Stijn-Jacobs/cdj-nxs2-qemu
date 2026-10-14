/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 interrupt + compare-match timer core (xc/cmt.c). */
#ifndef CDJ850_CMT_H
#define CDJ850_CMT_H
#include "cdj_common.h"

void cdj850_cmt_init(MemoryRegion *sysmem, SuperHCPU *cpu);
#endif
