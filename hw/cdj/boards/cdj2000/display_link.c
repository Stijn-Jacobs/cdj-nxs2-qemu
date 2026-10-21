/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifdef CDJ_DISPLAY_LINK_TEST
#include "tests/display_link_stubs.h"
#else
#include "sh7763.h"
#include "cdj_getenv.h"
#include "exec/cpu-common.h"
#include "qemu/cutils.h"
#include "qemu/timer.h"
#endif
#include <sys/stat.h>
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
#define LINK_QUEUE      64

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
    /* With pacing on, answers that land while the receive is not armed, or
     * its last frame not yet retired, wait here in order instead of being
     * dropped. */
    uint8_t q[LINK_QUEUE * LINK_BUFFER];
    uint32_t q_len[LINK_QUEUE];
    uint32_t q_head, q_count;
    uint64_t q_dropped;
    /* Wire pacing: when the last answer went into MAIN's buffer (virtual ns),
     * and the timer that hands over the next one once its wire time is up. */
    int64_t rx_last_ns;
    QEMUTimer *rx_gap;
    /* Test-only MAIN TX owns one immutable copied DMA packet until delivery.
     * Positive-delay snapshots are explicitly refused; default snapshots keep
     * their original version3 fields and never have a delayed owner. */
    QEMUTimer *tx_delay;
    uint8_t tx_packet[LINK_BUFFER];
    uint32_t tx_length;
    bool tx_pending, fault_delivering;
    int64_t tx_scheduled_ns, tx_due_ns;
    uint64_t tx_applied, tx_refused;
} LinkPort;

/* Four one-shot cold-boot fault recipes. All bytes belong to this bounded
 * copied owner; MAIN memory and its original DMA completion are unchanged. */
typedef enum LinkFaultCase {
    FAULT_NONE, FAULT_NATIVE_ROOT_CRC, FAULT_NATIVE_WAVE_TRUNCATE,
    FAULT_STOCK_BROWSER_CRC, FAULT_STOCK_WAVE_TRUNCATE
} LinkFaultCase;
static LinkPort link_rx, link_tx;
static struct {
    LinkFaultCase kind;
    const char *name;
    FILE *report;
    bool applied, announcement_valid, prior_unit1;
    uint8_t original[LINK_BUFFER], delivered[LINK_BUFFER], announcement[64];
    uint32_t original_length, delivered_length;
} link_fault;
static uint16_t fault_u16(const uint8_t *p) {return p[0] | (uint16_t)p[1] << 8;}
static uint32_t fault_u32(const uint8_t *p) {return fault_u16(p) | (uint32_t)fault_u16(p+2) << 16;}
static bool fault_crc(const uint8_t *p, uint32_t len)
{
    if (len < 2) {return false;}
    uint16_t crc=0;
    for (uint32_t n=0;n<len-2;n++) {
        crc^=(uint16_t)p[n]<<8;
        for (unsigned bit=0;bit<8;bit++) {crc=(crc<<1)^((crc&0x8000)?0x1021:0);}
    }
    return crc==fault_u16(p+len-2);
}
static bool fault_native(const uint8_t *p, uint32_t len)
{
    if (len<48 || len>LINK_BUFFER || len%16 || memcmp(p,"DLNK",4) || p[4]!=3 ||
        p[5]!=1 || p[7]!=3 || p[25] || !fault_u32(p+8) || !fault_u32(p+12) ||
        !fault_u32(p+16) || !fault_u32(p+28) || fault_u32(p+36) || !fault_crc(p,len)) {return false;}
    uint32_t payload=fault_u16(p+26);
    if (fault_u16(p+34)!=len || ((40+payload+2+15)&~15u)!=len) {return false;}
    for (uint32_t n=40+payload;n<len-2;n++) {if (p[n]) {return false;}}
    return true;
}
static bool fault_select(const uint8_t *p, uint32_t len)
{
    if (!link_fault.kind || link_fault.applied) {return false;}
    bool match=false;
    if (link_fault.kind==FAULT_NATIVE_ROOT_CRC || link_fault.kind==FAULT_NATIVE_WAVE_TRUNCATE) {
        if (!fault_native(p,len)) {return false;}
        if (link_fault.kind==FAULT_NATIVE_ROOT_CRC) {
            /* Primary PageRequest context+8/key+12, Page count+48, after
             * the native 40-byte header; compact one-row extent >=86. */
            match=p[6]==2 && fault_u16(p+32)==0x110 && fault_u16(p+26)>=86 &&
                  fault_u32(p+48)==1 && !fault_u32(p+52) && p[88]>=1 && p[88]<=6 &&
                  p[89]<=1 && !p[90] && !p[91];
        } else {
            match=p[6]==3 && fault_u16(p+32)==0x103 && fault_u16(p+26)==892 &&
                  len==944 && fault_u32(p+40)==2;
        }
    } else {
        bool valid=len>=32 && len%2==0 && fault_crc(p,len);
        unsigned command=valid?(fault_u16(p)&0x7fff):0;
        bool body=valid && link_fault.announcement_valid &&
            (fault_u16(link_fault.announcement+58)&0x7fff)==command &&
            (uint32_t)fault_u16(link_fault.announcement+60)*2==len;
        if (!body) {
            if (len==64 && fault_crc(p,len) && memcmp(p,"DLNK",4)) {
                unsigned announcement=fault_u16(p+58)&0x7fff;
                link_fault.announcement_valid=announcement==0x10 || announcement==0x20;
                if (link_fault.announcement_valid) {memcpy(link_fault.announcement,p,64);}
            }
            return false;
        }
        if (command==0x20 && fault_u32(p+2)==1) {link_fault.prior_unit1=true;}
        match=link_fault.kind==FAULT_STOCK_BROWSER_CRC ? command==0x10 :
              command==0x20 && fault_u32(p+2)==2 && link_fault.prior_unit1;
    }
    if (!match) {return false;}
    memcpy(link_fault.original,p,len);memcpy(link_fault.delivered,p,len);
    link_fault.original_length=len;link_fault.delivered_length=len;
    if (link_fault.kind==FAULT_NATIVE_ROOT_CRC) {link_fault.delivered[40]^=1;}
    else if (link_fault.kind==FAULT_STOCK_BROWSER_CRC) {link_fault.delivered[20]^=1;}
    else {link_fault.delivered_length-=16;}
    return true;
}
static void fault_hex(FILE *f,const uint8_t *p,uint32_t len)
{
    fputc('"',f);for (uint32_t n=0;n<len;n++) {fprintf(f,"%02x",p[n]);}fputc('"',f);
}
static void fault_report(LinkPort *p)
{
    FILE *f=link_fault.report;
    const uint8_t *b=link_fault.original;
    bool native=link_fault.kind==FAULT_NATIVE_ROOT_CRC || link_fault.kind==FAULT_NATIVE_WAVE_TRUNCATE;
    fprintf(f,"{\"schema\":1,\"case\":\"%s\",\"scope\":\"submitted_to_sport\","
        "\"applied_count\":1,\"snapshot_loaded\":false,\"direction\":\"main_to_gui\","
        "\"at_virtual_ns\":%" PRId64 ",\"original_length\":%u,\"delivered_length\":%u,\"original_hex\":",
        link_fault.name,qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),link_fault.original_length,link_fault.delivered_length);
    fault_hex(f,b,link_fault.original_length);fputs(",\"delivered_hex\":",f);
    fault_hex(f,link_fault.delivered,link_fault.delivered_length);fputs(",\"announcement_hex\":",f);
    if (native) {fputs("null",f);} else {fault_hex(f,link_fault.announcement,64);}
    fputs(",\"selector\":{",f);
    if (native) {
        fprintf(f,"\"kind\":%u,\"lane\":%u,\"flags\":%u,\"opcode\":%u,\"offset\":%u,\"payload_length\":%u,"
            "\"source_epoch\":%u,\"destination_epoch\":%u,\"sequence\":%u,\"transaction\":%u",
            b[5],b[6],b[7],fault_u16(b+32),fault_u32(b+36),fault_u16(b+26),
            fault_u32(b+8),fault_u32(b+12),fault_u32(b+16),fault_u32(b+28));
        if (link_fault.kind==FAULT_NATIVE_ROOT_CRC) {
            fprintf(f,",\"context\":%u,\"context_key\":%u,\"row_count\":%u",fault_u32(b+48),fault_u32(b+52),b[88]);
        } else {fprintf(f,",\"unit\":%u",fault_u32(b+40));}
    } else {
        fprintf(f,"\"command\":%u,\"announced_length\":%u,\"unit\":",fault_u16(b)&0x7fff,fault_u16(link_fault.announcement+60)*2u);
        if (link_fault.kind==FAULT_STOCK_WAVE_TRUNCATE) {fprintf(f,"%u",fault_u32(b+2));} else {fputs("null",f);}
        fprintf(f,",\"prior_unit1\":%s",link_fault.kind==FAULT_STOCK_WAVE_TRUNCATE?"true":"false");
    }
    fprintf(f,"},\"matches\":1,\"overflow\":%" PRIu64 ",\"refused\":%" PRIu64 "}\n",link_rx.q_dropped,p->tx_refused);
    bool failed=ferror(f) || fflush(f);
    if (fclose(f)) {failed=true;}
    link_fault.report=NULL;
    if (failed) {error_report("display link: fault witness write failed after delivery");exit(1);}
    info_report("display link: fault delivered case=%s original=%u delivered=%u applied=1",link_fault.name,link_fault.original_length,link_fault.delivered_length);
}

/* CDJ_LINK_RX_PACE=1 queues the display's answers and paces them at wire
 * rate. SPORT1 runs at 2.891 Mbit/s (TCLKDIV=16 at 98.304 MHz SCLK): a frame
 * cannot land sooner after the one before than its own bits take on the wire.
 * CDJ_LINK_RX_GAP_US=<us> paces with a fixed minimum gap instead, and implies
 * the queue. Without either an answer is written at once, and dropped when
 * the receive DMA is not armed. */
#define LINK_WIRE_BPS   2891294
static bool link_rx_paced;
static int64_t link_gap_us = -1;
static uint64_t link_gap_deferrals;
static uint32_t link_tx_delay_us;

static int64_t link_wire_ns(uint32_t len)
{
    return link_gap_us >= 0 ? link_gap_us * 1000
                            : (int64_t)len * 8 * NANOSECONDS_PER_SECOND / LINK_WIRE_BPS;
}

static bool link_log, link_log_full;

/* CDJ_LINK_CAPTURE=<path>: every MAIN-to-display packet as "SPRX", u32le
 * length, bytes. Flushed per record so a reader can follow a live run. */
static FILE *link_cap;
static Notifier link_cap_exit;

static void link_capture(const uint8_t *pkt, uint32_t len)
{
    uint8_t head[8] = { 'S', 'P', 'R', 'X', len, len >> 8, len >> 16, len >> 24 };

    if (!link_cap || !len) {
        return;
    }
    if (fwrite(head, sizeof(head), 1, link_cap) != 1 ||
        fwrite(pkt, len, 1, link_cap) != 1 || fflush(link_cap)) {
        /* Stop at once: a record cut short would desync the reader. */
        error_report("display link: capture write failed, capture stopped");
        fclose(link_cap);
        link_cap = NULL;
    }
}

static void link_capture_close(Notifier *n, void *data)
{
    if (link_cap) {
        fclose(link_cap);
        link_cap = NULL;
    }
}

static void link_dump(const char *dir, const uint8_t *pkt, size_t len)
{
    if (link_log) {
        info_report("display link: %s, %zu bytes", dir, len);
        qemu_hexdump(stderr, "display link", pkt, MIN(len, link_log_full ? LINK_BUFFER : 64));
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

/* This is the original delivery/completion order, shared by the unchanged
 * zero-delay path and actual timer expiry. No completion precedes delivery. */
static void link_send_complete(LinkPort *p, const uint8_t *pkt, uint32_t len)
{
    bool selected=fault_select(pkt,len);
    const uint8_t *delivered=selected?link_fault.delivered:pkt;
    uint32_t delivered_length=selected?link_fault.delivered_length:len;
    p->fault_delivering=link_fault.kind!=FAULT_NONE;
    link_dump("MAIN to display", delivered, delivered_length);
    link_capture(delivered, delivered_length);
    cdj2000_display_send(delivered, delivered_length);
    if (selected) {link_fault.applied=true;fault_report(p);}
    p->fault_delivering=false;
    p->reg[DMA_CTRL / 4] &= ~CTRL_START;
    p->units = len / LINK_UNIT;
    link_update(p);
    p->reg[SER_EVENT / 4] |= SER_TX_DONE;
    link_ser_update(p);
}

static void link_tx_delay_up(void *opaque)
{
    LinkPort *p = opaque;
    if (!p->tx_pending) { return; }
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now < p->tx_due_ns) { timer_mod(p->tx_delay, p->tx_due_ns); return; }
    /* Keep the owner busy through delivery: even a reentrant consumer cannot
     * overwrite bytes while cdj2000_display_send borrows this packet. */
    link_send_complete(p, p->tx_packet, p->tx_length);
    p->tx_pending = false;
    if (p->tx_applied != UINT64_MAX) { p->tx_applied++; }
    if (link_log) {
        info_report("display link: tx-delay delivered delay_us=%u scheduled_ns=%" PRId64
                    " due_ns=%" PRId64 " delivered_ns=%" PRId64 " applied=%" PRIu64,
                    link_tx_delay_us, p->tx_scheduled_ns, p->tx_due_ns, now, p->tx_applied);
    }
}

static void link_send(LinkPort *p)
{
    uint32_t len = link_len(p);
    if (link_tx_delay_us) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t delay = (int64_t)link_tx_delay_us * 1000;
        if (now > INT64_MAX - delay) {
            error_report("display link: tx-delay virtual deadline overflow; request refused");
            p->reg[DMA_CTRL / 4] &= ~CTRL_START;
            return;
        }
        cpu_physical_memory_read(A7ADDR(p->reg[p->addr_reg / 4]), p->tx_packet, len);
        p->tx_length = len;
        p->tx_scheduled_ns = now;
        p->tx_due_ns = now + delay;
        p->tx_pending = true;
        timer_mod(p->tx_delay, p->tx_due_ns);
        return;
    }
    uint8_t pkt[LINK_BUFFER];
    cpu_physical_memory_read(A7ADDR(p->reg[p->addr_reg / 4]), pkt, len);
    link_send_complete(p, pkt, len);
}

static void link_deliver(LinkPort *p, const uint8_t *pkt, size_t len)
{
    uint32_t room = link_len(p);

    link_dump("display to MAIN", pkt, len);
    cpu_physical_memory_write(A7ADDR(p->reg[p->addr_reg / 4]), pkt,
                              MIN(len, room));
    p->reg[DMA_CTRL / 4] &= ~CTRL_START;
    p->units = room / LINK_UNIT;
    link_update(p);
}

/* Hands the oldest waiting answer over once the receive is armed and idle. */
static void link_rx_pump(LinkPort *p)
{
    if (p->q_count && (p->reg[DMA_CTRL / 4] & CTRL_START) && !p->units) {
        uint32_t h = p->q_head;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        int64_t due = p->rx_last_ns + link_wire_ns(p->q_len[h]);

        if (now < due) {
            if (link_log && link_gap_us > 0) {
                if (link_gap_deferrals != UINT64_MAX) { link_gap_deferrals++; }
                info_report("display link: rx-gap deferred gap_us=%" PRId64
                            " now_ns=%" PRId64 " due_ns=%" PRId64 " applied=%" PRIu64,
                            link_gap_us, now, due, link_gap_deferrals);
            }
            timer_mod(p->rx_gap, due);
            return;
        }
        p->rx_last_ns = now;

        p->q_head = (h + 1) % LINK_QUEUE;
        p->q_count--;
        link_deliver(p, p->q + h * LINK_BUFFER, p->q_len[h]);
    }
}

static void link_rx_gap_up(void *opaque)
{
    link_rx_pump(opaque);
}

void cdj2000_display_link_receive(const uint8_t *pkt, size_t len)
{
    LinkPort *p = &link_rx;
    uint32_t t;

    if (!link_rx_paced) {
        if (!(p->reg[DMA_CTRL / 4] & CTRL_START)) {
            warn_report("display link: answer of %zu bytes dropped, receive DMA "
                        "not armed", len);
            return;
        }
        link_deliver(p, pkt, len);
        return;
    }
    if (p->q_count == LINK_QUEUE) {
        /* Fail loud: a dropped link frame is a broken run. */
        if (!p->q_dropped++) {
            error_report("display link: receive queue full, oldest answer dropped");
        }
        p->q_head = (p->q_head + 1) % LINK_QUEUE;
        p->q_count--;
    }
    t = (p->q_head + p->q_count++) % LINK_QUEUE;
    memcpy(p->q + t * LINK_BUFFER, pkt, MIN(len, LINK_BUFFER));
    p->q_len[t] = MIN(len, LINK_BUFFER);
    link_rx_pump(p);
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
        if (p == &link_rx) {
            link_rx_pump(p);
        }
        return;
    }
    p->reg[off / 4] = deposit32(old, shift, 8 * size, val);
    switch (off / 4) {
    case DMA_CTRL / 4:
        if (p == &link_tx && (p->tx_pending || p->fault_delivering)) {
            /* A reset/restart cannot replace a captured delayed packet. Keep
             * the original owner and restore its busy bit until it completes. */
            p->reg[DMA_CTRL / 4] |= CTRL_START;
            if (p->tx_refused != UINT64_MAX) { p->tx_refused++; }
            if (p->tx_pending) {
                error_report("display link: tx-delay control refused while packet pending, refused=%" PRIu64,p->tx_refused);
            } else {
                error_report("display link: fault control refused while copied packet borrowed, refused=%" PRIu64,p->tx_refused);
            }
            return;
        }
        if (p == &link_tx && !(old & CTRL_START) &&
            (p->reg[off / 4] & CTRL_START)) {
            link_send(p);
        }
        if (p == &link_rx) {
            link_rx_pump(p);
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

static int link_pre_save(void *opaque)
{
    if (link_fault.kind) {error_report("display link: snapshots unavailable with CDJ_LINK_FAULT");return -EINVAL;}
    if (link_tx_delay_us) {
        error_report("display link: snapshots unavailable with CDJ_LINK_TX_DELAY_US");
        return -EINVAL;
    }
    return 0;
}
static int link_post_load(void *opaque, int version_id)
{
    return link_pre_save(opaque);
}

static const VMStateDescription vmstate_link_port = {
    .name = "cdj2000-display-link",
    .version_id = 3,
    .minimum_version_id = 3,
    .pre_save = link_pre_save,
    .pre_load = link_pre_save,
    .post_load = link_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(units, LinkPort),
        VMSTATE_UINT32_ARRAY(reg, LinkPort, LINK_WINDOW / 4),
        VMSTATE_BUFFER(q, LinkPort),
        VMSTATE_UINT32_ARRAY(q_len, LinkPort, LINK_QUEUE),
        VMSTATE_UINT32(q_head, LinkPort),
        VMSTATE_UINT32(q_count, LinkPort),
        VMSTATE_INT64(rx_last_ns, LinkPort),
        VMSTATE_TIMER_PTR(rx_gap, LinkPort),
        VMSTATE_END_OF_LIST()
    }
};

static void link_port_init(LinkPort *p, MemoryRegion *sysmem, const char *name,
                           hwaddr base, hwaddr addr_reg, qemu_irq irq,
                           qemu_irq ser_irq)
{
    p->addr_reg = addr_reg;
    p->rx_gap = timer_new_ns(QEMU_CLOCK_VIRTUAL, link_rx_gap_up, p);
    p->tx_delay = timer_new_ns(QEMU_CLOCK_VIRTUAL, link_tx_delay_up, p);
    p->len_reg = addr_reg + 8;
    p->irq = irq;
    p->ser_irq = ser_irq;
    vmstate_register_any(NULL, &vmstate_link_port, p);
    memory_region_init_io(&p->iomem, NULL, &link_ops, p, name, LINK_WINDOW);
    memory_region_add_subregion(sysmem, base, &p->iomem);
}

/* Decimal microseconds 0..50000; unset is 0. */
static bool link_us_parse(const char *text, uint32_t *us)
{
    uint32_t n = 0;
    if (!text) { *us = 0; return true; }
    if (!*text) { return false; }
    for (; *text; text++) {
        if (*text < '0' || *text > '9') { return false; }
        unsigned d = *text - '0';
        if (n > 5000 || (n == 5000 && d != 0)) { return false; }
        n = n * 10 + d;
    }
    *us = n;
    return true;
}

static bool link_fault_configure(const char *name,const char *report)
{
    if (!name && !report) {return true;}
    if (!name || !report || !*name || report[0]!='/' || !report[1]) {return false;}
    static const char *names[]={"native-root-crc","native-wave-truncate","stock-browser-crc","stock-wave-truncate"};
    unsigned kind=0;
    for (unsigned n=0;n<4;n++) {if (!strcmp(name,names[n])) {kind=n+1;break;}}
    uint32_t ppi=0;
    const char *cap=getenv("CDJ_LINK_CAPTURE");
    if (!kind || link_tx_delay_us || getenv("CDJ_LINK_RX_GAP_US") ||
        !link_us_parse(getenv("CDJ_BF531_PPI_DELAY_US"),&ppi) || ppi ||
        getenv("CDJ_BF531_PPI_DELAY_COUNT") || (cap && !strcmp(cap,report))) {return false;}
    FILE *f=fopen(report,"wx");
    if (!f) {return false;}
    link_fault.kind=kind;link_fault.name=names[kind-1];link_fault.report=f;
    return true;
}

/* CDJ_LINK_CAPTURE=<path> records MAIN's packets (see link_capture).
 * CDJ_BF531_LINK_LOG=1 logs every packet: direction, length and its first
 * 64 bytes. CDJ_LINK_LOG_FULL=1 logs up to the complete 2048-byte buffer.
 * CDJ_LINK_TX_DELAY_US=0..50000 is a cold-boot fault injection: each MAIN
 * packet is copied when sent and delivered that much virtual time later, and
 * only then do the DMA end and the serial TX_DONE follow, in the usual order.
 * A restart while a packet is held is refused, and so are snapshots. */
void cdj2000_display_link_init(MemoryRegion *sysmem, qemu_irq rx, qemu_irq tx,
                               qemu_irq tx_ser)
{
    const char *gap = getenv("CDJ_LINK_RX_GAP_US");
    uint32_t gap_us;

    if (!link_us_parse(getenv("CDJ_LINK_TX_DELAY_US"), &link_tx_delay_us)) {
        error_report("display link: CDJ_LINK_TX_DELAY_US must be decimal 0..50000");
        exit(1);
    }
    if (!link_us_parse(gap, &gap_us)) {
        error_report("display link: CDJ_LINK_RX_GAP_US must be decimal 0..50000");
        exit(1);
    }

    if (!link_fault_configure(getenv("CDJ_LINK_FAULT"),getenv("CDJ_LINK_FAULT_REPORT"))) {
        error_report("display link: invalid/conflicting CDJ_LINK_FAULT configuration or report is not fresh");exit(1);
    }
    const char *cap = getenv("CDJ_LINK_CAPTURE");

    link_log = getenv("CDJ_BF531_LINK_LOG");
    link_log_full = g_strcmp0(getenv("CDJ_LINK_LOG_FULL"), "1") == 0;
    if (cap) {
        link_cap = fopen(cap, "wb");
        if (!link_cap) {
            error_report("display link: cannot open capture %s", cap);
        } else {
            if (link_fault.report) {
                struct stat capture,report;
                if (fstat(fileno(link_cap),&capture) || fstat(fileno(link_fault.report),&report) ||
                    (capture.st_dev==report.st_dev && capture.st_ino==report.st_ino)) {
                    error_report("display link: fault report/capture files conflict");exit(1);
                }
            }
            link_cap_exit.notify = link_capture_close;
            cdj_add_exit_report(&link_cap_exit);
        }
    }
    if (gap) {
        link_gap_us = gap_us;
    }
    link_rx_paced = gap || getenv("CDJ_LINK_RX_PACE");
    link_port_init(&link_rx, sysmem, "sh7763.display-rx", 0xFF400000,
                   0x1018, rx, NULL);
    link_port_init(&link_tx, sysmem, "sh7763.display-tx", 0xFF500000,
                   0x1008, tx, tx_ser);
}
