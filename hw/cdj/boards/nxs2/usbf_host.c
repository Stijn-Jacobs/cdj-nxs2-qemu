/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
#include "cdj_panelkeys.h"
/*
 * A host on the rear USB port. It enumerates the deck the way a PC does and
 * then relays the MIDI and HID endpoints over UDP, so a program on the
 * desktop can be the application (scripts/net/usb_midi_port.py).
 *
 *   CDJ_USBF_HOST=<port>   plug the host in; packets go to 127.0.0.1:<port>
 *
 * One datagram per packet, both ways: byte 0 is the endpoint address, the
 * rest the payload. The host's replies come back to the sending socket. A
 * deck -> host datagram has the guest's virtual time in nanoseconds, 8 bytes
 * little-endian, between the endpoint byte and the payload, so the receiver can
 * replay the packets at the deck's own pace however unevenly the emulator ran.
 * Byte 0 = 0 is a line of text about the port itself, without the time:
 *
 *   configured vid=2b73 pid=0005 midi_in=85 midi_out=04 hid_in=83 hid_out=02
 *   detached
 *
 * "configured" is repeated every second, for a program started later. An
 * endpoint the deck does not have reads 00.
 */
#define CDJ_USBF_HOST_ENV "CDJ_USBF_HOST"

enum { HOST_DETACHED, HOST_SETTLE, HOST_ENUMERATE, HOST_CONFIGURED, HOST_FAILED };
enum { XFER_SETUP, XFER_DATA, XFER_STATUS };
/* The enumeration, one control transfer per step. */
enum {
    STEP_DEVICE_DESC, STEP_SET_ADDRESS, STEP_CONFIG_HEAD, STEP_CONFIG_DESC,
    STEP_SET_CONFIG,
};

typedef struct {
    CdjUsbfState *dev;
    QEMUTimer *timer;
    int fd;
    unsigned state, step;
    int64_t since_ms;               /* when the state or the transfer began */
    int64_t told_ms;

    uint8_t setup[8];
    unsigned stage;
    uint8_t data[1024];             /* the transfer's IN data stage */
    unsigned len;

    uint16_t vid, pid;
    uint8_t maxp0, config;
    uint8_t midi_in, midi_out, hid_in, hid_out;

    uint8_t out[1 + 1024];          /* a host->deck datagram not yet taken */
    int out_len;
} CdjUsbfHost;

static void cdj_usbf_host_request(CdjUsbfHost *h, uint8_t type, uint8_t request,
                                  uint16_t value, uint16_t length)
{
    h->setup[0] = type;
    h->setup[1] = request;
    stw_le_p(h->setup + 2, value);
    stw_le_p(h->setup + 4, 0);
    stw_le_p(h->setup + 6, length);
    h->stage = XFER_SETUP;
    h->since_ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
}

/* Advance the control transfer by one token; true when it has completed.
 * A NAK is simply tried again on the next tick. */
static bool cdj_usbf_host_control(CdjUsbfHost *h)
{
    unsigned want = lduw_le_p(h->setup + 6);
    int n;

    switch (h->stage) {
    case XFER_SETUP:
        if (cdj_usbf_setup(h->dev, h->setup) < 0) {
            return false;
        }
        h->len = 0;
        h->stage = want ? XFER_DATA : XFER_STATUS;
        return false;
    case XFER_DATA:
        n = cdj_usbf_in(h->dev, 0, h->data + h->len, want - h->len);
        if (n < 0) {
            return false;
        }
        h->len += n;
        /* A short packet ends the data stage; before the device descriptor
         * has been read the packet size is unknown. */
        if (h->len < want && n && (!h->maxp0 || n == h->maxp0)) {
            return false;
        }
        h->stage = XFER_STATUS;
        return false;
    default:
        n = want ? cdj_usbf_out(h->dev, 0, NULL, 0)
                 : cdj_usbf_in(h->dev, 0, NULL, 0);
        return n >= 0;
    }
}

/* The MIDI streaming interface's bulk endpoints and the HID interface's
 * interrupt endpoints, from the configuration descriptor. */
static void cdj_usbf_host_parse(CdjUsbfHost *h)
{
    unsigned off, class = 0, subclass = 0;

    for (off = 0; off + 2 <= h->len && h->data[off]; off += h->data[off]) {
        const uint8_t *d = h->data + off;
        uint8_t *ep = NULL;

        if (d[1] == USB_DT_INTERFACE && off + 9 <= h->len) {
            class = d[5];
            subclass = d[6];
        } else if (d[1] == USB_DT_ENDPOINT && off + 4 <= h->len) {
            bool in = d[2] & USB_DIR_IN;

            if (class == USB_CLASS_AUDIO && subclass == 3) {
                ep = in ? &h->midi_in : &h->midi_out;
            } else if (class == USB_CLASS_HID) {
                ep = in ? &h->hid_in : &h->hid_out;
            }
            if (ep && !*ep) {
                *ep = d[2];
            }
        }
    }
}

static void cdj_usbf_host_tell(CdjUsbfHost *h)
{
    char msg[96];
    int n;

    msg[0] = 0;
    if (h->state == HOST_CONFIGURED) {
        n = snprintf(msg + 1, sizeof(msg) - 1, "configured vid=%04x pid=%04x "
                     "midi_in=%02x midi_out=%02x hid_in=%02x hid_out=%02x",
                     h->vid, h->pid, h->midi_in, h->midi_out, h->hid_in,
                     h->hid_out);
    } else {
        n = snprintf(msg + 1, sizeof(msg) - 1, "detached");
    }
    (void)send(h->fd, msg, n + 1, 0);
    h->told_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
}

static void cdj_usbf_host_next_step(CdjUsbfHost *h)
{
    switch (h->step++) {
    case STEP_DEVICE_DESC:
        h->maxp0 = h->data[7];
        h->vid = lduw_le_p(h->data + 8);
        h->pid = lduw_le_p(h->data + 10);
        cdj_usbf_host_request(h, 0x00, USB_REQ_SET_ADDRESS, 1, 0);
        break;
    case STEP_SET_ADDRESS:
        cdj_usbf_host_request(h, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                              USB_DT_CONFIG << 8, 9);
        break;
    case STEP_CONFIG_HEAD:
        cdj_usbf_host_request(h, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                              USB_DT_CONFIG << 8,
                              MIN(lduw_le_p(h->data + 2), sizeof(h->data)));
        break;
    case STEP_CONFIG_DESC:
        h->config = h->data[5];
        cdj_usbf_host_parse(h);
        cdj_usbf_host_request(h, 0x00, USB_REQ_SET_CONFIGURATION, h->config, 0);
        break;
    case STEP_SET_CONFIG:
        h->state = HOST_CONFIGURED;
        info_report("usb host: deck %04x:%04x configured, MIDI in 0x%02x "
                    "out 0x%02x, HID in 0x%02x out 0x%02x", h->vid, h->pid,
                    h->midi_in, h->midi_out, h->hid_in, h->hid_out);
        cdj_usbf_host_tell(h);
        break;
    }
}

/* Poll the IN endpoints and deliver what the desktop sent. A full-speed
 * frame carries at most 19 64-byte bulk packets. */
static void cdj_usbf_host_relay(CdjUsbfHost *h)
{
    const uint8_t in[] = { h->midi_in, h->hid_in };
    uint8_t pkt[1 + 8 + 1024];
    unsigned i, k;
    int n;

    for (i = 0; i < ARRAY_SIZE(in); i++) {
        for (k = 0; in[i] && k < 19; k++) {
            n = cdj_usbf_in(h->dev, in[i] & 0x0f, pkt + 9, sizeof(pkt) - 9);
            if (n < 0) {
                break;
            }
            if (n) {
                pkt[0] = in[i];
                stq_le_p(pkt + 1, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
                (void)send(h->fd, pkt, n + 9, 0);
            }
        }
    }

    if (h->out_len <= 0) {
        h->out_len = recv(h->fd, h->out, sizeof(h->out), 0);
    }
    if (h->out_len > 0) {
        if (!h->out[0] || (h->out[0] & USB_DIR_IN)
            || cdj_usbf_out(h->dev, h->out[0] & 0x0f, h->out + 1,
                            h->out_len - 1) != CDJ_USBF_NAK) {
            h->out_len = 0;
        }
    }
}

static void cdj_usbf_host_tick(void *opaque)
{
    CdjUsbfHost *h = opaque;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    if (h->state != HOST_DETACHED && h->state != HOST_FAILED
        && !cdj_usbf_attached(h->dev)) {
        h->state = HOST_DETACHED;
        cdj_usbf_host_tell(h);
    }

    switch (h->state) {
    case HOST_DETACHED:
        if (cdj_usbf_attached(h->dev)) {
            h->state = HOST_SETTLE;
            h->since_ms = now;
        }
        break;
    case HOST_SETTLE:
        /* USB 2.0 7.1.7.3: 100 ms for the connection to settle. */
        if (now - h->since_ms >= 100) {
            cdj_usbf_bus_reset(h->dev);
            h->maxp0 = h->midi_in = h->midi_out = h->hid_in = h->hid_out = 0;
            h->step = STEP_DEVICE_DESC;
            h->state = HOST_ENUMERATE;
            cdj_usbf_host_request(h, USB_DIR_IN, USB_REQ_GET_DESCRIPTOR,
                                  USB_DT_DEVICE << 8, 18);
        }
        break;
    case HOST_ENUMERATE:
        if (cdj_usbf_host_control(h)) {
            cdj_usbf_host_next_step(h);
        } else if (now - h->since_ms > 5000) {
            warn_report("usb host: the deck did not answer request 0x%02x "
                        "(enumeration step %u)", h->setup[1], h->step);
            h->state = HOST_FAILED;
        }
        break;
    case HOST_CONFIGURED:
        cdj_usbf_host_relay(h);
        if (qemu_clock_get_ms(QEMU_CLOCK_REALTIME) - h->told_ms >= 1000) {
            cdj_usbf_host_tell(h);
        }
        break;
    }

    timer_mod(h->timer, now + 1);
}

void cdj_usbf_host_init(CdjUsbfState *dev)
{
    const char *port = getenv(CDJ_USBF_HOST_ENV);
    CdjUsbfHost *h = g_new0(CdjUsbfHost, 1);

    h->dev = dev;
    h->fd = cdj_panelsock_connect(port);
    if (h->fd < 0) {
        warn_report("usb host: no UDP socket for %s=%s", CDJ_USBF_HOST_ENV,
                    port);
    } else {
        info_report("usb host: plugged into the rear USB port, packets to "
                    "udp %u", cdj_panelsock_port(port));
    }
    cdj_usbf_vbus(dev, true);
    h->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, cdj_usbf_host_tick, h);
    timer_mod(h->timer, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + 1);
}
