/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
#include "cdj_auth_chip.h"
#include "cdj_getenv.h"
/*
 * The I2C master at 0xFFE70000 and the authentication chip behind it. There
 * is no manual page for this block in hand; the layout is what the driver at
 * 0x0427F5D4..0x0427FAE8 does with it, all of it byte accesses:
 *
 *   +0x04  control. Written 0x89 (start), 0x88 (start released), 0x8A (stop
 *          asked for). Read before a start: bit 5 must be clear and bit 6
 *          set (bus free).
 *   +0x0C  event flags, cleared by writing 0 to them. The driver polls:
 *            bit 0  start sent (both directions)
 *            bit 1  address acknowledged, or a byte received (read)
 *            bit 3  address acknowledged, or a byte sent (write)
 *            bit 4  stop done
 *            bit 6  no acknowledge, checked once after the stop of a write
 *   +0x18  pin configuration, read-modify-write only
 *   +0x20  slave address and direction: 0x20 write, 0x21 read (slave 0x10)
 *   +0x24  data: the first byte of a write is stored before the start, the
 *          rest before each flag clear; a read takes it after bit 1 rises
 *
 * Clearing bit 1 or bit 3 releases the clock for the next byte. The chip is
 * the one the NXS2 has at the same address: the check at 0x0410FE7C writes
 * 0x00 and wants 0x05 back, then writes 0x01 and wants 0x01, as 0x08214D98
 * does there.
 */
#define IIC_BASE        0xFFE70000
#define IIC_SIZE        0x10000
#define IIC_CTRL        0x04
#define IIC_FLAGS       0x0C
#define IIC_ADDR        0x20
#define IIC_DATA        0x24

#define CTRL_START      0x01
#define CTRL_STOP       0x02
#define STAT_BUSY       0x20
#define STAT_FREE       0x40

#define FLAG_START      0x01
#define FLAG_RX         0x02
#define FLAG_TX         0x08
#define FLAG_STOP       0x10
#define FLAG_NACK       0x40

#define AUTH_CHIP_ADDR  0x10

typedef enum {
    IIC_IDLE,
    IIC_ADDRESS,                /* start sent, address going out            */
    IIC_WAIT_RELEASE,           /* byte done, waiting for its flag to clear */
    IIC_BYTE,                   /* one data byte on the wire                */
    IIC_STOP,
} IicPhase;

typedef struct CdjIic {
    MemoryRegion iomem;
    uint8_t reg[IIC_SIZE];
    IicPhase phase;
    bool reading;
    bool stop_asked;
    CdjAuthChip chip;
} CdjIic;

static void iic_bus_event(CdjIic *s);

/*
 * Bus events complete inside the register write that causes them. The driver
 * gives each wait a fixed poll budget, and a timer-driven event that landed
 * late in guest time (a stop 4 ms behind the byte) made it give up before
 * the read of the second auth command, which raised E-7206.
 */
static void iic_arm(CdjIic *s, IicPhase phase)
{
    s->phase = phase;
    iic_bus_event(s);
}

static void iic_bus_event(CdjIic *s)
{
    uint8_t *flags = &s->reg[IIC_FLAGS];
    uint8_t done = s->reading ? FLAG_RX : FLAG_TX;

    switch (s->phase) {
    case IIC_ADDRESS:
        *flags |= FLAG_START;
        if ((s->reg[IIC_ADDR] >> 1) == AUTH_CHIP_ADDR) {
            *flags |= done;
        } else {
            *flags |= FLAG_NACK | done;
        }
        s->phase = IIC_WAIT_RELEASE;
        break;
    case IIC_BYTE:
        if (s->reading) {
            s->reg[IIC_DATA] = cdj_auth_chip_read(&s->chip);
        } else {
            cdj_auth_chip_write(&s->chip, s->reg[IIC_DATA]);
        }
        *flags |= done;
        s->phase = IIC_WAIT_RELEASE;
        if (s->stop_asked) {
            iic_arm(s, IIC_STOP);
        }
        break;
    case IIC_STOP:
        *flags |= FLAG_STOP;
        s->phase = IIC_IDLE;
        s->stop_asked = false;
        break;
    default:
        break;
    }
}

static uint64_t iic_read(void *opaque, hwaddr off, unsigned size)
{
    CdjIic *s = opaque;

    if (off == IIC_CTRL) {
        return STAT_FREE | (s->phase != IIC_IDLE ? STAT_BUSY : 0);
    }
    return s->reg[off];
}

static void iic_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjIic *s = opaque;

    switch (off) {
    case IIC_CTRL:
        if (val & CTRL_START) {
            s->reading = s->reg[IIC_ADDR] & 1;
            s->stop_asked = false;
            iic_arm(s, IIC_ADDRESS);
        } else if ((val & CTRL_STOP) && !s->stop_asked) {
            s->stop_asked = true;
            if (!s->reading && s->phase == IIC_WAIT_RELEASE) {
                iic_arm(s, IIC_STOP);
            }
        }
        break;
    case IIC_FLAGS: {
        uint8_t released = s->reg[IIC_FLAGS] & ~val & (FLAG_RX | FLAG_TX);

        s->reg[IIC_FLAGS] &= val;
        if (released && s->phase == IIC_WAIT_RELEASE) {
            iic_arm(s, IIC_BYTE);
        }
        break;
    }
    default:
        s->reg[off] = val;
    }
}

static const MemoryRegionOps iic_ops = {
    .read = iic_read,
    .write = iic_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 1 },
};

static const VMStateDescription vmstate_iic = {
    .name = "cdj2000-iic",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, CdjIic, IIC_SIZE),
        CDJ_VMSTATE_SPAN(CdjIic, phase, stop_asked),
        VMSTATE_UINT8(chip.cmd, CdjIic),
        VMSTATE_END_OF_LIST()
    }
};

void cdj2000_iic_init(MemoryRegion *sysmem,
                      const CdjAuthAnswer *answers, unsigned n)
{
    CdjIic *s = g_new0(CdjIic, 1);

    cdj_auth_chip_init(&s->chip, answers, n);
    vmstate_register_any(NULL, &vmstate_iic, s);
    memory_region_init_io(&s->iomem, NULL, &iic_ops, s, "sh7763.iic",
                          IIC_SIZE);
    memory_region_add_subregion(sysmem, IIC_BASE, &s->iomem);
}
