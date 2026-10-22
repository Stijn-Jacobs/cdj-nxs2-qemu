/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The gdb remote session against a bfin_core running a small program
 * (gdb_prog.s): registers, memory, breakpoints, single-step, watchpoints,
 * table limits and detach. The test plays the host's run loop.
 */
#include "bfin_priv.h"
#include "bfin_gdb.h"
#include "gdb_prog.h"
#include <stdio.h>
#include <stdlib.h>

#define DATA 0x2000u

static uint8_t ram[0x10000];
static int failures;
static bfin_core *core;
static bfin_gdb *gdb;
static char outbuf[1 << 16];
static size_t outlen;

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

static void sink(void *o, const uint8_t *b, size_t n)
{
    memcpy(outbuf + outlen, b, n);
    outlen += n;
    outbuf[outlen] = 0;
}

/* The host's side: runs what gdb asked for until the core stops. */
static void drive(void)
{
    bfin_gdb_run r = bfin_gdb_run_request(gdb);
    bfin_stop st;

    if (r == BFIN_GDB_STEP) {
        bfin_gdb_stopped(gdb, bfin_stepi(core));
    } else if (r == BFIN_GDB_CONT) {
        uint64_t n;

        if (bfin_gdb_detached(gdb)) {
            return;
        }
        st = bfin_step(core, 1000, &n);
        bfin_gdb_stopped(gdb, st == BFIN_STOP_BUDGET ? BFIN_STOP_BUDGET : st);
    }
}

/* Sends one packet, drives the core, returns the payload of the last reply
 * (valid until the next call). */
static const char *xfer(const char *payload)
{
    static char reply[1 << 15];
    char frame[1 << 15];
    unsigned sum = 0;
    char *end;

    for (const char *p = payload; *p; p++) {
        sum += (uint8_t)*p;
    }
    snprintf(frame, sizeof frame, "$%s#%02x", payload, sum & 0xFF);
    outlen = 0;
    outbuf[0] = 0;
    bfin_gdb_feed(gdb, (const uint8_t *)frame, strlen(frame));
    drive();
    end = strrchr(outbuf, '#');
    if (!end) {
        reply[0] = 0;
        return reply;
    }
    *end = 0;
    {
        char *start = strrchr(outbuf, '$');

        snprintf(reply, sizeof reply, "%s", start ? start + 1 : "");
    }
    return reply;
}

static void restart(void)
{
    memcpy(ram, gdb_prog, sizeof gdb_prog);
    memset(ram + DATA, 0, 16);
    bfin_reset(core, START);
    core->code_gen++;
}

static void setup(void)
{
    static const bfin_bus bus = { NULL, bus_read, bus_write };
    bfin_gdb_ops ops = { .write = sink };

    if (gdb) {
        bfin_gdb_free(gdb);
        bfin_free(core);
    }
    core = bfin_new(&bus);
    bfin_map_ram(core, 0, sizeof ram, ram);
    gdb = bfin_gdb_new(core, &ops);
    restart();
}

static uint32_t le(const char *hex)
{
    uint32_t v = 0;

    for (int i = 0; i < 4; i++) {
        unsigned b;

        sscanf(hex + 2 * i, "%2x", &b);
        v |= b << (8 * i);
    }
    return v;
}

static void test_regs(void)
{
    static const struct { int n; unsigned grp, reg; } map[] = {
        { 0, 0, 0 }, { 7, 0, 7 }, { 8, 1, 0 }, { 14, 1, 6 }, { 15, 1, 7 },
        { 16, 2, 0 }, { 20, 2, 4 }, { 24, 3, 0 }, { 28, 3, 4 }, { 32, 4, 0 },
        { 33, 4, 1 }, { 34, 4, 2 }, { 35, 4, 3 }, { 36, 4, 6 }, { 37, 4, 7 },
        { 38, 6, 0 }, { 43, 6, 5 }, { 44, 6, 6 }, { 45, 6, 7 }, { 46, 7, 0 },
        { 47, 7, 1 }, { 52, 7, 6 },
    };
    const char *g;
    char buf[64];

    setup();
    for (unsigned k = 0; k < 8; k++) {
        bfin_set_reg(core, 0, k, 0x11110000 + k);
        bfin_set_reg(core, 1, k, 0x22220000 + k);
    }
    bfin_set_reg(core, 2, 5, 0x3333);
    bfin_set_reg(core, 3, 6, 0x4444);
    bfin_set_reg(core, 4, 1, 0x5555);
    bfin_set_reg(core, 6, 3, 0x6666);
    g = xfer("g");
    CHECK(strlen(g) == 54 * 8, "g length %zu", strlen(g));
    for (unsigned k = 0; k < sizeof(map) / sizeof(map[0]); k++) {
        uint32_t want = bfin_get_reg(core, map[k].grp, map[k].reg);

        CHECK(le(g + 8 * map[k].n) == want, "reg %d: %08x want %08x", map[k].n,
              le(g + 8 * map[k].n), want);
    }
    CHECK(le(g + 8 * 53) == bfin_get_pc(core), "pc in g");
    CHECK(le(g + 8 * 9) == 0x22220001, "P1 in g");

    CHECK(!strcmp(xfer("P0=78563412"), "OK"), "P0");
    CHECK(bfin_get_reg(core, 0, 0) == 0x12345678, "P write stuck");
    CHECK(!strcmp(xfer("p0"), "78563412"), "p0 reads back");
    CHECK(!strcmp(xfer("Pe=00100000"), "OK"), "SP");
    CHECK(bfin_get_reg(core, 1, 6) == 0x1000, "SP stuck");
    CHECK(!strcmp(xfer("P35=00040000"), "OK") && bfin_get_pc(core) == 0x400,
          "PC write");
    CHECK(!strcmp(xfer("P99=00000000"), "E01"), "bad register");
    /* G writes the whole file back. */
    g = xfer("g");
    snprintf(buf, sizeof buf, "%s", g);
    {
        static char gg[54 * 8 + 4];

        snprintf(gg, sizeof gg, "G%s", g);
        memcpy(gg + 1 + 8 * 8, "efbeadde", 8);          /* R... slot 8 = P0 */
        CHECK(!strcmp(xfer(gg), "OK"), "G");
        CHECK(bfin_get_reg(core, 1, 0) == 0xdeadbeef, "G wrote P0");
    }
}

static void test_mem(void)
{
    setup();
    CHECK(!strcmp(xfer("M3000,4:deadbeef"), "OK"), "M");
    CHECK(!strcmp(xfer("m3000,4"), "deadbeef"), "m round trip");
    CHECK(ram[0x3000] == 0xde, "host RAM written");
    CHECK(!strcmp(xfer("X3004,3:a}]c"), "OK"), "X");     /* a, 0x7d, c */
    CHECK(!strcmp(xfer("m3004,3"), "617d63"), "X escape: %s", xfer("m3004,3"));
    CHECK(!strcmp(xfer("m10000000,4"), "E14"), "unmapped read");
    CHECK(!strcmp(xfer("M10000000,1:00"), "E14"), "unmapped write");
}

static void test_break_step(void)
{
    const char *r;
    char pkt[64];

    /* Run to the end first so the core has decoded blocks over the path. */
    setup();
    CHECK(!strcmp(xfer("c"), "T02"), "free run ends on the budget");
    restart();
    snprintf(pkt, sizeof pkt, "Z0,%x,2", L_B);
    CHECK(!strcmp(xfer(pkt), "OK"), "Z0");
    r = xfer("c");
    CHECK(!strcmp(r, "T05swbreak:;"), "stop reply %s", r);
    CHECK(bfin_get_pc(core) == L_B, "pc %x at the breakpoint", bfin_get_pc(core));
    CHECK(bfin_get_reg(core, 0, 0) == 5 && bfin_get_reg(core, 0, 1) == 7 &&
          bfin_get_reg(core, 0, 2) == 0, "stopped before the instruction");

    r = xfer("s");                                  /* 16-bit add */
    CHECK(!strcmp(r, "T05") && bfin_get_pc(core) == L_B + 2, "step 16-bit");
    CHECK(bfin_get_reg(core, 0, 2) == 12, "step executed it");
    r = xfer("s");                                  /* 16-bit store */
    CHECK(bfin_get_pc(core) == L_AFTER && ram[DATA] == 12, "step 16-bit");
    snprintf(pkt, sizeof pkt, "Z0,%x,2", L_BUNDLE);
    xfer(pkt);
    r = xfer("c");
    CHECK(!strcmp(r, "T05swbreak:;") && bfin_get_pc(core) == L_BUNDLE, "bp 2");
    r = xfer("s");                                  /* the parallel bundle */
    CHECK(bfin_get_pc(core) == L_END, "step over a bundle: pc %x", bfin_get_pc(core));
    CHECK(bfin_get_reg(core, 0, 6) == 12, "bundle executed");
    snprintf(pkt, sizeof pkt, "z0,%x,2", L_B);
    CHECK(!strcmp(xfer(pkt), "OK"), "z0");
    CHECK(!strcmp(xfer("z0,1000,2"), "E01"), "z0 of an unset bp errors");

    restart();
    xfer("z0,12,2");
    CHECK(!strcmp(xfer("s"), "T05") && bfin_get_pc(core) == 4, "step 32-bit");
    CHECK(!strcmp(xfer("s"), "T05") && bfin_get_pc(core) == 8 &&
          bfin_get_reg(core, 1, 0) == DATA, "step second 32-bit");

    /* Odd address is refused, and a hit exactly at the resume pc is skipped. */
    CHECK(!strcmp(xfer("Z0,1001,2"), "E01"), "odd bp");
    setup();
    snprintf(pkt, sizeof pkt, "Z0,%x,2", L_A);
    xfer(pkt);
    CHECK(!strcmp(xfer("c"), "T05swbreak:;") && bfin_get_pc(core) == L_A, "bp at L_A");
    xfer("c");                                      /* resumes past it */
    CHECK(bfin_get_pc(core) == L_SPIN, "resumed past the bp, ran on");
}

static void test_watch(void)
{
    const char *r;
    char pkt[64];

    setup();
    xfer("c");                                      /* warm the block cache */
    restart();
    snprintf(pkt, sizeof pkt, "Z2,%x,4", DATA);
    CHECK(!strcmp(xfer(pkt), "OK"), "Z2");
    r = xfer("c");
    snprintf(pkt, sizeof pkt, "T05watch:%x;", DATA);
    CHECK(!strcmp(r, pkt), "reply %s", r);
    CHECK(bfin_get_pc(core) == L_AFTER && ram[DATA] == 12,
          "stopped right after the store: pc %x", bfin_get_pc(core));
    r = xfer("c");                                  /* the bundle's store */
    CHECK(!strcmp(r, pkt) && bfin_get_pc(core) == L_END,
          "bundle store: %s pc %x", r, bfin_get_pc(core));

    /* Read and access watchpoints; two hits in one instruction count once. */
    restart();
    snprintf(pkt, sizeof pkt, "z2,%x,4", DATA);
    xfer(pkt);
    snprintf(pkt, sizeof pkt, "Z3,%x,4", DATA);
    CHECK(!strcmp(xfer(pkt), "OK"), "Z3");
    r = xfer("c");
    snprintf(pkt, sizeof pkt, "T05rwatch:%x;", DATA);
    CHECK(!strcmp(r, pkt) && bfin_get_pc(core) == L_BUNDLE, "rwatch %s", r);
    restart();
    snprintf(pkt, sizeof pkt, "Z4,%x,4", DATA);
    xfer(pkt);
    snprintf(pkt, sizeof pkt, "Z2,%x,4", DATA);
    xfer(pkt);
    xfer("c");                                      /* the first store */
    CHECK(bfin_watch_extra(core) == 1, "extra hit counted: %u",
          bfin_watch_extra(core));
    {
        uint32_t a;
        int k;

        CHECK(bfin_watch_hit(core, &a, &k) && a == DATA && k == BFIN_WATCH_ACCESS,
              "first hit is the first watchpoint");
    }
    /* Overlap by a different width. */
    setup();
    snprintf(pkt, sizeof pkt, "Z2,%x,1", DATA + 3);
    xfer(pkt);
    xfer("c");
    CHECK(bfin_get_pc(core) == L_AFTER, "byte watch inside a word store");
    snprintf(pkt, sizeof pkt, "Z2,%x,1", DATA + 4);
    setup();
    xfer(pkt);
    CHECK(!strcmp(xfer("c"), "T02") && bfin_get_pc(core) == L_SPIN,
          "no hit outside the range");
}

static void test_tables(void)
{
    char pkt[64];

    setup();
    for (unsigned n = 0; n < BFIN_MAX_BREAK; n++) {
        snprintf(pkt, sizeof pkt, "Z0,%x,2", 0x100 + 2 * n);
        CHECK(!strcmp(xfer(pkt), "OK"), "bp %u", n);
    }
    CHECK(!strcmp(xfer("Z0,200,2"), "E01"), "breakpoint table full");
    CHECK(!strcmp(xfer("Z0,100,2"), "OK"), "duplicate is fine");
    for (unsigned n = 0; n < BFIN_MAX_WATCH; n++) {
        snprintf(pkt, sizeof pkt, "Z2,%x,1", 0x4000 + n);
        CHECK(!strcmp(xfer(pkt), "OK"), "wp %u", n);
    }
    CHECK(!strcmp(xfer("Z2,5000,1"), "E01"), "watchpoint table full");
    CHECK(!strcmp(xfer("Z1,100,2"), ""), "hw breakpoint unsupported");
}

static void test_detach_noack(void)
{
    char pkt[64];
    uint64_t n;

    setup();
    xfer("QStartNoAckMode");
    snprintf(pkt, sizeof pkt, "Z0,%x,2", L_B);
    outlen = 0;
    xfer("QStartNoAckMode");
    CHECK(outbuf[0] == '$', "no '+' in no-ack mode: %.8s", outbuf);
    xfer(pkt);
    snprintf(pkt, sizeof pkt, "Z2,%x,4", DATA);
    xfer(pkt);
    CHECK(!strcmp(xfer("D"), "OK") && bfin_gdb_detached(gdb), "D");
    CHECK(bfin_step(core, 100, &n) == BFIN_STOP_BUDGET &&
          bfin_get_pc(core) == L_SPIN, "runs freely after detach: pc %x",
          bfin_get_pc(core));
    CHECK(core->nbp == 0 && core->nwp == 0, "tables cleared");
}

static void test_misc(void)
{
    const char *r;

    setup();
    r = xfer("qSupported:multiprocess+");
    CHECK(strstr(r, "swbreak+") && strstr(r, "hwbreak+") &&
          strstr(r, "QStartNoAckMode+") && strstr(r, "PacketSize="), "qSupported: %s", r);
    CHECK(!strcmp(xfer("?"), "T05"), "?");
    CHECK(!strcmp(xfer("qAttached"), "1"), "qAttached");
    CHECK(!strcmp(xfer("qC"), "QC1"), "qC");
    CHECK(!strcmp(xfer("qfThreadInfo"), "m1") && !strcmp(xfer("qsThreadInfo"), "l"),
          "thread info");
    CHECK(!strcmp(xfer("vCont?"), "vCont;c;C;s;S"), "vCont?");
    CHECK(!strcmp(xfer("vCont;s:1"), "T05") && bfin_get_pc(core) == 4, "vCont s");
    CHECK(!strcmp(xfer("vFoo"), ""), "unknown packet is empty");
    /* A bad checksum is NAKed. */
    outlen = 0;
    bfin_gdb_feed(gdb, (const uint8_t *)"$g#00", 5);
    CHECK(outbuf[0] == '-', "nak");
    /* Interrupt while running. */
    xfer("c");                       /* drive() ends it on budget: T02 */
    CHECK(bfin_gdb_run_request(gdb) == BFIN_GDB_HALT, "halted after stop");
    bfin_gdb_feed(gdb, (const uint8_t *)"$c#63", 5);
    CHECK(bfin_gdb_run_request(gdb) == BFIN_GDB_CONT, "running");
    bfin_gdb_feed(gdb, (const uint8_t *)"\x03", 1);
    CHECK(bfin_gdb_interrupted(gdb), "interrupt seen");
    outlen = 0;
    bfin_gdb_stopped(gdb, BFIN_STOP_BUDGET);
    CHECK(strstr(outbuf, "$T02#"), "SIGINT reply: %s", outbuf);
}

int main(void)
{
    test_regs();
    test_mem();
    test_break_step();
    test_watch();
    test_tables();
    test_detach_noack();
    test_misc();
    printf(failures ? "gdb_test: %d FAILURES\n" : "gdb_test: all passed\n",
           failures);
    return failures != 0;
}
