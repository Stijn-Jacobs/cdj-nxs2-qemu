/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What a browser page reads from either NXS2 board while it runs: the guest
 * pc, the virtual clock (under -icount it counts executed instructions) and
 * guest memory, so progress shows even while the screen is still black; the
 * face's keys and lamps; and how it saves a booted machine so a later visit
 * starts from there.
 */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "system/physmem.h"
#include "cpu.h"
#include "qemu/aio.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "qapi/qapi-commands-migration.h"
#include "qapi/qapi-commands-misc.h"
#include "migration/migration.h"
#include "cdj_web.h"
#include <emscripten.h>

EMSCRIPTEN_KEEPALIVE uint32_t cdj_wasm_pc(void)
{
    return first_cpu ? SUPERH_CPU(first_cpu)->env.pc : 0;
}

EMSCRIPTEN_KEEPALIVE double cdj_wasm_vclock_ms(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1e6;
}

/* In guest byte order: little-endian on MAIN, big-endian on the display. */
EMSCRIPTEN_KEEPALIVE uint32_t cdj_wasm_peek32(uint32_t addr)
{
    uint8_t b[4];

    physical_memory_read(addr, b, 4);
    return ldl_p(b);
}

/*
 * The page's face keys, as the same "off:val:dur:op" lines the panel key
 * socket carries natively: a head and a tail counter, then RING_LINES
 * NUL-terminated slots. The page is the only producer and moves head; the
 * panel link moves tail once per frame.
 */
#define RING_LINES 256
#define RING_WIDTH 64

static struct {
    uint32_t head;
    uint32_t tail;
    char line[RING_LINES][RING_WIDTH];
} panel_ring;

EMSCRIPTEN_KEEPALIVE void *cdj_web_panel_ring(void)
{
    return &panel_ring;
}

bool cdj_web_panel_line(char *msg, size_t size)
{
    uint32_t tail = panel_ring.tail;

    if (tail == qatomic_load_acquire(&panel_ring.head)) {
        return false;
    }
    g_strlcpy(msg, panel_ring.line[tail % RING_LINES], MIN(size, RING_WIDTH));
    qatomic_store_release(&panel_ring.tail, tail + 1);
    return true;
}

/*
 * MAIN's latest panel frame for the page's lamps: a sequence word, then the
 * frame. The sequence is odd while the bytes change, so a reader that sees
 * the same even value before and after its copy has a whole frame. It moves
 * on every frame, changed or not, so the page can tell a stopped MAIN.
 */
static struct {
    uint32_t seq;
    uint8_t frame[CDJ_WEB_STATE_FRAME];
} panel_state;

EMSCRIPTEN_KEEPALIVE void *cdj_web_state(void)
{
    return &panel_state;
}

void cdj_web_state_frame(const uint8_t *frame, unsigned len)
{
    qatomic_store_release(&panel_state.seq, panel_state.seq + 1);
    memcpy(panel_state.frame, frame, MIN(len, CDJ_WEB_STATE_FRAME));
    qatomic_store_release(&panel_state.seq, panel_state.seq + 1);
}

/*
 * A saved point, the way scripts/run/snapshot_deck.py saves one natively
 * through the monitor: stop, migrate to a file, cont. The page has no monitor,
 * so it asks for each step here and they run on the main loop; the save is
 * whole once cdj_wasm_save_status() reads cdj_wasm_save_completed(), and
 * gone if it reads cdj_wasm_save_failed(); either way the board stays stopped
 * until cdj_wasm_cont().
 */
static void cdj_web_save_bh(void *opaque)
{
    Error *err = NULL;

    qmp_stop(NULL);
    qmp_migrate(opaque, false, NULL, false, false, &err);
    if (err) {
        error_report_err(err);
    }
}

static void cdj_web_cont_bh(void *opaque)
{
    qmp_cont(NULL);
}

/* The lab page's MAIN-alone save. */
EMSCRIPTEN_KEEPALIVE void cdj_wasm_save(void)
{
    aio_bh_schedule_oneshot(qemu_get_aio_context(), cdj_web_save_bh,
                            (void *)"file:/main.vm");
}

/* The app's saved boot, one file per board under the same name. */
EMSCRIPTEN_KEEPALIVE void cdj_web_save_state(void)
{
    aio_bh_schedule_oneshot(qemu_get_aio_context(), cdj_web_save_bh,
                            (void *)"file:/boot.vmstate");
}

/* A MigrationStatus. */
EMSCRIPTEN_KEEPALIVE int cdj_wasm_save_status(void)
{
    return qatomic_read(&migrate_get_current()->state);
}

EMSCRIPTEN_KEEPALIVE int cdj_wasm_save_completed(void)
{
    return MIGRATION_STATUS_COMPLETED;
}

EMSCRIPTEN_KEEPALIVE int cdj_wasm_save_failed(void)
{
    return MIGRATION_STATUS_FAILED;
}

EMSCRIPTEN_KEEPALIVE void cdj_wasm_cont(void)
{
    aio_bh_schedule_oneshot(qemu_get_aio_context(), cdj_web_cont_bh, NULL);
}
