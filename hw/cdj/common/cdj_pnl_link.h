/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The front-panel MCU link: a synchronous, DMA-fed SCIF exchange, the same
 * register protocol on every CDJ board so far (CDJ-2000NXS2, CDJ-2000,
 * CDJ-2000NXS) -- only the frame length differs (40 bytes on the NXS2, 24 on
 * the others). MAIN drives a full-duplex exchange of that many bytes:
 * payload, an 8-bit end-around-carry checksum, a sync byte; the panel only
 * ever answers, it never sends unsolicited frames.
 *
 * This file is the register-level protocol and the generic key-schedule /
 * key-socket plumbing (CDJ_PANEL_PRESS, CDJ_PANEL_KEYSOCK, CDJ_PANEL_FILL,
 * CDJ_PANEL_REGLOG, CDJ_PANEL_DEBUG). A board's own fields -- the NXS2's
 * touch screen and tempo-slider default, a DSP-transport model reacting to
 * specific keys -- hook in through CdjPnlLinkHooks instead of being copied.
 *
 * Named "pnl_link", not "panel": the NXS2's host_display.c already has an
 * unrelated CdjPanelState/cdj_panel_init for the on-screen settings panel.
 */
#ifndef CDJ_PNL_LINK_H
#define CDJ_PNL_LINK_H
#include "cdj_common.h"

typedef struct CdjPnlLinkState CdjPnlLinkState;

typedef struct CdjPnlLinkHooks {
    /* A full frame just arrived from MAIN (frame_len bytes), before the
     * reply is built. */
    void (*on_frame)(CdjPnlLinkState *s, void *extra, const uint8_t *frame);
    /* Once per frame, before the reply payload is cleared. */
    void (*poll)(CdjPnlLinkState *s, void *extra, int64_t now);
    /* Right after the payload is cleared, before live 'lvl' overrides and
     * presses are laid in -- a board's own default field values. rx is
     * payload_len bytes. */
    void (*build_defaults)(CdjPnlLinkState *s, void *extra, uint8_t *rx);
    /* Last, after CDJ_PANEL_FILL, so it cannot be filled over and cannot
     * clobber a live 'lvl' override either. rx is payload_len bytes. */
    void (*build_last)(CdjPnlLinkState *s, void *extra, uint8_t *rx,
                       int64_t now);
    /* One key-socket datagram, before the generic "off:val:dur:op" parser.
     * True = fully handled, skip the generic parser (the NXS2's touch
     * messages, which would otherwise fail the offset range check). */
    bool (*drain_extra)(CdjPnlLinkState *s, void *extra, const char *msg,
                        int64_t now);
    /* An "or"/"hold" key was recognised at (off, mask), with the raw
     * duration from the message (0 if none was given) and whether it is a
     * hold; called before the press itself is scheduled. */
    void (*key_down)(CdjPnlLinkState *s, void *extra, unsigned off,
                     unsigned mask, unsigned dur_ms, bool held, int64_t now);
    /* A "rel" matched a previously-held press. */
    void (*key_up)(CdjPnlLinkState *s, void *extra, unsigned off,
                   unsigned mask, int64_t now);
    /* A second device on the same port (the CDJ-900's display processor):
     * while link_selected says it is addressed, each transmitted byte goes
     * to link_byte and the byte it returns is what MAIN reads back, one for
     * one, with no frame, checksum or panel reply around it. */
    bool (*link_selected)(void *extra);
    uint8_t (*link_byte)(void *extra, uint8_t tx);
    /* Extra exit-summary lines, printed after the generic counters. */
    void (*summary)(CdjPnlLinkState *s, void *extra);
} CdjPnlLinkHooks;

/*
 * Maps the panel at addr (and its P1/P2-stripped alias), frame_len bytes per
 * exchange (payload_len = frame_len - 2: an end-around-carry checksum byte,
 * then sync). hooks may be NULL for a board with nothing to add.
 *
 * Returns a zeroed, extra_size-byte block the board owns (NULL if
 * extra_size is 0): hooks are always called with this same pointer, so a
 * board keeps its own state there instead of a module-level global.
 */
void *cdj_pnl_link_init(MemoryRegion *sysmem, hwaddr addr, const char *name,
                        unsigned frame_len, uint8_t sync,
                        const CdjPnlLinkHooks *hooks, size_t extra_size);

/* Schedules a press the board generates itself (e.g. a synthetic key tied to
 * another model's state machine), same shape as a live "or" key send. */
void cdj_pnl_link_press(CdjPnlLinkState *s, unsigned off, uint8_t mask,
                        int64_t at_ms, int64_t dur_ms);

#endif
