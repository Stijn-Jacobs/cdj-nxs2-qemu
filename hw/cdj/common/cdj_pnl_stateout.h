/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_PNL_STATEOUT_H
#define CDJ_PNL_STATEOUT_H
#include "cdj_common.h"

/* Publish MAIN's latest frame to the panel MCU as "pnl <hex>" on the state
 * socket (CDJ_PANEL_STATESOCK, or CDJ_PANEL_KEYSOCK with "panel-keys" read
 * as "panel-state"). */
void cdj_pnl_stateout(const uint8_t *frame, unsigned len);

#endif
