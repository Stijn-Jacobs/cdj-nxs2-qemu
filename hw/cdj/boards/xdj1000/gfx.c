/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xdj1000.h"
#include "qemu/timer.h"
#include "hw/irq.h"
#include "qemu/log.h"
#include "exec/address-spaces.h"
/*
 * The SH7724 blocks the XDJ's display drivers program around the LCD
 * controller: the ICB control registers at 0xE8000000 with the 128 KiB MERAM
 * behind them, and the 2D graphics engine at 0x04680000. All of them keep
 * what is written to them. The firmware starts the 2DG by writing three
 * buffer addresses to 0x48, 0x50 and 0x54, a length to 0x5C, then 1 to 0x08 and
 * 0x00. Its driver waits for the transfer-end interrupt (TRI): the handler
 * reads bit 0 of the status word at 0x04 and clears it by writing 1 to 0x08.
 * The model sets that bit shortly after the start and draws nothing.
 */
#define ICB_BASE        0xE8000000
#define ICB_SIZE        0x1000
#define MERAM_BASE      0xE8080000
#define MERAM_SIZE      (128 * KiB)
#define GFX2D_BASE      0x04680000
#define GFX2D_SIZE      0x1000
#define GFX2D_START     0x00
#define GFX2D_STATUS    0x04
#define GFX2D_STATUS_CLR 0x08
#define GFX2D_TRI       (1u << 0)
#define GFX2D_RUN_NS    100000

typedef struct XdjRegs {
    MemoryRegion iomem;
    uint8_t *reg;
} XdjRegs;

static uint64_t regs_read(void *opaque, hwaddr off, unsigned size)
{
    XdjRegs *s = opaque;

    return ldn_le_p(&s->reg[off], size);
}

static void regs_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    XdjRegs *s = opaque;

    stn_le_p(&s->reg[off], size, val);
}

static const MemoryRegionOps regs_ops = {
    .read = regs_read,
    .write = regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void regs_init(MemoryRegion *sysmem, const char *name, hwaddr base,
                      hwaddr size)
{
    XdjRegs *s = g_new0(XdjRegs, 1);

    s->reg = g_malloc0(size);
    memory_region_init_io(&s->iomem, NULL, &regs_ops, s, name, size);
    memory_region_add_subregion(sysmem, base, &s->iomem);
}

typedef struct XdjGfx2d {
    XdjRegs regs;
    qemu_irq irq;
    QEMUTimer *done_timer;
} XdjGfx2d;

static void gfx2d_update_irq(XdjGfx2d *s)
{
    qemu_set_irq(s->irq, ldl_le_p(&s->regs.reg[GFX2D_STATUS]) & GFX2D_TRI);
}

static void gfx2d_done(void *opaque)
{
    XdjGfx2d *s = opaque;

    stl_le_p(&s->regs.reg[GFX2D_STATUS],
             ldl_le_p(&s->regs.reg[GFX2D_STATUS]) | GFX2D_TRI);
    gfx2d_update_irq(s);
}

/*
 * The list at 0x48 is words: 0x18 sets a register (offset, value), 0x82 blits
 * and the 0x12 word ends the list; 0x40 and 0x08 are skipped and 0xA0 fills
 * (gfx2d_shape). A blit is eight words: the command (bit 11 of its flags
 * turns the colour key in register 0x80 on), the source offset and size as
 * x << 16 | y, a zero, and the four destination corners, top left first.
 * Register 0x4C is the source bitmap and 0x58 its pitch, both RGB565, and
 * 0x50 the destination with the pitch in 0x5C. Only axis-aligned
 * destinations are drawn.
 */
#define GFX2D_LIST_MAX      1024
#define GFX2D_PHYS_MASK     0x1ffffff0
#define GFX2D_CMD_SET_REG   0x18
#define GFX2D_CMD_BLIT      0x82
#define GFX2D_CMD_END       0x12
#define GFX2D_CMD_SKIP      0x40
#define GFX2D_CMD_SHAPE     0xa0
#define GFX2D_BLIT_WORDS    8
#define GFX2D_SHAPE_WORDS   6
#define GFX2D_BLIT_KEYED    0x800
#define GFX2D_REG_LIST      0x48
#define GFX2D_REG_DEST      0x50
#define GFX2D_REG_DEST_PITCH 0x5c
#define GFX2D_REG_SRC       0x4c
#define GFX2D_REG_SRC_PITCH 0x58
#define GFX2D_REG_KEY       0x80
#define GFX2D_REG_CLIP_MIN  0xd4
#define GFX2D_REG_CLIP_MAX  0xd0

static uint32_t gfx2d_reg(XdjGfx2d *s, hwaddr off)
{
    return ldl_le_p(&s->regs.reg[off]);
}

static void gfx2d_blit(XdjGfx2d *s, const uint32_t *w)
{
    uint32_t src_xy = w[1], src_wh = w[2];
    int x0 = w[4] >> 16, y0 = w[4] & 0xffff, x1 = w[5] >> 16, y1 = w[7] & 0xffff;
    int clip_x0 = gfx2d_reg(s, GFX2D_REG_CLIP_MIN) >> 16;
    int clip_y0 = gfx2d_reg(s, GFX2D_REG_CLIP_MIN) & 0xffff;
    int clip_x1 = gfx2d_reg(s, GFX2D_REG_CLIP_MAX) >> 16;
    int clip_y1 = gfx2d_reg(s, GFX2D_REG_CLIP_MAX) & 0xffff;
    int src_w = src_wh >> 16, src_h = src_wh & 0xffff;
    int dst_w = x1 - x0 + 1, dst_h = y1 - y0 + 1;
    uint32_t src_pitch = gfx2d_reg(s, GFX2D_REG_SRC_PITCH) * 2;
    uint32_t dst_pitch = gfx2d_reg(s, GFX2D_REG_DEST_PITCH) * 2;
    hwaddr src = (gfx2d_reg(s, GFX2D_REG_SRC) & GFX2D_PHYS_MASK) +
                 (src_xy & 0xffff) * src_pitch + (src_xy >> 16) * 2;
    hwaddr dst = gfx2d_reg(s, GFX2D_REG_DEST) & GFX2D_PHYS_MASK;
    bool keyed = w[0] & GFX2D_BLIT_KEYED;
    uint16_t key = gfx2d_reg(s, GFX2D_REG_KEY);
    uint16_t src_row[1024], dst_row[1024];
    int x, y;

    if (dst_w <= 0 || dst_h <= 0 || dst_w > 1024 || src_w <= 0 || src_w > 1024
        || src_h <= 0) {
        return;
    }
    for (y = MAX(y0, clip_y0); y <= MIN(y1, clip_y1); y++) {
        int sy = (y - y0) * src_h / dst_h;
        int xa = MAX(x0, clip_x0), xb = MIN(x1, clip_x1);

        if (xa > xb) {
            continue;
        }
        address_space_read(&address_space_memory, src + sy * src_pitch,
                           MEMTXATTRS_UNSPECIFIED, src_row, src_w * 2);
        address_space_read(&address_space_memory,
                           dst + y * dst_pitch + xa * 2,
                           MEMTXATTRS_UNSPECIFIED, dst_row, (xb - xa + 1) * 2);
        for (x = xa; x <= xb; x++) {
            uint16_t px = le16_to_cpu(src_row[(x - x0) * src_w / dst_w]);

            if (!keyed || px != key) {
                dst_row[x - xa] = cpu_to_le16(px);
            }
        }
        address_space_write(&address_space_memory,
                            dst + y * dst_pitch + xa * 2,
                            MEMTXATTRS_UNSPECIFIED, dst_row,
                            (xb - xa + 1) * 2);
    }
}

/*
 * A 0xa0 command is six words: the command, the raster operation 0xcc, the
 * fill colour, then a rectangle around an origin: left << 16 | right extent,
 * up << 16 | down extent and the origin x << 16 | y, clipped to the clip
 * registers. The firmware issues three of them at the start of every frame,
 * W/2-1 | W/2 and H/2-1 | H/2 around (W/2-1, H/2-1), which clears the whole
 * destination bitmap and removes the boot logo's earlier positions from the
 * compose buffer. Mid-frame ones clear one part of a panel before it is
 * redrawn: on the CDJ-900NXS rows 234-351 of the 688x352 deck panel before
 * the time and track digits, then rows 0-233 before the waveform half, and
 * its 1-pixel position markers have no horizontal extent. Read as a box
 * spanned by three points, that second clear covers x 116-338, y 116-335 and
 * wipes the minutes of the time readout and the second track digit.
 */
static void gfx2d_fill(XdjGfx2d *s, int x0, int y0, int x1, int y1,
                       uint16_t colour)
{
    uint32_t dst_pitch = gfx2d_reg(s, GFX2D_REG_DEST_PITCH) * 2;
    hwaddr dst = gfx2d_reg(s, GFX2D_REG_DEST) & GFX2D_PHYS_MASK;
    uint16_t row[1024];
    int x, y;

    if (x1 < x0 || y1 < y0 || x1 - x0 >= 1024) {
        return;
    }
    for (x = 0; x <= x1 - x0; x++) {
        row[x] = cpu_to_le16(colour);
    }
    for (y = y0; y <= y1; y++) {
        address_space_write(&address_space_memory, dst + y * dst_pitch + x0 * 2,
                            MEMTXATTRS_UNSPECIFIED, row, (x1 - x0 + 1) * 2);
    }
}

static void gfx2d_shape(XdjGfx2d *s, const uint32_t *w)
{
    uint32_t clip_min = gfx2d_reg(s, GFX2D_REG_CLIP_MIN);
    uint32_t clip_max = gfx2d_reg(s, GFX2D_REG_CLIP_MAX);
    int ox = w[5] >> 16, oy = w[5] & 0xffff;
    int x0 = MAX((int)(clip_min >> 16), ox - (int)(w[3] >> 16));
    int x1 = MIN((int)(clip_max >> 16), ox + (int)(w[3] & 0xffff));
    int y0 = MAX((int)(clip_min & 0xffff), oy - (int)(w[4] >> 16));
    int y1 = MIN((int)(clip_max & 0xffff), oy + (int)(w[4] & 0xffff));

    gfx2d_fill(s, x0, y0, x1, y1, w[2]);
}

static void gfx2d_run_list(XdjGfx2d *s)
{
    uint32_t w[GFX2D_LIST_MAX];
    int i = 0;

    address_space_read(&address_space_memory,
                       gfx2d_reg(s, GFX2D_REG_LIST) & GFX2D_PHYS_MASK,
                       MEMTXATTRS_UNSPECIFIED, w, sizeof(w));
    while (i < GFX2D_LIST_MAX) {
        switch (le32_to_cpu(w[i]) >> 24) {
        case GFX2D_CMD_SET_REG:
            stl_le_p(&s->regs.reg[le32_to_cpu(w[i + 1]) & (GFX2D_SIZE - 1)],
                     le32_to_cpu(w[i + 2]));
            i += 3;
            break;
        case GFX2D_CMD_BLIT:
            if (i + GFX2D_BLIT_WORDS <= GFX2D_LIST_MAX) {
                uint32_t cmd[GFX2D_BLIT_WORDS];
                int k;

                for (k = 0; k < GFX2D_BLIT_WORDS; k++) {
                    cmd[k] = le32_to_cpu(w[i + k]);
                }
                gfx2d_blit(s, cmd);
            }
            i += GFX2D_BLIT_WORDS;
            break;
        case GFX2D_CMD_END:
            return;
        case GFX2D_CMD_SKIP:
            i += 2;
            break;
        case GFX2D_CMD_SHAPE:
            if (i + GFX2D_SHAPE_WORDS <= GFX2D_LIST_MAX) {
                uint32_t cmd[GFX2D_SHAPE_WORDS];
                int k;

                for (k = 0; k < GFX2D_SHAPE_WORDS; k++) {
                    cmd[k] = le32_to_cpu(w[i + k]);
                }
                gfx2d_shape(s, cmd);
            }
            i += GFX2D_SHAPE_WORDS;
            break;
        default:
            i++;
            break;
        }
    }
}

static void gfx2d_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    XdjGfx2d *s = opaque;

    if (off == GFX2D_STATUS_CLR) {
        stl_le_p(&s->regs.reg[GFX2D_STATUS],
                 ldl_le_p(&s->regs.reg[GFX2D_STATUS]) & ~val);
        gfx2d_update_irq(s);
        return;
    }
    regs_write(&s->regs, off, val, size);
    if (off == GFX2D_START && (val & 1)) {
        gfx2d_run_list(s);
        timer_mod(s->done_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + GFX2D_RUN_NS);
    }
}

static uint64_t gfx2d_read(void *opaque, hwaddr off, unsigned size)
{
    XdjGfx2d *s = opaque;

    return regs_read(&s->regs, off, size);
}

static const MemoryRegionOps gfx2d_ops = {
    .read = gfx2d_read,
    .write = gfx2d_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void gfx2d_init(MemoryRegion *sysmem, qemu_irq irq)
{
    XdjGfx2d *s = g_new0(XdjGfx2d, 1);

    s->regs.reg = g_malloc0(GFX2D_SIZE);
    s->irq = irq;
    s->done_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gfx2d_done, s);
    memory_region_init_io(&s->regs.iomem, NULL, &gfx2d_ops, s, "sh7724.2dg",
                          GFX2D_SIZE);
    memory_region_add_subregion(sysmem, GFX2D_BASE, &s->regs.iomem);
}

void xdj1000_gfx_init(MemoryRegion *sysmem, qemu_irq gfx2d_irq)
{
    MemoryRegion *meram = g_new(MemoryRegion, 1);

    regs_init(sysmem, "sh7724.icb", ICB_BASE, ICB_SIZE);
    memory_region_init_ram(meram, NULL, "sh7724.meram", MERAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, MERAM_BASE, meram);
    gfx2d_init(sysmem, gfx2d_irq);
}
