/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "xdj1000.h"
/*
 * The pins MAIN's boot code bit-bangs to the DSP. They sit in the pin
 * function controller's data registers, and carry the same 2-bit command and
 * busy line as the older decks' port latch:
 *   0x128 bit 7   out: command bit 1
 *   0x164 bit 1   out: command bit 0
 *   0x16C bit 0   in:  the DSP's HINT pin, active low
 *   0x12E bit 1   in:  USB power switch overcurrent, active low
 *   0x12E bit 4   in:  USB port state, low drops the stick
 * Every other register keeps what is written to it.
 */
#define PFC_BASE        0x04050000
#define PFC_SIZE        0x1000
#define PFC_CMD_HI      0x128
#define PFC_CMD_HI_BIT  0x80
#define PFC_CMD_LO      0x164
#define PFC_CMD_LO_BIT  0x02
#define PFC_DSP_BUSY    0x16C
#define PFC_DSP_BUSY_BIT 0x01
/* The task at 0x08CFCD04 reads this byte every 51 ticks and, when bit 1 is
 * low, posts display message 0x92 ("USB Error. Remove the device."). Bit 4
 * is sampled with a 3-count debounce at 0x08CF8B88, and while it reads low
 * the USB stack deletes the drive right after adding it, so the stick never
 * lists. Nothing pulls either bit low here: both read high. */
#define PFC_USB_STATUS  0x12E
#define PFC_USB_OC_BIT  0x02
#define PFC_USB_PORT_BIT 0x10

/* The DSP samples the pins every few cycles. MAIN changes the two command
 * bits with two byte stores, bit 1 first, so a change of both at once (0 to 3
 * and 3 to 0) would show the DSP the command 2 or 1 in between, and it would
 * act on it. A change of bit 1 therefore waits for the store of bit 0 that
 * always follows it. The stores can be tens of microseconds apart on the
 * host, so the wait is generous; nothing depends on a change of bit 1 alone. */
#define PFC_CMD_SETTLE_NS 2000000

typedef struct XdjPfc {
    MemoryRegion iomem;
    uint8_t reg[PFC_SIZE];
    const CdjDspWires *dsp;
    QEMUTimer *settle;
} XdjPfc;

static uint64_t pfc_read(void *opaque, hwaddr off, unsigned size)
{
    XdjPfc *s = opaque;
    uint64_t val = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        uint8_t b = s->reg[off + i];

        if (off + i == PFC_DSP_BUSY) {
            b &= ~PFC_DSP_BUSY_BIT;
            if (s->dsp->busy(s->dsp->opaque)) {
                b |= PFC_DSP_BUSY_BIT;
            }
        }
        if (off + i == PFC_USB_STATUS) {
            b |= PFC_USB_OC_BIT | PFC_USB_PORT_BIT;
        }
        val |= (uint64_t)b << (8 * i);
    }
    return val;
}

static unsigned pfc_command(const XdjPfc *s)
{
    return (s->reg[PFC_CMD_HI] & PFC_CMD_HI_BIT ? 2 : 0) |
           (s->reg[PFC_CMD_LO] & PFC_CMD_LO_BIT ? 1 : 0);
}

static void pfc_settled(void *opaque)
{
    XdjPfc *s = opaque;

    s->dsp->command(s->dsp->opaque, pfc_command(s));
}

static void pfc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    XdjPfc *s = opaque;
    uint8_t cmd_hi = s->reg[PFC_CMD_HI];
    unsigned i;

    for (i = 0; i < size; i++) {
        s->reg[off + i] = val >> (8 * i);
    }
    if (off <= PFC_CMD_LO && PFC_CMD_LO < off + size) {
        timer_del(s->settle);
        s->dsp->command(s->dsp->opaque, pfc_command(s));
    } else if ((cmd_hi ^ s->reg[PFC_CMD_HI]) & PFC_CMD_HI_BIT) {
        timer_mod(s->settle,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PFC_CMD_SETTLE_NS);
    }
}

static const MemoryRegionOps pfc_ops = {
    .read = pfc_read,
    .write = pfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_pfc = {
    .name = "xdj1000-pfc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, XdjPfc, PFC_SIZE),
        VMSTATE_TIMER_PTR(settle, XdjPfc),
        VMSTATE_END_OF_LIST()
    }
};

void xdj1000_pfc_init(MemoryRegion *sysmem, const CdjDspWires *dsp)
{
    XdjPfc *s = g_new0(XdjPfc, 1);

    s->dsp = dsp;
    s->settle = timer_new_ns(QEMU_CLOCK_VIRTUAL, pfc_settled, s);
    vmstate_register_any(NULL, &vmstate_pfc, s);
    memory_region_init_io(&s->iomem, NULL, &pfc_ops, s, "sh7724.pfc", PFC_SIZE);
    memory_region_add_subregion(sysmem, PFC_BASE, &s->iomem);
}
