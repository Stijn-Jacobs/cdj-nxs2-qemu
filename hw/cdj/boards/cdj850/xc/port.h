/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 ports/PFC at 0xFFFE3800 (Renesas SH7263 general I/O). */
#ifndef CDJ850_PORT_H
#define CDJ850_PORT_H
#include "cdj_common.h"

void cdj850_port_init(MemoryRegion *sysmem, SuperHCPU *cpu);
#endif
