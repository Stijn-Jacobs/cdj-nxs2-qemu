/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/* ---------------------------------------------------------------------------
 * ATA/ATAPI task file for the CD drive, modelled as an empty, healthy drive.
 * Enabled with CDJ_ATA=1.
 *
 * Without it boot subsystem 4 registers status 2 with fatal_register
 * (0x084026A6) and the firmware shows E-7001 DISC DRIVE ERROR. Subsystem 4
 * passes when *(0x092225A0) == 2, set at 0x0820A96E when the first IDENTIFY
 * through 0x084526A8 succeeds and word 0 has bit 15 set (ATAPI signature).
 *
 * Registers at 0x04DA2100 + 60 * channel, 4-byte stride (0x0845270C):
 * +0x04 Features, +0x08 Sector Count, +0x10/+0x14/+0x18 LBA, +0x1C
 * Command/Status, +0x38 control. Commands used: 0xEC IDENTIFY DEVICE and 0xA1
 * IDENTIFY PACKET DEVICE. The ready-wait at 0x08453BFA treats status 0xFF as
 * no device and needs BSY and DRQ clear; 0x0845296A needs ERR clear.
 *
 * No medium is needed: disc presence is checked later with TEST UNIT READY.
 */
#define CDJ_ATA_BASE    0xA4DA2100
#define CDJ_ATA_SIZE    0x100
#define CDJ_ATA_STRIDE  60          /* base + 60*channel (0x0845270C) */

typedef struct {
    MemoryRegion iomem;
    unsigned id_idx[2];             /* PIO read cursor into the IDENTIFY block */
    unsigned id_left[2];            /* words still to hand over -- drives DRQ  */
    uint64_t reads, writes, cmds;
    Notifier exit;
} CdjAtaState;

/* An IDENTIFY block is 256 words of PIO. */
#define CDJ_ATA_ID_WORDS 256

static bool cdj_ata_on(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("CDJ_ATA");

        on = e && *e != '0';
    }
    return on;
}

/* Only word 0 is tested (bit 15, at 0x0820A970). 0x85C0 is the usual ATAPI
 * CD-ROM signature; the other words are left zero. */
static uint16_t cdj_ata_identify(unsigned idx)
{
    return idx == 0 ? 0x85C0 : 0x0000;
}

static uint64_t cdj_ata_read(void *opaque, hwaddr off, unsigned size)
{
    CdjAtaState *s = opaque;
    unsigned ch = off >= CDJ_ATA_STRIDE ? 1 : 0;
    hwaddr reg = off - ch * CDJ_ATA_STRIDE;

    s->reads++;
    switch (reg) {
    case 0x1C:
        /* Status: DRQ is set while IDENTIFY data is still to be read.
         *   0x58 = DRDY | DSC | DRQ   (data waiting)
         *   0x50 = DRDY | DSC         (idle, command complete)
         */
        return s->id_left[ch] ? 0x58 : 0x50;
    case 0x00: {
        uint16_t w = cdj_ata_identify(s->id_idx[ch]);

        s->id_idx[ch]++;
        if (s->id_left[ch]) {
            s->id_left[ch]--;
        }
        return w;
    }
    default:
        return 0;
    }
}

static void cdj_ata_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjAtaState *s = opaque;
    unsigned ch = off >= CDJ_ATA_STRIDE ? 1 : 0;
    hwaddr reg = off - ch * CDJ_ATA_STRIDE;

    s->writes++;
    /* Each IDENTIFY command gets a fresh 256-word block. */
    if (reg == 0x1C && (val == 0xA1 || val == 0xEC)) {
        s->id_idx[ch] = 0;
        s->id_left[ch] = CDJ_ATA_ID_WORDS;
        s->cmds++;
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
                " IDENTIFY commands", s->reads, s->writes, s->cmds);
}

void cdj_ata_init(MemoryRegion *sysmem)
{
    CdjAtaState *s;

    if (!cdj_ata_on()) {
        return;
    }
    s = g_new0(CdjAtaState, 1);
    s->exit.notify = cdj_ata_summary;
    qemu_add_exit_notifier(&s->exit);
    memory_region_init_io(&s->iomem, NULL, &cdj_ata_ops, s, "sh7724.atapi",
                          CDJ_ATA_SIZE);
    memory_region_add_subregion_overlap(sysmem, A7ADDR(CDJ_ATA_BASE),
                                        &s->iomem, 1);
    info_report("ata: empty ATAPI drive at 0x%08x (status 0x50, ATAPI "
                "signature 0x85C0)", CDJ_ATA_BASE);
}

