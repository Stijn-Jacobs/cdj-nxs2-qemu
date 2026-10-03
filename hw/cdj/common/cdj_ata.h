/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ATA/ATAPI task file for the CD drive, the same SH7724-family IP on the
 * CDJ-2000NXS2 (0xA4DA2100, P1) and the CDJ-2000/CDJ-2000NXS (0xFFF00000,
 * already a physical bus address -- the caller passes whichever base its own
 * board needs, with any P1 stripping already done).
 */
#ifndef CDJ_ATA_H
#define CDJ_ATA_H
#include "cdj_common.h"

/* Modelled as an empty, healthy drive: IDENTIFY passes and every medium
 * command reports no disc. irq is the drive's INTRQ gated by the controller's
 * INT_ENABLE, or NULL where the board polls. Gated on CDJ_ATA=1 (off maps
 * nothing). */
void cdj_ata_init(MemoryRegion *sysmem, const char *name, hwaddr base,
                  qemu_irq irq);

#endif
