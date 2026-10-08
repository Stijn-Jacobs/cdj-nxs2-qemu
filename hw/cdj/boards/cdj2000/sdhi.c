/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
/*
 * The SDHI (TMIO-style, 16-bit registers) with no card in the slot.
 *
 * The driver sends a command by writing SD_CMD and then polls SD_INFO1 for
 * RESPEND (0x041FF45C, 65535 rounds with a task delay every 8), so a plain
 * register file costs that whole timeout on every command of the card init.
 * It also clears status by writing the inverted mask, which a plain register
 * file stores as set bits, and spins with no limit while SD_INFO2 shows CBSY
 * (0x04200A74). So: every command ends at once with a response timeout, the
 * status registers clear only the bits written as 0, and CBSY never shows.
 */

#define SDHI_BASE       0xFFE40000
#define SDHI_SIZE       0x1000
#define SD_CMD          0x00
#define SD_INFO1        0x1C
#define SD_INFO2        0x1E
#define INFO1_RESPEND   (1u << 0)
#define INFO2_RESTIMEOUT (1u << 6)
#define INFO2_SCLKDIVEN (1u << 13)

typedef struct CdjSdhi {
    MemoryRegion iomem;
    uint16_t reg[SDHI_SIZE / 2];
} CdjSdhi;

static uint64_t sdhi_read(void *opaque, hwaddr off, unsigned size)
{
    CdjSdhi *s = opaque;

    if (off == SD_INFO2) {
        return s->reg[off / 2] | INFO2_SCLKDIVEN;
    }
    return s->reg[off / 2];
}

static void sdhi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjSdhi *s = opaque;

    switch (off) {
    case SD_INFO1:
    case SD_INFO2:
        s->reg[off / 2] &= val;
        break;
    case SD_CMD:
        s->reg[off / 2] = val;
        s->reg[SD_INFO1 / 2] |= INFO1_RESPEND;
        s->reg[SD_INFO2 / 2] |= INFO2_RESTIMEOUT;
        break;
    default:
        s->reg[off / 2] = val;
    }
}

static const MemoryRegionOps sdhi_ops = {
    .read = sdhi_read,
    .write = sdhi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 2, .max_access_size = 2 },
};

static const VMStateDescription vmstate_sdhi = {
    .name = "cdj2000-sdhi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(reg, CdjSdhi, SDHI_SIZE / 2),
        VMSTATE_END_OF_LIST()
    }
};

void cdj2000_sdhi_init(MemoryRegion *sysmem)
{
    CdjSdhi *s = g_new0(CdjSdhi, 1);

    vmstate_register_any(NULL, &vmstate_sdhi, s);
    memory_region_init_io(&s->iomem, NULL, &sdhi_ops, s, "sh7763.sdhi",
                          SDHI_SIZE);
    memory_region_add_subregion(sysmem, SDHI_BASE, &s->iomem);
}
