/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SH7724-family EtherMAC (E-DMAC + EtherC): the same register layout on the
 * CDJ-2000NXS2 (base 0x04600000) and the CDJ-2000/CDJ-2000NXS (base
 * 0xFEF00000, SH7763's fast EtherC) -- only the base differs.
 */
#ifndef CDJ_ETHER_H
#define CDJ_ETHER_H
#include "cdj_common.h"

/*
 * Maps the EtherMAC at base (MemoryRegion name = name) and attaches irq.
 * mdio_ta_default is this board's read-turnaround lead-in (see
 * cdj_ether_mdio_ta() in cdj_ether.c): 0 for the NXS2, 1 for the SH7763
 * boards. CDJ_ETHER_MDIO_TA still overrides either. The MAC address
 * override, netdev id and other debug knobs are environment variables
 * (CDJ_ETHER_*), so nothing else is board-specific.
 */
void cdj_ether_init(MemoryRegion *sysmem, const char *name, hwaddr base,
                    qemu_irq irq, unsigned mdio_ta_default);

#endif
