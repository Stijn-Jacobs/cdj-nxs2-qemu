/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual PPI DMA scheduling/completion; no firmware or emulator workflows. */
#include "bf531.h"
#include "bfin_priv.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DMA0 0xffc00c00u
#define START 4
#define CONFIG 8
#define XCOUNT 0x10
#define XMOD 0x14
#define YCOUNT 0x18
#define YMOD 0x1c
#define IRQ_STATUS 0x28
#define SIC_ISR 0xffc00120u
#define TARGET 0x1000
#define DONE 1
#define RUN 8
#define PERIOD (64 * (BF531_CCLK_HZ / 60 / 262))
static bf531 *chip;
static bfin_core *core;
static unsigned frames;
static uint16_t expected;
static uint32_t rd(unsigned off) { return bf531_bus_read(chip, DMA0 + off, 4); }
static void wr(unsigned off, uint32_t value) { bf531_bus_write(chip, DMA0 + off, value, 4); }
static void frame(void *opaque, const uint16_t *pixels, unsigned w, unsigned h)
{
    assert(w == 64 && h == 64);
    assert((rd(IRQ_STATUS) & (RUN | DONE)) == RUN); /* pixels before DONE */
    assert(!(bf531_bus_read(chip, SIC_ISR, 4) & (1u << 8)));
    for (unsigned i = 0; i < w * h; i++) { assert(pixels[i] == expected); }
    frames++;
}
static void fresh(void)
{
    bf531_host host = { .frame = frame };
    if (chip) { bf531_free(chip); }
    chip = bf531_new(0x10000, &host, NULL); core = bf531_core(chip);
    bfin_reset(core, 0); /* zero SDRAM: NOPs */
    frames = 0; expected = 0;
    wr(START, TARGET); wr(XCOUNT, 64); wr(XMOD, 2); wr(YCOUNT, 64); wr(YMOD, 2);
}
static void pixels(uint16_t value)
{
    expected = value;
    uint16_t *p = (uint16_t *)bf531_mem(chip, TARGET, 64 * 64 * 2);
    for (unsigned i = 0; i < 64 * 64; i++) { p[i] = value; }
}
static bf531_ppi_injection witness(void)
{
    bf531_ppi_injection result; bf531_ppi_delay_read(chip, &result); return result;
}
static void marker(uint32_t pc)
{
    bfin_set_pc(core, pc); assert(bfin_stepi(core) == BFIN_STOP_STEP);
    assert(bfin_timing_started(core));
}
static void at(uint64_t cycle)
{
    assert(cycle > bfin_cycles(core));
    bfin_skip_cycles(core, cycle - bfin_cycles(core) - 1);
    assert(bf531_stepi(chip) == BFIN_STOP_STEP);
    assert(bfin_cycles(core) == cycle);
}
static void done(unsigned count)
{
    assert(frames == count && bf531_frames(chip) == count);
    assert(rd(IRQ_STATUS) & DONE);
    assert(bf531_bus_read(chip, SIC_ISR, 4) & (1u << 8));
    wr(IRQ_STATUS, DONE);
}
static void json_status(const char *status, const char *last)
{
    FILE *f = tmpfile(); char bytes[1024]; assert(f);
    assert(!bf531_ppi_delay_json(chip, f)); rewind(f);
    size_t n = fread(bytes, 1, sizeof(bytes) - 1, f); bytes[n] = 0; fclose(f);
    assert(strstr(bytes, status) && strstr(bytes, last));
}
int main(int argc, char **argv)
{
    fresh(); assert(!witness().delay_us && !witness().applied_count);
    wr(CONFIG, 0x91); at(PERIOD - 1); assert(!frames && rd(IRQ_STATUS) == RUN);
    pixels(0x1234); at(PERIOD); done(1); assert(!witness().applied_count);
    assert(bf531_ppi_delay_configure(chip, 1, 1) == -1); /* ever-started */

    fresh(); assert(bf531_ppi_delay_configure(chip, 50001, 1) == -1);
    assert(bf531_ppi_delay_configure(chip, 1, 1025) == -1);
    assert(bf531_ppi_delay_configure(chip, 1, 0) == -1);
    assert(!bf531_ppi_delay_configure(chip, 50000, 1024));
    assert(bf531_ppi_delay_configure(chip, 1, 1) == -1); /* one shot */
    assert(!bf531_timing_enable(chip, 1, 4));
    wr(CONFIG, 0x91); at(PERIOD); done(1); /* unseen marker: no delay */
    assert(!witness().applied_count);
    json_status("\"status\":\"unavailable\"", "\"last\":null");

    fresh(); assert(!bf531_ppi_delay_configure(chip, 50000, 1));
    assert(!bf531_timing_enable(chip, 0, 0));
    wr(CONFIG, 0x91); at(PERIOD); done(1); assert(!witness().applied_count);
    fresh(); assert(!bf531_ppi_delay_configure(chip, 0, 32));
    assert(!bf531_timing_enable(chip, 1, 0)); marker(0);
    wr(CONFIG, 0x91); at(1 + PERIOD); done(1); assert(!witness().applied_count);

    fresh(); assert(!bf531_ppi_delay_configure(chip, 25000, 2));
    assert(!bf531_timing_enable(chip, 1, 4));
    wr(CONFIG, 0x1091); /* recurring 2D PPI; boot schedule unchanged */
    assert(!witness().applied_count); marker(4);
    at(PERIOD); done(1);
    bf531_ppi_injection i = witness();
    assert(i.applied_count == 1 && i.scheduled_cycle == PERIOD);
    assert(i.original_due_cycle == 2 * PERIOD);
    assert(i.delayed_due_cycle == i.original_due_cycle + 10000000);
    at(i.original_due_cycle); assert(frames == 1 && rd(IRQ_STATUS) == RUN);
    at(i.delayed_due_cycle - 1); assert(frames == 1 && rd(IRQ_STATUS) == RUN);
    pixels(0x7fff); at(i.delayed_due_cycle); done(2);
    i = witness(); assert(i.applied_count == 2);
    assert(i.original_due_cycle == i.scheduled_cycle + PERIOD);
    assert(i.delayed_due_cycle == i.original_due_cycle + 10000000);
    at(i.delayed_due_cycle); done(3);
    assert(witness().applied_count == 2); /* finite budget exhausted */
    at(i.delayed_due_cycle + PERIOD); done(4);
    assert(witness().applied_count == 2);
    json_status("\"status\":\"applied\"", "\"delayed_due_cycle\":");
    if (argc > 1) {
        FILE *sample = fopen(argv[1], "w");
        assert(sample && !bf531_ppi_delay_json(chip, sample)); fclose(sample);
    }

    fresh(); assert(!bf531_ppi_delay_configure(chip, 50000, 1));
    assert(!bf531_timing_enable(chip, 1, 0)); marker(0);
    size_t n = bf531_save(chip, NULL); uint8_t *snapshot = malloc(n); assert(snapshot);
    bf531_save(chip, snapshot); bf531_load(chip, snapshot); free(snapshot);
    assert(!bfin_timing_started(core)); wr(CONFIG, 0x91); at(1 + PERIOD); done(1);
    assert(!witness().applied_count); /* snapshots cannot satisfy gate */
    json_status("\"snapshot_loaded\":true", "\"status\":\"unavailable\"");

    /* Canceling an extended schedule publishes no pixels or completion. The
     * finite budget counts extended schedules even when firmware cancels one. */
    fresh(); assert(!bf531_ppi_delay_configure(chip, 50000, 1));
    assert(!bf531_timing_enable(chip, 1, 0)); marker(0);
    wr(CONFIG, 0x91); i = witness(); assert(i.applied_count == 1);
    at(i.original_due_cycle); assert(!frames); wr(CONFIG, 0);
    at(i.delayed_due_cycle + 1);
    assert(!frames && !rd(IRQ_STATUS));
    assert(!(bf531_bus_read(chip, SIC_ISR, 4) & (1u << 8)));
    uint64_t restart = bfin_cycles(core); wr(CONFIG, 0x91);
    at(restart + PERIOD); done(1); assert(witness().applied_count == 1);

    /* Other channels retain immediate SPORT delivery even with remaining budget. */
    fresh(); assert(!bf531_ppi_delay_configure(chip, 50000, 1));
    assert(!bf531_timing_enable(chip, 1, 0)); marker(0);
    const uint8_t packet[] = {1, 2, 3, 4};
    bf531_bus_write(chip, DMA0 + 3 * 0x40 + START, TARGET, 4);
    bf531_bus_write(chip, DMA0 + 3 * 0x40 + XCOUNT, 2, 4);
    bf531_bus_write(chip, DMA0 + 3 * 0x40 + XMOD, 2, 4);
    bf531_bus_write(chip, DMA0 + 3 * 0x40 + CONFIG, 0x81, 4);
    bf531_sport1_rx(chip, packet, sizeof(packet)); bf531_stepi(chip);
    assert(!memcmp(bf531_mem(chip, TARGET, 4), packet, 4));
    assert(bf531_bus_read(chip, DMA0 + 3 * 0x40 + IRQ_STATUS, 4) & DONE);
    assert(!witness().applied_count);
    bf531_free(chip);
    puts("ppi_delay_test: runtime gate, exact completion, finite budget, snapshots and unaffected SPORT passed");
    return 0;
}
