/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The SPI link between the two boards when both run in one browser page.
 * Natively it is a stream socket named "spilink"; here each board's heap holds
 * a transmit and a receive byte ring and the page copies one board's transmit
 * ring into the other board's receive ring. The rings carry exactly the
 * socket's bytes, so both boards' link code runs unchanged: a full transmit
 * ring is a short write, and the receive side hands the frontend only what
 * its can_receive window takes.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/timer.h"
#include "chardev/char.h"
#include "qapi/error.h"
#include "qom/object.h"
#include "cdj_link_ring.h"
#include <emscripten.h>

#define LINK_RING_SIZE (64 << 10)

/* The page reads head and tail as the first two words. Both count bytes
 * forever and wrap through the mask. */
typedef struct {
    uint32_t head;
    uint32_t tail;
    uint8_t data[LINK_RING_SIZE];
} LinkRing;

static LinkRing link_tx, link_rx;

EMSCRIPTEN_KEEPALIVE LinkRing *cdj_wasm_spi_tx(void)
{
    return &link_tx;
}

EMSCRIPTEN_KEEPALIVE LinkRing *cdj_wasm_spi_rx(void)
{
    return &link_rx;
}

EMSCRIPTEN_KEEPALIVE uint32_t cdj_wasm_spi_ring_size(void)
{
    return LINK_RING_SIZE;
}

#define TYPE_CHARDEV_CDJ_RING "chardev-cdj-ring"

typedef struct {
    Chardev parent;
    QEMUTimer *pump;
} CdjRingChardev;

DECLARE_INSTANCE_CHECKER(CdjRingChardev, CDJ_RING_CHARDEV, TYPE_CHARDEV_CDJ_RING)

static int cdj_ring_write(Chardev *chr, const uint8_t *buf, int len)
{
    uint32_t head = link_tx.head;
    uint32_t room = LINK_RING_SIZE - (head - qatomic_load_acquire(&link_tx.tail));
    uint32_t n = MIN((uint32_t)len, room);
    uint32_t at = head & (LINK_RING_SIZE - 1);
    uint32_t first = MIN(n, LINK_RING_SIZE - at);

    if (!n) {
        /* qemu_chr_fe_write hands MAIN a short write, which it holds and
         * retries; the display's qemu_chr_fe_write_all retries itself. */
        errno = EAGAIN;
        return -1;
    }
    memcpy(link_tx.data + at, buf, first);
    memcpy(link_tx.data, buf + first, n - first);
    qatomic_store_release(&link_tx.head, head + n);
    return n;
}

/* Main loop, every millisecond and whenever the frontend makes room. */
static void cdj_ring_pump(void *opaque)
{
    CdjRingChardev *d = opaque;
    Chardev *chr = CHARDEV(d);
    uint32_t tail = link_rx.tail;
    uint32_t head = qatomic_load_acquire(&link_rx.head);
    int room;

    while (head != tail && (room = qemu_chr_be_can_write(chr)) > 0) {
        uint32_t at = tail & (LINK_RING_SIZE - 1);
        uint32_t n = MIN(MIN(head - tail, LINK_RING_SIZE - at), (uint32_t)room);

        qemu_chr_be_write(chr, link_rx.data + at, n);
        tail += n;
        qatomic_store_release(&link_rx.tail, tail);
    }
    timer_mod(d->pump, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 1);
}

static void cdj_ring_accept_input(Chardev *chr)
{
    timer_mod(CDJ_RING_CHARDEV(chr)->pump, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
}

static bool cdj_ring_open(Chardev *chr, ChardevBackend *backend, Error **errp)
{
    CdjRingChardev *d = CDJ_RING_CHARDEV(chr);

    d->pump = timer_new_ms(QEMU_CLOCK_REALTIME, cdj_ring_pump, d);
    timer_mod(d->pump, qemu_clock_get_ms(QEMU_CLOCK_REALTIME));
    qemu_chr_be_event(chr, CHR_EVENT_OPENED);
    return true;
}

static void cdj_ring_class_init(ObjectClass *oc, const void *data)
{
    ChardevClass *cc = CHARDEV_CLASS(oc);

    cc->chr_open = cdj_ring_open;
    cc->chr_write = cdj_ring_write;
    cc->chr_accept_input = cdj_ring_accept_input;
}

static const TypeInfo cdj_ring_type = {
    .name = TYPE_CHARDEV_CDJ_RING,
    .parent = TYPE_CHARDEV,
    .instance_size = sizeof(CdjRingChardev),
    .class_init = cdj_ring_class_init,
};

static void cdj_ring_register(void)
{
    type_register_static(&cdj_ring_type);
}

type_init(cdj_ring_register);

/* A -chardev backend would need a new QAPI ChardevBackendKind, so the board
 * creates the ring itself. */
Chardev *cdj_link_ring_chardev(const char *id)
{
    const char *e = getenv("CDJ_WASM_LINK");

    if (!e || *e != '1') {
        return NULL;
    }
    return qemu_chardev_new(id, TYPE_CHARDEV_CDJ_RING, NULL, NULL, &error_fatal);
}
