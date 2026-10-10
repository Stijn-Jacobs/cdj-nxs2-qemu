/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ900_DISPLAY_M16C_H
#define CDJ900_DISPLAY_M16C_H
#include "cdj_common.h"

/* The M16C/63 display processor in its own window. Exits unless
 * CDJ_M16C_GUI names its flash image. */
bool cdj900_gui_init(void);

/* Runs the chip up to MAIN's clock; called before the chip-select wire
 * changes so the chip sees the old level for the time it was in force. */
void cdj900_gui_sync(void);

/* Provided by the board: the chip-select wire, true while the chip is not
 * selected. */
bool cdj900_gui_link_idle(void);

/* The chip's answer-valid output: high once it has accepted a frame from MAIN.
 * Runs the chip up to MAIN's clock first. */
bool cdj900_gui_answer_valid(void);

/* The chip select changed: the exchange so far is over (logged when
 * CDJ_M16C_LINK_LOG asks for it). */
void cdj900_gui_link_end(void);

/* One byte clocked over MAIN's link to it; returns the byte it shifts back. */
uint8_t cdj900_gui_link_byte(uint8_t tx);

#endif
