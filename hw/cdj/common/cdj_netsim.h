/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pioneer CDJ-2000: two or more players on one Pro DJ Link segment, in one
 * guest time (see cdj_netsim.c).
 */
#ifndef CDJ_NETSIM_H
#define CDJ_NETSIM_H

/* An Ethernet frame from the segment, handed over at the guest time it
   reaches this player. */
typedef void (*CdjNetsimFrameFn)(void *opaque, const uint8_t *frame,
                                 unsigned len);

/* Requires -icount with sleep=off, or CDJ_NETSIM_CLOCK=wall and a fresh
   CDJ_NETSIM_CLOCK_FILE shared by local decks (without -icount).
   CDJ_NETSIM=HOST:PORT or unix:PATH (a synchronising hub) turns it on;
   false, and nothing else happens, without it. */
bool cdj_netsim_init(CdjNetsimFrameFn sink, void *opaque);
bool cdj_netsim_active(void);

/* A frame this player sent, stamped with the guest time it left. */
void cdj_netsim_send(const uint8_t *frame, unsigned len);

#endif
