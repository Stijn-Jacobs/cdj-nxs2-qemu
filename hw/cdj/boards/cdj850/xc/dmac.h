/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 direct memory access controller at 0xFFFE1000 (xc/dmac.c). */
#ifndef CDJ850_DMAC_H
#define CDJ850_DMAC_H
#include "cdj_common.h"

/* DMARS MID/RID bytes of the two USB requests (manual Table 10.4). */
#define CDJ850_DMARS_USB_DMA0  0x03            /* MID 000000, RID 11: D0FIFO */
#define CDJ850_DMARS_USB_DMA1  0x07            /* MID 000001, RID 11: D1FIFO */

void cdj850_dmac_init(MemoryRegion *sysmem, SuperHCPU *cpu);

/* A peripheral's transfer request (DMARS byte) became active: serve the
 * channels selecting it, shortly, outside the caller's register access. */
void cdj850_dmac_request(uint8_t dmars);
/* The same, served now, inside the caller's access (USB transmit). */
void cdj850_dmac_serve(uint8_t dmars);
#endif
