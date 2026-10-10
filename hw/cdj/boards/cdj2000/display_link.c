/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
#include "cdj_getenv.h"
#include "exec/cpu-common.h"
#include "qemu/cutils.h"
/*
 * MAIN's end of the display link: two twin blocks, 0xFF400000 receiving from
 * the display processor and 0xFF500000 sending to it. Each is a DMA engine at
 * +0x1000 and a serial engine at +0x2000. The driver is at 0x042A38FC, its
 * interrupt handlers at 0x042A3F4C (receive DMA), 0x042A40DC (receive
 * serial), 0x042A3DB0 (transmit DMA) and 0x042A3EB4 (transmit serial).
 *
 * DMA registers, offsets from the block base:
 *   +0x1008 / +0x1018  buffer address (transmit / receive): 0xA4500800 and
 *                      0xA4500000
 *   +0x1010 / +0x1020  byte length in the high 28 bits
 *   +0x1028            control; bit 0 starts the transfer, and on the receive
 *                      side means armed and waiting
 *   +0x1188            status, acknowledged by writing back status & 31
 *   +0x1190            interrupt mask, 1 = masked: the driver unmasks bits 2,
 *                      3 and 4, the ones its handlers act on
 *
 * The transmit handler counts status bit 4 in 0x07DB3530 and, on bit 2 with
 * the send-done flag 0x07DB353C still clear, stops the DMA and sets that
 * flag. The receive handler counts bit 4 in 0x07DB3534, which the send
 * routine zeroes before every packet, and on bit 2 takes a count of 3 (the
 * 48-byte answer) as a good frame and anything else as bad. So bit 4 is one
 * 16-byte unit done and bit 2 the last one. Units retire one per
 * acknowledge: the handler loops on the status word, and two units folded
 * into one read would count short.
 *
 * The serial engines have a control word at +0x2000 and an event word at
 * +0x2004, which the handlers read and then write back as 0. The receive
 * engine only ever reports errors, which nothing here raises. The transmit
 * engine reports the end of a packet in event bit 25, which interrupts once
 * the driver's DMA-end handler has set the same bit in the control word; the
 * serial handler 0x042A3EB4 then clears the send-busy byte 0x07DB3541, which
 * the send poller at 0x04215722 waits on before it sends the next packet.
 */

#define LINK_WINDOW     0x10000
#define LINK_UNIT       16
#define LINK_BUFFER     0x800

#define DMA_CTRL        0x1028
#define DMA_STATUS      0x1188
#define DMA_MASK        0x1190

#define SER_CTRL        0x2000
#define SER_EVENT       0x2004
#define SER_TX_DONE     0x02000000

#define CTRL_START      0x01
#define STATUS_LAST     0x04
#define STATUS_UNIT     0x10
#define STATUS_ACK      0x1F

typedef struct LinkPort {
    MemoryRegion iomem;
    hwaddr addr_reg;
    hwaddr len_reg;
    qemu_irq irq;
    qemu_irq ser_irq;
    unsigned units;
    uint32_t reg[LINK_WINDOW / 4];
} LinkPort;

static LinkPort link_rx, link_tx;
static bool link_log;

static void link_dump(const char *dir, const uint8_t *pkt, size_t len)
{
    if (link_log) {
        info_report("display link: %s, %zu bytes", dir, len);
        qemu_hexdump(stderr, "display link", pkt, MIN(len, 64));
    }
}

static uint32_t link_status(const LinkPort *p)
{
    if (!p->units) {
        return 0;
    }
    return STATUS_UNIT | (p->units == 1 ? STATUS_LAST : 0);
}

static void link_update(LinkPort *p)
{
    qemu_set_irq(p->irq, link_status(p) & ~p->reg[DMA_MASK / 4]);
}

static void link_ser_update(LinkPort *p)
{
    qemu_set_irq(p->ser_irq, p->reg[SER_EVENT / 4] & p->reg[SER_CTRL / 4] &
                             SER_TX_DONE);
}

static uint32_t link_len(const LinkPort *p)
{
    return MIN(p->reg[p->len_reg / 4] & ~(LINK_UNIT - 1), LINK_BUFFER);
}

static void link_send(LinkPort *p)
{
    uint8_t pkt[LINK_BUFFER];
    uint32_t len = link_len(p);

    cpu_physical_memory_read(A7ADDR(p->reg[p->addr_reg / 4]), pkt, len);
    link_dump("MAIN to display", pkt, len);
    cdj2000_display_send(pkt, len);
    p->reg[DMA_CTRL / 4] &= ~CTRL_START;
    p->units = len / LINK_UNIT;
    link_update(p);
    p->reg[SER_EVENT / 4] |= SER_TX_DONE;
    link_ser_update(p);
}

void cdj2000_display_link_receive(const uint8_t *pkt, size_t len)
{
    LinkPort *p = &link_rx;
    uint32_t room = link_len(p);

    if (!(p->reg[DMA_CTRL / 4] & CTRL_START)) {
        warn_report("display link: answer of %zu bytes dropped, receive DMA "
                    "not armed", len);
        return;
    }
    link_dump("display to MAIN", pkt, len);
    cpu_physical_memory_write(A7ADDR(p->reg[p->addr_reg / 4]), pkt,
                              MIN(len, room));
    p->reg[DMA_CTRL / 4] &= ~CTRL_START;
    p->units = room / LINK_UNIT;
    link_update(p);
}

static uint64_t link_read(void *opaque, hwaddr off, unsigned size)
{
    LinkPort *p = opaque;
    uint32_t v = off / 4 == DMA_STATUS / 4 ? link_status(p) : p->reg[off / 4];

    return extract32(v, 8 * (off & 3), 8 * size);
}

static void link_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    LinkPort *p = opaque;
    unsigned shift = 8 * (off & 3);
    uint32_t old = p->reg[off / 4];

    if (off / 4 == DMA_STATUS / 4) {
        if ((val << shift) & STATUS_UNIT && p->units) {
            p->units--;
        }
        link_update(p);
        return;
    }
    p->reg[off / 4] = deposit32(old, shift, 8 * size, val);
    switch (off / 4) {
    case DMA_CTRL / 4:
        if (p == &link_tx && !(old & CTRL_START) &&
            (p->reg[off / 4] & CTRL_START)) {
            link_send(p);
        }
        break;
    case DMA_MASK / 4:
        link_update(p);
        break;
    case SER_CTRL / 4:
    case SER_EVENT / 4:
        if (p == &link_tx) {
            link_ser_update(p);
        }
        break;
    }
}

static const MemoryRegionOps link_ops = {
    .read = link_read,
    .write = link_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_link_port = {
    .name = "cdj2000-display-link",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(units, LinkPort),
        VMSTATE_UINT32_ARRAY(reg, LinkPort, LINK_WINDOW / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void link_port_init(LinkPort *p, MemoryRegion *sysmem, const char *name,
                           hwaddr base, hwaddr addr_reg, qemu_irq irq,
                           qemu_irq ser_irq)
{
    p->addr_reg = addr_reg;
    p->len_reg = addr_reg + 8;
    p->irq = irq;
    p->ser_irq = ser_irq;
    vmstate_register_any(NULL, &vmstate_link_port, p);
    memory_region_init_io(&p->iomem, NULL, &link_ops, p, name, LINK_WINDOW);
    memory_region_add_subregion(sysmem, base, &p->iomem);
}

/* CDJ_BF531_LINK_LOG=1 logs every packet: direction, length and its first
 * 64 bytes. */
void cdj2000_display_link_init(MemoryRegion *sysmem, qemu_irq rx, qemu_irq tx,
                               qemu_irq tx_ser)
{
    link_log = getenv("CDJ_BF531_LINK_LOG");
    link_port_init(&link_rx, sysmem, "sh7763.display-rx", 0xFF400000,
                   0x1018, rx, NULL);
    link_port_init(&link_tx, sysmem, "sh7763.display-tx", 0xFF500000,
                   0x1008, tx, tx_ser);
}
