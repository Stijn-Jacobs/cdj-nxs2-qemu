/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 panel CPU as seen over SCIF1 (clock master peer, xc/panel_peer.c). */
#ifndef CDJ850_PANEL_PEER_H
#define CDJ850_PANEL_PEER_H
#include "cdj_common.h"

/* Env: CDJ850_PANEL=0 disables the peer, CDJ850_PANEL_PERIOD_MS=<n> sets the
 * idle gap between transactions (default 10). */
void cdj850_panel_peer_init(void);

/* Completed transactions (at least frames 1 and 2 exchanged). */
unsigned cdj850_panel_peer_transactions(void);
/* MAIN's TX frame n (0-2 = frame 1-3) from the most recent transaction. */
const uint8_t *cdj850_panel_peer_last_tx(unsigned n);

/* 8-bit end-around-carry sum of n bytes, as the firmware's 0x0C0F9EE8. */
uint8_t cdj850_panel_sum8ec(const uint8_t *p, unsigned n);
#endif
