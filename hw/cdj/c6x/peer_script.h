/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A scripted MAIN on the command link: queues requests one at a time the way
 * MAIN's DspSerialIF does (next request once the previous tag is answered),
 * ships uPP blocks when the DSP reports a receive armed, and records what the
 * DSP publishes. Offline test bench for the command link and the audio data
 * plane.
 */
#ifndef PEER_SCRIPT_H
#define PEER_SCRIPT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "peer_main.h"

typedef struct peer_script_io {
    void *opaque;
    uint64_t (*now_ns)(void *opaque);
    /* Bytes MAIN ships on CS6, which the FPGA forwards into uPP channel A. */
    void (*upp_ship)(void *opaque, const uint8_t *buf, size_t len);
} peer_script_io;

typedef struct peer_script peer_script;

peer_script *peer_script_new(const peer_script_io *io);
void         peer_script_free(peer_script *s);

/* The SPI seam: one word each way. */
uint16_t peer_script_spi(peer_script *s, uint16_t dsp_word);

/* Print the first n DSP frames raw. */
void peer_script_trace_frames(peer_script *s, unsigned n);

/* Script steps, run in order once peer_script_start() is called. */
void peer_script_request(peer_script *s, unsigned type, const uint32_t *args, unsigned nargs);
void peer_script_ship(peer_script *s, const uint8_t *buf, size_t len);   /* waits for flag 0x80 */
void peer_script_wait_ns(peer_script *s, uint64_t ns);
void peer_script_mark(peer_script *s, const char *label);
/* Append run `run` of a corpus file, frames with seq <= upto. Returns count. */
int  peer_script_corpus(peer_script *s, const char *path, unsigned run, unsigned upto);

/* Append the "MAIN frame N (k hw) at T ms: hw..." lines of a machine log, N <= upto. */
int  peer_script_mainlog(peer_script *s, const char *path, unsigned upto);

void peer_script_start(peer_script *s);
int  peer_script_done(const peer_script *s);

/* The reply words to the most recent request of `type` whose first index/arg
 * matched `arg0`, or NULL. */
const uint32_t *peer_script_last_reply(const peer_script *s, unsigned type, uint32_t arg0,
                                       unsigned *nwords);

void peer_script_report(const peer_script *s, FILE *f);

#endif
