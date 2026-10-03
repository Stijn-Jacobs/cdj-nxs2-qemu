/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_ata.h"
#include "cdj_getenv.h"
/* ---------------------------------------------------------------------------
 * ATA/ATAPI task file for the CD drive, modelled as an empty, healthy drive.
 * Enabled with CDJ_ATA=1.
 *
 * On the NXS2, without it boot subsystem 4 registers status 2 with
 * fatal_register (0x084026A6) and the firmware shows E-7001 DISC DRIVE
 * ERROR. Subsystem 4 passes when *(0x092225A0) == 2, set at 0x0820A96E when
 * the first IDENTIFY through 0x084526A8 succeeds and word 0 has bit 15 set
 * (ATAPI signature).
 *
 * Registers at base + 60*channel, 4-byte stride (NXS2 0x0845270C), in the
 * usual ATA order: +0x00 Data, +0x04 Features/Error, +0x08 Sector Count
 * (Interrupt Reason during PACKET), +0x10/+0x14 Byte Count low/high, +0x18
 * Device, +0x1C Command/Status, +0x38 Device Control/Alternate Status.
 * Commands used: 0xEC IDENTIFY DEVICE, 0xA1 IDENTIFY PACKET DEVICE and 0xA0
 * PACKET. The ready-wait (NXS2 0x08453BFA) treats status 0xFF as no device
 * and needs BSY and DRQ clear; (NXS2 0x0845296A) needs ERR clear.
 *
 * PACKET runs the PIO protocol of an empty tray: every medium command ends in
 * CHECK CONDITION with sense NOT READY / MEDIUM NOT PRESENT (02/3A/00), and
 * REQUEST SENSE and INQUIRY hand their data over the data register.
 *
 * +0x80 starts the controller block in the SH7724 ATAPI layout (CONTROL1,
 * STATUS, INT_ENABLE, ...). The CDJ-2000 sends a packet with INT_ENABLE =
 * DEVINT and sleeps until its ISR (0x04109180) sees STATUS bit 4, the drive's
 * INTRQ; without the interrupt every packet waits out a timeout and a reset,
 * which holds the first browse list for ~4 min.
 */
#define CDJ_ATA_SIZE    0x100
#define CDJ_ATA_STRIDE  60          /* base + 60*channel */

#define ST_ERR   0x01
#define ST_DRQ   0x08
#define ST_IDLE  0x50               /* DRDY | DSC */

#define DC_NIEN  0x02               /* Device Control: INTRQ disabled */

/* Interrupt Reason (Sector Count during PACKET): CoD and IO. */
#define IR_COD   0x01
#define IR_IO    0x02

#define SENSE_NOT_READY         0x02
#define ASC_MEDIUM_NOT_PRESENT  0x3A

#define CTL_BASE        0x80
#define CTL_STATUS      0x84
#define CTL_INT_ENABLE  0x88
#define CTL_DEVINT      0x10

typedef struct {
    uint8_t status, error, nsect, lbam, lbah, devctl;
    bool intrq;
    uint8_t pkt[12];
    unsigned pkt_len;               /* command packet bytes received */
    uint8_t buf[512];               /* PIO data-in: IDENTIFY or a reply */
    unsigned buf_pos, buf_len;
    uint8_t sense_key, asc;
} CdjAtaChannel;

typedef struct {
    MemoryRegion iomem;
    qemu_irq irq;
    bool irq_level;
    CdjAtaChannel ch[2];
    uint32_t ctl[16];               /* +0x80..+0xBC, read back as written */
    uint64_t reads, writes, cmds, packets, not_ready, irqs;
    Notifier exit;
} CdjAtaState;

static bool cdj_ata_on(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("CDJ_ATA");

        on = e && *e != '0';
    }
    return on;
}

static bool cdj_ata_devint(CdjAtaState *s)
{
    return (s->ch[0].intrq && !(s->ch[0].devctl & DC_NIEN)) ||
           (s->ch[1].intrq && !(s->ch[1].devctl & DC_NIEN));
}

static void cdj_ata_update_irq(CdjAtaState *s)
{
    bool level = cdj_ata_devint(s) &&
                 (s->ctl[(CTL_INT_ENABLE - CTL_BASE) / 4] & CTL_DEVINT);

    if (level && !s->irq_level) {
        s->irqs++;
    }
    s->irq_level = level;
    if (s->irq) {
        qemu_set_irq(s->irq, level);
    }
}

static void cdj_ata_raise(CdjAtaState *s, CdjAtaChannel *c)
{
    c->intrq = true;
    cdj_ata_update_irq(s);
}

static void cdj_ata_data_in(CdjAtaChannel *c, const uint8_t *data,
                            unsigned len, uint8_t reason)
{
    memcpy(c->buf, data, len);
    c->buf_pos = 0;
    c->buf_len = len;
    c->nsect = reason;
    c->status = ST_IDLE | ST_DRQ;
}

static void cdj_ata_complete(CdjAtaState *s, CdjAtaChannel *c,
                             uint8_t sense_key, uint8_t asc)
{
    c->sense_key = sense_key;
    c->asc = asc;
    c->nsect = IR_COD | IR_IO;
    c->error = sense_key << 4;
    c->status = sense_key ? ST_IDLE | ST_ERR : ST_IDLE;
    cdj_ata_raise(s, c);
}

static void cdj_ata_packet(CdjAtaState *s, CdjAtaChannel *c)
{
    uint8_t reply[36] = { 0 };
    unsigned len;

    s->packets++;
    switch (c->pkt[0]) {
    case 0x03:                      /* REQUEST SENSE */
        reply[0] = 0x70;
        reply[2] = c->sense_key;
        reply[7] = 10;
        reply[12] = c->asc;
        len = MIN(c->pkt[4], 18);
        break;
    case 0x12:                      /* INQUIRY: CD-ROM, removable */
        reply[0] = 0x05;
        reply[1] = 0x80;
        reply[3] = 0x21;
        reply[4] = 31;
        memcpy(&reply[8], "PIONEER DVD-ROM DVD-118        1.00", 28);
        len = MIN(c->pkt[4], 36);
        break;
    default:
        s->not_ready++;
        cdj_ata_complete(s, c, SENSE_NOT_READY, ASC_MEDIUM_NOT_PRESENT);
        return;
    }
    /* The host's byte count limit, from Byte Count low/high. */
    if (c->lbam | c->lbah) {
        len = MIN(len, c->lbam | c->lbah << 8);
    }
    c->sense_key = c->asc = 0;
    if (!len) {
        cdj_ata_complete(s, c, 0, 0);
        return;
    }
    c->lbam = len;
    c->lbah = len >> 8;
    cdj_ata_data_in(c, reply, len, IR_IO);
    cdj_ata_raise(s, c);
}

static uint64_t cdj_ata_read(void *opaque, hwaddr off, unsigned size)
{
    CdjAtaState *s = opaque;
    unsigned chn = off >= CDJ_ATA_STRIDE ? 1 : 0;
    CdjAtaChannel *c = &s->ch[chn];
    hwaddr reg = off - chn * CDJ_ATA_STRIDE;
    uint16_t w;

    s->reads++;
    if (off >= CTL_BASE) {
        if (off == CTL_STATUS) {
            return s->ctl[(CTL_STATUS - CTL_BASE) / 4] |
                   (cdj_ata_devint(s) ? CTL_DEVINT : 0);
        }
        return s->ctl[(off - CTL_BASE) / 4];
    }
    switch (reg) {
    case 0x00:
        if (!(c->status & ST_DRQ) || c->buf_pos >= c->buf_len) {
            return 0;
        }
        w = c->buf[c->buf_pos] | c->buf[c->buf_pos + 1] << 8;
        c->buf_pos += 2;
        if (c->buf_pos >= c->buf_len) {
            if (c->nsect & IR_IO) {
                cdj_ata_complete(s, c, 0, 0);
            } else {
                c->status = ST_IDLE;
            }
        }
        return w;
    case 0x04:
        return c->error;
    case 0x08:
        return c->nsect;
    case 0x10:
        return c->lbam;
    case 0x14:
        return c->lbah;
    case 0x1C:
        /* Reading Status acknowledges the drive's interrupt. */
        c->intrq = false;
        cdj_ata_update_irq(s);
        return c->status;
    case 0x38:
        return c->status;
    default:
        return 0;
    }
}

static void cdj_ata_command(CdjAtaState *s, CdjAtaChannel *c, uint8_t cmd)
{
    s->cmds++;
    c->error = 0;
    c->intrq = false;
    switch (cmd) {
    case 0xA1:                      /* IDENTIFY PACKET DEVICE */
    case 0xEC:                      /* IDENTIFY DEVICE */
        /* Only word 0 is tested (NXS2 bit 15, at 0x0820A970). 0x85C0 is
         * the usual ATAPI CD-ROM signature, with DRQ for a command packet
         * raised without an interrupt; the other words are zero. */
        memset(c->buf, 0, sizeof(c->buf));
        c->buf[0] = 0xC0;
        c->buf[1] = 0x85;
        c->buf_pos = 0;
        c->buf_len = sizeof(c->buf);
        c->nsect = 0;
        c->status = ST_IDLE | ST_DRQ;
        cdj_ata_raise(s, c);
        break;
    case 0xA0:                      /* PACKET: ask for the command packet */
        c->pkt_len = 0;
        c->buf_len = 0;
        c->nsect = IR_COD;
        c->status = ST_IDLE | ST_DRQ;
        cdj_ata_update_irq(s);
        break;
    default:
        c->status = ST_IDLE;
        cdj_ata_raise(s, c);
        break;
    }
}

static void cdj_ata_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjAtaState *s = opaque;
    unsigned chn = off >= CDJ_ATA_STRIDE ? 1 : 0;
    CdjAtaChannel *c = &s->ch[chn];
    hwaddr reg = off - chn * CDJ_ATA_STRIDE;

    s->writes++;
    if (off >= CTL_BASE) {
        if (off == CTL_STATUS) {
            /* The latched bits clear by writing 0; DEVINT follows INTRQ. */
            s->ctl[(CTL_STATUS - CTL_BASE) / 4] &= val;
        } else {
            s->ctl[(off - CTL_BASE) / 4] = val;
        }
        cdj_ata_update_irq(s);
        return;
    }
    switch (reg) {
    case 0x00:
        if ((c->status & ST_DRQ) && c->nsect == IR_COD) {
            c->pkt[c->pkt_len++] = val;
            c->pkt[c->pkt_len++] = val >> 8;
            if (c->pkt_len >= sizeof(c->pkt)) {
                cdj_ata_packet(s, c);
            }
        }
        break;
    case 0x08:
        c->nsect = val;
        break;
    case 0x10:
        c->lbam = val;
        break;
    case 0x14:
        c->lbah = val;
        break;
    case 0x1C:
        cdj_ata_command(s, c, val);
        break;
    case 0x38:
        c->devctl = val;
        cdj_ata_update_irq(s);
        break;
    }
}

static const MemoryRegionOps cdj_ata_ops = {
    .read = cdj_ata_read,
    .write = cdj_ata_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_ata_summary(Notifier *n, void *unused)
{
    CdjAtaState *s = container_of(n, CdjAtaState, exit);

    info_report("ata: %" PRIu64 " reads, %" PRIu64 " writes, %" PRIu64
                " commands, %" PRIu64 " packets (%" PRIu64 " not ready), %"
                PRIu64 " interrupts", s->reads, s->writes, s->cmds,
                s->packets, s->not_ready, s->irqs);
}

void cdj_ata_init(MemoryRegion *sysmem, const char *name, hwaddr base,
                  qemu_irq irq)
{
    CdjAtaState *s;

    if (!cdj_ata_on()) {
        return;
    }
    s = g_new0(CdjAtaState, 1);
    s->irq = irq;
    s->ch[0].status = s->ch[1].status = ST_IDLE;
    s->exit.notify = cdj_ata_summary;
    qemu_add_exit_notifier(&s->exit);
    memory_region_init_io(&s->iomem, NULL, &cdj_ata_ops, s, name,
                          CDJ_ATA_SIZE);
    memory_region_add_subregion_overlap(sysmem, base, &s->iomem, 1);
    info_report("ata: empty ATAPI drive at 0x%08x (status 0x50, ATAPI "
                "signature 0x85C0)", (unsigned)base);
}
