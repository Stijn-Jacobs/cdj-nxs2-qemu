/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Elapsed time, architectural CYCLES and core timer have different IDLE rules. */
#include "bfin_priv.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

#define TCNTL 0xffe03000u
#define TCOUNT 0xffe0300cu
static uint8_t code[16] = {0x20,0, 0,0, 0,0x20}; /* IDLE; NOP; JUMP.S self */

static uint64_t counter(bfin_core *c)
{
    uint32_t lo = bfin_get_reg(c, 6, 6);
    return (uint64_t)bfin_get_reg(c, 6, 7) << 32 | lo;
}

static void ownership_timing(void)
{
    bfin_bus bus = {0};
    bfin_core *c = bfin_new(&bus);
    bfin_ownership_timing t, repeated;
    bfin_map_ram(c, 0, sizeof(code), code); bfin_reset(c, 0);
    bfin_set_models(c, 1, 0);
    assert(bfin_timing_enable(c, 1, 3) == -1);
    assert(bfin_timing_enable(c, 1, 2) == 0);
    assert(bfin_timing_enable(c, 1, 2) == -1);
    bfin_skip_cycles(c, 10); bfin_cli(c, 0);
    bfin_stepi(c); assert(c->idle); /* first instruction IDLE */
    bfin_skip_cycles(c, 100);
    bfin_timing_read(c, &t); bfin_timing_read(c, &repeated);
    assert(!t.start_seen && t.phase[0].imask.open);
    assert(t.phase[0].imask.intervals == 0 && t.phase[0].imask.censored_cycles == 111);
    assert(t.phase[0].imask.censored_idle_cycles == 100);
    assert(!memcmp(&t, &repeated, sizeof(t))); /* reads never consume an interval */
    bfin_wake(c); bfin_stepi(c); /* actual marker instruction at PC2 */
    bfin_skip_cycles(c, 10); bfin_sti(c, 0xffe0);
    bfin_timing_read(c, &t);
    assert(t.start_seen && t.start_cycle == 111);
    assert(!t.phase[0].imask.open && t.phase[0].imask.intervals == 0);
    assert(t.phase[0].imask.censored_cycles == 111);
    assert(t.phase[1].imask.intervals == 1 && t.phase[1].imask.total_cycles == 11);
    assert(t.phase[1].imask.total_awake_cycles == 11 && !t.phase[1].imask.total_idle_cycles);
    bfin_cli(c, 0); bfin_skip_cycles(c, 3); bfin_cli(c, 1);
    bfin_skip_cycles(c, 4); bfin_sti(c, 0x1f); bfin_skip_cycles(c, 5);
    bfin_mmr_write(c, 0xffe02104, 0xffe0); /* direct IMASK writes use the same hook */
    bfin_timing_read(c, &t);
    assert(t.phase[1].imask.intervals == 2 && t.phase[1].imask.max_cycles == 12);
    assert(t.phase[1].imask.total_cycles == 23);
    c->ipend = 0; c->evt[6] = 4; bfin_raise(c, 6); bfin_stepi(c);
    bfin_skip_cycles(c, 7); bfin_reti_pushed(c, 1);
    bfin_timing_read(c, &t);
    assert(t.phase[1].ipend_global.intervals == 1 && t.phase[1].ipend_global.max_cycles == 7);
    assert(t.phase[1].imask.total_cycles == 23); /* global bit4 is a separate metric */
    bfin_reti_pushed(c, 0); bfin_skip_cycles(c, 2); bfin_return(c, EV_IVHW);
    bfin_timing_read(c, &t); assert(t.phase[1].ipend_global.total_cycles == 9);
    c->ownership.phase[1].imask.total_cycles = UINT64_MAX;
    bfin_cli(c, 0); bfin_skip_cycles(c, 1); bfin_sti(c, 0xffe0);
    bfin_timing_read(c, &t); assert(t.overflow && t.phase[1].imask.total_cycles == UINT64_MAX);
    size_t n = bfin_snap_save(c, NULL); uint8_t *snapshot = malloc(n);
    bfin_snap_save(c, snapshot); bfin_snap_load(c, snapshot); free(snapshot);
    bfin_timing_read(c, &t); assert(t.snapshot_loaded);
    bfin_free(c);

    c = bfin_new(&bus); bfin_map_ram(c, 0, sizeof(code), code); bfin_reset(c, 0);
    bfin_set_models(c, 1, 0);
    assert(!bfin_timing_enable(c, 0, 0)); bfin_sti(c, 0xffe0);
    bfin_timing_read(c, &t); assert(!t.start_seen && !t.start_configured);
    bfin_free(c);
    c = bfin_new(&bus); bfin_map_ram(c, 0, sizeof(code), code); bfin_reset(c, 0);
    bfin_set_models(c, 1, 0);
    assert(!bfin_timing_enable(c, 1, 0x100)); bfin_sti(c, 0xffe0);
    bfin_set_wake(c, 1); bfin_step(c, 20, NULL);
    bfin_timing_read(c, &t); assert(!t.start_seen); /* unmask is not a phase marker */
    bfin_free(c);
    c = bfin_new(&bus); bfin_map_ram(c, 0, sizeof(code), code); bfin_reset(c, 0);
    bfin_set_models(c, 1, 0);
    assert(!bfin_timing_enable(c, 1, 2)); bfin_set_wake(c, 1);
    bfin_step(c, 20, NULL); bfin_timing_read(c, &t);
    assert(t.start_seen && t.start_cycle == 1); /* normal block execution honors marker */
    bfin_free(c);
}

/* Without the models: CYCLES runs without CCEN, the core timer runs through
 * IDLE, a wake level is ignored and TINT is write-one-to-clear. */
static void default_models(void)
{
    bfin_bus bus = {0};
    bfin_core *c = bfin_new(&bus);
    bfin_map_ram(c, 0, sizeof(code), code); bfin_reset(c, 0);
    bfin_skip_cycles(c, 20);
    assert(counter(c) == 20);
    bfin_mmr_write(c, TCOUNT, 10);
    bfin_mmr_write(c, TCNTL, 3);
    bfin_set_wake(c, 1);
    assert(bfin_stepi(c) == BFIN_STOP_STEP && c->idle && !c->wake_level);
    assert(bfin_timer_due(c) != UINT64_MAX);
    bfin_skip_cycles(c, bfin_timer_due(c));
    bfin_stepi(c);
    assert(bfin_mmr_read(c, TCNTL) & 8);
    bfin_mmr_write(c, TCNTL, 1);          /* writing zero retains TINT */
    assert(bfin_mmr_read(c, TCNTL) & 8);
    bfin_mmr_write(c, TCNTL, 9);          /* one clears it */
    assert(!(bfin_mmr_read(c, TCNTL) & 8));
}

int main(void)
{
    bfin_bus bus = {0};
    bfin_core *c = bfin_new(&bus);
    bfin_map_ram(c, 0, sizeof(code), code);
    bfin_set_models(c, 1, 1);
    bfin_reset(c, 0);
    bfin_skip_cycles(c, 20);
    assert(counter(c) == 0);             /* CCEN resets disabled */
    bfin_set_reg(c, 7, 2, 0x32);
    bfin_mmr_write(c, TCOUNT, 10);
    bfin_mmr_write(c, TCNTL, 3);
    assert(bfin_stepi(c) == BFIN_STOP_STEP && c->idle);
    uint64_t before = counter(c), elapsed = bfin_cycles(c);
    uint32_t remaining = bfin_mmr_read(c, TCOUNT);
    assert(bfin_timer_due(c) == UINT64_MAX);
    bfin_skip_cycles(c, 1000);
    assert(bfin_cycles(c) == elapsed + 1000);
    assert(counter(c) == before);
    assert(bfin_mmr_read(c, TCOUNT) == remaining);
    assert(!(bfin_mmr_read(c, TCNTL) & 8));

    size_t bytes = bfin_snap_save(c, NULL);
    uint8_t *snapshot = malloc(bytes);
    bfin_snap_save(c, snapshot);
    bfin_set_wake(c, 1);
    assert(!c->idle && bfin_timer_due(c) == remaining);
    bfin_skip_cycles(c, remaining);
    bfin_stepi(c);
    assert(bfin_mmr_read(c, TCNTL) & 8);
    bfin_mmr_write(c, TCNTL, 9);          /* writing one retains TINT */
    assert(bfin_mmr_read(c, TCNTL) & 8);
    bfin_mmr_write(c, TCNTL, 1);          /* zero clears it */
    assert(!(bfin_mmr_read(c, TCNTL) & 8));
    bfin_snap_load(c, snapshot);
    free(snapshot);
    assert(c->idle && !c->wake_level && counter(c) == before);
    assert(bfin_mmr_read(c, TCOUNT) == remaining);

    /* A preexisting wake level prevents IDLE even with CEC masked. */
    bfin_set_wake(c, 1);
    bfin_set_pc(c, 0);
    bfin_stepi(c);
    assert(!c->idle);
    bfin_set_wake(c, 0);
    bfin_set_pc(c, 0);
    bfin_stepi(c);
    assert(c->idle);
    bfin_wake(c);

    /* Low reads latch the high word across rollover. CCEN excludes later
     * elapsed awake time while disabled, then resumes the retained count. */
    bfin_set_reg(c, 6, 7, 1);
    bfin_set_reg(c, 6, 6, 0xfffffffeu);
    assert(bfin_get_reg(c, 6, 6) == 0xfffffffeu);
    bfin_skip_cycles(c, 4);
    assert(bfin_get_reg(c, 6, 7) == 1);
    assert(counter(c) == UINT64_C(0x200000002));
    bfin_set_reg(c, 7, 2, 0x30);
    before = counter(c);
    bfin_skip_cycles(c, 1000);
    assert(counter(c) == before);
    bfin_set_reg(c, 7, 2, 0x32);
    bfin_skip_cycles(c, 3);
    assert(counter(c) == before + 3);
    bfin_free(c);
    ownership_timing();
    default_models();
    puts("PASS elapsed/IDLE clocks, CYCLES latch/CCEN, core timer, snapshots and default models");
    return 0;
}
