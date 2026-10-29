/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the browser page reads from the display board beyond common/cdj_web.c:
 * the console surface the LCD refresh draws into (800x480, 32 bpp); and the
 * ring the page plays a recorded MAIN link into when no MAIN runs beside it.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "web_display.h"
#include <emscripten.h>

static DisplaySurface *gui_surface(void)
{
    QemuConsole *con = qemu_console_lookup_by_index(0);

    return con ? qemu_console_surface(con) : NULL;
}

EMSCRIPTEN_KEEPALIVE uint32_t *cdj_wasm_surface(void)
{
    DisplaySurface *s = gui_surface();

    return s ? surface_data(s) : NULL;
}

EMSCRIPTEN_KEEPALIVE int cdj_wasm_width(void)
{
    DisplaySurface *s = gui_surface();

    return s ? surface_width(s) : 0;
}

EMSCRIPTEN_KEEPALIVE int cdj_wasm_height(void)
{
    DisplaySurface *s = gui_surface();

    return s ? surface_height(s) : 0;
}

/*
 * The page writes recorded link bytes at head and publishes head with
 * Atomics.store; the receive DMA takes from tail. Both count bytes forever and
 * wrap through the mask, so the page sees the room as size - (head - tail).
 */
#define LINK_RING_SIZE (4 << 20)

static uint8_t link_ring[LINK_RING_SIZE];
static uint32_t link_head, link_tail;
static double link_first_ms = -1;

EMSCRIPTEN_KEEPALIVE uint8_t *cdj_wasm_link_ring(void)
{
    return link_ring;
}

EMSCRIPTEN_KEEPALIVE uint32_t cdj_wasm_link_ring_size(void)
{
    return LINK_RING_SIZE;
}

EMSCRIPTEN_KEEPALIVE uint32_t *cdj_wasm_link_head(void)
{
    return &link_head;
}

EMSCRIPTEN_KEEPALIVE uint32_t *cdj_wasm_link_tail(void)
{
    return &link_tail;
}

/* Guest ms of the first receive: the recording's time zero. */
EMSCRIPTEN_KEEPALIVE double cdj_wasm_link_first_ms(void)
{
    return link_first_ms;
}

/* All n bytes or none, so a frame never splits across two receives. */
uint32_t cdj_wasm_link_take(uint8_t *dst, uint32_t n)
{
    uint32_t tail = link_tail;
    uint32_t head = qatomic_load_acquire(&link_head);
    uint32_t at = tail & (LINK_RING_SIZE - 1);
    uint32_t first = MIN(n, LINK_RING_SIZE - at);

    if (link_first_ms < 0) {
        link_first_ms = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e6;
    }
    if (head - tail < n) {
        return 0;
    }
    memcpy(dst, link_ring + at, first);
    memcpy(dst + first, link_ring, n - first);
    qatomic_store_release(&link_tail, tail + n);
    return n;
}
