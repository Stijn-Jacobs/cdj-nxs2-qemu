/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 panel CPU as seen over SCIF1 (clock master peer, xc/panel_peer.c). */
#ifndef CDJ850_PANEL_PEER_H
#define CDJ850_PANEL_PEER_H
#include "cdj_common.h"

/* Env: CDJ850_PANEL=0 disables the peer, CDJ850_PANEL_PERIOD_MS=<n> sets the
 * idle gap between transactions (default 10), CDJ850_PANEL_SCRIPT schedules
 * inputs (<ms>:<btn>[:<hold_ms>], <ms>:+<btn>, <ms>:-<btn>, <ms>:rot+/-N,
 * <ms>:fader=<raw 0-1023>).
 * CDJ850_SCIF1_CAPTURE=<new path> records MAIN TX in closed windows. */
void cdj850_panel_peer_init(void);

/* Invalidate the status readouts and discard pre-reset status windows for them. */
void cdj850_panel_peer_reset_readouts(void);

/* Completed transactions (at least frames 1 and 2 exchanged). */
unsigned cdj850_panel_peer_transactions(void);
/* MAIN's TX frame n (0-2 = frame 1-3) from the most recent transaction. */
const uint8_t *cdj850_panel_peer_last_tx(unsigned n);

/* 8-bit end-around-carry sum of n bytes, as the firmware's 0x0C0F9EE8. */
uint8_t cdj850_panel_sum8ec(const uint8_t *p, unsigned n);

/* Programmatic panel controls (buttons and rotary encoder). */
bool cdj850_panel_peer_set_button(const char *name, bool pressed);
void cdj850_panel_peer_rotate(int delta);
uint16_t cdj850_panel_peer_rotary(void);

/* Build frame 0 (type 1) or 1 (type 2) using current input state. */
void cdj850_panel_peer_build_rx(unsigned frame, uint8_t f[16]);

/* Decoded 3-line display state from MAIN->panel 0x80..0xAE frames. */
uint8_t cdj850_panel_peer_display_mode(void);
const char *cdj850_panel_peer_display_line(unsigned row);
uint8_t cdj850_panel_peer_display_attr(unsigned row);
unsigned cdj850_panel_peer_display_updates(void);

/* Feed a raw 16-byte MAIN->panel frame into the display decoder
 * (used internally at transaction completion and by unit tests). */
void cdj850_panel_peer_feed_display_tx(const uint8_t tx[16]);
#endif
