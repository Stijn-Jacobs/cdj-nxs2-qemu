/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CDJ-850 SCIF0 (debug console stub) and SCIF1 (panel link, xc/scif.c). */
#ifndef CDJ850_SCIF_H
#define CDJ850_SCIF_H
#include "cdj_common.h"

void cdj850_scif_init(MemoryRegion *sysmem, SuperHCPU *cpu);

/*
 * Panel-side view of SCIF1 (clock-synchronous slave; the panel CPU is the
 * clock master). Used by xc/panel_peer.c.
 *
 * tx_pending: bytes MAIN has queued in the TX FIFO (16 = a full frame).
 * tie:        SCSCR.TIE is set (MAIN is waiting for TXI1).
 * exchange:   clock one 16-byte frame. MAIN's queued TX bytes go to tx_out
 *             (zero padded), rx[] goes into the RX FIFO, TDFE/TEND become 1
 *             and TXI1 is raised if TIE is set.
 */
unsigned cdj850_scif1_tx_pending(void);
bool cdj850_scif1_tie(void);
void cdj850_scif1_exchange(const uint8_t rx[16], uint8_t tx_out[16]);

#endif
