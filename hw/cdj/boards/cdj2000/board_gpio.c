/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
/*
 * The board's port latch at 0xFFF10000: 16-bit registers MAIN reads back and
 * modifies bit by bit. Two of them carry wires to the DSP, as the uploader
 * (0x041FC156 and 0x041FC198) uses them:
 *   +0x5C bits 1:0  out: the command MAIN gives the DSP's loader
 *   +0x40 bit 4     in:  the DSP's HINT pin (active low), MAIN waits for it
 *                        to go low
 * Both DSP boards use the same two wires (the CDJ-2000 from 0x041C743C).
 * +0x60 bit 2 is the LED the firmware blinks when it has crashed.
 * The rest store and read back, which is all the boot asks of them.
 */

#define LATCH_BASE      0xFFF10000
#define LATCH_SIZE      0x100
#define REG_STATUS      0x40
#define STATUS_DSP_BUSY (1u << 4)
#define REG_DSP_CMD     0x5C
#define DSP_CMD_MASK    3u

typedef struct CdjLatch {
    MemoryRegion iomem;
    uint16_t reg[LATCH_SIZE / 2];
    const CdjDspWires *dsp;
} CdjLatch;

static uint64_t latch_read(void *opaque, hwaddr off, unsigned size)
{
    CdjLatch *s = opaque;
    uint32_t val = s->reg[off / 2];

    if (off == REG_STATUS && s->dsp) {
        val &= ~STATUS_DSP_BUSY;
        if (s->dsp->busy(s->dsp->opaque)) {
            val |= STATUS_DSP_BUSY;
        }
    }
    return val;
}

static void latch_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjLatch *s = opaque;

    s->reg[off / 2] = val;
    if (off == REG_DSP_CMD && s->dsp) {
        s->dsp->command(s->dsp->opaque, val & DSP_CMD_MASK);
    }
}

static const MemoryRegionOps latch_ops = {
    .read = latch_read,
    .write = latch_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 2, .max_access_size = 2 },
};

/* Over the unimplemented GPIO block, which keeps logging everything else. */
void cdj2000_latch_init(MemoryRegion *sysmem, const CdjDspWires *dsp)
{
    CdjLatch *s = g_new0(CdjLatch, 1);

    s->dsp = dsp;
    memory_region_init_io(&s->iomem, NULL, &latch_ops, s, "cdj2000.latch",
                          LATCH_SIZE);
    memory_region_add_subregion_overlap(sysmem, LATCH_BASE, &s->iomem, 1);
}
