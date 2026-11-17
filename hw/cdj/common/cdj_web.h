/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_WEB_H
#define CDJ_WEB_H

/* What the browser page and the boards hand each other (cdj_web.c). */

#define CDJ_WEB_STATE_FRAME 40

/* The next face key line the page queued, NUL-terminated into msg. */
bool cdj_web_panel_line(char *msg, size_t size);

/* MAIN's latest panel frame, for the page's lamps. */
void cdj_web_state_frame(const uint8_t *frame, unsigned len);

#endif
