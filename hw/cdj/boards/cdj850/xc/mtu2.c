/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "mtu2.h"
#include "trace.h"

/*
 * MTU2 register capture, 0xFFFE4200-0xFFFE43FF (SH7263 Hardware Manual
 * section 11, register list pages 462 and 1622).
 *
 * Storage only: nothing counts, no compare match happens, no interrupt is
 * raised (the firmware's vector-146 MTU2 interrupt is unrelated). Every
 * register reads back what was written, with the manual's reset values for
 * the channel 3 general registers (H'FFFF) and the channel 3/4 status
 * registers (H'C0).
 *
 * Why it exists: the CD loader drives its motor with channel 3 in PWM mode 1
 * on TIOC3C (PD10 = PWMLO), and the loader mechanism model (xc/loader.c)
 * needs the duty the firmware programmed. Addresses used, checked against
 * the manual's register list and against the firmware's writes in
 * 0x0C106638:
 *   TCR_3   0xFFFE4200  TMDR_3  0xFFFE4202  TIORL_3 0xFFFE4205
 *   TGRC_3  0xFFFE4224  TGRD_3  0xFFFE4226  TSTR    0xFFFE4280 (CST3 = bit 6)
 * Every other register in the block (channel 4, the shared registers) is
 * stored and traced the same way and has no meaning here.
 */
#define MTU2_BASE       0xFFFE4200
#define MTU2_SIZE       0x200
#define MTU2_TCR_3      0x00
#define MTU2_TMDR_3     0x02
#define MTU2_TIORL_3    0x05
#define MTU2_TSR_3      0x2C
#define MTU2_TSR_4      0x2D
#define MTU2_TGRA_3     0x18
#define MTU2_TGRB_3     0x1A
#define MTU2_TGRC_3     0x24
#define MTU2_TGRD_3     0x26
#define MTU2_TSTR       0x80
#define MTU2_CST3       0x40
#define MTU2_MD_MASK    0x0F
#define MTU2_MD_PWM1    0x02

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[MTU2_SIZE];
} Cdj850Mtu2;

static Cdj850Mtu2 *cdj850_mtu2;

static uint16_t mtu2_reg16(const Cdj850Mtu2 *s, unsigned off)
{
    return (s->reg[off] << 8) | s->reg[off + 1];
}

bool cdj850_mtu2_ch3_pwm(uint8_t *tiorl, uint16_t *tgrc, uint16_t *tgrd)
{
    Cdj850Mtu2 *s = cdj850_mtu2;

    if (!s || !(s->reg[MTU2_TSTR] & MTU2_CST3) ||
        (s->reg[MTU2_TMDR_3] & MTU2_MD_MASK) != MTU2_MD_PWM1) {
        return false;
    }
    *tiorl = s->reg[MTU2_TIORL_3];
    *tgrc = mtu2_reg16(s, MTU2_TGRC_3);
    *tgrd = mtu2_reg16(s, MTU2_TGRD_3);
    return true;
}

static uint64_t cdj850_mtu2_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Mtu2 *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) & (MTU2_SIZE - 1)];
    }
    if (cdj850_trace()) {
        info_report("cdj850: mtu2 read 0x%08x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)(MTU2_BASE + off), size,
                    (unsigned)v, s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_mtu2_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    Cdj850Mtu2 *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->reg[(off + size - 1 - i) & (MTU2_SIZE - 1)] = val >> (8 * i);
    }
    if (cdj850_trace()) {
        info_report("cdj850: mtu2 write 0x%08x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)(MTU2_BASE + off), size,
                    (unsigned)val, s->cpu->env.pc, cdj850_vms());
    }
}

static const MemoryRegionOps cdj850_mtu2_ops = {
    .read = cdj850_mtu2_read,
    .write = cdj850_mtu2_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_mtu2 = {
    .name = "cdj850-mtu2",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Mtu2, MTU2_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

void cdj850_mtu2_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    static const unsigned tgr3[] = {
        MTU2_TGRA_3, MTU2_TGRB_3, MTU2_TGRC_3, MTU2_TGRD_3,
    };
    Cdj850Mtu2 *s = g_new0(Cdj850Mtu2, 1);
    unsigned a;

    s->cpu = cpu;
    cdj850_mtu2 = s;
    for (a = 0; a < ARRAY_SIZE(tgr3); a++) {
        s->reg[tgr3[a]] = 0xFF;
        s->reg[tgr3[a] + 1] = 0xFF;
    }
    s->reg[MTU2_TSR_3] = 0xC0;
    s->reg[MTU2_TSR_4] = 0xC0;
    vmstate_register_any(NULL, &vmstate_cdj850_mtu2, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_mtu2_ops, s, "cdj850.mtu2",
                          MTU2_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, MTU2_BASE, &s->iomem, 1);
}
