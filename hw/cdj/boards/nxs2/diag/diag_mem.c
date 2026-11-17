/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj.h"
#include "cdj_getenv.h"
/*
 * A held memory write applied from a QEMU timer, like the CDJ_MPOKE knobs in
 * common/cdj_mpoke.c. Off by default.
 *
 * CDJ_DIRTY_MS=<ms>: set the display frame-dirty flag 0x10DC1510 every <ms>.
 * The tick at 0x0838E952 rebuilds the tx frame only while it reads 1; the
 * firmware sets it once at init (0x0838E764) and clears it at 0x0838EE20.
 */
#define CDJ_DIRTY_FLAG_ADDR  0x10DC1510

static QEMUTimer *cdj_dirty_timer;
static uint64_t cdj_dirty_writes;

static int64_t cdj_dirty_ms(void)
{
    static int64_t ms = -1;

    if (ms < 0) {
        const char *e = getenv("CDJ_DIRTY_MS");

        ms = e ? (int64_t)strtoll(e, NULL, 0) : 0;
    }
    return ms;
}

static void cdj_dirty_tick(void *opaque)
{
    uint8_t one = 1;

    physical_memory_write(CDJ_DIRTY_FLAG_ADDR, &one, 1);
    cdj_dirty_writes++;
    timer_mod(cdj_dirty_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + cdj_dirty_ms() * 1000000);
}

static void cdj_dirty_report(Notifier *n, void *unused)
{
    if (cdj_dirty_writes) {
        info_report("display: frame-dirty flag re-asserted %" PRIu64
                    " times (CDJ_DIRTY_MS=%" PRId64 ")",
                    cdj_dirty_writes, cdj_dirty_ms());
    }
}

static Notifier cdj_dirty_exit;

void cdj_dirty_init(void)
{
    if (cdj_dirty_ms()) {
        cdj_dirty_exit.notify = cdj_dirty_report;
        qemu_add_exit_notifier(&cdj_dirty_exit);
        cdj_dirty_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_dirty_tick,
                                       NULL);
        timer_mod(cdj_dirty_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                  + cdj_dirty_ms() * 1000000);
        info_report("display: holding the frame-dirty flag set every %" PRId64
                    " ms", cdj_dirty_ms());
    }
}
