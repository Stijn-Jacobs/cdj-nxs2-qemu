/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 VFD text display (deck window, text, not pixels). */
#ifndef CDJ850_VFD_H
#define CDJ850_VFD_H
#include "cdj_common.h"

/* Create the deck window. Safe headless (no window without a display). */
void vfd_init(void);

/* Accessor: the two display strings (DATA FL line, JOG FL ring state).
 * Always NUL-terminated; callers provide the buffers. */
void vfd_text(char *line0, unsigned n0, char *line1, unsigned n1);

/* Fed by panel_link.c for every MAIN->panel byte (FL frame capture). */
void vfd_note_tx(uint8_t b);

/* Panel display frame format (Task VFD-1; full evidence trail with builder
 * PCs in notes/cdj850-bringup.md).
 *
 * The panel link carries binary control frames, not text: the panel CPU
 * (IC1007, own firmware) renders DATA/JOG FL glass content from these, so
 * MAIN sends indices only and this window passes them through.
 * - 6-byte exchange frame (full duplex with the 6-byte status reply):
 *   TX staging at MAIN RAM 0x0C7F671B, assembled by the TX-prep helper
 *   (0x0C005D54) and the per-task builders (cddabuff 0x0C005F04).
 *   TX[0] = flag byte, bit 7 always set (bset #7), bits 6/5/4/1 from
 *   task state, low bits from the mode byte; TX[1..2] = state bytes;
 *   TX[3..5] = a 24-bit state value, big-endian. Parked idle frame:
 *   C2 00 00 08 00 00 (RX answers 00 00 00 01 01 01).
 * - 16-bit command words (driver 0x0C07800A, HI byte to SSTDR0 first):
 *   boot 0x1604/0x10FF/0x11FF, then per-poll 0x1300 (0x1310 when the
 *   object-table bit is set; builder 0x0C006386).
 * - Boot upload (driver 0x0C077B06): 1140 binary panel-init bytes, no text.
 *
 * No MAIN-resident font reaches the wire (the message/charset tables at
 * 0x0C1544C8+ have no code path to any SSU TX site), so bytes outside the
 * hex passthrough have no glyph mapping: they render via the frame sync
 * below, never as '?'-substituted text. The '?' + log-once glyph policy
 * in vfd.c stays in place for the DisplayTask-text future.
 */
#define VFD_FRAME_LEN   6
/* TX[0] valid/start bit, set unconditionally by the TX-prep helper. */
#define VFD_TX0_VALID   0x80

#endif
