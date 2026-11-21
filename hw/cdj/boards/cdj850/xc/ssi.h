/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ850_SSI_H
#define CDJ850_SSI_H
#include "cdj_common.h"

/* CDJ-850 SSI0/SSI1 audio serial ports (SH7263 manual Table 18.2). */
void cdj850_ssi_init(MemoryRegion *sysmem, SuperHCPU *cpu);
bool cdj850_ssi_tx_dma_request(unsigned ch);
void cdj850_ssi_kick(void);
void cdj850_ssi_notify_rx_space(void);
#endif
