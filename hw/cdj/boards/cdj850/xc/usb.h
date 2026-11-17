/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 on-chip USB 2.0 host/function module at 0xFFFC1C00 (xc/usb.c). */
#ifndef CDJ850_USB_H
#define CDJ850_USB_H
#include "cdj_common.h"

void cdj850_usb_init(MemoryRegion *sysmem, SuperHCPU *cpu);

/* USB_DMA0 (n = 1, D0FIFO) / USB_DMA1 (n = 2, D1FIFO) request state, for
 * the DMAC (xc/dmac.c). */
bool cdj850_usb_dma_request(unsigned n);
#endif
