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

/* A Pioneer GUI update section (or a whole .UPD, whose first section it is): a 0x20-byte title, then the LDR boot stream
 * and the resources the firmware reads back from flash. The stream is loaded
 * as the boot loader would load it from flash, and the section is placed
 * in flash where the firmware reads it back. Returns 0, or -1 when it is
 * not an LDR stream. */
int bf531_load_update(bf531 *s, const uint8_t *img, size_t len);

/* Hands the SoC one SPORT1 RX packet from MAIN: it lands the next time the
 * firmware arms DMA3, wherever it points and however many halfwords it asks
 * for (extra bytes are dropped, a short packet is zero-padded by the DMA's
 * own byte count). When DMA3 is already armed the packet lands at once.
 * NULL clears a standing packet without delivering it. */
void bf531_sport1_rx(bf531 *s, const uint8_t *data, size_t len);

/* The PF pins the firmware drives as outputs, bit n for PFn; inputs read 0. */
uint16_t bf531_flags(const bf531 *s);

/* Runs the chip for about n core cycles; returns early on an unimplemented
 * instruction (BFIN_STOP_UNDEF) or when it idles with no event left. */
bfin_stop bf531_run(bf531 *s, uint64_t n);

bfin_core *bf531_core(bf531 *s);
uint64_t   bf531_frames(const bf531 *s);
const uint8_t *bf531_sdram(const bf531 *s, uint32_t *size);

#endif
