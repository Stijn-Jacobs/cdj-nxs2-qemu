/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MAIN's end of the DSP command link, as a word-level SPI slave.
 *
 * The C6655 is SPI master on CS0 and clocks 64 halfwords per exchange in both
 * directions. This peer returns MAIN's frame one
 * word per DSP word, collects the DSP's 64 words into a frame, and decodes it.
 * It has no QEMU or core dependency: the offline runner and the machine device
 * both feed it from their spi_xfer seam.
 *
 * Frame formats (little-endian 16-bit units):
 *   MAIN -> DSP  0x5533, (L << 8) | variant, [8 hw id-block if variant],
 *                type, tag, args as 2 hw each (L = 2 + 2*nargs), 0x0000, 0xAACC
 *   DSP -> MAIN  0x5533, ((len2 << 8) | flags | kind), status vector of `kind`,
 *                len2 hw reply message, zero fill, 0x0000, 0xAACC at index 63
 */
#ifndef PEER_MAIN_H
#define PEER_MAIN_H

#include <stdint.h>

enum { PEER_FRAME_HW = 64, PEER_IDBLOCK_HW = 8, PEER_MAX_ARGS = 29 };

/* Status-vector halfwords the DSP's builder places before the reply, by kind. */
unsigned peer_status_hw(unsigned kind);

typedef struct peer_dsp_frame {
    uint16_t hw[PEER_FRAME_HW];
    int      valid;            /* 0x5533 at 0                                    */
    int      trailer_ok;       /* 0xAACC at 63, which MAIN's driver requires     */
    unsigned kind;             /* byte2 & 0x3F: MAIN's reply id                  */
    unsigned flags;            /* byte2 & 0xC0: bit 7 = uPP receive armed        */
    unsigned len2;             /* byte3: reply-message halfwords                 */
    const uint16_t *status;    /* peer_status_hw(kind) halfwords                 */
    const uint16_t *reply;     /* len2 halfwords, or NULL                        */
} peer_dsp_frame;

void peer_decode(const uint16_t *hw, peer_dsp_frame *out);

/* Build a MAIN request. idblock may be NULL (variant 0). Returns halfwords. */
unsigned peer_build_request(uint16_t *out, unsigned type, unsigned tag,
                            const uint32_t *args, unsigned nargs,
                            const uint16_t *idblock);

typedef struct peer_main peer_main;
typedef void (*peer_frame_fn)(void *opaque, const peer_dsp_frame *f);

peer_main *peer_main_new(peer_frame_fn on_frame, void *opaque);
void       peer_main_free(peer_main *p);

/* Queue a MAIN frame for a later exchange (at most one per exchange, the way
 * MAIN's frame driver sends). Returns 0 if the queue is full. */
int      peer_main_queue(peer_main *p, const uint16_t *hw, unsigned n);
unsigned peer_main_queued(const peer_main *p);

/* One SPI word, full duplex: the DSP's word in, MAIN's word back. */
uint16_t peer_main_xfer(peer_main *p, uint16_t dsp_word);

/* Exchanges completed, ones without the 0x5533 header, and ones whose trailer
 * MAIN's frame driver would reject. */
uint64_t peer_main_exchanges(const peer_main *p);
uint64_t peer_main_bad_frames(const peer_main *p);
uint64_t peer_main_bad_trailers(const peer_main *p);

#endif
