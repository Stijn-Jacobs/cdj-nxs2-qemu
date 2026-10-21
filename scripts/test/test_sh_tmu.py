#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host-only TMU regression: compile production sh_timer.c and ptimer.c.

Only QEMU's clock/IRQ plumbing is stubbed; countdown, transactions, TCR
reads/writes and underflow callbacks are the production implementations.
An optional source path allows the same assertions to test an older tree.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SRC = ROOT / "qemu-src"


def main():
    timer = Path(sys.argv[1]) if len(sys.argv) > 1 else SRC / "hw/timer/sh_timer.c"
    sh = timer.read_text()
    first = sh.index("static const VMStateDescription")
    init_start = sh.index("static void *sh_timer_init(", first)
    init_end = sh.index("typedef struct {", init_start)
    sh = sh[sh.index("#define TIMER_TCR_TPSC"):first] + sh[init_start:init_end]
    reset_source = timer.read_text()
    reset_start = reset_source.index("static void sh_timer_chan_reset(")
    reset_end = reset_source.index("static void tmu012_reset(", reset_start)
    sh += reset_source[reset_start:reset_end]
    pt = (SRC / "hw/core/ptimer.c").read_text()
    pt = re.sub(r'^#include .*\n', '', pt, flags=re.M)
    start = pt.index("const VMStateDescription vmstate_ptimer")
    end = pt.index("ptimer_state *ptimer_init", start)
    pt = pt[:start] + pt[end:]
    header = (SRC / "include/hw/ptimer.h").read_text()
    header = header[header.index("#define PTIMER_POLICY_LEGACY"):header.index("extern const VMStateDescription")]
    # Take clock inputs from the board actually installed by the patch workflow.
    board = (ROOT / "hw/cdj/boards/cdj2000/board.c").read_text()
    clock_defines = "\n".join(re.findall(
        r'^#define (?:CDJ2000NXS_PERIPH_HZ|CDJ2000NXS_FRQCR_BOOT|SH7763_FRQCR_BOOT)\s+\S+',
        board, re.M))
    nxs = board[board.index("static const CdjBoardDesc cdj2000nxs_board"):
                board.index("static const CdjBoardDesc cdj2000_board")]
    assert ".periph_hz = CDJ2000NXS_PERIPH_HZ" in nxs
    assert ".frqcr = CDJ2000NXS_FRQCR_BOOT" in nxs
    program = r'''
#include <glib.h>
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint64_t hwaddr;
typedef struct { uint64_t period; } Clock;
static uint64_t clock_get(const Clock *c) { return c->period; }
static uint64_t extract64(uint64_t v, int s, int n)
{ return (v >> s) & (~UINT64_C(0) >> (64 - n)); }
static int clz64(uint64_t v) { return v ? __builtin_clzll(v) : 64; }
#define QEMU_CLOCK_VIRTUAL 0
static int64_t now;
static int64_t qemu_clock_get_ns(int type) { (void)type; return now; }
static bool qtest_enabled(void) { return true; }
static bool icount_enabled(void) { return false; }
typedef struct { int64_t expires; void (*cb)(void *); void *opaque; } QEMUTimer;
static QEMUTimer *timer_new_ns(int type, void (*cb)(void *), void *opaque)
{ (void)type; QEMUTimer *t = g_new0(QEMUTimer, 1); t->cb = cb; t->opaque = opaque; t->expires = -1; return t; }
static void timer_mod(QEMUTimer *t, int64_t ns) { t->expires = ns; }
static void timer_del(QEMUTimer *t) { t->expires = -1; }
static void timer_free(QEMUTimer *t) { g_free(t); }
typedef struct { bool level; } IRQ;
typedef IRQ *qemu_irq;
static void qemu_set_irq(qemu_irq irq, int level) { irq->level = !!level; }
#define HWADDR_PRIx PRIx64
#define LOG_GUEST_ERROR 0
#define qemu_log_mask(...) ((void)0)
#define qemu_log(...) ((void)0)
#define trace_sh_timer_start_stop(...) ((void)0)
static const int vmstate_sh_timer;
#define vmstate_register_any(...) ((void)vmstate_sh_timer)
''' + clock_defines + "\n" + header + pt + sh + r'''
static unsigned underflows;
static void counted_tick(void *opaque) { underflows++; sh_timer_tick(opaque); }
static void init(SHTimerState *s, IRQ *irq)
{
    now = 0; underflows = 0;
    *irq = (IRQ){0};
    SHTimerState *allocated = sh_timer_init(54000000, 0, irq);
    *s = *allocated;
    g_free(allocated);
    s->timer->callback = counted_tick;
    s->timer->callback_opaque = s;
    sh_timer_write(s, OFFSET_TCOR * 4, 13500);
    sh_timer_write(s, OFFSET_TCNT * 4, 13500);
    sh_timer_write(s, OFFSET_TCR * 4, TIMER_TCR_UNIE);
    sh_timer_start_stop(s, 1);
}
static void run_due(SHTimerState *s)
{
    QEMUTimer *t = s->timer->timer;
    unsigned guard = 0;
    while (t->expires >= 0 && t->expires <= now) {
        assert(++guard < 100000);
        timer_del(t); t->cb(t->opaque);
    }
}
static unsigned drain(SHTimerState *s)
{
    unsigned count = 0;
    while (s->irq->level) {
        assert(++count < 100000);
        uint32_t tcr = sh_timer_read(s, OFFSET_TCR * 4);
        assert(tcr & TIMER_TCR_UNF);
        sh_timer_write(s, OFFSET_TCR * 4, tcr & ~TIMER_TCR_UNF);
    }
    /* A repeated clear with no outstanding event must create no interrupt. */
    sh_timer_write(s, OFFSET_TCR * 4, TIMER_TCR_UNIE);
    assert(!s->irq->level);
    return count;
}
static void burst_test(void)
{
    SHTimerState s; IRQ irq; init(&s, &irq);
    unsigned delivered = 0;
    for (unsigned ms = 10; ms <= 1000; ms += 10) {
        now = (int64_t)ms * 1000000;
        run_due(&s); /* callbacks catch up at one late virtual timestamp */
        delivered += drain(&s);
    }
    printf("bursty/late clears: %u underflows, %u interrupts\n", underflows, delivered);
    assert(underflows == 1000 && delivered == 1000);
    ptimer_free(s.timer);
}
static void phase_test(void)
{
    SHTimerState s; IRQ irq; init(&s, &irq);
    unsigned delivered = 0;
    for (unsigned ms = 1; ms <= 1000; ms++) {
        now = (int64_t)ms * 1000000;
        run_due(&s);
        /* Clear near the next expiry, while TCNT rounds down to zero. */
        now = s.timer->timer->expires - 1;
        int64_t deadline = s.timer->timer->expires;
        unsigned before = underflows;
        delivered += drain(&s);
        if (underflows != before || s.timer->timer->expires != deadline) {
            fprintf(stderr, "UNF clear created %u callback(s), deadline moved by %" PRId64
                    " ns; %u interrupt entries for %u scheduled expiries\n",
                    underflows - before, s.timer->timer->expires - deadline, delivered, ms);
            assert(delivered == ms);
            abort();
        }
        assert(delivered == ms);
    }
    assert(underflows == 1000 && delivered == 1000);
    printf("near-expiry repeated clears: 1000 underflows, 1000 interrupts; phase preserved\n");
    ptimer_free(s.timer);
}
static void boundary_test(void)
{
    SHTimerState s; IRQ irq; init(&s, &irq);
    now = 1000000; run_due(&s);
    assert(underflows == 1 && irq.level);
    uint32_t tcr = sh_timer_read(&s, OFFSET_TCR * 4);
    sh_timer_write(&s, OFFSET_TCR * 4, tcr); /* UNF=1 preserves pending */
    assert(irq.level);
    /* Expiry due, but callback not yet dispatched: clearing must neither
     * synthesize a callback nor remove the scheduled expiry. */
    now = s.timer->timer->expires;
    sh_timer_write(&s, OFFSET_TCR * 4, tcr & ~TIMER_TCR_UNF);
    assert(underflows == 1 && !irq.level);
    sh_timer_write(&s, OFFSET_TCR * 4, tcr & ~TIMER_TCR_UNF);
    assert(underflows == 1 && !irq.level);
    run_due(&s);
    assert(underflows == 2 && drain(&s) == 1);
    /* Callback between the guest read and clear: retire the old event,
     * retain the new one, including across interrupt masking. */
    now += 1000000; run_due(&s);
    tcr = sh_timer_read(&s, OFFSET_TCR * 4);
    now += 1000000; run_due(&s);
    assert(underflows == 4);
    sh_timer_write(&s, OFFSET_TCR * 4, tcr & ~TIMER_TCR_UNF);
    sh_timer_write(&s, OFFSET_TCR * 4, TIMER_TCR_UNF);
    assert(!irq.level && (sh_timer_read(&s, OFFSET_TCR * 4) & TIMER_TCR_UNF));
    sh_timer_write(&s, OFFSET_TCR * 4, TIMER_TCR_UNF | TIMER_TCR_UNIE);
    assert(drain(&s) == 1);
    printf("clear/callback order, RMW and UNIE masking: PASS\n");
    ptimer_free(s.timer);
}
static void lifecycle_test(void)
{
    SHTimerState s; IRQ irq; init(&s, &irq);
    now = 5000000000LL; run_due(&s);
    assert(underflows == 5000);
    assert(drain(&s) == 5000); /* no one-second backlog cap */
    now += 10000000; run_due(&s);
    assert(irq.level);
    sh_timer_start_stop(&s, 0);
    assert(!irq.level);
    sh_timer_write(&s, OFFSET_TCNT * 4, 13500);
    sh_timer_start_stop(&s, 1);
    now += 1000000; run_due(&s);
    assert(drain(&s) == 1); /* stop discarded the old backlog */
    now += 10000000; run_due(&s);
    sh_timer_chan_reset(&s);
    assert(!irq.level);
    sh_timer_write(&s, OFFSET_TCOR * 4, 13500);
    sh_timer_write(&s, OFFSET_TCNT * 4, 13500);
    sh_timer_write(&s, OFFSET_TCR * 4, TIMER_TCR_UNIE);
    sh_timer_start_stop(&s, 1);
    now += 1000000; run_due(&s);
    assert(drain(&s) == 1); /* reset discarded the old backlog */
    sh_timer_write(&s, OFFSET_TCR * 4, TIMER_TCR_UNF);
    now += 10000000; run_due(&s);
    assert(!irq.level && (sh_timer_read(&s, OFFSET_TCR * 4) & TIMER_TCR_UNF));
    sh_timer_write(&s, OFFSET_TCR * 4, 0);
    assert(!(sh_timer_read(&s, OFFSET_TCR * 4) & TIMER_TCR_UNF));
    ptimer_free(s.timer);
    /* TSTR before the first TCR write must use the reset /4 prescaler. */
    SHTimerState *fresh = sh_timer_init(54000000, 0, &irq);
    sh_timer_write(fresh, OFFSET_TCOR * 4, 13500);
    sh_timer_write(fresh, OFFSET_TCNT * 4, 13500);
    sh_timer_start_stop(fresh, 1);
    sh_timer_write(fresh, OFFSET_TCR * 4, TIMER_TCR_UNIE);
    now += 1000000; run_due(fresh);
    assert(drain(fresh) == 1);
    ptimer_free(fresh->timer);
    g_free(fresh);
    printf("5000-event burst, stop/reset, polling UNF and start order: PASS\n");
}
static unsigned stock_tick_rate(uint32_t hz, uint32_t frqcr, bool pioneer)
{
    now = 0; underflows = 0;
    IRQ irq = {0};
    SHTimerState *s = sh_timer_init(hz, 0, &irq);
    s->timer->callback = counted_tick;
    s->timer->callback_opaque = s;
    /* Stock 042c5b70/042c5cd0 branch, 042c5d16 N-1 setup, and
     * 04308040's unconditional TMU4 setup. No firmware executes here. */
    uint32_t n = (frqcr & 0xf0000000) == 0x30000000 ? 11250 : 13500;
    uint32_t reload = pioneer ? 13500 : n - 1;
    sh_timer_write(s, OFFSET_TCOR * 4, reload);
    sh_timer_write(s, OFFSET_TCNT * 4, reload);
    sh_timer_write(s, OFFSET_TCR * 4, 0x20);
    sh_timer_start_stop(s, 1);
    now = 1000000000; run_due(s);
    unsigned ticks = drain(s);
    assert(ticks == underflows);
    ptimer_free(s->timer); g_free(s);
    return ticks;
}
static void board_clock_test(void)
{
    assert(CDJ2000NXS_PERIPH_HZ == 26965000 * 12 / 6);
    assert(SH7763_FRQCR_BOOT == 0x30000000);
    assert(CDJ2000NXS_FRQCR_BOOT == 0x40320044);
    assert(stock_tick_rate(54000000, 0x30000000, false) == 1200);
    assert(stock_tick_rate(54000000, 0x30000000, true) == 1000);
    assert(stock_tick_rate(CDJ2000NXS_PERIPH_HZ, CDJ2000NXS_FRQCR_BOOT, false) == 998);
    assert(stock_tick_rate(CDJ2000NXS_PERIPH_HZ, CDJ2000NXS_FRQCR_BOOT, true) == 998);
    /* Owned runtime TMU2 free-run, TMU1 one-shot: both use Pphi/4.
     * Its counts*2/27 conversion deliberately assumes nominal 54 MHz. */
    now = 0; IRQ irq = {0};
    SHTimerState *s = sh_timer_init(CDJ2000NXS_PERIPH_HZ, 0, &irq);
    sh_timer_write(s, OFFSET_TCOR * 4, UINT32_MAX);
    sh_timer_write(s, OFFSET_TCNT * 4, UINT32_MAX);
    sh_timer_write(s, OFFSET_TCR * 4, 0);
    sh_timer_start_stop(s, 1);
    now = 1000000000;
    uint32_t counts = UINT32_MAX - sh_timer_read(s, OFFSET_TCNT * 4);
    /* ptimer rounds remaining counts down; its fixed-point period can
     * place this exact-second read one count beyond the integer value. */
    assert(counts >= 13482500 && counts <= 13482501);
    assert((uint64_t)counts * 2 / 27 == 998703);
    sh_timer_start_stop(s, 0);
    sh_timer_write(s, OFFSET_TCOR * 4, 13500000);
    sh_timer_write(s, OFFSET_TCNT * 4, 13500000);
    sh_timer_write(s, OFFSET_TCR * 4, 0x20);
    sh_timer_start_stop(s, 1);
    int64_t interval = s->timer->timer->expires - now;
    assert(interval >= 1001297978 && interval <= 1001297979);
    ptimer_free(s->timer); g_free(s);
    printf("stock clock: old TMU0=1200/s, TMU4=1000/s; corrected both=998 in 1s; owned us/alarm: PASS\n");
}
int main(int argc, char **argv)
{
    (void)argc;
    if (argv[1] && !strcmp(argv[1], "phase")) phase_test();
    else if (argv[1] && !strcmp(argv[1], "boundary")) boundary_test();
    else if (argv[1] && !strcmp(argv[1], "lifecycle")) lifecycle_test();
    else if (argv[1] && !strcmp(argv[1], "clock")) board_clock_test();
    else burst_test();
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="tmu-test-", dir=ROOT) as directory:
        tmp = Path(directory)
        (tmp / "test.c").write_text(program)
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(tmp / "test.c"),
            "-o", str(tmp / "test"), *flags], check=True)
        failures = []
        for case in ("phase", "burst", "boundary", "lifecycle", "clock"):
            result = subprocess.run([str(tmp / "test"), case])
            if result.returncode:
                failures.append(case)
        if failures:
            raise SystemExit("FAIL: " + ", ".join(failures))


if __name__ == "__main__":
    main()
