/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Tracepoints against trace_prog.s: records (order, pc, R0-R2, cycles), a
 * zero-tracepoint run that matches, bfin_stepi, a hardware loop, ring full.
 */
#include "bfin_priv.h"
#include "trace_prog.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t ram[0x10000];
static int failures;

#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            printf("FAIL line %d: ", __LINE__);                         \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
            failures++;                                                 \
        }                                                               \
    } while (0)

static uint32_t bus_read(void *o, uint32_t a, unsigned s) { return 0; }
static void bus_write(void *o, uint32_t a, uint32_t v, unsigned s) { }

static bfin_core *fresh(uint32_t ring)
{
    static const bfin_bus bus = { NULL, bus_read, bus_write };
    bfin_core *c = bfin_new(&bus);

    memset(ram, 0, sizeof ram);
    memcpy(ram, trace_prog, sizeof trace_prog);
    bfin_map_ram(c, 0, sizeof ram, ram);
    bfin_reset(c, START);
    if (ring) {
        CHECK(bfin_trace_ring(c, ring) == 0, "ring size");
    }
    return c;
}

static void add3(bfin_core *c)
{
    CHECK(bfin_trace_add(c, F, 1) == 0, "add F");
    CHECK(bfin_trace_add(c, G, 2) == 0, "add G");
    CHECK(bfin_trace_add(c, LP_TOP, 3) == 0, "add LP_TOP");
}

static void run_step(bfin_core *c)
{
    uint64_t n;

    bfin_step(c, 500, &n);
}

static void run_stepi(bfin_core *c)
{
    for (int i = 0; i < 40; i++) {
        bfin_stepi(c);
    }
}

static void check_recs(bfin_core *c, uint64_t *cyc)
{
    static const struct { uint16_t id; uint32_t pc, r0, r1, r2; } want[] = {
        { 1, F,      1, 2, 3 },
        { 2, G,      10, 20, 3 },
        { 3, LP_TOP, 7, 20, 3 },
        { 3, LP_TOP, 8, 21, 3 },
        { 3, LP_TOP, 9, 22, 3 },
    };
    bfin_trace_rec r[16];
    size_t n = bfin_trace_drain(c, r, 16);

    CHECK(n == 5, "got %zu records, want 5", n);
    for (size_t i = 0; i < n && i < 5; i++) {
        CHECK(r[i].id == want[i].id && r[i].pc == want[i].pc &&
              r[i].r0 == want[i].r0 && r[i].r1 == want[i].r1 &&
              r[i].r2 == want[i].r2,
              "rec %zu: id %u pc %x r0 %u r1 %u r2 %u", i, r[i].id, r[i].pc,
              r[i].r0, r[i].r1, r[i].r2);
        CHECK(i == 0 || r[i].cycles > r[i - 1].cycles, "cycles not rising");
        cyc[i] = r[i].cycles;
    }
    CHECK(bfin_trace_lost(c) == 0, "lost %llu",
          (unsigned long long)bfin_trace_lost(c));
    CHECK(bfin_trace_drain(c, r, 16) == 0, "drain empties the ring");
}

int main(void)
{
    uint64_t cyc_a[5] = { 0 }, cyc_b[5] = { 0 };
    bfin_trace_rec r[8];
    bfin_core *c;
    uint32_t r0, r1;

    /* No tracepoints: nothing recorded, final state is the reference. */
    c = fresh(0);
    run_step(c);
    CHECK(bfin_trace_drain(c, r, 8) == 0 && bfin_trace_lost(c) == 0,
          "records with no tracepoints");
    r0 = c->r[0];
    r1 = c->r[1];
    CHECK(r0 == 10 && r1 == 23, "reference run r0 %u r1 %u", r0, r1);
    bfin_free(c);

    /* Tracepoints change nothing the program computes. */
    c = fresh(0);
    add3(c);
    run_step(c);
    CHECK(c->r[0] == r0 && c->r[1] == r1, "state differs with tracepoints");
    check_recs(c, cyc_a);
    bfin_free(c);

    /* Single-stepping sees the same hits at the same cycles. */
    c = fresh(0);
    add3(c);
    run_stepi(c);
    CHECK(c->r[0] == r0 && c->r[1] == r1, "stepi state differs");
    check_recs(c, cyc_b);
    CHECK(!memcmp(cyc_a, cyc_b, sizeof cyc_a), "cycles differ run vs stepi");

    /* Clearing stops recording; re-adding after run flushes cached blocks. */
    bfin_trace_clear(c);
    bfin_reset(c, START);
    run_step(c);
    CHECK(bfin_trace_drain(c, r, 8) == 0, "records after clear");
    bfin_trace_add(c, F, 9);
    bfin_reset(c, START);
    run_step(c);
    CHECK(bfin_trace_drain(c, r, 8) == 1 && r[0].id == 9, "re-add");
    CHECK(bfin_trace_add(c, 1, 0) == -1, "odd address accepted");
    for (uint32_t a = 0x100; a < 0x100 + 2 * BFIN_MAX_TRACE; a += 2) {
        if (bfin_trace_add(c, a, 0) < 0) {
            CHECK(c->ntp == BFIN_MAX_TRACE, "table full at %d", c->ntp);
            break;
        }
    }
    bfin_free(c);

    /* A ring of 2 keeps the first two, counts the other three, and the
     * program still runs to the end. */
    c = fresh(2);
    add3(c);
    run_step(c);
    CHECK(c->r[0] == r0 && c->r[1] == r1, "state differs with full ring");
    CHECK(bfin_trace_lost(c) == 3, "lost %llu, want 3",
          (unsigned long long)bfin_trace_lost(c));
    CHECK(bfin_trace_drain(c, r, 8) == 2 && r[0].id == 1 && r[1].id == 2,
          "full ring kept the wrong records");
    bfin_free(c);

    printf(failures ? "trace_test: %d FAILED\n" : "trace_test: ok\n", failures);
    return failures != 0;
}
