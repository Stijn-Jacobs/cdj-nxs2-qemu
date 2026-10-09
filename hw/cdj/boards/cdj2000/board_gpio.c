/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
/*
 * The board's port latch at 0xFFF10000: 16-bit registers MAIN reads back and
 * modifies bit by bit. Two of them carry wires to the DSP, as the uploader
 * (0x041FC3EA and 0x041FC42C) uses them:
 *   +0x5C bits 1:0  out: the command MAIN gives the DSP's loader
 *   +0x40 bit 4     in:  the DSP's HINT pin (active low), MAIN waits for it
 *                        to go low
 * Both DSP boards use the same two wires (the CDJ-2000 from 0x041C743C).
 * +0x60 bit 2 is the LED the firmware blinks when it has crashed. Bit 1 is
 * an input that reads 1 while the display processor is fitted: with it clear
 * the start-up panel check (0x0428D3CC) leaves the display mode word at 0 and
 * the send task (0x04215722) never counts its idle passes towards a packet.
 * Bit 4 is an active-low input that only the tick hook reads (0x042918D0,
 * 0x042FAA82 on the CDJ-2000NXS): every 51st tick that finds it low posts
 * event 0x92, which the display's message table maps to "USB Error. Remove
 * the device.", the same poll the CDJ-2000NXS2 runs on the overcurrent pin
 * of its USB power switch. Nothing writes it, so it reads high: no fault.
 * +0x48 bit 2 is the display processor's PF1 when it runs: the GUI link
 * tasks (0x04215268, 0x0421566E) start the link once it reads 0, and the
 * answer handler (0x042134CC) accepts an answer only while it reads 1.
 * The rest store and read back, which is all the boot asks of them.
 */

#define LATCH_BASE      0xFFF10000
#define LATCH_SIZE      0x100
#define REG_STATUS      0x40
#define STATUS_DSP_BUSY (1u << 4)
#define REG_DSP_CMD     0x5C
#define DSP_CMD_MASK    3u
#define REG_DISPLAY     0x48
#define DISPLAY_PF1     (1u << 2)
#define REG_PANEL       0x60
#define PANEL_DISPLAY_UP (1u << 1)
#define PANEL_USB_OC_N  (1u << 4)

typedef struct CdjLatch {
    MemoryRegion iomem;
    uint16_t reg[LATCH_SIZE / 2];
    const CdjDspWires *dsp;
    bool display;
} CdjLatch;

static void (*latch_watch_hook)(unsigned off);
static bool (*answer_pin_level)(void);
static uint16_t answer_pin_mask;

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
    if (off == REG_DISPLAY && s->display) {
        val &= ~DISPLAY_PF1;
        if (cdj2000_display_pf1()) {
            val |= DISPLAY_PF1;
        }
    }
    if (off == REG_DISPLAY && answer_pin_level) {
        val &= ~answer_pin_mask;
        if (answer_pin_level()) {
            val |= answer_pin_mask;
        }
    }
    if (off == REG_PANEL) {
        val |= PANEL_USB_OC_N;
        if (answer_pin_level) {
            val |= PANEL_DISPLAY_UP;
        }
    }
    return val;
}

static void latch_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjLatch *s = opaque;

    if (latch_watch_hook) {
        latch_watch_hook(off);
    }
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

static CdjLatch *cdj_latch;

void cdj2000_latch_watch(void (*fn)(unsigned off))
{
    latch_watch_hook = fn;
}

void cdj2000_latch_answer_pin(bool (*level)(void), uint16_t mask)
{
    answer_pin_level = level;
    answer_pin_mask = mask;
}

uint16_t cdj2000_latch_get(unsigned off)
{
    return cdj_latch->reg[off / 2];
}

static const VMStateDescription vmstate_latch = {
    .name = "cdj2000-latch",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(reg, CdjLatch, LATCH_SIZE / 2),
        VMSTATE_END_OF_LIST()
    }
};

/* Over the unimplemented GPIO block, which keeps logging everything else. */
void cdj2000_latch_init(MemoryRegion *sysmem, const CdjDspWires *dsp,
                        bool display)
{
    CdjLatch *s = g_new0(CdjLatch, 1);

    cdj_latch = s;

    s->dsp = dsp;
    s->display = display;
    if (display) {
        s->reg[REG_PANEL / 2] |= PANEL_DISPLAY_UP;
    }
    vmstate_register_any(NULL, &vmstate_latch, s);
    memory_region_init_io(&s->iomem, NULL, &latch_ops, s, "cdj2000.latch",
                          LATCH_SIZE);
    memory_region_add_subregion_overlap(sysmem, LATCH_BASE, &s->iomem, 1);
}
