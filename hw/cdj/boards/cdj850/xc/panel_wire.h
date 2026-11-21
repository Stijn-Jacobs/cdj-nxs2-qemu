/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ850_PANEL_WIRE_H
#define CDJ850_PANEL_WIRE_H

#define PANEL_FRAME_LEN          16
#define PANEL_TX_MAX             64

/* One line per closed window, T <seq> <start ns> <end ns> <frame count>
 * followed by <frame ns>:<32 lowercase hex digits> for each MAIN TX frame. */
#define PANEL_CAPTURE_MAGIC      "CDJ850-SCIF1 1\n"
#define PANEL_CAPTURE_LIMIT      "LIMIT\n"
#define PANEL_CAPTURE_LINE_MAX   (80 + PANEL_TX_MAX * 55)
#define PANEL_CAPTURE_MAX_BYTES  (8 * 1024 * 1024)

#endif
