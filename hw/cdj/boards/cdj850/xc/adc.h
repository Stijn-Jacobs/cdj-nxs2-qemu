/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 A/D converter (tempo slider), single SH7263 ADC unit. */
#ifndef CDJ850_ADC_H
#define CDJ850_ADC_H
#include "cdj_common.h"

void cdj850_adc_init(MemoryRegion *sysmem, SuperHCPU *cpu);

#endif
