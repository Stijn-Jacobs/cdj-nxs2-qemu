/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/*
 * SH7724 USB 2.0 module at 0xA4D80000 (physical 0x04D80000), a Renesas
 * R8A66597-family controller used in host mode. QEMU has no model for it, so
 * this is a small single-port HCD that connects the firmware to a QEMU USB
 * device (normally usb-storage for the media stick).
 *
 *   CDJ_USB_DEBUG=1      log every register access and transfer
 *   CDJ_USB_ATTACH=1     report the port occupied even with no device
 *   CDJ_USB_ATTACH_MS=n  delay before the attach interrupt (default 500)
 *   CDJ_USB_STUB=1       map an unimplemented device instead (board.c)
 */
/* Registers, manual section 31.4. Only the ones the firmware touches are named. */
#define CDJ_USB_SYSSTS0  0x04
#define CDJ_USB_DVSTCTR0 0x08
/* Three windows onto the pipe buffers, each with its own CURPIPE: CFIFO is the
 * CPU's, D0FIFO and D1FIFO are wired to the DMAC. The driver reads descriptors
 * and the CSW through CFIFO and sectors through D0FIFO. */
#define CDJ_USB_CFIFO    0x14
#define CDJ_USB_CFIFOSEL 0x20
#define CDJ_USB_CFIFOCTR 0x22
#define CDJ_USB_D0FIFOSEL 0x28
#define CDJ_USB_D0FIFOCTR 0x2A
#define CDJ_USB_D1FIFOSEL 0x2C
#define CDJ_USB_D1FIFOCTR 0x2E
#define CDJ_USB_D0FIFO   0x100
#define CDJ_USB_D1FIFO   0x120
#define CDJ_USB_D0FIFO_ALT 0x18
#define CDJ_USB_D1FIFO_ALT 0x1C
#define CDJ_USB_NR_FIFO_PORTS 3
#define CDJ_USB_INTENB0  0x30
#define CDJ_USB_INTENB1  0x32
#define CDJ_USB_INTSTS0  0x40
#define CDJ_USB_INTSTS1  0x42
#define CDJ_USB_BRDYENB  0x36
#define CDJ_USB_NRDYENB  0x38
#define CDJ_USB_BEMPENB  0x3A
#define CDJ_USB_BRDYSTS  0x46
#define CDJ_USB_NRDYSTS  0x48
#define CDJ_USB_BEMPSTS  0x4A
#define CDJ_USB_USBREQ   0x54
#define CDJ_USB_USBVAL   0x56
#define CDJ_USB_USBINDX  0x58
#define CDJ_USB_USBLENG  0x5A
#define CDJ_USB_DCPCFG   0x5C
#define CDJ_USB_DCPMAXP  0x5E
#define CDJ_USB_DCPCTR   0x60
#define CDJ_USB_PIPESEL  0x64
#define CDJ_USB_PIPECFG  0x68
#define CDJ_USB_PIPEBUF  0x6A
#define CDJ_USB_PIPEMAXP 0x6C
#define CDJ_USB_PIPEPERI 0x6E
/* PIPE1CTR..PIPE9CTR are consecutive from +0x70. */
#define CDJ_USB_PIPECTR(n) (0x70 + 2 * ((n) - 1))
/* Transaction counters, pipes 1..5 only: TRE bit 9 TRENB, bit 8 TRCLR. */
#define CDJ_USB_PIPETRE(n) (0x90 + 4 * ((n) - 1))
#define CDJ_USB_PIPETRN(n) (0x92 + 4 * ((n) - 1))
#define CDJ_USB_NR_PIPES 10          /* index 0 is the DCP and stays unused */

/* INTENB1/INTSTS1 bit 14: bus change -- the device-attached notification. */
#define CDJ_USB_BCHG     0x4000
/* INTSTS1 bit 4: the setup transaction was ACKed by the device. */
#define CDJ_USB_SACK     0x0010
/* INTSTS0 bit 8 BRDY / bit 9 NRDY / bit 10 BEMP: the per-pipe FIFO events.
 * BRDYSTS/NRDYSTS/BEMPSTS carry which pipe, one bit each, pipe 0 = the DCP. */
#define CDJ_USB_BRDY     0x0100
#define CDJ_USB_NRDY     0x0200
#define CDJ_USB_BEMP     0x0400

/* DCPCTR: bit 15 BSTS, 14 SUREQ, 11 SUREQCLR, 7 SQSET, 2 CCPL, 1:0 PID. */
#define CDJ_USB_BSTS     0x8000
#define CDJ_USB_SUREQ    0x4000
#define CDJ_USB_SUREQCLR 0x0800
#define CDJ_USB_CCPL     0x0004
#define CDJ_USB_PID_MASK 0x0003
#define CDJ_USB_PID_BUF  0x0001

/* CFIFOCTR: bit 15 BVAL, 14 BCLR, 13 FRDY, 11:0 DTLN (receive data length). */
#define CDJ_USB_BVAL     0x8000
#define CDJ_USB_BCLR     0x4000
#define CDJ_USB_FRDY     0x2000
#define CDJ_USB_DTLN     0x0FFF

/* DCPCFG/PIPECFG bit 4: transfer direction, 1 = transmitting. */
#define CDJ_USB_DIR      0x0010
/* PIPECFG bit 7 SHTNAK: a receiving pipe's PID goes to NAK at end of
 * transfer (manual 31.4.24). */
#define CDJ_USB_SHTNAK   0x0080

/* DVSTCTR0 bit 6: drive the USB reset condition onto the port. */
#define CDJ_USB_USBRST   0x0040

#define TYPE_CDJ_USB "cdj.usb"
OBJECT_DECLARE_SIMPLE_TYPE(CdjUsbState, CDJ_USB)

struct CdjUsbState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    USBBus bus;
    USBPort port;
    USBPacket packet;
    QEMUTimer *attach_timer;
    bool attached;                 /* the guest has been told about the device */
    bool in_port_reset;            /* driving USBRST, not losing the device */

    /* Each pipe owns a slice of buffer RAM (partitioned by PIPEBUF); a FIFO
     * window only chooses which slice it looks at. The driver runs a transfer
     * on pipe 1 while CFIFOSEL still names pipe 0, then switches to drain it. */
    struct {
        uint8_t data[1024];
        unsigned len;              /* bytes in the buffer */
        unsigned pos;              /* bytes the guest has read back */
        /* A receiving transfer can outgrow the buffer (multi-sector READ(10)),
         * so it is refilled as the driver drains it: cdj_usb_pipe_fill(). */
        unsigned trn_left;         /* IN transactions still armed, 0 = untracked */
        bool more;                 /* the transfer has not reached its end */
    } buf[CDJ_USB_NR_PIPES];
    /* Which pipe each FIFO window is currently wired to. */
    unsigned port_pipe[CDJ_USB_NR_FIFO_PORTS];

    /* PIPESEL is a window: PIPECFG/PIPEBUF/PIPEMAXP/PIPEPERI refer to the pipe
     * it names, so they are stored per pipe. */
    Notifier exit;
    uint16_t pipecfg[CDJ_USB_NR_PIPES];
    uint16_t pipebuf[CDJ_USB_NR_PIPES];
    uint16_t pipemaxp[CDJ_USB_NR_PIPES];
    uint16_t pipeperi[CDJ_USB_NR_PIPES];

    uint16_t reg[CDJ_USB_SIZE / 2];
};

/*
 * INTENB0/INTSTS0 and INTENB1/INTSTS1 use matching bit positions. INTSTS0's
 * low byte holds state fields, but the matching INTENB0 bits are reserved-zero.
 *
 * INTSTS0's BRDY, NRDY and BEMP are derived: each is the OR of its per-pipe
 * status ANDed with its per-pipe enable (manual 31.4.11-31.4.13). The driver
 * relies on this: it masks pipe 1's BRDYENB during a DMA sector read, and the
 * event is delivered only when it re-enables the pipe.
 */
static uint16_t cdj_usb_intsts0(CdjUsbState *s)
{
    uint16_t v = s->reg[CDJ_USB_INTSTS0 / 2] & ~(uint16_t)0x0700;

    if (s->reg[CDJ_USB_BRDYSTS / 2] & s->reg[CDJ_USB_BRDYENB / 2]) {
        v |= CDJ_USB_BRDY;
    }
    if (s->reg[CDJ_USB_NRDYSTS / 2] & s->reg[CDJ_USB_NRDYENB / 2]) {
        v |= CDJ_USB_NRDY;
    }
    if (s->reg[CDJ_USB_BEMPSTS / 2] & s->reg[CDJ_USB_BEMPENB / 2]) {
        v |= CDJ_USB_BEMP;
    }
    return v;
}

static void cdj_usb_update_irq(CdjUsbState *s)
{
    unsigned pending =
        (cdj_usb_intsts0(s) & s->reg[CDJ_USB_INTENB0 / 2]) |
        (s->reg[CDJ_USB_INTSTS1 / 2] & s->reg[CDJ_USB_INTENB1 / 2]);

    qemu_set_irq(s->irq, pending != 0);
}

static bool cdj_usb_debug(void)
{
    return getenv("CDJ_USB_DEBUG") != NULL;
}

/* A device on the port, or CDJ_USB_ATTACH forcing the port to read as
 * occupied with nothing behind it. */
static bool cdj_usb_present(CdjUsbState *s)
{
    return s->port.dev != NULL || getenv("CDJ_USB_ATTACH") != NULL;
}

static void cdj_usb_attach_fire(void *opaque)
{
    CdjUsbState *s = opaque;

    /* usb_claim_port() inserts a hub when the last free port is taken, and
     * this controller has one root port, so a plain "-device usb-storage"
     * puts a hub on the wire and the firmware never finds storage. port=1
     * avoids it. */
    if (s->port.dev &&
        object_dynamic_cast(OBJECT(s->port.dev), "usb-hub")) {
        warn_report("usb: a hub was auto-inserted on the root port -- add "
                    "port=1 to the -device option to attach the device "
                    "directly");
    }

    s->attached = true;
    s->reg[CDJ_USB_INTSTS1 / 2] |= CDJ_USB_BCHG;
    if (cdj_usb_debug()) {
        info_report("usb: attach -- port occupied, INTSTS1 BCHG raised");
    }
    cdj_usb_update_irq(s);
}

/* Raise BRDY for the DCP: data has arrived in the buffer and can be read. */
static void cdj_usb_raise_brdy(CdjUsbState *s)
{
    s->reg[CDJ_USB_BRDYSTS / 2] |= 1;      /* bit 0 == pipe 0 == the DCP */
}

/* Raise BEMP for the DCP: the buffer has been emptied onto the bus. */
static void cdj_usb_raise_bemp(CdjUsbState *s)
{
    s->reg[CDJ_USB_BEMPSTS / 2] |= 1;
}

/* Run one transaction and return the byte count, or < 0 on a device error.
 * buf may be NULL for a zero-length status-stage packet. */
static int cdj_usb_xfer(CdjUsbState *s, int pid, unsigned epnum, uint8_t addr,
                        uint8_t *buf, unsigned len)
{
    USBEndpoint *ep;
    USBDevice *dev;

    if (!s->port.dev) {
        if (cdj_usb_debug()) {
            info_report("usb: pid %d ep%u -- no device in the port", pid, epnum);
        }
        return -1;
    }
    dev = usb_find_device(&s->port, addr);
    if (!dev) {
        if (cdj_usb_debug()) {
            info_report("usb: pid %d ep%u -- no device at address %u "
                        "(port dev addr %u, state %u)", pid, epnum, addr,
                        s->port.dev->addr, s->port.dev->state);
        }
        return -1;
    }
    ep = usb_ep_get(dev, pid, epnum);
    usb_packet_setup(&s->packet, pid, ep, 0, 0, false, true);
    if (len) {
        usb_packet_addbuf(&s->packet, buf, len);
    }
    usb_handle_packet(dev, &s->packet);

    /* usb-storage completes through the block layer, so a CSW can come back
     * USB_RET_ASYNC. The driver polls for completion on its very next access
     * and treats any deferral as a timeout, so drain the block layer here. */
    if (s->packet.status == USB_RET_ASYNC) {
        blk_drain_all();
    }
    if (s->packet.status == USB_RET_ASYNC) {
        warn_report("usb: pid %d ep%u stayed async past a block drain", pid,
                    epnum);
        usb_cancel_packet(&s->packet);
        return -1;
    }
    if (s->packet.status != USB_RET_SUCCESS) {
        if (cdj_usb_debug()) {
            info_report("usb: pid %d ep%u %u bytes -> status %d", pid, epnum,
                        len, s->packet.status);
        }
        return s->packet.status == USB_RET_NAK ? 0 : -1;
    }
    return (int)s->packet.actual_length;
}

/* DCPMAXP[15:12] / PIPEMAXP[15:12] DEVSEL is the target device address (manual
 * 31.4.28); the firmware sets DEVSEL = 1 after SET_ADDRESS(1). */
static int cdj_usb_ep0(CdjUsbState *s, int pid, uint8_t *buf, unsigned len)
{
    uint8_t addr = (s->reg[CDJ_USB_DCPMAXP / 2] >> 12) & 0x0f;

    return cdj_usb_xfer(s, pid, 0, addr, buf, len);
}

/* SUREQ: send the setup packet built in USBREQ/USBVAL/USBINDX/USBLENG.
 * The driver polls SUREQ on its very next access and cancels with SUREQCLR if
 * it is still set, so the transaction completes synchronously (manual 31.4.29).
 */
static void cdj_usb_setup(CdjUsbState *s)
{
    uint8_t setup[8];
    uint16_t req = s->reg[CDJ_USB_USBREQ / 2];

    /* USBREQ packs bRequest in [15:8] and bmRequestType in [7:0]. */
    setup[0] = req & 0xff;
    setup[1] = req >> 8;
    stw_le_p(&setup[2], s->reg[CDJ_USB_USBVAL / 2]);
    stw_le_p(&setup[4], s->reg[CDJ_USB_USBINDX / 2]);
    stw_le_p(&setup[6], s->reg[CDJ_USB_USBLENG / 2]);

    if (cdj_usb_debug()) {
        info_report("usb: SETUP bmRequestType 0x%02x bRequest 0x%02x "
                    "wValue 0x%04x wIndex 0x%04x wLength %u",
                    setup[0], setup[1], s->reg[CDJ_USB_USBVAL / 2],
                    s->reg[CDJ_USB_USBINDX / 2], s->reg[CDJ_USB_USBLENG / 2]);
    }

    s->reg[CDJ_USB_DCPCTR / 2] &= ~CDJ_USB_SUREQ;
    s->buf[0].len = s->buf[0].pos = 0;

    if (cdj_usb_ep0(s, USB_TOKEN_SETUP, setup, sizeof(setup)) < 0) {
        /* SIGN: the setup transaction was not acknowledged (no device). */
        s->reg[CDJ_USB_INTSTS1 / 2] |= 0x0020;     /* SIGN */
    } else {
        s->reg[CDJ_USB_INTSTS1 / 2] |= CDJ_USB_SACK;
    }
}

/* PID = BUF on the DCP: run the data or status stage of the control transfer.
 * DCPCFG DIR gives the data stage direction; CCPL marks the status stage, a
 * zero-length packet in the opposite direction. */
static void cdj_usb_dcp_run(CdjUsbState *s)
{
    bool out = (s->reg[CDJ_USB_DCPCFG / 2] & CDJ_USB_DIR) != 0;
    unsigned maxp = s->reg[CDJ_USB_DCPMAXP / 2] & 0x7f;
    unsigned want;
    int n;

    if (!maxp || maxp > sizeof(s->buf[0].data)) {
        maxp = 64;
    }

    if (s->reg[CDJ_USB_DCPCTR / 2] & CDJ_USB_CCPL) {
        /* Status stage: opposite direction, zero length. */
        n = cdj_usb_ep0(s, out ? USB_TOKEN_IN : USB_TOKEN_OUT, NULL, 0);
        if (cdj_usb_debug()) {
            info_report("usb: control status stage -> %d", n);
        }
        s->reg[CDJ_USB_DCPCTR / 2] &= ~(CDJ_USB_PID_MASK | CDJ_USB_CCPL);
        cdj_usb_raise_bemp(s);
        return;
    }

    if (out) {
        n = cdj_usb_ep0(s, USB_TOKEN_OUT, s->buf[0].data, s->buf[0].len);
        if (cdj_usb_debug()) {
            info_report("usb: control OUT %u bytes -> %d", s->buf[0].len, n);
        }
        s->buf[0].len = s->buf[0].pos = 0;
        cdj_usb_raise_bemp(s);
        return;
    }

    /* Keep issuing IN tokens until the requested length is buffered or the
     * device sends a short packet; the driver reads the whole descriptor at
     * once. */
    want = s->reg[CDJ_USB_USBLENG / 2];
    if (want > sizeof(s->buf[0].data)) {
        want = sizeof(s->buf[0].data);
    }
    s->buf[0].len = 0;
    s->buf[0].pos = 0;
    /* Always send at least one token: a control transfer without a data stage
     * (SET_ADDRESS, SET_CONFIGURATION) ends with a zero-length IN, and that is
     * what makes the device act on it. */
    do {
        unsigned chunk = MIN(maxp, want - s->buf[0].len);

        n = cdj_usb_ep0(s, USB_TOKEN_IN, s->buf[0].data + s->buf[0].len, chunk);
        if (n < 0) {
            s->reg[CDJ_USB_NRDYSTS / 2] |= 1;
            return;
        }
        s->buf[0].len += (unsigned)n;
        if ((unsigned)n < chunk) {
            break;                    /* short packet ends the data stage */
        }
    } while (s->buf[0].len < want);
    if (cdj_usb_debug()) {
        info_report("usb: control IN %u of %u bytes", s->buf[0].len, want);
    }
    cdj_usb_raise_brdy(s);
}

/* Pull IN packets into a receiving pipe's buffer until it is full or the
 * transfer ends; return whether it ended. PIPECFG CNTMD is set on the bulk
 * pipes, so the controller keeps issuing IN tokens. The transaction counter
 * (PIPEnTRE TRENB + PIPEnTRN) stops it at the end of a reply, e.g. 1 before
 * each 13-byte CSW, so the data phase and CSW are not merged. */
static bool cdj_usb_pipe_fill(CdjUsbState *s, unsigned pipe, unsigned epnum,
                              uint8_t addr, unsigned maxp)
{
    bool ended = false;

    s->buf[pipe].len = 0;
    s->buf[pipe].pos = 0;
    while (s->buf[pipe].len + maxp <= sizeof(s->buf[pipe].data)) {
        int n = cdj_usb_xfer(s, USB_TOKEN_IN, epnum, addr,
                             s->buf[pipe].data + s->buf[pipe].len, maxp);

        if (n < 0) {
            s->reg[CDJ_USB_NRDYSTS / 2] |= 1u << pipe;
            s->buf[pipe].more = false;
            return false;
        }
        s->buf[pipe].len += (unsigned)n;
        if ((unsigned)n < maxp) {
            ended = true;            /* short packet ends the transfer */
            break;
        }
        if (s->buf[pipe].trn_left && --s->buf[pipe].trn_left == 0) {
            ended = true;            /* the armed transaction count is used up */
            break;
        }
    }
    if (ended) {
        s->buf[pipe].more = false;
    }
    if (cdj_usb_debug()) {
        info_report("usb: pipe %u IN ep%u addr %u, %u bytes%s", pipe, epnum,
                    addr, s->buf[pipe].len, s->buf[pipe].more ? " (partial)" : "");
    }
    s->reg[CDJ_USB_BRDYSTS / 2] |= 1u << pipe;
    return ended;
}

/* Is one of the DMAC-wired FIFO windows (D0FIFOSEL/D1FIFOSEL bit 12 DREQE)
 * currently pointed at this pipe? */
static bool cdj_usb_pipe_has_dreq(CdjUsbState *s, unsigned pipe)
{
    return ((s->reg[CDJ_USB_D0FIFOSEL / 2] & 0x1000) &&
            s->port_pipe[1] == pipe) ||
           ((s->reg[CDJ_USB_D1FIFOSEL / 2] & 0x1000) &&
            s->port_pipe[2] == pipe);
}

/* End of a receiving transfer (short packet or transaction count reached):
 * PIPEnTRN reads back 0 (31.4.37), and with PIPECFG SHTNAK the PID moves to
 * NAK. */
static void cdj_usb_pipe_in_end(CdjUsbState *s, unsigned pipe, bool ended)
{
    if (ended && pipe <= 5) {
        s->reg[CDJ_USB_PIPETRN(pipe) / 2] = 0;
        if (s->pipecfg[pipe] & CDJ_USB_SHTNAK) {
            s->reg[CDJ_USB_PIPECTR(pipe) / 2] &= ~(uint16_t)CDJ_USB_PID_MASK;
        }
    }
}

/* PID = BUF on PIPEnCTR: run the bulk transfer the pipe is configured for.
 * PIPECFG gives the endpoint and direction, PIPEMAXP the device address
 * (DEVSEL) and max packet size. */
static void cdj_usb_pipe_run(CdjUsbState *s, unsigned pipe)
{
    uint16_t cfg = s->pipecfg[pipe];
    uint16_t maxpreg = s->pipemaxp[pipe];
    unsigned epnum = cfg & 0x000f;
    bool out = (cfg & CDJ_USB_DIR) != 0;
    uint8_t addr = (maxpreg >> 12) & 0x0f;
    unsigned maxp = maxpreg & 0x07ff;
    unsigned trn = 0;
    bool ended = false;              /* the transfer reached a defined end */
    int n;

    if (!epnum) {
        return;                      /* pipe never configured */
    }
    if (!maxp || maxp > sizeof(s->buf[0].data)) {
        maxp = 64;
    }

    if (out) {
        /* A transmitting pipe whose FIFO window is DMAC-wired (DREQE) raises a
         * DMA request for its data once armed. */
        if (!s->buf[pipe].len && cdj_usb_pipe_has_dreq(s, pipe)) {
            cdj_dmac_dreq();
        }
        if (!s->buf[pipe].len) {
            /* An empty buffer here is the zero-length packet that follows a
             * completed data phase. usb-storage would STALL it; the module
             * just reports the buffer empty (BEMP). */
            if (cdj_usb_debug()) {
                info_report("usb: pipe %u OUT ep%u addr %u, zero-length -- "
                            "completed locally", pipe, epnum, addr);
            }
            s->reg[CDJ_USB_BEMPSTS / 2] |= 1u << pipe;
            return;
        }
        n = cdj_usb_xfer(s, USB_TOKEN_OUT, epnum, addr,
                         s->buf[pipe].data, s->buf[pipe].len);
        if (cdj_usb_debug()) {
            info_report("usb: pipe %u OUT ep%u addr %u, %u bytes -> %d",
                        pipe, epnum, addr, s->buf[pipe].len, n);
        }
        s->buf[pipe].len = s->buf[pipe].pos = 0;
        if (n < 0) {
            s->reg[CDJ_USB_NRDYSTS / 2] |= 1u << pipe;
            return;
        }
        s->reg[CDJ_USB_BEMPSTS / 2] |= 1u << pipe;
        return;
    }

    if (pipe <= 5 && (s->reg[CDJ_USB_PIPETRE(pipe) / 2] & 0x0200)) {
        trn = s->reg[CDJ_USB_PIPETRN(pipe) / 2];
    }
    s->buf[pipe].trn_left = trn;
    s->buf[pipe].more = true;

    ended = cdj_usb_pipe_fill(s, pipe, epnum, addr, maxp);
    if (!ended && !s->buf[pipe].more) {
        return;                      /* the transaction was refused */
    }
    cdj_usb_pipe_in_end(s, pipe, ended);

    /* DREQE: the window is wired to the DMAC, which carries sector data.
     * Only the first instalment kicks the DMAC; a continuation runs from
     * inside the DMAC's own FIFO read and must not re-enter it. */
    if (cdj_usb_pipe_has_dreq(s, pipe)) {
        if (cdj_usb_debug()) {
            info_report("usb: pipe %u DREQ -- handing %u bytes to the DMAC",
                        pipe, s->buf[pipe].len);
        }
        cdj_dmac_dreq();
    }
}

/* Send a full transmitting buffer and free it for the rest of the transfer.
 * A write data phase is often larger than the buffer (the DMAC is handed 8160
 * bytes at once), and the hardware transmits as the buffer fills. */
static void cdj_usb_pipe_flush_out(CdjUsbState *s, unsigned pipe)
{
    uint16_t cfg = s->pipecfg[pipe];
    unsigned epnum = cfg & 0x000f;
    int n;

    if (!epnum || !(cfg & CDJ_USB_DIR) || !s->buf[pipe].len) {
        return;
    }
    n = cdj_usb_xfer(s, USB_TOKEN_OUT, epnum, (s->pipemaxp[pipe] >> 12) & 0x0f,
                     s->buf[pipe].data, s->buf[pipe].len);
    if (cdj_usb_debug()) {
        info_report("usb: pipe %u OUT ep%u %u bytes -> %d (buffer full)",
                    pipe, epnum, s->buf[pipe].len, n);
    }
    s->buf[pipe].len = s->buf[pipe].pos = 0;
    if (n < 0) {
        s->reg[CDJ_USB_NRDYSTS / 2] |= 1u << pipe;
        return;
    }
    s->reg[CDJ_USB_BEMPSTS / 2] |= 1u << pipe;
}

/* Next instalment of a receiving transfer that outgrew the pipe's buffer. */
static void cdj_usb_pipe_continue(CdjUsbState *s, unsigned pipe)
{
    uint16_t cfg = s->pipecfg[pipe];
    uint16_t maxpreg = s->pipemaxp[pipe];
    unsigned epnum = cfg & 0x000f;
    unsigned maxp = maxpreg & 0x07ff;
    bool ended;

    if (!epnum || (cfg & CDJ_USB_DIR)) {
        s->buf[pipe].more = false;
        return;
    }
    if (!maxp || maxp > sizeof(s->buf[0].data)) {
        maxp = 64;
    }

    ended = cdj_usb_pipe_fill(s, pipe, epnum, (maxpreg >> 12) & 0x0f, maxp);
    if (ended || s->buf[pipe].more) {
        cdj_usb_pipe_in_end(s, pipe, ended);
    }
}

/* Which FIFO window a data-port offset falls in, or -1. The ports are 4 bytes
 * wide; the byte lane only narrows the access width. */
static int cdj_usb_data_port(hwaddr off)
{
    if (off >= CDJ_USB_CFIFO && off < CDJ_USB_CFIFO + 4) {
        return 0;
    }
    if (off >= CDJ_USB_D0FIFO && off < CDJ_USB_D0FIFO + 4) {
        return 1;
    }
    if (off >= CDJ_USB_D1FIFO && off < CDJ_USB_D1FIFO + 4) {
        return 2;
    }
    /* The R8A66597's own D0FIFO/D1FIFO offsets, aliases of the 0x100/0x120
     * windows. The driver uses both in one transfer: the DMAC writes the bulk
     * through 0x100 and the CPU writes the tail through 0x18. */
    if (off >= CDJ_USB_D0FIFO_ALT && off < CDJ_USB_D0FIFO_ALT + 4) {
        return 1;
    }
    if (off >= CDJ_USB_D1FIFO_ALT && off < CDJ_USB_D1FIFO_ALT + 4) {
        return 2;
    }
    return -1;
}

/* Which FIFO window a xFIFOSEL / xFIFOCTR register belongs to, or -1. */
static int cdj_usb_sel_port(hwaddr off)
{
    switch (off) {
    case CDJ_USB_CFIFOSEL:  return 0;
    case CDJ_USB_D0FIFOSEL: return 1;
    case CDJ_USB_D1FIFOSEL: return 2;
    default:                return -1;
    }
}

static int cdj_usb_ctr_port(hwaddr off)
{
    switch (off) {
    case CDJ_USB_CFIFOCTR:  return 0;
    case CDJ_USB_D0FIFOCTR: return 1;
    case CDJ_USB_D1FIFOCTR: return 2;
    default:                return -1;
    }
}

static uint64_t cdj_usb_read(void *opaque, hwaddr off, unsigned size)
{
    CdjUsbState *s = opaque;
    unsigned sel;
    int port;
    uint64_t v;

    port = cdj_usb_data_port(off);
    if (port >= 0) {
        /* Each read consumes the next bytes of the selected pipe's buffer,
         * little end first. */
        unsigned pipe = s->port_pipe[port];
        unsigned i;

        /* Refill a multi-instalment transfer when the buffer runs empty, as
         * the hardware issues further IN tokens while the FIFO drains. */
        if (s->buf[pipe].pos >= s->buf[pipe].len && s->buf[pipe].more) {
            cdj_usb_pipe_continue(s, pipe);
        }

        v = 0;
        for (i = 0; i < size; i++) {
            uint8_t b = 0;

            if (s->buf[pipe].pos < s->buf[pipe].len) {
                b = s->buf[pipe].data[s->buf[pipe].pos++];
            }
            v |= (uint64_t)b << (8 * i);
        }
        if (cdj_usb_debug()) {
            info_report("usb: fifo%d pipe %u read %u bytes = 0x%08x "
                        "(%u/%u consumed)", port, pipe, size, (unsigned)v,
                        s->buf[pipe].pos, s->buf[pipe].len);
        }
        return v;
    }

    v = s->reg[off / 2];
    sel = s->reg[CDJ_USB_PIPESEL / 2] & 0x000f;

    port = cdj_usb_ctr_port(off);
    if (port >= 0) {
        /* FRDY, and DTLN = bytes left in the selected pipe's buffer. */
        unsigned pipe = s->port_pipe[port];

        v &= ~(uint64_t)(CDJ_USB_FRDY | CDJ_USB_DTLN);
        v |= CDJ_USB_FRDY;           /* empty is still writable */
        if (s->buf[pipe].pos < s->buf[pipe].len) {
            v |= (s->buf[pipe].len - s->buf[pipe].pos) & CDJ_USB_DTLN;
        }
        if (cdj_usb_debug()) {
            info_report("usb: read  +0x%03x = 0x%04x (size %u)",
                        (unsigned)off, (unsigned)v, size);
        }
        return v;
    }

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
        v = cdj_usb_intsts0(s);      /* BRDY/NRDY/BEMP are gated per pipe */
        break;

    case CDJ_USB_SYSSTS0:
        if (s->attached) {
            v |= 0x0001;             /* LNST = FS-J: something is plugged in */
        }
        break;
    case CDJ_USB_DVSTCTR0:
        if (s->attached) {
            v = (v & ~0x0007ULL) | 0x0002;      /* RHST = full speed */
        }
        break;
    case CDJ_USB_DCPCTR:
        if (s->buf[0].pos < s->buf[0].len) {
            v |= CDJ_USB_BSTS;       /* buffer access enabled */
        }
        break;

    default:
        if (off >= CDJ_USB_PIPECTR(1) && off <= CDJ_USB_PIPECTR(9) &&
            !(off & 1)) {
            unsigned pipe = (off - CDJ_USB_PIPECTR(1)) / 2 + 1;

            if (s->buf[pipe].pos < s->buf[pipe].len) {
                v |= CDJ_USB_BSTS;
            }
        }
        break;
    }

    if (cdj_usb_debug()) {
        info_report("usb: read  +0x%03x = 0x%04x (size %u)",
                    (unsigned)off, (unsigned)v, size);
    }
    return v;
}

static void cdj_usb_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjUsbState *s = opaque;
    unsigned sel;
    int port;

    if (cdj_usb_debug()) {
        info_report("usb: write +0x%03x = 0x%04x (size %u)",
                    (unsigned)off, (unsigned)val, size);
    }

    port = cdj_usb_data_port(off);
    if (port >= 0) {
        unsigned pipe = s->port_pipe[port];
        unsigned i;

        for (i = 0; i < size; i++) {
            if (s->buf[pipe].len == sizeof(s->buf[0].data)) {
                cdj_usb_pipe_flush_out(s, pipe);
                if (s->buf[pipe].len) {
                    break;           /* not a transmitting pipe -- drop the rest */
                }
            }
            s->buf[pipe].data[s->buf[pipe].len++] = (val >> (8 * i)) & 0xff;
        }
        return;
    }

    if (off == CDJ_USB_INTSTS0 || off == CDJ_USB_INTSTS1 ||
        off == CDJ_USB_BRDYSTS || off == CDJ_USB_NRDYSTS ||
        off == CDJ_USB_BEMPSTS) {
        /* Status bits are cleared by writing 0; the driver writes ~BIT. */
        s->reg[off / 2] &= (uint16_t)val;
    } else {
        s->reg[off / 2] = (uint16_t)val;
    }

    sel = s->reg[CDJ_USB_PIPESEL / 2] & 0x000f;

    port = cdj_usb_sel_port(off);
    if (port >= 0) {
        /* Switching a window must not clear the pipe buffer: the driver drains
         * data that arrived while the window named another pipe. */
        s->port_pipe[port] = (val & 0x000f) < CDJ_USB_NR_PIPES
                             ? (val & 0x000f) : 0;
        cdj_usb_update_irq(s);
        return;
    }

    port = cdj_usb_ctr_port(off);
    if (port >= 0) {
        if (val & CDJ_USB_BCLR) {
            unsigned pipe = s->port_pipe[port];

            s->buf[pipe].len = s->buf[pipe].pos = 0;
        }
        cdj_usb_update_irq(s);
        return;
    }

    switch (off) {
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

    case CDJ_USB_DVSTCTR0:
        /* USBRST moves the device from powered to default state, where
         * address 0 answers. */
        if ((val & CDJ_USB_USBRST) && s->port.dev) {
            /* usb_port_reset() detaches and re-attaches internally; the flag
             * stops the detach callback reporting it as a removal (DTCH). */
            s->in_port_reset = true;
            usb_port_reset(&s->port);
            s->in_port_reset = false;
            if (cdj_usb_debug()) {
                info_report("usb: bus reset driven onto the port");
            }
        }
        break;

    case CDJ_USB_DCPCTR:
        if (val & CDJ_USB_SUREQCLR) {
            s->reg[CDJ_USB_DCPCTR / 2] &= ~CDJ_USB_SUREQ;
        } else if (val & CDJ_USB_SUREQ) {
            cdj_usb_setup(s);
        } else if ((val & CDJ_USB_PID_MASK) == CDJ_USB_PID_BUF) {
            cdj_usb_dcp_run(s);
        }
        break;

    case CDJ_USB_INTENB1:
        /* A device already in the port produces no edge, so fire BCHG shortly
         * after the driver arms it, like plugging the stick in.
         * CDJ_USB_ATTACH_MS overrides the delay. */
        if ((val & CDJ_USB_BCHG) && s->attach_timer && !s->attached &&
            cdj_usb_present(s) && !timer_pending(s->attach_timer)) {
            const char *ms = getenv("CDJ_USB_ATTACH_MS");
            int64_t delay = ms ? strtoll(ms, NULL, 0) : 500;

            timer_mod(s->attach_timer,
                      qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + delay);
        }
        break;

    default:
        if (off >= CDJ_USB_PIPECTR(1) && off <= CDJ_USB_PIPECTR(9) &&
            !(off & 1) && (val & CDJ_USB_PID_MASK) == CDJ_USB_PID_BUF) {
            cdj_usb_pipe_run(s, (off - CDJ_USB_PIPECTR(1)) / 2 + 1);
        }
        break;
    }

    cdj_usb_update_irq(s);
}

static const MemoryRegionOps cdj_usb_ops = {
    .read = cdj_usb_read,
    .write = cdj_usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_usb_port_attach(USBPort *port)
{
    /* Reported by the attach timer instead: usb-storage attaches during
     * machine init, before the driver has armed BCHGE. */
}

static void cdj_usb_port_detach(USBPort *port)
{
    CdjUsbState *s = port->opaque;

    if (s->in_port_reset) {
        return;
    }
    s->attached = false;
    s->reg[CDJ_USB_INTSTS1 / 2] |= 0x1000;         /* DTCH */
    cdj_usb_update_irq(s);
}

static void cdj_usb_port_child_detach(USBPort *port, USBDevice *child)
{
}

static void cdj_usb_port_complete(USBPort *port, USBPacket *packet)
{
}

static USBPortOps cdj_usb_port_ops = {
    .attach       = cdj_usb_port_attach,
    .detach       = cdj_usb_port_detach,
    .child_detach = cdj_usb_port_child_detach,
    .complete     = cdj_usb_port_complete,
};

static USBBusOps cdj_usb_bus_ops = { };

static void cdj_usb_realize(DeviceState *dev, Error **errp)
{
    CdjUsbState *s = CDJ_USB(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &cdj_usb_ops, s,
                          "sh7724.usb", CDJ_USB_SIZE);
    /* A DREQ-driven DMA reads the FIFO port from inside this region's own
     * write handler. The recursion is bounded (it only touches the pipe
     * buffer), but the default reentrancy guard would return zeros. See
     * cdj_dmac_dreq(). */
    s->iomem.disable_reentrancy_guard = true;
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);

    s->attach_timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, cdj_usb_attach_fire, s);
    /* usb_packet_setup() resets the packet; its iovec is allocated once. */
    usb_packet_init(&s->packet);

    usb_bus_new(&s->bus, sizeof(s->bus), &cdj_usb_bus_ops, dev);
    /* The firmware programs full speed (DEVADD0 USBSPD = 10) and an 8-byte
     * default control pipe. */
    usb_register_port(&s->bus, &s->port, s, 0, &cdj_usb_port_ops,
                      USB_SPEED_MASK_FULL);
}

static void cdj_usb_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = cdj_usb_realize;
    dc->desc = "SH7724 USB 2.0 host module (R8A66597)";
}

static const TypeInfo cdj_usb_info = {
    .name          = TYPE_CDJ_USB,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(CdjUsbState),
    .class_init    = cdj_usb_class_init,
};

static void cdj_usb_register_types(void)
{
    type_register_static(&cdj_usb_info);
}
type_init(cdj_usb_register_types)

/* Controller state at exit; status and enable pairs side by side show which
 * event is pending but masked. */
static void cdj_usb_dump(Notifier *n, void *unused)
{
    CdjUsbState *s = container_of(n, CdjUsbState, exit);
    unsigned pipe;

    info_report("usb: INTSTS0=0x%04x (gated 0x%04x) INTENB0=0x%04x "
                "INTSTS1=0x%04x INTENB1=0x%04x",
                s->reg[CDJ_USB_INTSTS0 / 2], cdj_usb_intsts0(s),
                s->reg[CDJ_USB_INTENB0 / 2], s->reg[CDJ_USB_INTSTS1 / 2],
                s->reg[CDJ_USB_INTENB1 / 2]);
    info_report("usb: BRDY sts=0x%04x enb=0x%04x | NRDY sts=0x%04x enb=0x%04x "
                "| BEMP sts=0x%04x enb=0x%04x",
                s->reg[CDJ_USB_BRDYSTS / 2], s->reg[CDJ_USB_BRDYENB / 2],
                s->reg[CDJ_USB_NRDYSTS / 2], s->reg[CDJ_USB_NRDYENB / 2],
                s->reg[CDJ_USB_BEMPSTS / 2], s->reg[CDJ_USB_BEMPENB / 2]);
    info_report("usb: CFIFOSEL=0x%04x D0FIFOSEL=0x%04x D1FIFOSEL=0x%04x "
                "(windows on pipes %u/%u/%u)",
                s->reg[CDJ_USB_CFIFOSEL / 2], s->reg[CDJ_USB_D0FIFOSEL / 2],
                s->reg[CDJ_USB_D1FIFOSEL / 2], s->port_pipe[0],
                s->port_pipe[1], s->port_pipe[2]);
    for (pipe = 1; pipe < CDJ_USB_NR_PIPES; pipe++) {
        if (!s->pipecfg[pipe] && !s->buf[pipe].len) {
            continue;
        }
        info_report("usb: pipe %u cfg=0x%04x ctr=0x%04x tre=0x%04x trn=%u "
                    "buffer %u/%u consumed", pipe, s->pipecfg[pipe],
                    s->reg[CDJ_USB_PIPECTR(pipe) / 2],
                    pipe <= 5 ? s->reg[CDJ_USB_PIPETRE(pipe) / 2] : 0,
                    pipe <= 5 ? s->reg[CDJ_USB_PIPETRN(pipe) / 2] : 0,
                    s->buf[pipe].pos, s->buf[pipe].len);
    }
}

/* irq is the board INTC's USB0 (USI0) line. */
void cdj_usb_init(MemoryRegion *sysmem, qemu_irq irq)
{
    DeviceState *dev = qdev_new(TYPE_CDJ_USB);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    CdjUsbState *s = CDJ_USB(dev);

    sysbus_realize_and_unref(sbd, &error_fatal);
    s->exit.notify = cdj_usb_dump;
    qemu_add_exit_notifier(&s->exit);
    sysbus_connect_irq(sbd, 0, irq);
    memory_region_add_subregion_overlap(sysmem, CDJ_USB_BASE,
                                        sysbus_mmio_get_region(sbd, 0), 1);
}

