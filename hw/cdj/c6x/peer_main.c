/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "peer_main.h"

#include <stdlib.h>
#include <string.h>

enum { QUEUE_DEPTH = 64 };

struct peer_main {
    peer_frame_fn on_frame;
    void         *opaque;
    uint16_t      rx[PEER_FRAME_HW];
    uint16_t      tx[PEER_FRAME_HW];
    unsigned      pos;
    uint16_t      queue[QUEUE_DEPTH][PEER_FRAME_HW];
    unsigned      qhead, qlen;
    uint64_t      exchanges, bad, bad_trailer;
};

/* The builder's region-1 sizes, 0x00802F88. */
unsigned peer_status_hw(unsigned kind)
{
    static const unsigned hw[] = { 0, 32, 42, 52, 38, 4, 20 };
    return kind < sizeof hw / sizeof hw[0] ? hw[kind] : 0;
}

void peer_decode(const uint16_t *hw, peer_dsp_frame *f)
{
    memcpy(f->hw, hw, sizeof f->hw);
    f->valid = hw[0] == 0x5533;
    f->trailer_ok = hw[PEER_FRAME_HW - 1] == 0xAACC;
    f->kind = hw[1] & 0x3F;
    f->flags = hw[1] & 0xC0;
    f->len2 = hw[1] >> 8;
    unsigned st = peer_status_hw(f->kind);
    f->status = f->hw + 2;
    f->reply = f->len2 && 2 + st + f->len2 <= PEER_FRAME_HW - 1 ? f->hw + 2 + st : NULL;
}

unsigned peer_build_request(uint16_t *out, unsigned type, unsigned tag,
                            const uint32_t *args, unsigned nargs,
                            const uint16_t *idblock)
{
    unsigned n = 0, len = 2 + 2 * nargs;
    out[n++] = 0x5533;
    out[n++] = (uint16_t)(len << 8 | (idblock ? 1 : 0));
    if (idblock)
        for (unsigned i = 0; i < PEER_IDBLOCK_HW; i++)
            out[n++] = idblock[i];
    out[n++] = type;
    out[n++] = tag;
    for (unsigned i = 0; i < nargs; i++) {
        out[n++] = args[i] & 0xFFFF;
        out[n++] = args[i] >> 16;
    }
    out[n++] = 0x0000;
    out[n++] = 0xAACC;
    return n;
}

peer_main *peer_main_new(peer_frame_fn on_frame, void *opaque)
{
    peer_main *p = calloc(1, sizeof *p);
    p->on_frame = on_frame;
    p->opaque = opaque;
    return p;
}

void peer_main_free(peer_main *p)
{
    free(p);
}

int peer_main_queue(peer_main *p, const uint16_t *hw, unsigned n)
{
    if (p->qlen == QUEUE_DEPTH || n > PEER_FRAME_HW)
        return 0;
    uint16_t *slot = p->queue[(p->qhead + p->qlen) % QUEUE_DEPTH];
    memset(slot, 0, sizeof p->queue[0]);
    memcpy(slot, hw, n * sizeof *hw);
    p->qlen++;
    return 1;
}

unsigned peer_main_queued(const peer_main *p)
{
    return p->qlen;
}

uint16_t peer_main_xfer(peer_main *p, uint16_t dsp_word)
{
    if (p->pos == 0) {
        /* MAIN's TX FIFO holds zeros when nothing is pending; the DSP's parser
         * scans the window for 0x5533, so an idle exchange is all zero. */
        if (p->qlen) {
            memcpy(p->tx, p->queue[p->qhead], sizeof p->tx);
            p->qhead = (p->qhead + 1) % QUEUE_DEPTH;
            p->qlen--;
        } else {
            memset(p->tx, 0, sizeof p->tx);
        }
    }
    uint16_t out = p->tx[p->pos];
    p->rx[p->pos++] = dsp_word;
    if (p->pos == PEER_FRAME_HW) {
        p->pos = 0;
        p->exchanges++;
        peer_dsp_frame f;
        peer_decode(p->rx, &f);
        if (!f.valid)
            p->bad++;
        else if (!f.trailer_ok)
            p->bad_trailer++;
        if (p->on_frame)
            p->on_frame(p->opaque, &f);
    }
    return out;
}

uint64_t peer_main_exchanges(const peer_main *p)
{
    return p->exchanges;
}

uint64_t peer_main_bad_frames(const peer_main *p)
{
    return p->bad;
}

uint64_t peer_main_bad_trailers(const peer_main *p)
{
    return p->bad_trailer;
}
