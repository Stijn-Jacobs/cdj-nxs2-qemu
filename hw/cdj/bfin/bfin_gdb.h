/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * GDB remote serial protocol session for the Blackfin core. Pure C over byte
 * callbacks: the host owns the transport and the run loop. It feeds received
 * bytes in, runs the core when bfin_gdb_run() says so and calls
 * bfin_gdb_stopped() when the core stops.
 */
#ifndef BFIN_GDB_H
#define BFIN_GDB_H

#include "bfin.h"
#include <stddef.h>

typedef struct bfin_gdb bfin_gdb;

typedef struct bfin_gdb_ops {
    void *opaque;
    /* Sends bytes to gdb. */
    void (*write)(void *opaque, const uint8_t *buf, size_t len);
    /* Memory for m/M/X; 0 or -1 when the range is not accessible. Optional:
     * without them only the RAM mapped into the core is reachable. */
    int (*read_mem)(void *opaque, uint32_t addr, uint8_t *buf, size_t len);
    int (*write_mem)(void *opaque, uint32_t addr, const uint8_t *buf,
                     size_t len);
} bfin_gdb_ops;

typedef enum { BFIN_GDB_HALT = 0, BFIN_GDB_CONT, BFIN_GDB_STEP } bfin_gdb_run;

bfin_gdb *bfin_gdb_new(bfin_core *core, const bfin_gdb_ops *ops);
void      bfin_gdb_free(bfin_gdb *g);

/* Bytes received from gdb. */
void bfin_gdb_feed(bfin_gdb *g, const uint8_t *buf, size_t len);

/* What gdb has asked the target to do since the last stop: HALT while the
 * core stays stopped, CONT or STEP once it resumes it. */
bfin_gdb_run bfin_gdb_run_request(const bfin_gdb *g);
/* Gdb sent the interrupt byte while the core was running. The host stops it
 * and calls bfin_gdb_stopped(g, BFIN_STOP_BUDGET). */
int bfin_gdb_interrupted(const bfin_gdb *g);
/* Gdb detached or killed: every breakpoint and watchpoint is gone and the
 * core should run freely; the host drops the connection. */
int bfin_gdb_detached(const bfin_gdb *g);

/* The core stopped: reports it to gdb (and ends the run request). BREAK is
 * swbreak, WATCH is watch/rwatch/awatch, STEP and IDLE end a step, UNDEF is
 * SIGILL and BUDGET means stopped on request (SIGINT). */
void bfin_gdb_stopped(bfin_gdb *g, bfin_stop reason);

#endif
