/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Touch screen of the older decks that have one (XDJ-1000, XDJ-1000MK2,
 * XDJ-700). The panel MCU digitises the resistive panel and sends two raw
 * 10-bit values in the panel report, which MAIN filters and calibrates to an
 * 800x480 pixel position; this model writes those two values into the report
 * from the host pointer or from a key-socket "<x>:<y>:<ms>:tap" message.
 */
#ifndef CDJ_PNL_TOUCH_H
#define CDJ_PNL_TOUCH_H
#include "cdj_common.h"
#include "cdj_panelkeys.h"

typedef struct CdjPnlTouch {
    bool on;                        /* CDJ_TOUCH is not 0                     */
    CdjTouch host;                  /* latest message, LCD pixels             */
    uint16_t rx, ry;                /* host position in raw panel units       */
    bool want_down;                 /* the host's finger is down              */
    bool latched;                   /* a press not yet reported to the guest  */
    int64_t tap_until_ms;           /* timed tap: release at; 0 = none        */
    bool down;                      /* what the report is carrying            */
    unsigned down_frames, up_frames;
    unsigned min_frames, gap_frames;
    uint64_t presses;
} CdjPnlTouch;

void cdj_pnl_touch_init(CdjPnlTouch *t);

/* One key-socket datagram; true if it was a touch message. */
bool cdj_pnl_touch_drain(CdjPnlTouch *t, const char *msg, int64_t now);

/* Lay the current touch into the report, once per frame, after everything
 * else so a fill or an analogue override cannot clobber it. */
void cdj_pnl_touch_apply(CdjPnlTouch *t, uint8_t *rx, int64_t now);

void cdj_pnl_touch_summary(const CdjPnlTouch *t);

/* Makes the console window's left button the finger. Registering the
 * absolute pointer also stops the host grabbing the mouse. */
void cdj_pnl_touch_pointer_init(CdjPnlTouch *t);

#endif /* CDJ_PNL_TOUCH_H */
