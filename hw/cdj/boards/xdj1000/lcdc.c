/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xdj1000.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "exec/cpu-common.h"
#include "ui/console.h"
/*
 * The SH7724 LCD controller at 0xFE940000. The registers keep what is
 * written to them. LDPMR answers the panel driver's wait for the power
 * sequence it starts with LDCNTR: it reads 3 once the display is on and 0
 * once it is off. While the display is on, a frame timer sets the status
 * bits of the interrupts that LDINTR enables, and the line stays raised until
 * the handler writes 0 over them.
 *
 * The window shows the panel's 800x480 RGB565 frame buffer, read from the
 * bus address in LDSA1R with the line stride in LDMLSR.
 */
#define LCDC_BASE       0xFE940000
#define LCDC_SIZE       0x10000
#define LDPMR           0x460
#define LDPMR_POWERED   3
#define LDCNTR          0x474
#define LDCNTR_DON      (1u << 0)
#define LDINTR          0x468
#define LDINTR_STATUS   0x7f
#define LDINTR_ENABLE_SHIFT 8
#define LDSA1R          0x430
#define LDMLSR          0x438
#define LCD_W           800
#define LCD_H           480
#define FRAME_PERIOD_NS (1000000000 / 60)

typedef struct XdjLcdc {
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *frame_timer;
    QemuConsole *con;
    uint8_t reg[LCDC_SIZE];
} XdjLcdc;

static void lcdc_update_irq(XdjLcdc *s)
{
    uint32_t ldintr = ldl_le_p(&s->reg[LDINTR]);

    qemu_set_irq(s->irq, (ldintr & (ldintr >> LDINTR_ENABLE_SHIFT)
                          & LDINTR_STATUS) != 0);
}

static void lcdc_frame(void *opaque)
{
    XdjLcdc *s = opaque;
    uint32_t ldintr = ldl_le_p(&s->reg[LDINTR]);

    if (s->reg[LDCNTR] & LDCNTR_DON) {
        stl_le_p(&s->reg[LDINTR],
                 ldintr | ((ldintr >> LDINTR_ENABLE_SHIFT) & LDINTR_STATUS));
        lcdc_update_irq(s);
    }
    timer_mod(s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_PERIOD_NS);
}

static uint64_t lcdc_read(void *opaque, hwaddr off, unsigned size)
{
    XdjLcdc *s = opaque;

    if (off == LDPMR) {
        return s->reg[LDCNTR] & LDCNTR_DON ? LDPMR_POWERED : 0;
    }
    return ldn_le_p(&s->reg[off], size);
}

static void lcdc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    XdjLcdc *s = opaque;

    if (off == LDINTR && size == 4) {
        /* the status bits clear by writing 0 and are never set by a write */
        val = (val & ~LDINTR_STATUS) | (val & ldl_le_p(&s->reg[LDINTR])
                                         & LDINTR_STATUS);
    }
    stn_le_p(&s->reg[off], size, val);
    if (off == LDINTR) {
        lcdc_update_irq(s);
    }
}

static void lcdc_gfx_update(void *opaque)
{
    XdjLcdc *s = opaque;
    uint32_t *dst = surface_data(qemu_console_surface(s->con));
    hwaddr base = ldl_le_p(&s->reg[LDSA1R]);
    uint32_t stride = ldl_le_p(&s->reg[LDMLSR]);
    uint16_t line[LCD_W];

    if (!(s->reg[LDCNTR] & LDCNTR_DON)) {
        return;
    }
    for (unsigned y = 0; y < LCD_H; y++) {
        cpu_physical_memory_read(base + y * stride, line, sizeof(line));
        for (unsigned x = 0; x < LCD_W; x++) {
            uint16_t p = le16_to_cpu(line[x]);

            dst[y * LCD_W + x] = 0xFF000000 | ((p >> 11) * 255 / 31) << 16 |
                                 ((p >> 5 & 63) * 255 / 63) << 8 |
                                 (p & 31) * 255 / 31;
        }
    }
    dpy_gfx_update_full(s->con);
}

static const GraphicHwOps lcdc_gfx_ops = {
    .gfx_update = lcdc_gfx_update,
};

static const MemoryRegionOps lcdc_ops = {
    .read = lcdc_read,
    .write = lcdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

void xdj1000_lcdc_init(MemoryRegion *sysmem, qemu_irq irq)
{
    XdjLcdc *s = g_new0(XdjLcdc, 1);

    memory_region_init_io(&s->iomem, NULL, &lcdc_ops, s, "sh7724.lcdc",
                          LCDC_SIZE);
    memory_region_add_subregion(sysmem, LCDC_BASE, &s->iomem);
    s->irq = irq;
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lcdc_frame, s);
    timer_mod(s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + FRAME_PERIOD_NS);
    s->con = graphic_console_init(NULL, 0, &lcdc_gfx_ops, s);
    qemu_console_resize(s->con, LCD_W, LCD_H);
}
