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
 * high, so an all-zero payload means nothing pressed. The decoder at
 * 0x0428E1D6 turns payload bits into one state bit per key ID of the
 * firmware's key-name table (0x0405C44C); the ones that matter for browsing:
 *
 *   RotaryPush 0x11/0x01   DevLink 0x13/0x01  DevUSB 0x13/0x02
 *   DevSD 0x13/0x04        DevDisc 0x13/0x08  Browse 0x14/0x01
 *   Prepare 0x14/0x02      Information 0x14/0x04  MenuUtility 0x14/0x08
 *   Return 0x14/0x10       AddPrepare 0x14/0x20 (0x10 and 0x20 swap on a
 *                          deck whose model word at 0x04C06FA4+0x1674 is <= 9)
 *   PlayPause 0x10/0x01    Cue 0x10/0x02      TrackRev 0x12/0x02
 *
 * This model answers an idle frame plus whatever the key socket or
 * CDJ_PANEL_PRESS asks for, by raw offset -- both handled generically by
 * cdj_pnl_link_init().
 *
 * The idle frame is not all zero on the CDJ-2000: 0x0F/0x02 is the DIRECTION
 * lever (key ID 1, "DirectionRev" in the key-name table at 0x0405C440) and
 * it reads 1 in the FWD position. The input handler at 0x0426945A sets the
 * reverse flag 0x04FDC21A when the bit is clear, every PLAY command carries
 * that flag (0x04270FDC, 0x0428237E) and it reaches the DSP as bit 31 of
 * the host-port window word 0x0C0C7BC4. With the bit clear a loaded track
 * plays backwards, which at its first frame looks like a deck that never
 * starts; with it set the playhead runs forward and REMAIN counts down, and
 * clearing it mid-track turns the playhead round. The CDJ-2000NXS decodes
 * the same report bit to the same key ID.
 *
 * The lever is a position, not a key, so a press or hold of 0x0F/0x02 on
 * the key socket puts it in REV for as long as it lasts: the bit is
 * inverted after the presses are laid in, and reads 1 (FWD) otherwise.
 */
#define CDJ2000_PNL_FRAME 0x18
#define CDJ2000_PNL_SYNC  0x8F

static void cdj2000_panel_lever(CdjPnlLinkState *s, void *extra,
                                uint8_t *rx, int64_t now)
{
    rx[0x0F] ^= 0x02;
}

void cdj2000_panel_init(MemoryRegion *sysmem, hwaddr addr)
{
    static const CdjPnlLinkHooks hooks = {
        .build_last = cdj2000_panel_lever,
    };

    cdj_pnl_link_init(sysmem, addr, "sh7763.scif2-panel", CDJ2000_PNL_FRAME,
                      CDJ2000_PNL_SYNC, &hooks, 0);
}
