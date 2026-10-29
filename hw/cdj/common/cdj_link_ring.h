/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_LINK_RING_H
#define CDJ_LINK_RING_H

#include "chardev/char.h"

/*
 * In a browser page the boards' SPI link runs through rings the page copies
 * between them. Returns a new chardev under id when the page asks for it
 * (CDJ_WASM_LINK=1), else NULL. A native build always gets NULL: there the
 * link is a socket chardev given on the command line.
 */
#ifdef __EMSCRIPTEN__
Chardev *cdj_link_ring_chardev(const char *id);
#else
static inline Chardev *cdj_link_ring_chardev(const char *id)
{
    return NULL;
}
#endif

#endif
