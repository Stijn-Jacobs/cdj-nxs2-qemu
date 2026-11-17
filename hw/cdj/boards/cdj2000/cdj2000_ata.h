/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pioneer CDJ-2000 / CDJ-2000NXS disc drive: the SH7764 ATAPI host controller
 * and the slot-in drive behind it.
 *
 * `E-7001: DISC DRIVE ERROR` is device 6 in the status array, raised when the
 * chip is not on the bus.  The window was first found out of the `-d unimp`
 * log of an ordinary run -- MAIN writes 0xfff0001c with 0xA0, 0xEC and 0xA1
 * (PACKET, IDENTIFY DEVICE, IDENTIFY PACKET DEVICE) and polls it.  The
 * CDJ-2000NXS MAIN symbols name the whole block:
 *
 *   task file, one register per 32-bit word (channel stride 60, channel 0 only)
 *     +0x00 data            +0x10 byte count low     +0x1c status / command
 *     +0x04 error / feature +0x14 byte count high    +0x38 alt status / devctl
 *     +0x08 count / reason  +0x18 device
 *     +0x0c sector number
 *   controller (ATAPI_*, 0xfff00080)
 *     +0x80 COMMAND   0xa0/0x20 = device hard reset pulse (atapi_reset),
 *                     0x25 = start DMA (drive_io_wait_result), 0x26 = abort
 *     +0x84 STATUS    bit0 DMA active, bit1 DMA count done, bit2/bit8
 *                     errors, bit4 device INTRQ; written 0 to clear
 *     +0x88 INT_EN    the same bits, ANDed with STATUS onto irq 0x60
 *     +0x8c..+0x94    PIO / multiword / ultra timing (read back)
 *     +0x98 DTB_A     +0x9c DMA_START_ADR (physical)  +0xa0 DMA_TRANS_CNT
 *     +0xa4 CONTROL2  +0xb0 SIG_ST  +0xbc BYTE_SWAP (0: bytes as on the bus)
 *
 * The interrupt is irq 0x60, INTEVT 0xc00; `ATAPI_TSK` gives it a level in
 * INT2PRI6 and unmasks it through INT2MSKCR.
 *
 * The media is a path, not a QEMU -drive: CDJ_DISC=FILE (.iso or .cue) at
 * power-on, and the string property /machine/cdj-atapi.disc at run time:
 * write "insert PATH", "insert", "eject" or "remove"; read the slot state.
 * Without media, the drive remains present with an empty slot.
 *
 * Copyright (C) 2026 LycheeAPPF
 */
#ifndef CDJ2000_ATA_H
#define CDJ2000_ATA_H

#include "system/memory.h"
#include "hw/core/irq.h"

#define CDJ_ATA_BASE        0xfff00000
#define CDJ_ATA_SIZE        0x40
#define CDJ_ATA_CHIP_BASE   0xfff00080
#define CDJ_ATA_CHIP_SIZE   0x40

/* Attach the drive.  `irq` is the INTC line for INTEVT 0xc00. */
void cdj2000_ata_init(MemoryRegion *system, qemu_irq irq);

#endif /* CDJ2000_ATA_H */
