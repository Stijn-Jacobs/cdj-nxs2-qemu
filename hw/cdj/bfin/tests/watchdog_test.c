/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual BF531 bus/model tests: no firmware fixture or integrated workflow. */
#include "bf531.h"
#include "bfin_priv.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SYS 0xffc00000u
#define PLL_DIV (SYS + 4)
#define IMASK (SYS + 0x10c)
#define ISR (SYS + 0x120)
#define IWR (SYS + 0x124)
#define CTL (SYS + 0x200)
#define CNT (SYS + 0x204)
#define STAT (SYS + 0x208)
#define TIMER0 (SYS + 0x600)
#define TIMER_ENABLE (SYS + 0x640)
#define TIMER_DISABLE (SYS + 0x644)
#define TIMER_STATUS (SYS + 0x648)
#define WD_IRQ (1u << 23)
#define DISABLE 0xad4
#define WDRO 0x8000
static bf531 *chip;
static bfin_core *core;
static uint32_t rd(uint32_t a) { return bf531_bus_read(chip, a, a == CTL ? 2 : 4); }
static void wr(uint32_t a, uint32_t v) { bf531_bus_write(chip, a, v, a == CTL ? 2 : 4); }
static uint64_t cycles(void)
{
    uint32_t lo = bfin_get_reg(core, 6, 6);
    return (uint64_t)bfin_get_reg(core, 6, 7) << 32 | lo;
}
static void fresh(void)
{
    static const bf531_host host;
    static const uint8_t code[] = {0x20,0, 0,0, 0,0x20}; /* IDLE; NOP; self */
    if (chip) { bf531_free(chip); }
    chip = bf531_new(0x10000, &host, NULL);
    core = bf531_core(chip);
    bfin_set_models(core, 1, 0);
    memcpy(bf531_mem(chip, 0, sizeof(code)), code, sizeof(code));
    bfin_reset(core, 0);
    bfin_set_reg(core, 7, 2, 0x32); /* CCEN */
    wr(PLL_DIV, 4); /* stock board: CCLK/SCLK = 4 */
    assert(rd(IMASK) == 0 && rd(IWR) == UINT32_MAX);
}
static void idle(void)
{
    bfin_set_pc(core, 0);
    assert(bf531_stepi(chip) == BFIN_STOP_STEP);
}
static void arm(uint32_t count, unsigned event)
{
    wr(CTL, DISABLE);
    wr(CTL, DISABLE | WDRO);
    wr(CNT, count);
    wr(CTL, event);
}
static void advance(uint64_t n) { bfin_skip_cycles(core, n); (void)rd(STAT); }

/* A masked, latched PWM interrupt still wakes when IWR preserves reset bits.
 * This is the production backlight configuration, not a synthetic core IRQ. */
static void backlight_latch_wake_policy(void)
{
    const uint32_t timer_irq = 1u << 16, required_dma = 0x1f02;
    fresh();
    wr(TIMER0, 0x219);
    wr(TIMER0 + 8, 300000);
    wr(TIMER_ENABLE, 1);
    bfin_stop policy_stop = bf531_run(chip, 1200000);
    assert(policy_stop == BFIN_STOP_BUDGET);
    assert((rd(ISR) & timer_irq) && (rd(TIMER_STATUS) & 1));
    assert(rd(IMASK) == 0 && rd(IWR) == UINT32_MAX && core->wake_level);

    arm(1000, 4);
    uint64_t awake_before = cycles();
    uint32_t stat_before = rd(STAT);
    idle();
    assert(!core->idle); /* CLI/IMASK does not exclude a wake-only source. */
    assert(bf531_run(chip, 100) == BFIN_STOP_BUDGET);
    uint64_t awake = cycles() - awake_before;
    uint64_t system = (uint64_t)(stat_before - rd(STAT)) * 4;
    assert(awake == 101 && system <= awake && rd(STAT) != 0);

    /* Leave PWM timing and its latch intact, but admit only actual runtime
     * wake owners. The exact same pending TIMER0 must no longer defeat IDLE. */
    wr(IWR, rd(IMASK) | required_dma | WD_IRQ);
    assert((rd(ISR) & timer_irq) && !core->wake_level);
    arm(1000, 4);
    idle();
    assert(core->idle);
    awake_before = cycles();
    uint64_t elapsed_before = bfin_cycles(core);
    stat_before = rd(STAT);
    assert(bf531_run(chip, 100) == BFIN_STOP_BUDGET);
    assert(core->idle && cycles() == awake_before);
    assert(rd(STAT) < stat_before && !(rd(ISR) & WD_IRQ));
    assert(bf531_stepi(chip) == BFIN_STOP_IDLE);
    assert(!core->idle && (rd(ISR) & WD_IRQ) && rd(STAT) == 0);
    assert(cycles() == awake_before && bfin_cycles(core) > elapsed_before + 3900);
    system = (uint64_t)(stat_before - rd(STAT)) * 4;
    assert(system - (cycles() - awake_before) >= 3900); /* stopped CCLK correction */
    wr(CTL, DISABLE);
    wr(CTL, DISABLE | WDRO);
    assert(!core->wake_level && (rd(ISR) & timer_irq));
    assert(rd(TIMER0) == 0x219 && rd(TIMER0 + 8) == 300000);
}

static void ownership_timing(void)
{
    bf531_ownership_timing t;
    fresh(); assert(!bf531_timing_enable(chip, 1, 2));
    arm(3, 4); bfin_skip_cycles(core, 20); (void)rd(CTL);
    bf531_timing_read(chip, &t);
    assert(t.watchdog[0].arms == 1 && t.watchdog[0].expiries == 1);
    assert(!t.watchdog[0].stat_samples && t.watchdog[0].expired_without_sample == 1);
    assert(t.watchdog[0].pending_expiry && t.watchdog[0].max_observation_delay_cycles == 8);
    bfin_skip_cycles(core, 5); assert(rd(STAT) == 0); (void)rd(STAT); (void)rd(CTL);
    bf531_timing_read(chip, &t);
    assert(t.watchdog[0].expiries == 1 && t.watchdog[0].stat_samples == 1);
    assert(!t.watchdog[0].expired_without_sample && !t.watchdog[0].pending_expiry);
    assert(t.watchdog[0].last_sample.scheduled_cycle == 12);
    assert(t.watchdog[0].last_sample.observed_cycle == 20);
    assert(t.watchdog[0].last_sample.stat_cycle == 25);
    assert(t.watchdog[0].max_expiry_to_stat_cycles == 13);
    wr(CTL, DISABLE); arm(4, 4); wr(CTL, DISABLE);
    bf531_timing_read(chip, &t); assert(t.watchdog[0].cancelled_before_expiry == 1);
    arm(1, 4); bfin_skip_cycles(core, 4); (void)rd(CTL); wr(CTL, DISABLE);
    bf531_timing_read(chip, &t); assert(t.watchdog[0].expired_without_sample == 1);

    fresh(); assert(!bf531_timing_enable(chip, 1, 2));
    wr(PLL_DIV, 0x15); arm(3, 4); /* due ceil(15/2)=8 */
    bfin_skip_cycles(core, 2); assert(rd(STAT) == 3); /* retained fractional phase */
    bfin_skip_cycles(core, 7); (void)rd(CTL); (void)rd(STAT);
    bf531_timing_read(chip, &t);
    assert(t.watchdog[0].last_sample.scheduled_cycle == 8);
    assert(t.watchdog[0].last_sample.observed_cycle == 9);
    assert(t.watchdog[0].max_expiry_to_stat_cycles == 1);

    /* An enabled STAT write cancels exactly one prior arm and starts a new ID. */
    fresh(); assert(!bf531_timing_enable(chip, 1, 2)); arm(2, 4);
    bfin_skip_cycles(core, 2); wr(STAT, 0); bfin_skip_cycles(core, 8); (void)rd(STAT);
    bf531_timing_read(chip, &t);
    assert(t.watchdog[0].arms == 2 && t.watchdog[0].cancelled_before_expiry == 1);
    assert(t.watchdog[0].last_sample.arm == 2 && t.watchdog[0].last_sample.scheduled_cycle == 10);
    wr(STAT, 0); /* reload retains WDRO; telemetry still observes this new arm */
    bfin_skip_cycles(core, 8); (void)rd(STAT); (void)rd(STAT);
    bf531_timing_read(chip, &t);
    assert(t.watchdog[0].arms == 3 && t.watchdog[0].expiries == 2);
    assert(t.watchdog[0].stat_samples == 2 && t.watchdog[0].last_sample.arm == 3);
    assert(t.watchdog[0].last_sample.scheduled_cycle == 18);

    /* Expiry is boot-owned even when observation first occurs after the marker. */
    fresh(); assert(!bf531_timing_enable(chip, 1, 2)); arm(1, 4);
    bfin_skip_cycles(core, 8); bfin_set_wake(core, 1);
    bfin_stepi(core); bfin_stepi(core); /* deliberately no SoC watchdog sync */
    bf531_timing_read(chip, &t); assert(t.core.start_seen && !t.watchdog[1].carried_arms);
    (void)rd(STAT); bf531_timing_read(chip, &t);
    assert(t.watchdog[0].stat_samples == 1 && !t.watchdog[1].expiries);
    assert(t.watchdog[0].last_sample.scheduled_cycle < t.core.start_cycle);

    fresh(); assert(!bf531_timing_enable(chip, 1, 2)); arm(2, 4);
    bfin_set_wake(core, 1); bf531_stepi(chip); bf531_stepi(chip);
    bfin_skip_cycles(core, 8); (void)rd(STAT);
    bf531_timing_read(chip, &t);
    assert(t.core.start_seen && t.watchdog[0].arms == 1);
    assert(!t.watchdog[1].arms && t.watchdog[1].carried_arms == 1);
    assert(t.watchdog[1].expiries == 1 && t.watchdog[1].stat_samples == 1);
    FILE *json = tmpfile(); assert(json && !bf531_timing_json(chip, json));
    rewind(json); char text[8192]; size_t length = fread(text, 1, sizeof(text)-1, json);
    text[length] = 0; fclose(json);
    assert(strstr(text, "\"schema\":1") && strstr(text, "\"status\":\"measured\""));
    assert(strstr(text, "\"cycles_per_second\":400000000"));
    size_t n = bf531_save(chip, NULL); uint8_t *snap = malloc(n); assert(snap);
    bf531_save(chip, snap); bf531_load(chip, snap); free(snap);
    bf531_timing_read(chip, &t); assert(t.core.snapshot_loaded);
    json = tmpfile(); assert(json && !bf531_timing_json(chip, json)); rewind(json);
    length = fread(text, 1, sizeof(text)-1, json); text[length] = 0; fclose(json);
    assert(strstr(text, "\"snapshot_loaded\":true") && strstr(text, "\"status\":\"unavailable\""));

    fresh(); assert(!bf531_timing_enable(chip, 0, 0));
    json = tmpfile(); assert(json && !bf531_timing_json(chip, json)); rewind(json);
    length = fread(text, 1, sizeof(text)-1, json); text[length] = 0; fclose(json);
    assert(strstr(text, "\"start_pc\":null") && strstr(text, "\"status\":\"unavailable\""));
}

int main(void)
{
    fresh();
    assert(rd(CTL) == 0xad0 && rd(STAT) == 0);
    wr(CNT, 100);
    assert(rd(STAT) == 100);
    bf531_bus_write(chip, CTL, 4, 4); /* wrong widths never enable/change */
    bf531_bus_write(chip, CNT, 1, 2);
    assert(rd(CTL) == 0xad0 && rd(CNT) == 100);
    assert(bf531_bus_read(chip, CTL, 4) == 0);
    assert(bf531_bus_read(chip, CTL + 2, 2) == 0);
    wr(STAT, 99); /* disabled STAT writes ignored */
    assert(rd(STAT) == 100);
    wr(CTL, 4);
    wr(CNT, 1); /* enabled CNT writes ignored */
    advance(39);
    assert(rd(CNT) == 100 && rd(STAT) == 91);
    wr(STAT, 0xdeadbeef); /* any value reloads CNT, including fractional phase */
    assert(rd(STAT) == 100);
    advance(3);
    assert(rd(STAT) == 100);
    advance(1);
    assert(rd(STAT) == 99);
    wr(CTL, DISABLE);
    uint32_t retained = rd(STAT);
    advance(4000);
    assert(rd(STAT) == retained && !(rd(CTL) & WDRO));

    /* Masked peripheral IRQ remains a wake source. No handler is taken;
     * elapsed budgets advance, while architectural CYCLES stops in IDLE. */
    fresh();
    wr(IWR, WD_IRQ);
    arm(100, 4);
    idle();
    assert(core->idle);
    uint64_t before = cycles(), elapsed = bfin_cycles(core);
    assert(bf531_run(chip, 100) == BFIN_STOP_BUDGET);
    assert(core->idle && cycles() == before && bfin_cycles(core) == elapsed + 100);
    assert(bf531_stepi(chip) == BFIN_STOP_IDLE);
    assert(!core->idle && cycles() == before);
    assert(rd(STAT) == 0 && (rd(CTL) & WDRO));
    assert((rd(ISR) & WD_IRQ) && !(rd(IMASK) & WD_IRQ));
    assert(!(core->ipend & (1u << 13)) && !(core->ivg_level & (1u << 13)));
    wr(IMASK, WD_IRQ);
    assert(core->ivg_level & (1u << 13)); /* default SIC assignment */
    wr(IMASK, 0);
    assert(!(core->ivg_level & (1u << 13)) && core->wake_level);
    advance(100);
    assert(rd(STAT) == 0); /* stops at zero, no automatic reload */
    wr(CTL, 4 | WDRO); /* clearing while enabled is ignored */
    assert(rd(CTL) & WDRO);
    wr(CTL, DISABLE);
    assert(rd(CTL) & WDRO);
    wr(CTL, DISABLE | WDRO);
    assert(!(rd(CTL) & WDRO) && !(rd(ISR) & WD_IRQ) && !core->wake_level);
    idle();
    assert(core->idle && bf531_run(chip, 100) == BFIN_STOP_IDLE);

    /* Pending level before IDLE and enabling IWR after expiration both wake. */
    fresh();
    wr(IWR, 0);
    arm(4, 4);
    idle();
    bf531_stepi(chip);
    assert(core->idle && (rd(ISR) & WD_IRQ) && !core->wake_level);
    wr(IWR, WD_IRQ);
    assert(!core->idle && core->wake_level);
    idle();
    assert(!core->idle);
    wr(IWR, 0);
    idle();
    assert(core->idle);

    /* A different peripheral may wake early; WDOG keeps its residual count. */
    fresh();
    wr(IWR, WD_IRQ | (1u << 16));
    arm(1000, 4);
    wr(TIMER0, 0x11); /* IRQ-enabled PWM */
    wr(TIMER0 + 8, 10);
    wr(TIMER_ENABLE, 1);
    idle();
    before = cycles();
    assert(bf531_stepi(chip) == BFIN_STOP_IDLE && !core->idle);
    assert(cycles() == before && rd(STAT) > 980 && rd(STAT) < 1000);
    assert(!(rd(ISR) & WD_IRQ) && (rd(ISR) & (1u << 16)));
    wr(TIMER_DISABLE, 1);
    wr(TIMER_STATUS, 1);
    assert(!core->wake_level);

    /* Divider phase, maximum count arithmetic and full model snapshots. */
    fresh();
    wr(PLL_DIV, 0x15); /* SCLK/CCLK = 2/5 */
    arm(UINT32_MAX, 4);
    advance(2);
    assert(rd(STAT) == UINT32_MAX);
    size_t length = bf531_save(chip, NULL);
    uint8_t *snapshot = malloc(length);
    assert(snapshot);
    bf531_save(chip, snapshot);
    advance(1);
    assert(rd(STAT) == UINT32_MAX - 1);
    wr(IWR, 0);
    arm(1, 4);
    advance(3);
    assert(rd(CTL) & WDRO);
    bf531_load(chip, snapshot);
    free(snapshot);
    assert(rd(STAT) == UINT32_MAX && !(rd(CTL) & WDRO));
    assert(rd(IWR) == UINT32_MAX && !core->wake_level);
    advance(1);
    assert(rd(STAT) == UINT32_MAX - 1); /* fractional SCLK phase restored */
    advance((uint64_t)(UINT32_MAX - 1) * 5 / 2 + 3);
    assert(rd(STAT) == 0 && (rd(ISR) & WD_IRQ));

    /* Restore an in-progress sleep, then wake at the same elapsed deadline. */
    fresh();
    arm(30, 4);
    idle();
    assert(core->idle);
    bf531_run(chip, 10);
    before = cycles();
    elapsed = bfin_cycles(core);
    length = bf531_save(chip, NULL);
    snapshot = malloc(length);
    assert(snapshot);
    bf531_save(chip, snapshot);
    bf531_stepi(chip);
    uint64_t wake_at = bfin_cycles(core);
    assert(!core->idle && (rd(ISR) & WD_IRQ));
    bf531_load(chip, snapshot);
    free(snapshot);
    assert(core->idle && cycles() == before && bfin_cycles(core) == elapsed);
    assert(!(rd(ISR) & WD_IRQ) && !core->wake_level);
    bf531_stepi(chip);
    assert(!core->idle && bfin_cycles(core) == wake_at && cycles() == before);

    /* Zero and non-key WDEN; event-disabled mode expires without SIC wake. */
    fresh();
    arm(0, 6);
    assert((rd(CTL) & WDRO) && !(rd(ISR) & WD_IRQ));
    wr(CTL, DISABLE);
    wr(CTL, DISABLE | WDRO);
    wr(CNT, 1);
    wr(CTL, 0xac4); /* only 0xAD disables */
    advance(4);
    assert((rd(CTL) & WDRO) && (rd(ISR) & WD_IRQ));
    uint8_t peek[2];
    assert(!bf531_peek(chip, CTL, peek, sizeof(peek)));
    assert(((uint32_t)peek[0] | (uint32_t)peek[1] << 8) == rd(CTL));

    /* Reset/NMI are deliberately outside this qualification model. A clear
     * diagnostic and terminal stop must prevent silent workflow acceptance. */
    for (unsigned event = 0; event <= 2; event += 2) {
        fresh();
        arm(1, event);
        assert(bf531_run(chip, 10) == BFIN_STOP_UNDEF);
        assert(bf531_stepi(chip) == BFIN_STOP_UNDEF);
        assert(!(rd(ISR) & WD_IRQ));
    }
    backlight_latch_wake_policy();
    ownership_timing();
    bf531_free(chip);
    puts("PASS watchdog MMIO, SCLK, masked/early/level wake, snapshots and unsupported faults");
    return 0;
}
