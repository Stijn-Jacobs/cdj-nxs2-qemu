/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ_DSP_EDMA3_H
#define CDJ_DSP_EDMA3_H
#include "qemu/osdep.h"

#define EDMA3_BASE      0x01C00000u
#define EDMA3_SIZE      0x8000u

typedef struct CdjEdma3 {
    uint32_t w[EDMA3_SIZE / 4];
    /* The host byte behind a DSP address, NULL for anything but RAM. */
    uint8_t *(*ram)(void *chip, uint32_t addr);
    /* Called after a transfer has changed RAM behind ram(). */
    void (*stored)(void *chip, uint32_t addr, uint32_t len);
    void (*write_word)(void *chip, uint32_t addr, uint32_t val);
    void (*set_irq)(void *chip, int level);
    void *chip;
    int64_t deadline_ns;
    uint64_t events, completions;
} CdjEdma3;

uint32_t cdj_edma3_read(const CdjEdma3 *e, uint32_t addr);
void cdj_edma3_write(CdjEdma3 *e, uint32_t addr, uint32_t val);
void cdj_edma3_paced(CdjEdma3 *e, int64_t dsp_ns);

#endif
