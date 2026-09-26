/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
#include "cdj_getenv.h"
/*
 * The SH7763's interrupt controller: INTC2 for the on-chip modules at
 * 0xFFD40000, and the external IRQ pins at 0xFFD00000.
 *
 * INTEVT codes as the firmware's own dispatch uses them (SCIF0 H'700..H'760,
 * SCIF1 H'B80..H'BE0, DMAC DMTE0-3 H'640..H'6A0 and DMTE4-5 H'780/H'7A0) and
 * the register layout of the SH7763 (the same as Linux's support for this
 * chip describes): INT2PRI0-13 hold one 8-bit priority per module group,
 * INT2MSKR / INT2MSKCR mask and unmask a group per bit, MSB first.
 *
 * The kernel tick is TMU channel 4, H'E20.
 */

#define INT2PRI(n)  (0xFFD40000 + (n) * 4)
#define INT2PRI8_(n) (0xFFD400A0 + ((n) - 8) * 4)

static struct intc_vect s63_vectors[] = {
    INTC_VECT(S63_RTC0, 0x480), INTC_VECT(S63_RTC1, 0x4A0),
    INTC_VECT(S63_RTC2, 0x4C0), INTC_VECT(S63_WDT, 0x560),
    INTC_VECT(S63_TMU0, 0x580), INTC_VECT(S63_TMU1, 0x5A0),
    INTC_VECT(S63_TMU2, 0x5C0), INTC_VECT(S63_TMU2_TICPI, 0x5E0),
    INTC_VECT(S63_DMTE0, 0x640), INTC_VECT(S63_DMTE1, 0x660),
    INTC_VECT(S63_DMTE2, 0x680), INTC_VECT(S63_DMTE3, 0x6A0),
    INTC_VECT(S63_DMAE, 0x6C0),
    INTC_VECT(S63_SCIF0_ERI, 0x700), INTC_VECT(S63_SCIF0_RXI, 0x720),
    INTC_VECT(S63_SCIF0_BRI, 0x740), INTC_VECT(S63_SCIF0_TXI, 0x760),
    INTC_VECT(S63_DMTE4, 0x780), INTC_VECT(S63_DMTE5, 0x7A0),
    INTC_VECT(S63_IIC0, 0x8A0), INTC_VECT(S63_IIC1, 0x8C0),
    INTC_VECT(S63_CMT, 0x900),
    INTC_VECT(S63_GETHER0, 0x920), INTC_VECT(S63_GETHER1, 0x940),
    INTC_VECT(S63_GETHER2, 0x960),
    INTC_VECT(S63_SCIF1_ERI, 0xB80), INTC_VECT(S63_SCIF1_RXI, 0xBA0),
    INTC_VECT(S63_SCIF1_BRI, 0xBC0), INTC_VECT(S63_SCIF1_TXI, 0xBE0),
    INTC_VECT(S63_USBH, 0xC60), INTC_VECT(S63_USBF0, 0xC80),
    INTC_VECT(S63_USBF1, 0xCA0),
    INTC_VECT(S63_MMCIF0, 0xD00), INTC_VECT(S63_MMCIF1, 0xD20),
    INTC_VECT(S63_MMCIF2, 0xD40), INTC_VECT(S63_MMCIF3, 0xD60),
    INTC_VECT(S63_TMU3, 0xE00), INTC_VECT(S63_TMU4, 0xE20),
    INTC_VECT(S63_TMU5, 0xE40),
    INTC_VECT(S63_SCIF2_ERI, 0xF00), INTC_VECT(S63_SCIF2_RXI, 0xF20),
    INTC_VECT(S63_SCIF2_BRI, 0xF40), INTC_VECT(S63_SCIF2_TXI, 0xF60),
    INTC_VECT(S63_GPIO0, 0xF80), INTC_VECT(S63_GPIO1, 0xFA0),
    INTC_VECT(S63_GPIO2, 0xFC0), INTC_VECT(S63_GPIO3, 0xFE0),
    /* External IRQ pins in IRQ mode. */
    INTC_VECT(S63_IRQ0, 0x240), INTC_VECT(S63_IRQ1, 0x280),
    INTC_VECT(S63_IRQ2, 0x2C0), INTC_VECT(S63_IRQ3, 0x300),
    INTC_VECT(S63_IRQ4, 0x340), INTC_VECT(S63_IRQ5, 0x380),
    INTC_VECT(S63_IRQ6, 0x3C0), INTC_VECT(S63_IRQ7, 0x200),
};

static struct intc_group s63_groups[] = {
    INTC_GROUP(S63_TMU012, S63_TMU0, S63_TMU1, S63_TMU2, S63_TMU2_TICPI),
    INTC_GROUP(S63_TMU345, S63_TMU3, S63_TMU4, S63_TMU5),
    INTC_GROUP(S63_DMAC, S63_DMTE0, S63_DMTE1, S63_DMTE2, S63_DMTE3,
               S63_DMAE, S63_DMTE4, S63_DMTE5),
    INTC_GROUP(S63_SCIF0, S63_SCIF0_ERI, S63_SCIF0_RXI, S63_SCIF0_BRI,
               S63_SCIF0_TXI),
    INTC_GROUP(S63_SCIF1, S63_SCIF1_ERI, S63_SCIF1_RXI, S63_SCIF1_BRI,
               S63_SCIF1_TXI),
    INTC_GROUP(S63_SCIF2, S63_SCIF2_ERI, S63_SCIF2_RXI, S63_SCIF2_BRI,
               S63_SCIF2_TXI),
    INTC_GROUP(S63_GETHER, S63_GETHER0, S63_GETHER1, S63_GETHER2),
    INTC_GROUP(S63_RTC, S63_RTC0, S63_RTC1, S63_RTC2),
    INTC_GROUP(S63_USBF, S63_USBF0, S63_USBF1),
    INTC_GROUP(S63_MMCIF, S63_MMCIF0, S63_MMCIF1, S63_MMCIF2, S63_MMCIF3),
    INTC_GROUP(S63_GPIO, S63_GPIO0, S63_GPIO1, S63_GPIO2, S63_GPIO3),
};

/* sh_intc's set register enables: the mask-clear register goes there, the
 * mask register as its clear register (see common/sh4_intc.c). */
static struct intc_mask_reg s63_mask_registers[] = {
    /* INT2MSKCR / INT2MSKR, bit 31 first */
    { 0xFFD4003C, 0xFFD40038, 32,
      { 0, 0, 0, 0, 0, 0, S63_GPIO, 0,
        0, S63_MMCIF, 0, 0, 0, 0, 0, 0,
        0, 0, 0, S63_CMT, 0, 0, 0, S63_DMAC,
        0, 0, S63_WDT, S63_SCIF1, S63_SCIF0, S63_RTC, S63_TMU345, S63_TMU012 } },
    /* INT2MSKCR1 / INT2MSKR1 */
    { 0xFFD400D4, 0xFFD400D0, 32,
      { 0, 0, 0, 0, 0, 0, S63_SCIF2, S63_USBF,
        0, 0, 0, 0, 0, 0, S63_USBH, S63_GETHER,
        0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, S63_IIC1, S63_IIC0, 0, 0, 0, 0 } },
    /* INTMSKCLR0 / INTMSK0: the IRQ pins */
    { 0xFFD00064, 0xFFD00044, 32,
      { S63_IRQ0, S63_IRQ1, S63_IRQ2, S63_IRQ3,
        S63_IRQ4, S63_IRQ5, S63_IRQ6, S63_IRQ7 } },
};

static struct intc_prio_reg s63_prio_registers[] = {
    { INT2PRI(0), 0, 32, 8, { S63_TMU0, S63_TMU1, S63_TMU2, S63_TMU2_TICPI } },
    { INT2PRI(1), 0, 32, 8, { S63_TMU3, S63_TMU4, S63_TMU5, S63_RTC } },
    { INT2PRI(2), 0, 32, 8, { S63_SCIF0, S63_SCIF1, S63_WDT } },
    { INT2PRI(3), 0, 32, 8, { 0, S63_DMAC } },
    { INT2PRI(4), 0, 32, 8, { S63_CMT } },
    { INT2PRI(5), 0, 32, 8, { 0 } },
    { INT2PRI(6), 0, 32, 8, { 0, S63_USBF, S63_MMCIF } },
    { INT2PRI(7), 0, 32, 8, { S63_SCIF2, S63_GPIO } },
    { INT2PRI8_(8), 0, 32, 8, { 0 } },
    { INT2PRI8_(9), 0, 32, 8, { 0, 0, S63_IIC1, S63_IIC0 } },
    { INT2PRI8_(10), 0, 32, 8, { 0 } },
    { INT2PRI8_(11), 0, 32, 8, { 0 } },
    { INT2PRI8_(12), 0, 32, 8, { 0, 0, S63_USBH, S63_GETHER } },
    { INT2PRI8_(13), 0, 32, 8, { 0 } },
    /* INTPRI: the IRQ pins, 4 bits each */
    { 0xFFD00010, 0, 32, 4,
      { S63_IRQ0, S63_IRQ1, S63_IRQ2, S63_IRQ3,
        S63_IRQ4, S63_IRQ5, S63_IRQ6, S63_IRQ7 } },
};

struct intc_desc cdj_sh7763_intc;

/*
 * INT2MSKR and INT2MSKR1 read back which groups are masked, and the firmware
 * masks a group by or-ing its bit into that read. sh_intc keeps the enabled
 * groups instead, so without this the read-modify-write masks every group
 * enabled so far: the boot wrote INT2MSKR = 0x0200010B, the kernel tick's
 * TMU345 included, and the tick never ran again.
 */
typedef struct S63MaskRead {
    MemoryRegion iomem;
    const struct intc_mask_reg *reg;
} S63MaskRead;

static uint64_t s63_mask_read(void *opaque, hwaddr off, unsigned size)
{
    S63MaskRead *m = opaque;

    return (uint32_t)~m->reg->value;
}

static void s63_mask_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    S63MaskRead *m = opaque;

    memory_region_dispatch_write(&cdj_sh7763_intc.iomem,
                                 A7ADDR(m->reg->clr_reg), val, MO_32,
                                 MEMTXATTRS_UNSPECIFIED);
}

static const MemoryRegionOps s63_mask_ops = {
    .read = s63_mask_read,
    .write = s63_mask_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void s63_mask_read_init(MemoryRegion *sysmem,
                               const struct intc_mask_reg *reg)
{
    S63MaskRead *m = g_new0(S63MaskRead, 1);

    m->reg = reg;
    memory_region_init_io(&m->iomem, NULL, &s63_mask_ops, m, "sh7763.int2mskr",
                          4);
    memory_region_add_subregion_overlap(sysmem, P4ADDR(reg->clr_reg),
                                        &m->iomem, 1);
}

void cdj_sh7763_intc_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    cdj_intc_setup(sysmem, cpu, &cdj_sh7763_intc, S63_NR_SOURCES,
                   _INTC_ARRAY(s63_mask_registers),
                   _INTC_ARRAY(s63_prio_registers),
                   _INTC_ARRAY(s63_vectors),
                   _INTC_ARRAY(s63_groups));
    s63_mask_read_init(sysmem, &s63_mask_registers[0]);
    s63_mask_read_init(sysmem, &s63_mask_registers[1]);
}

/* The sources behind the IRQ counts at exit; the kernel tick (TMU3-5,
 * H'E00..H'E40) is always printed. */
void cdj_sh7763_intc_report(void)
{
    cdj_intc_report(&cdj_sh7763_intc, S63_NR_SOURCES, 0xE00, 0xE40);
}
