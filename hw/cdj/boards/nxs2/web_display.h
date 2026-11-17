/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_WEB_DISPLAY_H
#define CDJ_WEB_DISPLAY_H

/*
 * In the browser build, with no MAIN on the other end of the spilink, the
 * page plays a recording of the MAIN link into a ring. Takes the next n bytes
 * into dst and returns n, or returns 0 while fewer are queued.
 */
uint32_t cdj_wasm_link_take(uint8_t *dst, uint32_t n);

#endif
