/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
#include "cdj_pnl_link.h"
/*
 * SCIF2 (0xFFE20000): the front-panel MCU link. Same register layout and
 * DMA-fed exchange as the CDJ-2000NXS2's panel (cdj_panel.c, common/), but a
 * 24-byte frame instead of 40 and no touch screen (the CDJ-2000/NXS panel
 * has none).
 *
 * Init (0x04246DB6): SCSMR = 0x80 (clocked sync), SCBRR = 12, SCFCR = 6 then
 * 0x30. Exchange (0x04246E36, caller 0x0428FE74): full duplex, len 24, DMA
 * ch2 memory->SCFTDR (TCR 0x18, CHCR 0x40001800), ch1 SCFRDR->memory (TCR
 * 0x18, CHCR 0x40004800); both auto-request, so cdj_dmac_run() completes the
 * transfer synchronously on the CHCR DE store, same as the NXS2.
 *
 * Frame (validator 0x0428CDF8): [0x00..0x15] payload, [0x16] checksum
 * (8-bit sum with end-around carry), [0x17] sync = 0x8F. Keys are active
 * high, so an all-zero payload means nothing pressed. Which payload byte is
 * which key is not mapped yet (the firmware's own key-name table is at
 * 0x040A1330), so this model answers a clean idle frame plus whatever the
 * key socket or CDJ_PANEL_PRESS asks for, by raw offset -- both handled
 * generically by cdj_pnl_link_init().
 */
#define CDJ2000_PNL_FRAME 0x18
#define CDJ2000_PNL_SYNC  0x8F

void cdj2000_panel_init(MemoryRegion *sysmem, hwaddr addr)
{
    cdj_pnl_link_init(sysmem, addr, "sh7763.scif2-panel", CDJ2000_PNL_FRAME,
                      CDJ2000_PNL_SYNC, NULL, 0);
}
