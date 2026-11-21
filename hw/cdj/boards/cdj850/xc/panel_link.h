/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 MAIN<->panel link model (panel CPU side).
 *
 * The panel CPU (service manual IC1007, no firmware on file) is not
 * emulated; this file is the MAIN-side contract for what the peer must do,
 * derived only from MAIN's own driver code (see the implementation notes).
 * The wire itself is SSU channel 0 at 0xFFFE7000 (xc/ser.c); this file
 * decides the bytes that come back and keeps what MAIN sends.
 */
#ifndef CDJ850_PANEL_LINK_H
#define CDJ850_PANEL_LINK_H
#include "cdj_common.h"

/* Next panel->MAIN byte for an SSU receive slot (see panel_link.c for the
 * per-byte justification). */
uint8_t panel_ssu_rx_byte(void);

/* Marks the start of a MAIN-initiated exchange (SSER RE set): the reply
 * frame restarts at byte 0. */
void panel_exchange_start(void);

/* Task 8k: parse the hold-at-reset frame override (see panel_link.c).
 * Called once from board init; default (unset/invalid) is the idle frame. */
void panel_link_init(void);

/* One MAIN->panel byte, for the FL/lamp frame capture (vfd.c). */
void panel_ssu_tx_byte(uint8_t b);

/* Task 9 hook: queue a front-panel key press for the key-report exchange.
 * Recorded; consumed once the key-scan reply format is mapped. */
void panel_send_key(uint8_t code);

#endif
