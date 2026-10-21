/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include <stdarg.h>
/*
 * The rear USB-B function chip of the CDJ-2000 / CDJ-2000NXS: a Renesas
 * M66291GP behind a 16-bit window at 0x01000000 (the firmware uses the
 * uncached mirror 0xA1000000). Its INT# is wired to the SH7763's GPIO
 * channel 2 (INTEVT 0xFC0, INT2B7 bit 19, enabled through INT2GPIC).
 *
 * The model follows what the stock driver does with the chip (usbfd_task
 * 0x0435A9C2, the interrupt handler 0x0435B218, the EP0 FIFO helpers
 * 0x0410FBC4.. and the PIO channel service 0x041454C0 / 0x04145212):
 *
 *   +00 enable        +08 address (read only)  +0E polarity
 *   +10..16 INTENB0..3   +18 INTSTS0: 15 VBINT 14 RESM 13 SOF 12 DVST
 *        11 CTRT (write 0 clears), 10/9/8 BEMP/NRDY/BRDY summaries, 7 VBSTS,
 *        6:4 DVSQ (1 default, 2 address, 3 configured), 3 VALID, 2:0 CTSQ
 *        (1 read data, 2 read status, 3 write data, 4 write status,
 *        5 no-data status, 0 idle)
 *   +1A/1C/1E INTSTS1..3: BRDY, NRDY, BEMP (one bit per endpoint)
 *   +20..26 setup packet   +28 control transfer   +2C auto response
 *   +30/32/34/36 EP0 select / control (PID 15:14, VALID 13, CLR 12,
 *        BUSY 11, CCPL 10, length 8:0) / data / transmit length
 *   +40/42/44 CPU FIFO select / control / data
 *   +5E+4n/+5C+4n  EPn config1 (PID 15:14, ACLR 11, size 9:0) / config0
 *        (bit 13 = IN)
 *
 * SET_ADDRESS and SET_CONFIGURATION reach the driver like any request (no
 * data stage); the chip moves DVSQ and raises DVST when it completes them.
 *
 * The bus side is a host program, not a model of USB: with
 * CDJ_M66291_HOST=<udp port> the chip listens on 127.0.0.1:<port> and each
 * datagram is one host action, answered to the sender (little-endian):
 *   'V' on          Vbus; reply 'V' state (0 off, 1 on, 2 deck not ready)
 *   'R'             bus reset; reply 'R'
 *   'S' setup[8] [out data]
 *                   control transfer; reply 'S' status [in data], status
 *                   0 ok, 1 stall, 2 busy or timed out
 *   'O' ep data     OUT packet; reply 'O' ep status (0 ack, 1 nak, 2 none)
 *   'I' ep          IN poll; reply 'I' ep status (0 data follows, 1 nak)
 *   'O' 01 data     EP1 isochronous audio: queued (reply status 0, or 1
 *                   when the queue is full), one packet is put on the bus per
 *                   1 ms SOF; see below
 *   'L' on          lossless isochronous mode (a packet waits for the driver
 *                   to arm EP1 instead of being dropped); reply 'L' on
 *   'Q'             text dump of the chip state and isochronous counters
 * Without CDJ_M66291_HOST nothing ever happens on the bus: Vbus stays low.
 * CDJ_M66291_TRACE=1 logs register writes.
 *
 * Isochronous audio and SOF: while Vbus is up and the driver has enabled the
 * SOF interrupt (INTENB0 bit 13, which the stock USB audio task does when it
 * starts EP1) the chip raises INTSTS0.SOF every 1 ms of virtual time. 500 us
 * after each SOF (the driver's SOF handler has re-armed EP1 by then) the
 * head of the EP1 queue is put on the bus: if EP1's PID is BUF and its FIFO
 * is free the packet lands in the FIFO and BRDY(1) is raised; otherwise
 * isochronous has no NAK, so the packet is dropped and counted (or, in
 * lossless mode, kept for the next frame). A frame with an empty queue is
 * counted as an underrun once the stream has started.
 *
 * CDJ_M66291_LOG=<path> writes the audio log (one line per event, prefixed
 * "t=<virtual ms> sof=<frame>"; documented in scripts/usb/m66291_slot_diff.py):
 *   ISO frame=<n> len=<bytes> sum=<CRC-32 of the payload>  delivered to EP1
 *   ISODROP frame=<n> len=<bytes>                          EP1 not armed
 *   SLOTW/SLOTR s=<0|1> off=<hex> val=<hex>                MAIN's HPI write /
 *        read of a DSP audio slot (0x11837bc0 + s * 0x830), from dsp_c6747.c
 *
 * Not modelled: suspend/resume, NRDY, DMA FIFOs, single-bank FIFOs only, a
 * real frame number (the SOF flag carries none), the host-side audio
 * descriptors' timing rules (any packet length up to 1024 is accepted).
 */
#define USBF_BASE       0x01000000
#define USBF_SIZE       0x1000

#define R_ADDR          0x08
#define R_INTENB0       0x10
#define R_INTSTS0       0x18
#define R_SETUP         0x20
#define R_E0SEL         0x30
#define R_E0CTL         0x32
#define R_E0DATA        0x34
#define R_E0TXLEN       0x36
#define R_CPUSEL        0x40
#define R_CPUCTL        0x42
#define R_CPUDATA       0x44
#define R_CFG0(n)       (0x5c + 4 * (n))
#define R_CFG1(n)       (0x5e + 4 * (n))

#define S0_VBINT        0x8000
#define S0_DVST         0x1000
#define S0_CTRT         0x0800

#define PID_NAK         0
#define PID_BUF         1
#define PID_STALL       2

#define CTL_TIMEOUT_MS  5000

#define S0_SOF          0x2000
#define ISO_Q           64
#define ISO_MAX         1024
#define SOF_PERIOD_US   1000
#define ISO_DELAY_US    500

typedef struct UsbfEp {
    uint8_t in[1024];
    unsigned in_n;
    bool in_commit;
    uint8_t out[1024];
    unsigned out_n, out_p;
    bool out_full;
} UsbfEp;

typedef struct CdjUsbf {
    MemoryRegion iomem;
    qemu_irq irq;
    uint16_t reg[USBF_SIZE / 2];
    uint16_t l0, l3;            /* latched INTSTS0 (15:11) and INTSTS3 */
    bool valid, vbus;
    uint8_t dvsq, ctsq, addr, e0pid, cpu_ep;
    uint8_t e0tx[512], e0rx[64];
    unsigned e0tx_n, e0rx_n, e0rx_p;
    bool e0commit, e0end;
    UsbfEp ep[7];

    bool ctl;                   /* a control transfer is in progress */
    unsigned stage;
    uint8_t setup[8];
    uint8_t odata[1024];
    unsigned o_n, o_p;
    uint8_t resp[4096];
    unsigned resp_n;
    int64_t ctl_since;
    QEMUTimer *timer;

    struct {
        uint8_t d[ISO_Q][ISO_MAX];
        uint16_t n[ISO_Q];
        unsigned head, count;
        bool lossless, started;
        uint64_t sof, queued, delivered, dropped, empty;
        QEMUTimer *sof_timer, *put_timer;
    } iso;
    FILE *log;

    int fd;
    struct sockaddr_in peer;
    socklen_t peer_len;
    bool trace;
} CdjUsbf;

static CdjUsbf *usbf;

/* CRC-32 (IEEE), bitwise: only the audio log uses it. */
static uint32_t sum32(const uint8_t *p, unsigned n)
{
    uint32_t c = 0xffffffff;
    unsigned i, k;

    for (i = 0; i < n; i++) {
        c ^= p[i];
        for (k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xedb88320 & -(c & 1));
        }
    }
    return ~c;
}

void cdj2000_usbf_log(const char *fmt, ...)
{
    va_list ap;

    if (!usbf || !usbf->log) {
        return;
    }
    fprintf(usbf->log, "t=%" PRId64 " sof=%" PRIu64 " ",
            qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), usbf->iso.sof);
    va_start(ap, fmt);
    vfprintf(usbf->log, fmt, ap);
    va_end(ap);
    fputc('\n', usbf->log);
}

bool cdj2000_usbf_audio_started(void)
{
    return usbf && usbf->log && usbf->iso.sof > 0;
}

static unsigned ep_pid(CdjUsbf *s, unsigned n)
{
    return s->reg[R_CFG1(n) / 2] >> 14;
}

static bool ep_in(CdjUsbf *s, unsigned n)
{
    return s->reg[R_CFG0(n) / 2] & 0x2000;
}

static unsigned ep_maxp(CdjUsbf *s, unsigned n)
{
    unsigned m = s->reg[R_CFG1(n) / 2] & 0x3ff;

    return m ? m : 64;
}

static uint16_t sts1(CdjUsbf *s)
{
    uint16_t v = s->e0rx_p < s->e0rx_n;
    unsigned n;

    for (n = 1; n <= 6; n++) {
        UsbfEp *e = &s->ep[n];

        if (e->out_full || (ep_in(s, n) && !e->in_n && !e->in_commit)) {
            v |= 1u << n;
        }
    }
    return v;
}

static uint16_t sts0(CdjUsbf *s)
{
    uint16_t v = s->l0 | s->dvsq << 4 | s->ctsq | (s->valid ? 8 : 0);

    if (s->vbus) {
        v |= 0x80;
    }
    if (s->l3 & s->reg[0x16 / 2]) {
        v |= 0x400;
    }
    if (sts1(s) & s->reg[0x12 / 2]) {
        v |= 0x100;
    }
    return v;
}

static void usbf_update(CdjUsbf *s);

static bool sof_wanted(CdjUsbf *s)
{
    return s->vbus && (s->reg[R_INTENB0 / 2] & S0_SOF);
}

static void sof_kick(CdjUsbf *s)
{
    if (s->iso.sof_timer && sof_wanted(s) && !timer_pending(s->iso.sof_timer)) {
        timer_mod(s->iso.sof_timer, qemu_clock_get_us(QEMU_CLOCK_VIRTUAL) +
                  SOF_PERIOD_US);
    }
}

static void sof_tick(void *opaque)
{
    CdjUsbf *s = opaque;
    int64_t now = qemu_clock_get_us(QEMU_CLOCK_VIRTUAL);

    if (!sof_wanted(s)) {
        return;                 /* sof_kick restarts it */
    }
    s->iso.sof++;
    s->l0 |= S0_SOF;
    timer_mod(s->iso.put_timer, now + ISO_DELAY_US);
    timer_mod(s->iso.sof_timer, now + SOF_PERIOD_US);
    usbf_update(s);
}

static void iso_put(void *opaque)
{
    CdjUsbf *s = opaque;
    UsbfEp *e = &s->ep[1];
    unsigned h = s->iso.head;

    if (s->dvsq < 3) {
        return;
    }
    if (!s->iso.count) {
        if (s->iso.started) {
            s->iso.empty++;
        }
        return;
    }
    if (ep_in(s, 1) || ep_pid(s, 1) != PID_BUF || e->out_full) {
        if (!s->iso.lossless) {
            s->iso.dropped++;
            cdj2000_usbf_log("ISODROP frame=%" PRIu64 " len=%u", s->iso.sof,
                             s->iso.n[h]);
            s->iso.head = (h + 1) % ISO_Q;
            s->iso.count--;
        }
        return;
    }
    memcpy(e->out, s->iso.d[h], s->iso.n[h]);
    e->out_n = s->iso.n[h];
    e->out_p = 0;
    e->out_full = true;
    s->iso.delivered++;
    cdj2000_usbf_log("ISO frame=%" PRIu64 " len=%u sum=%08x", s->iso.sof,
                     e->out_n, sum32(e->out, e->out_n));
    s->iso.head = (h + 1) % ISO_Q;
    s->iso.count--;
    usbf_update(s);
}

static void usbf_update(CdjUsbf *s)
{
    qemu_set_irq(s->irq, (sts0(s) & s->reg[R_INTENB0 / 2] & 0xff00) != 0);
}

static void usbf_reply(CdjUsbf *s, const uint8_t *b, size_t n)
{
    if (s->fd >= 0 && s->peer_len) {
        (void)sendto(s->fd, b, n, 0, (struct sockaddr *)&s->peer, s->peer_len);
    }
}

static void ctl_done(CdjUsbf *s, uint8_t status)
{
    uint8_t b[2 + sizeof(s->resp)];

    b[0] = 'S';
    b[1] = status;
    if (!status) {
        memcpy(b + 2, s->resp, s->resp_n);
    }
    usbf_reply(s, b, 2 + (status ? 0 : s->resp_n));
    s->ctl = false;
}

static void ep0_flush(CdjUsbf *s)
{
    s->e0tx_n = s->e0rx_n = s->e0rx_p = 0;
    s->e0commit = s->e0end = false;
}

/* Advance the control transfer as far as the driver's register writes allow. */
static void ctl_progress(CdjUsbf *s)
{
    unsigned wlen = lduw_le_p(s->setup + 6);

    if (!s->ctl || s->stage < 1) {
        return;
    }
    if (s->e0pid == PID_STALL && s->stage != 2) {
        ctl_done(s, 1);
        return;
    }
    if (s->stage == 1) {
        unsigned txlen = s->reg[R_E0TXLEN / 2];

        if (!s->e0commit && txlen && s->e0tx_n >= txlen) {
            s->e0commit = true;
        }
        if (s->e0commit && s->e0pid == PID_BUF) {
            unsigned take = MIN(s->e0tx_n, wlen - s->resp_n);
            bool end = s->e0end;

            memcpy(s->resp + s->resp_n, s->e0tx, take);
            s->resp_n += take;
            s->e0tx_n = 0;
            s->e0commit = s->e0end = false;
            if (end || s->resp_n >= wlen) {
                s->stage = s->ctsq = 2;
                s->l0 |= S0_CTRT;
                ctl_done(s, 0);
                /* until the driver completes the status stage (CCPL) */
                s->ctl = true;
                s->ctl_since = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
            } else {
                s->l3 |= 1;
            }
        }
    } else if (s->stage == 3) {
        if (s->e0rx_p >= s->e0rx_n) {
            s->e0rx_n = s->e0rx_p = 0;
            if (s->o_p < s->o_n) {
                if (s->e0pid == PID_BUF) {
                    unsigned n = MIN(s->o_n - s->o_p, 64);

                    memcpy(s->e0rx, s->odata + s->o_p, n);
                    s->o_p += n;
                    s->e0rx_n = n;
                }
            } else {
                s->stage = s->ctsq = 4;
                s->l0 |= S0_CTRT;
            }
        }
    }
}

static void ctl_ccpl(CdjUsbf *s)
{
    if (!s->ctl || (s->stage != 2 && s->stage != 4 && s->stage != 5)) {
        return;
    }
    if (s->stage == 5 && s->setup[0] == 0x00) {
        /* the chip moves the device state once the status stage is done */
        uint16_t w = lduw_le_p(s->setup + 2);

        if (s->setup[1] == 5) {
            s->addr = w & 0x7f;
            s->dvsq = s->addr ? 2 : 1;
            s->l0 |= S0_DVST;
        } else if (s->setup[1] == 9) {
            s->dvsq = w ? 3 : 2;
            s->l0 |= S0_DVST;
        }
    }
    if (s->stage != 2) {
        s->resp_n = 0;
        ctl_done(s, 0);
    }
    s->ctl = false;
    s->stage = 0;
    s->ctsq = 0;
    s->l0 |= S0_CTRT;
}

static void ctl_timeout(void *opaque)
{
    CdjUsbf *s = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    if (s->ctl && now - s->ctl_since >= CTL_TIMEOUT_MS) {
        if (s->stage == 2) {
            s->ctl = false;     /* the driver never completed the status */
            s->stage = s->ctsq = 0;
        } else {
            warn_report("usbf: control request 0x%02x not answered by the "
                        "deck", s->setup[1]);
            ctl_done(s, 2);
        }
    }
    timer_mod(s->timer, now + 500);
}

static void ep_flush(CdjUsbf *s, unsigned n)
{
    UsbfEp *e = &s->ep[n];

    e->in_n = e->out_n = e->out_p = 0;
    e->in_commit = e->out_full = false;
}

static void ep_in_commit(CdjUsbf *s, unsigned n)
{
    if (ep_in(s, n)) {
        s->ep[n].in_commit = true;
    }
}


/* Take up to one access of bytes from a FIFO buffer (low byte first). */
static uint32_t fifo_pop(const uint8_t *buf, unsigned *p, unsigned n,
                         unsigned size, bool octl)
{
    unsigned i, take = (size == 2 && !octl) ? 2 : 1;
    uint32_t v = 0;

    for (i = 0; i < take && *p < n; i++) {
        v |= (uint32_t)buf[(*p)++] << (8 * i);
    }
    return v;
}

static unsigned fifo_push(uint8_t *buf, unsigned *n, unsigned cap, uint32_t v,
                          unsigned size, bool octl)
{
    unsigned i, put = (size == 2 && !octl) ? 2 : 1;

    for (i = 0; i < put && *n < cap; i++) {
        buf[(*n)++] = v >> (8 * i);
    }
    return *n;
}

static uint64_t usbf_read(void *opaque, hwaddr off, unsigned size)
{
    CdjUsbf *s = opaque;
    unsigned n = s->cpu_ep;
    uint32_t v;
    unsigned shift = (off & 1) * 8;

    switch (off & ~1) {
    case R_ADDR:
        v = s->addr;
        break;
    case R_INTSTS0:
        v = sts0(s);
        break;
    case 0x1a:
        v = sts1(s);
        break;
    case 0x1c:
        v = 0;
        break;
    case 0x1e:
        v = s->l3;
        break;
    case R_E0CTL:
        v = s->e0pid << 14;
        if (s->e0rx_p < s->e0rx_n) {
            v |= 0x2000 | (s->e0rx_n - s->e0rx_p);
        }
        break;
    case R_E0DATA:
        v = fifo_pop(s->e0rx, &s->e0rx_p, s->e0rx_n, size,
                     s->reg[R_E0SEL / 2] & 0x400);
        if (s->e0rx_p >= s->e0rx_n) {
            ctl_progress(s);
            usbf_update(s);
        }
        return v;
    case R_CPUCTL:
        v = 0;
        if (n >= 1 && n <= 6 && !ep_in(s, n) && s->ep[n].out_full) {
            v = 0x2000 | MIN(s->ep[n].out_n - s->ep[n].out_p, 0x7ff);
        }
        break;
    case R_CPUDATA:
        v = 0;
        if (n >= 1 && n <= 6) {
            UsbfEp *e = &s->ep[n];

            v = fifo_pop(e->out, &e->out_p, e->out_n, size,
                         s->reg[R_CPUSEL / 2] & 0x40);
            if (e->out_p >= e->out_n) {
                e->out_full = false;
                usbf_update(s);
            }
        }
        return v;
    default:
        v = s->reg[(off & ~1) / 2];
        break;
    }
    return size == 1 ? (v >> shift) & 0xff : v;
}

static void usbf_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjUsbf *s = opaque;
    unsigned n = s->cpu_ep;
    uint16_t v = val;
    hwaddr o = off & ~1;

    if (s->trace && o != R_E0DATA && o != R_CPUDATA) {
        info_report("usbf: w%u +%02x = %04x", size * 8, (unsigned)off, v);
    }
    if (size == 1) {
        uint16_t old = s->reg[o / 2];

        v = (off & 1) ? (old & 0xff) | v << 8 : (old & 0xff00) | (v & 0xff);
    }
    switch (o) {
    case R_ADDR:
        return;
    case R_INTSTS0:
        s->l0 &= v | 0x07ff;
        if (!(v & 8)) {
            s->valid = false;
        }
        break;
    case 0x1a:
    case 0x1c:
        return;
    case 0x1e:
        s->l3 &= v;
        break;
    case R_E0CTL:
        s->e0pid = (v >> 14) ? ((v >> 14) == 1 ? PID_BUF : PID_STALL) : PID_NAK;
        if (v & 0x1000) {
            ep0_flush(s);
        }
        if ((v & 0x2000) && s->ctl && s->stage == 1) {
            s->e0commit = s->e0end = true;
        }
        if (v & 0x400) {
            ctl_ccpl(s);
        }
        ctl_progress(s);
        break;
    case R_E0DATA:
        if (s->e0tx_n < sizeof(s->e0tx)) {
            fifo_push(s->e0tx, &s->e0tx_n, sizeof(s->e0tx), v, size,
                      s->reg[R_E0SEL / 2] & 0x400);
        }
        ctl_progress(s);
        break;
    case R_CPUSEL:
        s->reg[o / 2] = v;
        s->cpu_ep = v & 0xf;
        return;
    case R_CPUCTL:
        if (n >= 1 && n <= 6) {
            if (v & 0x1000) {
                ep_flush(s, n);
            }
            if (v & 0x2000) {
                ep_in_commit(s, n);
            }
        }
        break;
    case R_CPUDATA:
        if (n >= 1 && n <= 6 && ep_in(s, n) && !s->ep[n].in_commit) {
            UsbfEp *e = &s->ep[n];

            if (fifo_push(e->in, &e->in_n, sizeof(e->in), v, size,
                          s->reg[R_CPUSEL / 2] & 0x40) >= ep_maxp(s, n)) {
                e->in_commit = true;
            }
        }
        break;
    default:
        if (o >= R_CFG1(1) && o <= R_CFG1(6) && ((o - R_CFG1(1)) & 3) == 0
            && (v & 0x800)) {
            ep_flush(s, (o - R_CFG1(1)) / 4 + 1);
        }
        s->reg[o / 2] = v;
        if (o == R_INTENB0) {
            sof_kick(s);
        }
        break;
    }
    usbf_update(s);
}

static const MemoryRegionOps usbf_ops = {
    .read = usbf_read,
    .write = usbf_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 2 },
};

/* --- the host side ------------------------------------------------------- */

static void host_setup(CdjUsbf *s, const uint8_t *b, size_t n)
{
    uint8_t type = b[0];
    unsigned wlen = lduw_le_p(b + 6);

    memcpy(s->setup, b, 8);
    s->reg[R_SETUP / 2] = b[1] << 8 | type;
    s->reg[R_SETUP / 2 + 1] = lduw_le_p(b + 2);
    s->reg[R_SETUP / 2 + 2] = lduw_le_p(b + 4);
    s->reg[R_SETUP / 2 + 3] = wlen;
    s->resp_n = 0;
    ep0_flush(s);
    s->e0pid = PID_NAK;
    s->o_n = MIN(n - 8, sizeof(s->odata));
    s->o_p = 0;
    memcpy(s->odata, b + 8, s->o_n);
    s->stage = (type & 0x80) ? (wlen ? 1 : 5) : (wlen ? 3 : 5);
    s->ctsq = s->stage;
    s->valid = true;
    s->l0 |= S0_CTRT;
    s->ctl = true;
    s->ctl_since = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    ctl_progress(s);
}

static void host_packet(CdjUsbf *s, const uint8_t *b, size_t n)
{
    uint8_t r[8 + 1024];
    unsigned ep;

    switch (b[0]) {
    case 'V':
        r[0] = 'V';
        if (n < 2) {
            return;
        }
        if (!s->reg[R_INTENB0 / 2]) {
            r[1] = 2;
        } else {
            s->vbus = b[1];
            s->l0 |= S0_VBINT;
            sof_kick(s);
            if (!s->vbus) {
                s->dvsq = s->ctsq = 0;
                s->ctl = false;
            }
            r[1] = s->vbus;
        }
        usbf_reply(s, r, 2);
        break;
    case 'R':
        s->iso.count = 0;
        s->dvsq = 1;
        s->addr = 0;
        s->ctl = false;
        s->ctsq = 0;
        ep0_flush(s);
        for (ep = 1; ep <= 6; ep++) {
            ep_flush(s, ep);
        }
        s->l0 |= S0_DVST;
        usbf_reply(s, (const uint8_t *)"R", 1);
        break;
    case 'S':
        if (n < 9 || s->dvsq < 1) {
            r[0] = 'S';
            r[1] = 2;
            usbf_reply(s, r, 2);
        } else if (s->ctl) {
            r[0] = 'S';
            r[1] = 2;
            usbf_reply(s, r, 2);
        } else {
            host_setup(s, b + 1, n - 1);
        }
        break;
    case 'O':
        ep = n > 1 ? b[1] & 0xf : 0;
        r[0] = 'O';
        r[1] = ep;
        r[2] = 2;
        if (n >= 2 && ep >= 1 && ep <= 6 && s->dvsq >= 2 && !ep_in(s, ep)) {
            UsbfEp *e = &s->ep[ep];

            if (ep == 1) {
                if (s->iso.count >= ISO_Q || n - 2 > ISO_MAX) {
                    r[2] = 1;
                } else {
                    unsigned t = (s->iso.head + s->iso.count++) % ISO_Q;

                    memcpy(s->iso.d[t], b + 2, n - 2);
                    s->iso.n[t] = n - 2;
                    s->iso.queued++;
                    s->iso.started = true;
                    r[2] = 0;
                }
            } else if (ep_pid(s, ep) != PID_BUF || e->out_full
                       || !(s->reg[0x12 / 2] & 1u << ep)
                       || n - 2 > MIN(ep_maxp(s, ep), sizeof(e->out))) {
                r[2] = 1;
            } else {
                memcpy(e->out, b + 2, n - 2);
                e->out_n = n - 2;
                e->out_p = 0;
                e->out_full = true;
                r[2] = 0;
            }
        }
        usbf_reply(s, r, 3);
        break;
    case 'I':
        ep = n > 1 ? b[1] & 0xf : 0;
        r[0] = 'I';
        r[1] = ep;
        r[2] = 2;
        if (n >= 2 && ep >= 1 && ep <= 6 && s->dvsq >= 2 && ep_in(s, ep)) {
            UsbfEp *e = &s->ep[ep];

            r[2] = 1;
            if (e->in_commit && ep_pid(s, ep) == PID_BUF) {
                r[2] = 0;
                memcpy(r + 3, e->in, e->in_n);
                usbf_reply(s, r, 3 + e->in_n);
                e->in_n = 0;
                e->in_commit = false;
                s->l3 |= 1u << ep;
                break;
            }
        }
        usbf_reply(s, r, 3);
        break;
    case 'L':
        s->iso.lossless = n > 1 && b[1];
        r[0] = 'L';
        r[1] = s->iso.lossless;
        usbf_reply(s, r, 2);
        break;
    case 'Q': {
        char q[512];
        int k = snprintf(q, sizeof(q),
                         "Qvbus=%d dvsq=%u ctsq=%u addr=%u sts0=%04x enb0=%04x "
                         "sts1=%04x enb1=%04x sts3=%04x enb3=%04x ctl=%d "
                         "stage=%u sof=%" PRIu64 " iso_queued=%" PRIu64
                         " iso_delivered=%" PRIu64 " iso_dropped=%" PRIu64
                         " iso_empty=%" PRIu64 " iso_pending=%u",
                         s->vbus, s->dvsq, s->ctsq, s->addr, sts0(s),
                         s->reg[R_INTENB0 / 2], sts1(s), s->reg[0x12 / 2],
                         s->l3, s->reg[0x16 / 2], s->ctl, s->stage,
                         s->iso.sof, s->iso.queued, s->iso.delivered,
                         s->iso.dropped, s->iso.empty, s->iso.count);
        usbf_reply(s, (uint8_t *)q, k);
        break;
    }
    default:
        break;
    }
    usbf_update(s);
}

static void host_read(void *opaque)
{
    CdjUsbf *s = opaque;
    uint8_t b[2048];
    ssize_t n;

    s->peer_len = sizeof(s->peer);
    n = recvfrom(s->fd, b, sizeof(b), 0, (struct sockaddr *)&s->peer,
                 &s->peer_len);
    if (n > 0) {
        host_packet(s, b, n);
    }
}

static const VMStateDescription vmstate_usbf = {
    .name = "cdj2000-usbf-m66291",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(reg, CdjUsbf, USBF_SIZE / 2),
        VMSTATE_UINT16(l0, CdjUsbf),
        VMSTATE_UINT16(l3, CdjUsbf),
        VMSTATE_BOOL(valid, CdjUsbf),
        VMSTATE_BOOL(vbus, CdjUsbf),
        VMSTATE_UINT8(dvsq, CdjUsbf),
        VMSTATE_UINT8(ctsq, CdjUsbf),
        VMSTATE_UINT8(addr, CdjUsbf),
        VMSTATE_UINT8(e0pid, CdjUsbf),
        VMSTATE_UINT8(cpu_ep, CdjUsbf),
        VMSTATE_END_OF_LIST()
    }
};

void cdj2000_usbf_init(MemoryRegion *sysmem, qemu_irq irq)
{
    const char *port = getenv("CDJ_M66291_HOST");
    CdjUsbf *s = g_new0(CdjUsbf, 1);

    usbf = s;
    s->irq = irq;
    s->fd = -1;
    s->trace = getenv("CDJ_M66291_TRACE") != NULL;
    if (getenv("CDJ_M66291_LOG")) {
        s->log = fopen(getenv("CDJ_M66291_LOG"), "w");
        if (s->log) {
            setvbuf(s->log, NULL, _IOLBF, 0);
        } else {
            warn_report("usbf: cannot write %s", getenv("CDJ_M66291_LOG"));
        }
    }
    vmstate_register_any(NULL, &vmstate_usbf, s);
    memory_region_init_io(&s->iomem, NULL, &usbf_ops, s, "cdj2000.usbf",
                          USBF_SIZE);
    memory_region_add_subregion(sysmem, USBF_BASE, &s->iomem);
    if (port && *port) {
        struct sockaddr_in a = { .sin_family = AF_INET };

        a.sin_port = htons(atoi(port));
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        s->fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (s->fd < 0 || bind(s->fd, (struct sockaddr *)&a, sizeof(a))) {
            warn_report("usbf: cannot listen on udp %s", port);
            s->fd = -1;
            return;
        }
        qemu_socket_set_nonblock(s->fd);
        qemu_set_fd_handler(s->fd, host_read, NULL, s);
        s->iso.sof_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, sof_tick, s);
        s->iso.put_timer = timer_new_us(QEMU_CLOCK_VIRTUAL, iso_put, s);
        s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, ctl_timeout, s);
        timer_mod(s->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 500);
        info_report("usbf: M66291 host relay on udp 127.0.0.1:%s", port);
    }
}
