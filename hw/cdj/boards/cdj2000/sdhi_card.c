/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SH7764 SDHI, ported from cdj2000-emulator's firmware-derived TMIO model.
 * QEMU's sd-card supplies SD/SDSC/SDHC command and raw-block semantics.
 * See docs/sd-model.md for the observed driver contract and limitations. */
#include "sh7763.h"
#include "hw/sd/sd.h"
#include "hw/qdev-properties.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qapi/qmp/qdict.h"
#include "sysemu/block-backend.h"
#include "sysemu/blockdev.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define SDHI_BASE       0xffe40000
/* Widened past the TMIO register file so anything the driver's DMA path
   touches above it is seen rather than silently absorbed elsewhere.  */
#define SDHI_SIZE       0x1000
#define SDHI_CMD        0x00
#define SDHI_ARG0       0x04
#define SDHI_ARG1       0x06
#define SDHI_STOP       0x08
#define SDHI_SECCNT     0x0a
#define SDHI_RSP        0x0c    /* .. 0x1a, eight halfwords */
#define SDHI_INFO1      0x1c    /* bit 0 CMDRESPEND, bit 2 DATAEND, bit 5 card */
#define SDHI_INFO2      0x1e    /* bit 6 CMDTIMEOUT, bit 8 RXRDY, bit 14 busy */
#define SDHI_INFO1_MASK 0x20
#define SDHI_INFO2_MASK 0x22
#define SDHI_CLKCTL     0x24
#define SDHI_SIZEREG    0x26
#define SDHI_OPTION     0x28
#define SDHI_DATA       0x30
#define SDHI_DMAEN      0xd8

#define SDHI_INFO1_CMDRESPEND (1 << 0)
#define SDHI_INFO1_DATAEND    (1 << 2)
#define SDHI_INFO1_CARD       (1 << 5)
#define SDHI_INFO2_CMDTIMEOUT (1 << 6)
#define SDHI_INFO2_RXRDY      (1 << 8)
#define SDHI_INFO2_TXRQ       (1 << 9)
#define SDHI_INFO2_CBSY       (1 << 14)

#define SDHI_CMD_RESP_MASK    0x0700
#define SDHI_CMD_RESP_NONE    0x0300
#define SDHI_CMD_RESP_R2      0x0600
#define SDHI_CMD_DATA         0x0800
#define SDHI_CMD_READ         0x1000
#define SDHI_CMD_MULTI        0x2000

/* CTL_STOP bit 8: end a block-counted transfer with an automatic CMD12. */
#define SDHI_STOP_SEC_ENABLE  (1 << 8)

#define TYPE_CDJ_SDHI "cdj2000-sdhi"
OBJECT_DECLARE_SIMPLE_TYPE(CdjSdhiState, CDJ_SDHI)

struct CdjSdhiState {
    SysBusDevice parent_obj;

    SDBus sdbus;
    MemoryRegion iomem;

    uint16_t arg0, arg1, seccnt, sizereg, option, clkctl, stop;
    uint16_t info1, info2, info1_mask, info2_mask;
    uint16_t rsp[8];

    uint8_t buf[512];
    unsigned buf_len, buf_pos;
    unsigned blocks_left;
    bool multi;
    bool writing;


    bool inserted;
    bool ejected;                   /* logically removed through QOM */
    QEMUTimer *insert_timer;
    qemu_irq irq;
    QEMUTimer *data_timer;
    bool data_ready;
    bool lid_closed;
};

#define SDHI_XFER_NS 20000

static int64_t cdj_sdhi_block_ns(CdjSdhiState *s)
{
    return SDHI_XFER_NS;
}

/*
 * The data phase is interrupt-driven, not polled: sd_read_blocks waits on an RTOS
 * event with a 500-tick timeout, and sd_isr sets that event when INFO2
 * bit 8 (RXRDY) is set.  INFO2_MASK is "1 = masked" -- the driver writes 0 to
 * it before the transfer and 0x100 afterwards.
 */
static void cdj_sdhi_update_irq(CdjSdhiState *s)
{
    unsigned pending = s->info2 & ~s->info2_mask
                       & (SDHI_INFO2_RXRDY | SDHI_INFO2_TXRQ);

    if (s->irq) {
        qemu_set_irq(s->irq, pending ? 1 : 0);
    }
}

static void cdj_sdhi_fill_block(CdjSdhiState *s)
{
    unsigned i;

    for (i = 0; i < s->buf_len; i++) {
        s->buf[i] = sdbus_read_byte(&s->sdbus);
    }
    s->buf_pos = 0;
    s->data_ready = true;
    s->info2 |= SDHI_INFO2_RXRDY;
    cdj_sdhi_update_irq(s);
}

static void cdj_sdhi_store_response(CdjSdhiState *s, const uint8_t *rsp,
                                    size_t len)
{
    unsigned i;

    memset(s->rsp, 0, sizeof(s->rsp));
    if (len == 4) {
        uint32_t value = ldl_be_p(rsp);

        s->rsp[0] = value & 0xffff;
        s->rsp[1] = value >> 16;
    } else if (len == 16) {
    
        s->rsp[7] = rsp[0];
        for (i = 0; i < 7; i++) {
            s->rsp[6 - i] = ((uint16_t)rsp[1 + 2 * i] << 8) | rsp[2 + 2 * i];
        }
    }
}

static void cdj_sdhi_command(CdjSdhiState *s, uint16_t cmd)
{
    SDRequest request = {
        .cmd = cmd & 0x3f,
        .arg = s->arg0 | ((uint32_t)s->arg1 << 16),
    };
    uint8_t response[16];
    int len;

    /*
     * DATAEND has to be dropped here.  The driver only ever clears bit 3 of
     * INFO1 before a command (0x1ff518), so a DATAEND left over from the
     * previous transfer would still be set when it checks, and it would treat
     * the next data phase as already finished.
     */
    timer_del(s->data_timer);
    s->info1 &= ~(SDHI_INFO1_DATAEND | SDHI_INFO1_CMDRESPEND);
    s->info2 &= ~(0x7f | SDHI_INFO2_RXRDY | SDHI_INFO2_TXRQ);
    s->data_ready = false;
    s->buf_len = 0;
    s->buf_pos = 0;
    s->blocks_left = 0;

    /* A card the input channel took out answers nothing at all. */
    len = !s->inserted || s->ejected ? 0 :
          sdbus_do_command(&s->sdbus, &request, response);

    if (getenv("CDJ_SDHI_TRACE")) {
        fprintf(stderr, "cdj2000-sdhi: cmd %#06x (CMD%u) arg %#010x -> %u bytes\n",
                cmd, request.cmd, request.arg, (unsigned)len);
    }

    if ((cmd & SDHI_CMD_RESP_MASK) == SDHI_CMD_RESP_NONE) {
        /* CMD0 and friends: the card answering nothing is the correct answer. */
    } else if (len != 4 && len != 16) {
        /*
         * Report a response timeout rather than leaving the wait spinning:
         * 0x1ff45c has a 65535-iteration cap but only yields to the RTOS every
         * eighth pass, so an unanswered command costs real time.
         */
        s->info2 |= SDHI_INFO2_CMDTIMEOUT;
        s->info1 |= SDHI_INFO1_CMDRESPEND;
        cdj_sdhi_update_irq(s);
        return;
    } else {
        cdj_sdhi_store_response(s, response, len);
    }
    s->info1 |= SDHI_INFO1_CMDRESPEND;

    if (cmd & SDHI_CMD_DATA) {
        /* A card R1 error cannot manufacture a host data phase. */
        bool writing = !(cmd & SDHI_CMD_READ);
        if (writing ? !sdbus_receive_ready(&s->sdbus)
                    : !sdbus_data_ready(&s->sdbus)) {
            s->info2 |= 1u << 5; /* illegal access */
            cdj_sdhi_update_irq(s);
            return;
        }
        s->buf_len = s->sizereg ? s->sizereg : 512;
        if (s->buf_len > sizeof(s->buf)) {
            s->buf_len = sizeof(s->buf);
        }
        s->multi = (cmd & SDHI_CMD_MULTI) != 0;
        s->blocks_left = s->multi && s->seccnt ? s->seccnt : 1;
        s->writing = (cmd & SDHI_CMD_READ) == 0;
    
        timer_mod(s->data_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + cdj_sdhi_block_ns(s));
    }
    cdj_sdhi_update_irq(s);
}

static void cdj_sdhi_data_end(CdjSdhiState *s)
{
    s->blocks_left = 0;
    s->info1 |= SDHI_INFO1_DATAEND;

    if (s->multi && (s->stop & SDHI_STOP_SEC_ENABLE)) {
        SDRequest request = { .cmd = 12, .arg = 0 };
        uint8_t response[16];

        sdbus_do_command(&s->sdbus, &request, response);
        if (getenv("CDJ_SDHI_TRACE")) {
            fprintf(stderr, "cdj2000-sdhi: auto CMD12\n");
        }
    }
    s->multi = false;
}

static void cdj_sdhi_block_done(CdjSdhiState *s)
{
    s->data_ready = false;
    s->info2 &= ~(SDHI_INFO2_RXRDY | SDHI_INFO2_TXRQ);
    cdj_sdhi_update_irq(s);

    if (s->blocks_left > 1) {
        s->blocks_left--;
        timer_mod(s->data_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + cdj_sdhi_block_ns(s));
    } else {
        cdj_sdhi_data_end(s);
    }
}

/* A full block has been handed over; give it to the card and move on. */
static void cdj_sdhi_flush_block(CdjSdhiState *s)
{
    unsigned i;

    for (i = 0; i < s->buf_len; i++) {
        sdbus_write_byte(&s->sdbus, s->buf[i]);
    }
    s->buf_pos = 0;
    cdj_sdhi_block_done(s);
}

static uint16_t cdj_sdhi_read_halfword(CdjSdhiState *s, hwaddr offset)
{
    switch (offset) {
    case SDHI_ARG0:
        return s->arg0;
    case SDHI_ARG1:
        return s->arg1;
    case SDHI_SECCNT:
        return s->seccnt;
    case SDHI_STOP:
        return s->stop;
    case SDHI_RSP ... SDHI_RSP + 14:
        return s->rsp[(offset - SDHI_RSP) / 2];
    case SDHI_INFO1:
    
        /* NXS card-detect pin is active low, independent of W0C status. */
        return s->info1 | (!s->inserted ? SDHI_INFO1_CARD : 0);
    case SDHI_INFO2:
        return s->info2 | (1u << 13);
    case SDHI_INFO1_MASK:
        return s->info1_mask;
    case SDHI_INFO2_MASK:
        return s->info2_mask;
    case SDHI_CLKCTL:
        return s->clkctl;
    case SDHI_SIZEREG:
        return s->sizereg;
    case SDHI_OPTION:
        return s->option;
    case SDHI_DATA: {
        uint16_t value;
        if (!s->data_ready || s->writing) {
            return 0;
        }

        if (s->buf_pos + 1 >= s->buf_len) {
            /* Serve the tail, then advance to the next block or finish. */
            value = s->buf_pos < s->buf_len ? s->buf[s->buf_pos] : 0;
            s->buf_pos = s->buf_len;
        } else {
            value = s->buf[s->buf_pos] | (s->buf[s->buf_pos + 1] << 8);
            s->buf_pos += 2;
        }
        if (s->blocks_left && s->buf_pos >= s->buf_len) {
            cdj_sdhi_block_done(s);
        }
        return value;
    }
    default:
        if (offset >= 0x100 && getenv("CDJ_SDHI_TRACE")) {
            fprintf(stderr, "cdj2000-sdhi: READ HIGH +%#06x\n",
                    (unsigned)offset);
        }
        return 0;
    }
}

static uint64_t cdj_sdhi_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjSdhiState *s = opaque;
    uint64_t value;
    if ((offset & ~1ULL) == SDHI_DATA && size == 1) {
        if (!s->data_ready || s->writing || s->buf_pos >= s->buf_len) {
            return 0;
        }
        value = s->buf[s->buf_pos++];
        if (s->buf_pos == s->buf_len) {
            cdj_sdhi_block_done(s);
        }
        return value;
    }
    value = cdj_sdhi_read_halfword(s, offset & ~1ULL);
    /*
     * The driver reads these as halfwords, but a wider access has to see both
     * status registers rather than only the one it starts on: CTL_STATUS is one
     * 32-bit word whose upper half is INFO2.
     */
    if (size == 4) {
        hwaddr upper = offset == SDHI_DATA ? SDHI_DATA : offset + 2;
        value |= (uint64_t)cdj_sdhi_read_halfword(s, upper) << 16;
    }
    if (size == 1) {
        value = (value >> (8 * (offset & 1))) & 0xff;
    }
    return value;
}

static void cdj_sdhi_write_part(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    CdjSdhiState *s = opaque;

    if (size == 1 && (offset & ~1ULL) != SDHI_DATA) {
        unsigned shift = 8 * (offset & 1);
        uint16_t keep = (offset & ~1ULL) == SDHI_INFO1 ||
                        (offset & ~1ULL) == SDHI_INFO2
                        ? 0xffff : cdj_sdhi_read_halfword(s, offset & ~1ULL);
        value = (keep & ~(0xffu << shift)) | ((value & 0xff) << shift);
    }
    switch (offset & ~1ULL) {
    case SDHI_CMD:
        cdj_sdhi_command(s, value & 0xffff);
        break;
    case SDHI_ARG0:
        s->arg0 = value;
        break;
    case SDHI_ARG1:
        s->arg1 = value;
        break;
    case SDHI_SECCNT:
        s->seccnt = value;
        break;
    case SDHI_STOP:
        s->stop = value;
        break;
    case SDHI_DATA:
        /*
         * The by-hand path, taken when the source buffer is odd-aligned
         * (the driver tests buffer alignment).
         * A block is handed to the card only once it is whole, because sd.c
         * counts the bytes it receives and a short block desynchronises it.
         */
        if (!s->data_ready || !s->writing || s->buf_pos >= s->buf_len) {
            break;
        }
        s->buf[s->buf_pos++] = value & 0xff;
        if (size > 1 && s->buf_pos < s->buf_len) {
            s->buf[s->buf_pos++] = (value >> 8) & 0xff;
        }
        if (s->buf_pos >= s->buf_len) {
            cdj_sdhi_flush_block(s);
        }
        break;
    case SDHI_INFO1:
        /*
         * Write-to-clear, and only the bits the driver names.  The card-present
         * bit is the device's to own: sd_issue_command clears bit 3 before every
         * command, and a status register that simply took the written value
         * would drop the card between two commands.
         */
        s->info1 &= value | SDHI_INFO1_CARD;
        break;
    case SDHI_INFO2:
        s->info2 &= value;
        cdj_sdhi_update_irq(s);
        break;
    case SDHI_INFO1_MASK:
        s->info1_mask = value;
        cdj_sdhi_update_irq(s);
        break;
    case SDHI_INFO2_MASK:
        s->info2_mask = value;
        cdj_sdhi_update_irq(s);
        break;
    case SDHI_CLKCTL:
        s->clkctl = value;
        break;
    case SDHI_SIZEREG:
        s->sizereg = value;
        break;
    case SDHI_OPTION:
        s->option = value;
        break;
    default:
        break;
    }
}

static void cdj_sdhi_write(void *opaque, hwaddr offset, uint64_t value,
                           unsigned size)
{
    if (size == 4) {
        cdj_sdhi_write_part(opaque, offset, value & 0xffff, 2);
        cdj_sdhi_write_part(opaque, offset == SDHI_DATA ? SDHI_DATA : offset + 2,
                            value >> 16, 2);
    } else {
        cdj_sdhi_write_part(opaque, offset, value, size);
    }
}

static const MemoryRegionOps cdj_sdhi_ops = {
    .read = cdj_sdhi_read,
    .write = cdj_sdhi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_sdhi_data_ready(void *opaque)
{
    CdjSdhiState *s = opaque;

    if (!s->blocks_left || !s->inserted || !s->buf_len) {
        return;
    }
    if (s->writing) {
        /* Ready to take a block: the driver's half of RXRDY. */
        s->buf_pos = 0;
        s->data_ready = true;
        s->info2 |= SDHI_INFO2_TXRQ;
        cdj_sdhi_update_irq(s);
    } else {
        cdj_sdhi_fill_block(s);
    }
}

static void cdj_sdhi_cancel(CdjSdhiState *s)
{
    timer_del(s->data_timer);
    s->data_ready = false;
    s->buf_len = s->buf_pos = s->blocks_left = 0;
    s->multi = s->writing = false;
    s->info2 &= ~(SDHI_INFO2_RXRDY | SDHI_INFO2_TXRQ);
    cdj_sdhi_update_irq(s);
}

static void cdj_sdhi_insert(void *opaque)
{
    CdjSdhiState *s = opaque;
    s->lid_closed = true;
    s->inserted = true;
    s->ejected = false;
}

static char *cdj_sdhi_card_get(Object *obj, Error **errp)
{
    CdjSdhiState *s = CDJ_SDHI(obj);
    return g_strdup(s->inserted ? "inserted" : "ejected");
}

static void cdj_sdhi_card_set(Object *obj, const char *value, Error **errp)
{
    CdjSdhiState *s = CDJ_SDHI(obj);
    if (strcmp(value, "insert") && strcmp(value, "eject")) {
        error_setg(errp, "card expects insert or eject");
        return;
    }
    timer_del(s->insert_timer);
    cdj_sdhi_cancel(s);
    s->inserted = !strcmp(value, "insert");
    s->lid_closed = s->inserted;
    s->ejected = !s->inserted;
}

static void cdj_sdhi_reset(DeviceState *dev)
{
    CdjSdhiState *s = CDJ_SDHI(dev);
    const char *delay = getenv("CDJ_SD_INSERT");
    unsigned seconds = delay ? strtoul(delay, NULL, 0) : 20;

    timer_del(s->insert_timer);
    cdj_sdhi_cancel(s);
    s->arg0 = s->arg1 = s->seccnt = s->option = s->clkctl = s->stop = 0;
    s->info1 = s->info2 = 0;
    s->info1_mask = s->info2_mask = 0xffff;
    memset(s->rsp, 0, sizeof(s->rsp));
    s->sizereg = 512;
    s->inserted = s->ejected = false;
    s->lid_closed = true;
    if (seconds) {
        timer_mod(s->insert_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  (int64_t)seconds * NANOSECONDS_PER_SECOND);
    }
}

static void cdj_sdhi_realize(DeviceState *dev, Error **errp)
{
    CdjSdhiState *s = CDJ_SDHI(dev);
    memory_region_init_io(&s->iomem, OBJECT(s), &cdj_sdhi_ops, s,
                          "sh7764.sdhi", SDHI_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    qbus_init(&s->sdbus, sizeof(s->sdbus), TYPE_SD_BUS, dev, "sd-bus");
    s->insert_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_sdhi_insert, s);
    s->data_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_sdhi_data_ready, s);
    object_property_add_str(OBJECT(s), "card", cdj_sdhi_card_get,
                             cdj_sdhi_card_set);
}

/* Save host state and the in-flight virtual-time deadlines. The card and its
 * card state is handled by QEMU. The temporary write overlay is process-local. */
static int cdj_sdhi_post_load(void *opaque, int version)
{
    cdj_sdhi_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_sdhi_card = {
    .name = "sh7764-sdhi-card",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = cdj_sdhi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(arg0, CdjSdhiState),
        VMSTATE_UINT16(arg1, CdjSdhiState),
        VMSTATE_UINT16(seccnt, CdjSdhiState),
        VMSTATE_UINT16(sizereg, CdjSdhiState),
        VMSTATE_UINT16(option, CdjSdhiState),
        VMSTATE_UINT16(clkctl, CdjSdhiState),
        VMSTATE_UINT16(stop, CdjSdhiState),
        VMSTATE_UINT16(info1, CdjSdhiState),
        VMSTATE_UINT16(info2, CdjSdhiState),
        VMSTATE_UINT16(info1_mask, CdjSdhiState),
        VMSTATE_UINT16(info2_mask, CdjSdhiState),
        VMSTATE_UINT16_ARRAY(rsp, CdjSdhiState, 8),
        VMSTATE_UINT8_ARRAY(buf, CdjSdhiState, 512),
        VMSTATE_UINT32(buf_len, CdjSdhiState),
        VMSTATE_UINT32(buf_pos, CdjSdhiState),
        VMSTATE_UINT32(blocks_left, CdjSdhiState),
        VMSTATE_BOOL(multi, CdjSdhiState),
        VMSTATE_BOOL(writing, CdjSdhiState),
        VMSTATE_BOOL(inserted, CdjSdhiState),
        VMSTATE_BOOL(ejected, CdjSdhiState),
        VMSTATE_BOOL(data_ready, CdjSdhiState),
        VMSTATE_BOOL(lid_closed, CdjSdhiState),
        VMSTATE_TIMER_PTR(insert_timer, CdjSdhiState),
        VMSTATE_TIMER_PTR(data_timer, CdjSdhiState),
        VMSTATE_END_OF_LIST()
    }
};

static void cdj_sdhi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = cdj_sdhi_realize;
    dc->vmsd = &vmstate_sdhi_card;
    dc->reset = cdj_sdhi_reset;
}

static const TypeInfo cdj_sdhi_info = {
    .name = TYPE_CDJ_SDHI,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CdjSdhiState),
    .class_init = cdj_sdhi_class_init,
};

static void cdj_sdhi_register_types(void)
{
    type_register_static(&cdj_sdhi_info);
}
type_init(cdj_sdhi_register_types)

static CdjSdhiState *cdj_sdhi_singleton;

/* The SH7764 driver moves one block per RXRDY/TXRQ: read channel 5
 * (+0x80), write channel 4 (+0x70). */
bool cdj2000_sdhi_dma(bool writing, uint8_t *buf, unsigned len)
{
    CdjSdhiState *s = cdj_sdhi_singleton;
    if (!s || !s->inserted || !s->data_ready || s->writing != writing ||
        len > s->buf_len - s->buf_pos) {
        return false;
    }
    if (writing) {
        memcpy(s->buf + s->buf_pos, buf, len);
    } else {
        memcpy(buf, s->buf + s->buf_pos, len);
    }
    s->buf_pos += len;
    if (s->buf_pos == s->buf_len) {
        if (writing) {
            cdj_sdhi_flush_block(s);
        } else {
            cdj_sdhi_block_done(s);
        }
    }
    return true;
}

bool cdj2000_sdhi_card_init(MemoryRegion *sysmem, qemu_irq irq)
{
    const char *path = getenv("CDJ_SD_IMAGE");
    DeviceState *host, *card;
    BlockBackend *blk;
    QDict *options;

    if (!path || !*path) {
        return false;
    }
    /* Always use a temporary overlay, regardless of the caller's -snapshot.
     * Raw image source is opened read-only by the snapshot block driver. */
    options = qdict_new();
    qdict_put_str(options, "driver", "raw");
    blk = blk_new_open(path, NULL, options, BDRV_O_RDWR | BDRV_O_SNAPSHOT,
                       &error_fatal);
    host = qdev_new(TYPE_CDJ_SDHI);
    object_property_add_child(OBJECT(qdev_get_machine()), "sdhi", OBJECT(host));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(host), &error_fatal);
    cdj_sdhi_singleton = CDJ_SDHI(host);
    cdj_sdhi_singleton->irq = irq;
    cdj_dmac_fifo_connect(SDHI_BASE + SDHI_DATA, cdj2000_sdhi_dma);
    cdj_dmac_dei_connect(5, cdj_count_irq(
        cdj_sh7763_intc.irqs[S63_DMTE5], "SD read DMTE5"));
    sysbus_mmio_map(SYS_BUS_DEVICE(host), 0, SDHI_BASE);
    card = qdev_new(TYPE_SD_CARD);
    qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
    qdev_realize_and_unref(card, qdev_get_child_bus(host, "sd-bus"), &error_fatal);
    blk_unref(blk);
    return true;
}

bool cdj2000_sdhi_lid_closed(void)
{
    return cdj_sdhi_singleton && cdj_sdhi_singleton->lid_closed;
}
