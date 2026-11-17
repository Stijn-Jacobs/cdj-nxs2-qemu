/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ADSP-BF531 SoC around the Blackfin core, as the CDJ-2000/CDJ-2000NXS
 * display board uses it: SDRAM, L1, the boot flash on async bank 0, the SIC,
 * the three GP timers, the PPI with its DMA channel, SPI, GPIO and the EBIU.
 * SPORT1 carries the MAIN link through its DMA channels. No QEMU dependency.
 */
#ifndef BF531_H
#define BF531_H

#include "bfin.h"
#include <stddef.h>

typedef struct bf531 bf531;

typedef struct bf531_host {
    void *opaque;
    /* One PPI frame, RGB555 pixels, row after row. */
    void (*frame)(void *opaque, const uint16_t *px, unsigned w, unsigned h);
    /* One SPORT1 TX packet for MAIN, as the firmware armed DMA4 with it. */
    void (*sport1_tx)(void *opaque, const uint8_t *data, size_t len);
} bf531_host;

/* The core clock the firmware assumes: its core timer period of 400,000 is
 * its 1 ms tick. */
#define BF531_CCLK_HZ 400000000ull

bf531 *bf531_new(uint32_t sdram_size, const bf531_host *host, FILE *log);
void   bf531_free(bf531 *s);

typedef struct bf531_timing_sample {
    uint64_t arm, scheduled_cycle, observed_cycle, stat_cycle;
} bf531_timing_sample;
typedef struct bf531_watchdog_timing {
    uint64_t arms, carried_arms, expiries, stat_samples, cancelled_before_expiry;
    uint64_t expired_without_sample, max_observation_delay_cycles;
    uint64_t max_expiry_to_stat_cycles, total_expiry_to_stat_cycles;
    int pending_expiry;
    bf531_timing_sample last_sample, worst_sample;
} bf531_watchdog_timing;
typedef struct bf531_ownership_timing {
    bfin_ownership_timing core;
    bf531_watchdog_timing watchdog[2];
    uint64_t overflow;
} bf531_ownership_timing;
/* Opt-in, cold-boot host observations only. The caller binds start_pc to its
 * exact firmware ELF. No configured/observed marker means runtime unavailable.
 * bf531_timing_json writes them (CDJ_BF531_DUMP's ownership-timing.json): per
 * phase, boot before the marker and runtime after it, the spans with every
 * maskable CEC level masked (imask) and with IPEND's global bit set
 * (ipend_global), in elapsed model cycles at 400 MHz with IDLE included, and
 * the watchdog's arms, expiries and first STAT reads after expiry. Only
 * completed spans count; one cut by the marker or the dump is reported as
 * censored. A snapshot restore makes the runtime phase unavailable. */
int bf531_timing_enable(bf531 *s, int start_configured, uint32_t start_pc);
void bf531_timing_read(bf531 *s, bf531_ownership_timing *out);
int bf531_timing_json(bf531 *s, FILE *out);

/* Bounded test-only PPI scheduling delay. Configure once, before DMA0 has
 * been enabled. Only schedules after an observed cold-boot timing marker
 * qualify; pixel reads and completion publication still happen at due time. */
typedef struct bf531_ppi_injection {
    uint32_t delay_us, requested_count, applied_count;
    uint64_t scheduled_cycle, original_due_cycle, delayed_due_cycle;
} bf531_ppi_injection;
/* Fault injection (CDJ_BF531_PPI_DELAY_US, _COUNT): once the timing marker has
 * run in a cold boot, the next count PPI DMA0 schedules finish delay_us later
 * in model time. Pixels, DONE and the interrupt keep their order; only the
 * due time moves. bf531_ppi_delay_json writes what was applied. */
int bf531_ppi_delay_configure(bf531 *s, uint32_t delay_us, uint32_t count);
void bf531_ppi_delay_read(const bf531 *s, bf531_ppi_injection *out);
int bf531_ppi_delay_json(const bf531 *s, FILE *out);

/* A Pioneer GUI update section (or a whole .UPD, whose first section it is): a 0x20-byte title, then the LDR boot stream
 * and the resources the firmware reads back from flash. The stream is loaded
 * as the boot loader would load it from flash, and the section is placed
 * in flash where the firmware reads it back. Returns 0, or -1 when it is
 * not an LDR stream. */
int bf531_load_update(bf531 *s, const uint8_t *img, size_t len);

/* The flash (2 MiB, x16, AMD command set, 32 x 64 KiB sectors; see bf531.c).
 * bf531_flash_load takes exactly 2 MiB (0, or -1). bf531_flash_boot runs the
 * LDR stream at flash offset 0x10000 as the boot ROM would (0, or -1).
 * bf531_flash_save writes the 2 MiB array to path. Erases are logged to the
 * log as they happen; bf531_flash_report logs the totals. */
int  bf531_flash_load(bf531 *s, const uint8_t *img, size_t len);
int  bf531_flash_boot(bf531 *s);
int  bf531_flash_save(const bf531 *s, const char *path);
void bf531_flash_report(bf531 *s);

/* Counts the distinct 256-byte code lines in [lo, hi) the core executes;
 * bf531_code_range_report logs "RANGE 0x<lo>:0x<hi> lines_run=<N>". Slower
 * while on (no block chains). Call before running. */
void bf531_code_range(bf531 *s, uint32_t lo, uint32_t hi);
void bf531_code_range_report(bf531 *s);

/* A raw bus access as the core would make it, for host tests. */
uint32_t bf531_bus_read(bf531 *s, uint32_t addr, unsigned size);
void     bf531_bus_write(bf531 *s, uint32_t addr, uint32_t val, unsigned size);

/* Hands the SoC one SPORT1 RX packet from MAIN: it lands the next time the
 * firmware arms DMA3, wherever it points and however many halfwords it asks
 * for (extra bytes are dropped, a short packet is zero-padded by the DMA's
 * own byte count). When DMA3 is already armed the packet lands at once.
 * NULL clears a standing packet without delivering it. */
void bf531_sport1_rx(bf531 *s, const uint8_t *data, size_t len);

/* The PF pins the firmware drives as outputs, bit n for PFn; inputs read 0. */
uint16_t bf531_flags(const bf531 *s);

/* Runs the chip for about n elapsed core-clock cycles. CYCLES and the core
 * timer stop in IDLE; SCLK peripherals continue. Returns early on an
 * unimplemented instruction or unsupported watchdog reset/NMI expiry
 * (BFIN_STOP_UNDEF, with a model diagnostic), or IDLE with no event left. */
bfin_stop bf531_run(bf531 *s, uint64_t n);

/* Snapshots: the core, the SoC's registers and all its RAM as one image,
 * between two bf531_run calls; bf531_save(s, NULL) returns its size. The flash
 * (contents and command state) is in it. Only the
 * build that wrote an image can read it. */
size_t bf531_save(const bf531 *s, uint8_t *buf);
void   bf531_load(bf531 *s, const uint8_t *buf);

/* Like bf531_run for exactly one instruction or bundle (bfin_stepi). */
bfin_stop bf531_stepi(bf531 *s);
/* Debugger memory access: 0, or -1 when part of the range is not mapped (peek)
 * or not RAM (poke). Peeking has no side effect on the chip. */
int bf531_peek(bf531 *s, uint32_t addr, uint8_t *buf, size_t len);
int bf531_poke(bf531 *s, uint32_t addr, const uint8_t *buf, size_t len);

bfin_core *bf531_core(bf531 *s);
/* Receives armed too small for the standing SPORT1 record (it waited). */
uint64_t   bf531_sport1_waits(const bf531 *s);
uint64_t   bf531_frames(const bf531 *s);
uint8_t *bf531_mem(bf531 *s, uint32_t addr, size_t len);
const uint8_t *bf531_sdram(const bf531 *s, uint32_t *size);

#endif
