/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
#include "usb_r8a66597.h"
/*
 * SH7724 USB1 at 0xA4D90000 (physical 0x04D90000): the same module as USB0,
 * used as a function controller. It is the rear USB-B port, where the deck is
 * an audio + MIDI + HID device (VID 0x2B73, PID 0x0005).
 *
 * The register side follows manual section 31. The bus side is one call per
 * token (cdj_usbf_setup/in/out, cdj_usbf_bus_reset), made by whatever plays
 * the host; usbf_host.c is one. Isochronous pipes and DMA on the FIFO ports
 * are not modelled, so the audio interface carries nothing.
 *
 *   CDJ_USBF_DEBUG=1   log register accesses, tokens and state changes
 */
#define CDJ_USBF_BASE    0x04D90000
#define CDJ_USBF_SIZE    0x1000
/* The largest pipe buffer PIPEBUF can describe for a bulk pipe. */
#define CDJ_USBF_BUFSIZE 2048

/* SYSCFG: bit 7 HSE, bit 4 DPRPU (the D+ pull-up, i.e. "connect"), bit 0
 * USBE. */
#define CDJ_USBF_HSE     0x0080
#define CDJ_USBF_DPRPU   0x0010
#define CDJ_USBF_USBE    0x0001

/* INTSTS0: bit 15 VBINT, 12 DVST, 11 CTRT, 7 VBSTS, 6:4 DVSQ, 3 VALID, 2:0
 * CTSQ. The driver clears a status bit by writing it as 0. */
#define CDJ_USBF_VBINT   0x8000
#define CDJ_USBF_DVST    0x1000
#define CDJ_USBF_CTRT    0x0800
#define CDJ_USBF_VBSTS   0x0080
#define CDJ_USBF_VALID   0x0008
#define CDJ_USBF_STS0_CLEARABLE 0xF808

enum { DVSQ_POWERED, DVSQ_DEFAULT, DVSQ_ADDRESS, DVSQ_CONFIGURED };

/* Control transfer stage, manual figure 31.8. */
enum {
    CTSQ_IDLE,
    CTSQ_READ_DATA, CTSQ_READ_STATUS,
    CTSQ_WRITE_DATA, CTSQ_WRITE_STATUS,
    CTSQ_NODATA_STATUS,
};

/* CFIFOSEL bit 5 ISEL: the DCP buffer is accessed in the transmitting
 * direction. DxFIFOSEL bit 12 DREQE. */
#define CDJ_USBF_ISEL    0x0020
#define CDJ_USBF_DREQE   0x1000
/* PIPECFG 15:14 TYPE (0 = pipe unused), bit 8 CNTMD. */
#define CDJ_USBF_TYPE    0xC000
#define CDJ_USBF_CNTMD   0x0100
/* PIPEnCTR: bit 14 INBUFM, bit 9 ACLRM, bits 8 and 7 SQCLR/SQSET (write
 * only). PID bit 1 set is STALL. */
#define CDJ_USBF_INBUFM  0x4000
#define CDJ_USBF_ACLRM   0x0200
#define CDJ_USBF_SQBITS  0x0180
#define CDJ_USBF_PID_STALL 0x0002

#define TYPE_CDJ_USBF "cdj.usbf"
OBJECT_DECLARE_SIMPLE_TYPE(CdjUsbfState, CDJ_USBF)

/*
 * A pipe buffer has two sides. The FIFO port reads or writes one; on a
 * transmitting pipe a finished buffer moves to the other, which IN tokens
 * drain. A second buffer written meanwhile waits closed until that side is
 * free, which is what a double-buffered pipe does.
 */
typedef struct {
    uint8_t fifo[CDJ_USBF_BUFSIZE];
    unsigned len, pos;
    bool closed;
    uint8_t sie[CDJ_USBF_BUFSIZE];
    unsigned sie_len, sie_pos;
    bool sie_valid;                /* may be valid and empty: a zero-length packet */
} CdjUsbfPipe;

struct CdjUsbfState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    Notifier exit;

    bool vbus;
    unsigned dvsq, ctsq;
    /* A SET_ADDRESS the module answers itself takes effect at its status
     * stage, and so does the device state a SET_CONFIGURATION leads to
     * (manual figure 31.7: DVST on "execution" of the request). */
    bool addr_pending;
    uint8_t new_addr;
    unsigned next_dvsq;            /* 0 = none pending */

    CdjUsbfPipe pipe[CDJ_USB_NR_PIPES];
    unsigned port_pipe[CDJ_USB_NR_FIFO_PORTS];
    uint16_t pipecfg[CDJ_USB_NR_PIPES];
    uint16_t pipebuf[CDJ_USB_NR_PIPES];
    uint16_t pipemaxp[CDJ_USB_NR_PIPES];
    uint16_t pipeperi[CDJ_USB_NR_PIPES];

    uint16_t reg[CDJ_USBF_SIZE / 2];
};

static bool cdj_usbf_debug(void)
{
    return getenv("CDJ_USBF_DEBUG") != NULL;
}

static uint16_t cdj_usbf_intsts0(CdjUsbfState *s)
{
    uint16_t v = s->reg[CDJ_USB_INTSTS0 / 2] & CDJ_USBF_STS0_CLEARABLE;

    if (s->reg[CDJ_USB_BRDYSTS / 2] & s->reg[CDJ_USB_BRDYENB / 2]) {
        v |= CDJ_USB_BRDY;
    }
    if (s->reg[CDJ_USB_NRDYSTS / 2] & s->reg[CDJ_USB_NRDYENB / 2]) {
        v |= CDJ_USB_NRDY;
    }
    if (s->reg[CDJ_USB_BEMPSTS / 2] & s->reg[CDJ_USB_BEMPENB / 2]) {
        v |= CDJ_USB_BEMP;
    }
    if (s->vbus) {
        v |= CDJ_USBF_VBSTS;
    }
    return v | s->dvsq << 4 | s->ctsq;
}

static void cdj_usbf_update_irq(CdjUsbfState *s)
{
    qemu_set_irq(s->irq, (cdj_usbf_intsts0(s) & s->reg[CDJ_USB_INTENB0 / 2]
                          & 0xFF00) != 0);
}

static unsigned cdj_usbf_ctr(unsigned pipe)
{
    return (pipe ? CDJ_USB_PIPECTR(pipe) : CDJ_USB_DCPCTR) / 2;
}

static unsigned cdj_usbf_maxp(CdjUsbfState *s, unsigned pipe)
{
    unsigned maxp = pipe ? s->pipemaxp[pipe] & 0x07ff
                         : s->reg[CDJ_USB_DCPMAXP / 2] & 0x007f;

    return maxp && maxp <= CDJ_USBF_BUFSIZE ? maxp : 64;
}

/* How many bytes written to the FIFO port finish a buffer by themselves: one
 * packet, or with CNTMD the whole buffer (PIPEBUF 14:10 BUFSIZE, in 64-byte
 * blocks less one). */
static unsigned cdj_usbf_fill(CdjUsbfState *s, unsigned pipe)
{
    if (pipe && (s->pipecfg[pipe] & CDJ_USBF_CNTMD)) {
        return (((s->pipebuf[pipe] >> 10) & 0x1f) + 1) * 64;
    }
    return cdj_usbf_maxp(s, pipe);
}

static bool cdj_usbf_transmits(CdjUsbfState *s, unsigned pipe)
{
    return pipe ? (s->pipecfg[pipe] & CDJ_USB_DIR) != 0
                : (s->reg[CDJ_USB_CFIFOSEL / 2] & CDJ_USBF_ISEL) != 0;
}

static void cdj_usbf_pipe_clear(CdjUsbfPipe *p)
{
    p->len = p->pos = 0;
    p->sie_len = p->sie_pos = 0;
    p->closed = p->sie_valid = false;
}

/* The buffer on the FIFO side is complete: hand it to the bus side, or leave
 * it closed until the bus side has been sent. */
static void cdj_usbf_close(CdjUsbfPipe *p)
{
    if (p->sie_valid) {
        p->closed = true;
        return;
    }
    memcpy(p->sie, p->fifo, p->len);
    p->sie_len = p->len;
    p->sie_pos = 0;
    p->sie_valid = true;
    p->len = p->pos = 0;
    p->closed = false;
}

/* One IN packet out of the bus side. BEMP once everything written has been
 * sent; on a pipe, BRDY whenever the FIFO side can be written again (the DCP
 * raises no BRDY when transmitting). */
static int cdj_usbf_send(CdjUsbfState *s, unsigned pipe, uint8_t *buf,
                         unsigned max)
{
    CdjUsbfPipe *p = &s->pipe[pipe];
    unsigned n = MIN(MIN(p->sie_len - p->sie_pos, cdj_usbf_maxp(s, pipe)), max);

    memcpy(buf, p->sie + p->sie_pos, n);
    p->sie_pos += n;
    if (p->sie_pos == p->sie_len) {
        p->sie_valid = false;
        if (p->closed) {
            cdj_usbf_close(p);
        } else {
            s->reg[CDJ_USB_BEMPSTS / 2] |= 1u << pipe;
        }
        if (pipe) {
            s->reg[CDJ_USB_BRDYSTS / 2] |= 1u << pipe;
        }
    }
    return (int)n;
}

/* One OUT packet into the FIFO side, refused while the last one is unread. */
static int cdj_usbf_receive(CdjUsbfState *s, unsigned pipe, const uint8_t *buf,
                            unsigned len)
{
    CdjUsbfPipe *p = &s->pipe[pipe];

    if (p->pos < p->len || len > sizeof(p->fifo)) {
        return CDJ_USBF_NAK;
    }
    memcpy(p->fifo, buf, len);
    p->len = len;
    p->pos = 0;
    s->reg[CDJ_USB_BRDYSTS / 2] |= 1u << pipe;
    return (int)len;
}

/* What the PID bits make of a token: 0 to go ahead. */
static int cdj_usbf_pid(CdjUsbfState *s, unsigned pipe)
{
    uint16_t pid = s->reg[cdj_usbf_ctr(pipe)] & CDJ_USB_PID_MASK;

    if (pid & CDJ_USBF_PID_STALL) {
        return CDJ_USBF_STALL;
    }
    return pid == CDJ_USB_PID_BUF ? 0 : CDJ_USBF_NAK;
}

static unsigned cdj_usbf_find_pipe(CdjUsbfState *s, unsigned ep, bool transmit)
{
    unsigned pipe;

    for (pipe = 1; pipe < CDJ_USB_NR_PIPES; pipe++) {
        uint16_t cfg = s->pipecfg[pipe];

        if ((cfg & CDJ_USBF_TYPE) && (cfg & 0x000f) == ep
            && !(cfg & CDJ_USB_DIR) == !transmit) {
            return pipe;
        }
    }
    return 0;
}

static void cdj_usbf_stage(CdjUsbfState *s, unsigned ctsq)
{
    s->ctsq = ctsq;
    s->reg[CDJ_USB_INTSTS0 / 2] |= CDJ_USBF_CTRT;
}

static void cdj_usbf_device_state(CdjUsbfState *s, unsigned dvsq)
{
    s->dvsq = dvsq;
    s->reg[CDJ_USB_INTSTS0 / 2] |= CDJ_USBF_DVST;
    if (cdj_usbf_debug()) {
        info_report("usbf: device state %u", dvsq);
    }
}

/* The status stage: the driver ends the transfer with PID = BUF and CCPL. */
static int cdj_usbf_status(CdjUsbfState *s)
{
    int r = cdj_usbf_pid(s, 0);

    if (r) {
        return r;
    }
    if (!(s->reg[CDJ_USB_DCPCTR / 2] & CDJ_USB_CCPL)) {
        return CDJ_USBF_NAK;
    }
    s->reg[CDJ_USB_DCPCTR / 2] &= ~CDJ_USB_CCPL;
    cdj_usbf_stage(s, CTSQ_IDLE);
    if (s->next_dvsq) {
        cdj_usbf_device_state(s, s->next_dvsq);
        s->next_dvsq = 0;
    }
    return 0;
}

static int cdj_usbf_dcp_in(CdjUsbfState *s, uint8_t *buf, unsigned max)
{
    int r;

    if (s->addr_pending) {
        s->addr_pending = false;
        s->reg[CDJ_USB_USBADDR / 2] = s->new_addr;
        cdj_usbf_device_state(s, s->new_addr ? DVSQ_ADDRESS : DVSQ_DEFAULT);
        return 0;
    }
    switch (s->ctsq) {
    case CTSQ_READ_DATA:
        r = cdj_usbf_pid(s, 0);
        if (r) {
            return r;
        }
        return s->pipe[0].sie_valid ? cdj_usbf_send(s, 0, buf, max)
                                    : CDJ_USBF_NAK;
    case CTSQ_WRITE_DATA:
        cdj_usbf_stage(s, CTSQ_WRITE_STATUS);
        /* fall through */
    case CTSQ_WRITE_STATUS:
    case CTSQ_NODATA_STATUS:
        return cdj_usbf_status(s);
    default:
        return CDJ_USBF_NAK;
    }
}

static int cdj_usbf_dcp_out(CdjUsbfState *s, const uint8_t *buf, unsigned len)
{
    int r;

    switch (s->ctsq) {
    case CTSQ_WRITE_DATA:
        r = cdj_usbf_pid(s, 0);
        return r ? r : cdj_usbf_receive(s, 0, buf, len);
    case CTSQ_READ_DATA:
        cdj_usbf_stage(s, CTSQ_READ_STATUS);
        /* fall through */
    case CTSQ_READ_STATUS:
        return cdj_usbf_status(s);
    default:
        return CDJ_USBF_NAK;
    }
}

/* The deck has its pull-up on, so a host sees a device on the port. */
bool cdj_usbf_attached(CdjUsbfState *s)
{
    uint16_t need = CDJ_USBF_USBE | CDJ_USBF_DPRPU;

    return s->vbus && (s->reg[CDJ_USB_SYSCFG / 2] & need) == need;
}

void cdj_usbf_vbus(CdjUsbfState *s, bool on)
{
    s->vbus = on;
    s->reg[CDJ_USB_INTSTS0 / 2] |= CDJ_USBF_VBINT;
    cdj_usbf_update_irq(s);
}

void cdj_usbf_bus_reset(CdjUsbfState *s)
{
    unsigned pipe;

    for (pipe = 0; pipe < CDJ_USB_NR_PIPES; pipe++) {
        cdj_usbf_pipe_clear(&s->pipe[pipe]);
    }
    s->reg[CDJ_USB_USBADDR / 2] = 0;
    s->addr_pending = false;
    s->next_dvsq = 0;
    s->ctsq = CTSQ_IDLE;
    cdj_usbf_device_state(s, DVSQ_DEFAULT);
    cdj_usbf_update_irq(s);
}

/* A SETUP packet. The module answers a well-formed SET_ADDRESS itself (manual
 * 31.5.5 (d)); everything else goes to the driver as a CTRT interrupt with
 * the request in USBREQ/USBVAL/USBINDX/USBLENG. */
int cdj_usbf_setup(CdjUsbfState *s, const uint8_t setup[8])
{
    uint16_t value = lduw_le_p(setup + 2);
    uint16_t index = lduw_le_p(setup + 4);
    uint16_t length = lduw_le_p(setup + 6);

    if (!cdj_usbf_attached(s)) {
        return CDJ_USBF_NAK;
    }
    if (cdj_usbf_debug()) {
        info_report("usbf: SETUP bmRequestType 0x%02x bRequest 0x%02x "
                    "wValue 0x%04x wIndex 0x%04x wLength %u",
                    setup[0], setup[1], value, index, length);
    }

    if (setup[0] == 0x00 && setup[1] == USB_REQ_SET_ADDRESS && !index
        && !length && value <= 0x7f && s->dvsq != DVSQ_CONFIGURED) {
        s->addr_pending = true;
        s->new_addr = value;
        return 0;
    }

    s->reg[CDJ_USB_USBREQ / 2] = setup[0] | setup[1] << 8;
    s->reg[CDJ_USB_USBVAL / 2] = value;
    s->reg[CDJ_USB_USBINDX / 2] = index;
    s->reg[CDJ_USB_USBLENG / 2] = length;
    s->reg[CDJ_USB_INTSTS0 / 2] |= CDJ_USBF_VALID;
    s->reg[CDJ_USB_DCPCTR / 2] &= ~(CDJ_USB_PID_MASK | CDJ_USB_CCPL);
    cdj_usbf_pipe_clear(&s->pipe[0]);

    cdj_usbf_stage(s, !length ? CTSQ_NODATA_STATUS
                      : setup[0] & USB_DIR_IN ? CTSQ_READ_DATA
                      : CTSQ_WRITE_DATA);
    s->next_dvsq = 0;
    if (setup[0] == 0x00 && setup[1] == USB_REQ_SET_CONFIGURATION) {
        s->next_dvsq = value & 0xff ? DVSQ_CONFIGURED : DVSQ_ADDRESS;
    }
    cdj_usbf_update_irq(s);
    return 0;
}

/* An IN token: the packet's length, or CDJ_USBF_NAK / CDJ_USBF_STALL. */
int cdj_usbf_in(CdjUsbfState *s, unsigned ep, uint8_t *buf, unsigned max)
{
    int n;

    if (!ep) {
        n = cdj_usbf_dcp_in(s, buf, max);
    } else {
        unsigned pipe = cdj_usbf_find_pipe(s, ep, true);

        n = pipe ? cdj_usbf_pid(s, pipe) : CDJ_USBF_NAK;
        if (!n) {
            n = s->pipe[pipe].sie_valid ? cdj_usbf_send(s, pipe, buf, max)
                                        : CDJ_USBF_NAK;
        }
    }
    if (n >= 0 && cdj_usbf_debug()) {
        info_report("usbf: IN ep%u -> %d bytes", ep, n);
    }
    cdj_usbf_update_irq(s);
    return n;
}

/* An OUT token with its data packet: len if taken, or NAK / STALL. */
int cdj_usbf_out(CdjUsbfState *s, unsigned ep, const uint8_t *buf, unsigned len)
{
    int n;

    if (!ep) {
        n = cdj_usbf_dcp_out(s, buf, len);
    } else {
        unsigned pipe = cdj_usbf_find_pipe(s, ep, false);

        n = pipe ? cdj_usbf_pid(s, pipe) : CDJ_USBF_NAK;
        if (!n) {
            n = cdj_usbf_receive(s, pipe, buf, len);
        }
    }
    if (n >= 0 && cdj_usbf_debug()) {
        info_report("usbf: OUT ep%u %u bytes", ep, len);
    }
    cdj_usbf_update_irq(s);
    return n;
}

static uint64_t cdj_usbf_read(void *opaque, hwaddr off, unsigned size)
{
    CdjUsbfState *s = opaque;
    unsigned sel = s->reg[CDJ_USB_PIPESEL / 2] & 0x000f;
    uint64_t v = s->reg[off / 2];
    int port;

    port = cdj_usb_data_port(off);
    if (port >= 0) {
        CdjUsbfPipe *p = &s->pipe[s->port_pipe[port]];
        unsigned i;

        v = 0;
        for (i = 0; i < size && p->pos < p->len; i++) {
            v |= (uint64_t)p->fifo[p->pos++] << (8 * i);
        }
        return v;
    }

    port = cdj_usb_ctr_port(off);
    if (port >= 0) {
        unsigned pipe = s->port_pipe[port];
        CdjUsbfPipe *p = &s->pipe[pipe];

        v = p->closed ? 0 : CDJ_USB_FRDY;
        if (!cdj_usbf_transmits(s, pipe)) {
            v |= (p->len - p->pos) & CDJ_USB_DTLN;
        }
    } else if (off == CDJ_USB_DCPCTR ||
               (off >= CDJ_USB_PIPECTR(1) && off <= CDJ_USB_PIPECTR(9)
                && !(off & 1))) {
        unsigned pipe = off == CDJ_USB_DCPCTR
                        ? 0 : (off - CDJ_USB_PIPECTR(1)) / 2 + 1;
        CdjUsbfPipe *p = &s->pipe[pipe];

        if (cdj_usbf_transmits(s, pipe) ? !p->closed : p->pos < p->len) {
            v |= CDJ_USB_BSTS;
        }
        if (pipe && p->sie_valid) {
            v |= CDJ_USBF_INBUFM;
        }
    } else {
        switch (off) {
        case CDJ_USB_PIPECFG:
            v = s->pipecfg[sel];
            break;
        case CDJ_USB_PIPEBUF:
            v = s->pipebuf[sel];
            break;
        case CDJ_USB_PIPEMAXP:
            v = s->pipemaxp[sel];
            break;
        case CDJ_USB_PIPEPERI:
            v = s->pipeperi[sel];
            break;
        case CDJ_USB_INTSTS0:
            v = cdj_usbf_intsts0(s);
            break;
        case CDJ_USB_SYSSTS0:
            v = cdj_usbf_attached(s);           /* LNST: full-speed J */
            break;
        case CDJ_USB_DVSTCTR0:
            /* RHST once the bus has been reset: 3 high speed, 2 full speed. */
            v &= ~0x0007ULL;
            if (s->dvsq != DVSQ_POWERED) {
                v |= s->reg[CDJ_USB_SYSCFG / 2] & CDJ_USBF_HSE ? 3 : 2;
            }
            break;
        case CDJ_USB_FRMNUM:
            v = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) & 0x07ff;
            break;
        }
    }

    if (cdj_usbf_debug()) {
        info_report("usbf: read  +0x%03x = 0x%04x (size %u)",
                    (unsigned)off, (unsigned)v, size);
    }
    return v;
}

static void cdj_usbf_write(void *opaque, hwaddr off, uint64_t val,
                           unsigned size)
{
    CdjUsbfState *s = opaque;
    unsigned sel = s->reg[CDJ_USB_PIPESEL / 2] & 0x000f;
    int port;

    port = cdj_usb_data_port(off);
    if (port >= 0) {
        unsigned pipe = s->port_pipe[port];
        CdjUsbfPipe *p = &s->pipe[pipe];
        unsigned i;

        for (i = 0; i < size && !p->closed; i++) {
            p->fifo[p->len++] = (val >> (8 * i)) & 0xff;
            if (p->len == cdj_usbf_fill(s, pipe)) {
                cdj_usbf_close(p);
            }
        }
        return;
    }

    if (cdj_usbf_debug()) {
        info_report("usbf: write +0x%03x = 0x%04x (size %u)",
                    (unsigned)off, (unsigned)val, size);
    }

    port = cdj_usb_sel_port(off);
    if (port >= 0) {
        s->reg[off / 2] = (uint16_t)val;
        s->port_pipe[port] = (val & 0x000f) < CDJ_USB_NR_PIPES
                             ? (val & 0x000f) : 0;
        if (port && (val & CDJ_USBF_DREQE)) {
            warn_report_once("usbf: DMA on a FIFO port is not modelled");
        }
        return;
    }

    port = cdj_usb_ctr_port(off);
    if (port >= 0) {
        CdjUsbfPipe *p = &s->pipe[s->port_pipe[port]];

        if (val & CDJ_USB_BCLR) {
            p->len = p->pos = 0;
            p->closed = false;
        }
        /* BVAL ends a short buffer; on an empty one it is a zero-length
         * packet. */
        if ((val & CDJ_USB_BVAL) && !p->closed) {
            cdj_usbf_close(p);
        }
        cdj_usbf_update_irq(s);
        return;
    }

    switch (off) {
    case CDJ_USB_INTSTS0:
        s->reg[off / 2] &= (uint16_t)val | ~CDJ_USBF_STS0_CLEARABLE;
        break;
    case CDJ_USB_BRDYSTS:
    case CDJ_USB_NRDYSTS:
    case CDJ_USB_BEMPSTS:
        s->reg[off / 2] &= (uint16_t)val;
        break;

    case CDJ_USB_USBADDR:
    case CDJ_USB_USBREQ:
    case CDJ_USB_USBVAL:
    case CDJ_USB_USBINDX:
    case CDJ_USB_USBLENG:
        break;                       /* set by the host's SETUP, read-only here */

    case CDJ_USB_PIPECFG:
        s->pipecfg[sel] = (uint16_t)val;
        break;
    case CDJ_USB_PIPEBUF:
        s->pipebuf[sel] = (uint16_t)val;
        break;
    case CDJ_USB_PIPEMAXP:
        s->pipemaxp[sel] = (uint16_t)val;
        break;
    case CDJ_USB_PIPEPERI:
        s->pipeperi[sel] = (uint16_t)val;
        break;

    case CDJ_USB_SYSCFG:
        s->reg[off / 2] = (uint16_t)val;
        if (!cdj_usbf_attached(s)) {
            s->dvsq = DVSQ_POWERED;
        }
        break;

    case CDJ_USB_DCPCTR:
        /* PID cannot leave NAK while a new request is unacknowledged. */
        if (s->reg[CDJ_USB_INTSTS0 / 2] & CDJ_USBF_VALID) {
            val = (val & ~CDJ_USB_PID_MASK)
                  | (s->reg[off / 2] & CDJ_USB_PID_MASK);
        }
        s->reg[off / 2] = (uint16_t)val & ~CDJ_USBF_SQBITS;
        break;

    default:
        if (off >= CDJ_USB_PIPECTR(1) && off <= CDJ_USB_PIPECTR(9)
            && !(off & 1)) {
            if (val & CDJ_USBF_ACLRM) {
                cdj_usbf_pipe_clear(
                    &s->pipe[(off - CDJ_USB_PIPECTR(1)) / 2 + 1]);
            }
            val &= ~CDJ_USBF_SQBITS;
        }
        s->reg[off / 2] = (uint16_t)val;
        break;
    }

    cdj_usbf_update_irq(s);
}

static const MemoryRegionOps cdj_usbf_ops = {
    .read = cdj_usbf_read,
    .write = cdj_usbf_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_usbf_realize(DeviceState *dev, Error **errp)
{
    CdjUsbfState *s = CDJ_USBF(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &cdj_usbf_ops, s,
                          "sh7724.usb1", CDJ_USBF_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    s->reg[CDJ_USB_DCPMAXP / 2] = 0x0040;
}

static void cdj_usbf_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = cdj_usbf_realize;
    dc->desc = "SH7724 USB 2.0 function module (R8A66597)";
}

static const TypeInfo cdj_usbf_info = {
    .name          = TYPE_CDJ_USBF,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CdjUsbfState),
    .class_init    = cdj_usbf_class_init,
};

static void cdj_usbf_register_types(void)
{
    type_register_static(&cdj_usbf_info);
}
type_init(cdj_usbf_register_types)

/* Controller state at exit: how far the enumeration got and which pipes the
 * driver set up. */
static void cdj_usbf_dump(Notifier *n, void *unused)
{
    CdjUsbfState *s = container_of(n, CdjUsbfState, exit);
    unsigned pipe;

    info_report("usbf: SYSCFG=0x%04x INTSTS0=0x%04x INTENB0=0x%04x address %u",
                s->reg[CDJ_USB_SYSCFG / 2], cdj_usbf_intsts0(s),
                s->reg[CDJ_USB_INTENB0 / 2], s->reg[CDJ_USB_USBADDR / 2]);
    for (pipe = 1; pipe < CDJ_USB_NR_PIPES; pipe++) {
        if (s->pipecfg[pipe]) {
            info_report("usbf: pipe %u cfg=0x%04x maxp=0x%04x ctr=0x%04x",
                        pipe, s->pipecfg[pipe], s->pipemaxp[pipe],
                        s->reg[CDJ_USB_PIPECTR(pipe) / 2]);
        }
    }
}

/* irq is the board INTC's USB1 (USI1) line. */
CdjUsbfState *cdj_usbf_init(MemoryRegion *sysmem, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_CDJ_USBF);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    CdjUsbfState *s = CDJ_USBF(dev);

    sysbus_realize_and_unref(sbd, &error_fatal);
    s->exit.notify = cdj_usbf_dump;
    qemu_add_exit_notifier(&s->exit);
    sysbus_connect_irq(sbd, 0, irq);
    memory_region_add_subregion_overlap(sysmem, CDJ_USBF_BASE,
                                        sysbus_mmio_get_region(sbd, 0), 1);
    return s;
}
