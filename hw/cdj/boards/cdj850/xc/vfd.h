/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 DATA FL display reconstruction. */
#ifndef CDJ850_VFD_H
#define CDJ850_VFD_H
#include "cdj_common.h"

/* Create the deck window. Safe headless (no window without a display). */
void vfd_init(void);

/* Legacy diagnostic accessor: two summary strings (not rendered content).
 * Always NUL-terminated; callers provide the buffers. */
void vfd_text(char *line0, unsigned n0, char *line1, unsigned n1);

/* Legacy DSP diagnostic stream; does not supply visible display content. */
void vfd_note_tx(uint8_t b);

/* Fed by panel_peer.c when MAIN->panel SCIF1 frames decode 3 display lines. */
void vfd_note_panel_text(uint8_t mode, const char *l0, const char *l1,
                         const char *l2);

/* Selection tags decoded from the SCIF1 text records (bit 5 highlights). */
void vfd_note_panel_tags(const uint8_t tags[3]);

/* Legacy SSU0 DSP diagnostic framing; not a display protocol. */
#define VFD_FRAME_LEN 6
#define VFD_TX0_VALID 0x80
#endif
