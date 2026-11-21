/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared USB function controller (R8A66597) interface for CDJ boards. */
#ifndef CDJ_USBF_H
#define CDJ_USBF_H

#include "cdj_common.h"

/* The rear USB-B port's bus side: what a token gets back besides a length. */
#define CDJ_USBF_NAK   (-1)
#define CDJ_USBF_STALL (-2)

typedef struct CdjUsbfState CdjUsbfState;

/* When sysmem is NULL, the caller maps sysbus_mmio_get_region(s, 0) itself. */
CdjUsbfState *cdj_usbf_init(MemoryRegion *sysmem, qemu_irq irq);
bool cdj_usbf_attached(CdjUsbfState *s);
void cdj_usbf_vbus(CdjUsbfState *s, bool on);
bool cdj_usbf_has_vbus(CdjUsbfState *s);
void cdj_usbf_set_cable(CdjUsbfState *s, bool connected);
void cdj_usbf_reset(CdjUsbfState *s);
void cdj_usbf_bus_reset(CdjUsbfState *s);
int cdj_usbf_setup(CdjUsbfState *s, const uint8_t setup[8]);
int cdj_usbf_in(CdjUsbfState *s, unsigned ep, uint8_t *buf, unsigned max);
int cdj_usbf_out(CdjUsbfState *s, unsigned ep, const uint8_t *buf,
                 unsigned len);
void cdj_usbf_host_init(CdjUsbfState *dev);

#endif
