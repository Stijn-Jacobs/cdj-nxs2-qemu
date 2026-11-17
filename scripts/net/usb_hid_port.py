#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn the deck's rear USB-B HID endpoint into a HID device on this computer.

The machine's USB device port is enumerated inside QEMU and its endpoints relayed
as UDP datagrams (scripts/net/usb_midi_port.py is the MIDI half of the same
relay). Byte 0 is the endpoint address (bit 7 set = IN, deck -> host), the rest is
the report; a deck -> host datagram carries 8 bytes of the deck's virtual time,
little-endian, between the two. Byte 0 = 0 is a text line: the "configured
vid=... pid=... midi_in=... hid_in=..." status (repeated every second), a
"hid_desc=..." line with the deck's own HID report descriptor in hex, or
"detached".

The relay is byte-transparent: the deck's firmware speaks the whole Pioneer HID
protocol, handshake included, so this program only has to be a HID device with
the deck's own report descriptor and name.

  usage: python3 scripts/net/usb_hid_port.py [--port 27020] [--name CDJ-2000NXS2]
         python3 scripts/net/usb_hid_port.py --monitor   one line per report

Deck 1 sends from port 27020, deck 2 from 27021. --device uhid needs Linux
(/dev/uhid, no driver). Windows and macOS need a virtual-HID driver before
rekordbox can open the deck there; until then --monitor works everywhere and
shows the handshake happen. --name is the product string the host matches on, so
set it to the model the running firmware is.
"""
import argparse
import socket
import struct
import sys
import time

DEFAULT_PORT = 27020
DEFAULT_NAME = "CDJ-2000NXS2"

# linux/uhid.h, in declaration order
(UHID_CREATE, UHID_INPUT, UHID_OUTPUT, UHID_FEATURE, UHID_CLOSE, UHID_OPEN,
 UHID_DESTROY, UHID_START, UHID_STOP, UHID_GET_REPORT, UHID_SET_REPORT,
 UHID_GET_REPORT_REPLY, UHID_SET_REPORT_REPLY, UHID_CREATE2,
 UHID_INPUT2) = range(15)
UHID_DATA_SIZE = 4096
EVENT_SIZE = 4376                     # sizeof(struct uhid_event)
BUS_USB = 0x03

# the simple header's handshake types (see docs/rekordbox-usb-protocol.md)
HANDSHAKE = {0x11: "announce", 0x12: "host challenge", 0x13: "deck reply",
             0x14: "host reply", 0x15: "ack"}


def create2(rd_data, vendor, product, version, name):
    """The UHID_CREATE2 event body: a HID device with the deck's own report
    descriptor and identity."""
    body = struct.pack("<128s64s64sHHIIII", name.encode(), b"", b"",
                       len(rd_data), BUS_USB, vendor, product, version, 0)
    return body + rd_data.ljust(UHID_DATA_SIZE, b"\0")


def input2(report):
    """The UHID_INPUT2 event body: one report from the deck to the host."""
    return struct.pack("<H", len(report)) + report.ljust(UHID_DATA_SIZE, b"\0")


class Deck:
    """Who the deck is, which endpoints carry HID, and what it said."""

    def __init__(self, on_report, on_ready=None):
        self.on_report = on_report
        self.on_ready = on_ready
        self.addr = None
        self.vid = 0x2B73
        self.pid = 0x0005
        self.hid_in = 0x83
        self.hid_out = 0x02
        self.hid_desc = b""
        self.last_status = None

    def datagram(self, data, addr):
        self.addr = addr
        if data[0] == 0:
            self.status(data[1:].decode("ascii", "replace"))
        elif data[0] == self.hid_in:
            vt = int.from_bytes(data[1:9], "little")
            self.on_report(data[9:], vt)

    def status(self, text):
        if text.startswith("hid_desc="):
            self.hid_desc = bytes.fromhex(text[9:])
            if self.hid_desc and self.on_ready:
                self.on_ready(self)
            return
        # the machine repeats the rest every second
        if text == self.last_status:
            return
        self.last_status = text
        print("deck: " + text, flush=True)
        fields = dict(f.split("=", 1) for f in text.split() if "=" in f)
        self.vid = int(fields.get("vid", "%x" % self.vid), 16)
        self.pid = int(fields.get("pid", "%x" % self.pid), 16)
        self.hid_in = int(fields.get("hid_in", "83"), 16) or self.hid_in
        self.hid_out = int(fields.get("hid_out", "02"), 16) or self.hid_out

    def send(self, sock, report):
        if self.addr and self.hid_out:
            sock.sendto(bytes([self.hid_out]) + report, self.addr)


class Uhid:
    """/dev/uhid: a HID device made from user space."""

    def __init__(self, deck, name):
        import os
        self.os = os
        self.fd = os.open("/dev/uhid", os.O_RDWR)

        def send(event, body):
            os.write(self.fd, struct.pack("<I", event)
                     + body.ljust(EVENT_SIZE - 4, b"\0"))

        self.send = send
        self.send(UHID_CREATE2,
                  create2(deck.hid_desc, deck.vid, deck.pid, 0, name))

    def report(self, report, vt):
        self.send(UHID_INPUT2, input2(report))

    def pump(self, deck, sock):
        """Answer the kernel: what it writes goes to the deck.

        shortcut: GET/SET_REPORT are acknowledged empty -- the relay carries the
        interrupt endpoints only and the Pioneer handshake rides those, upgrade
        if a deck turns out to use feature reports
        """
        data = self.os.read(self.fd, EVENT_SIZE)
        event = struct.unpack_from("<I", data)[0]
        if event == UHID_OUTPUT:
            deck.send(sock, data[4:4 + UHID_DATA_SIZE].rstrip(b"\0"))
        elif event == UHID_GET_REPORT:
            self.send(UHID_GET_REPORT_REPLY, struct.pack("<IHH", 0, 0, 0))
        elif event == UHID_SET_REPORT:
            self.send(UHID_SET_REPORT_REPLY, struct.pack("<IH", 0, 0))
        # UHID_START/STOP/OPEN/CLOSE say the host is there or gone; nothing to do


def describe(report):
    """One line for --monitor: the handshake name when there is one.

    A handshake report is [0] deck, [1] 0xF0, [2..3] index, [4..5] total, then
    the super-TLV whose type (0x11..0x15) is byte 6.
    """
    if len(report) > 6 and report[1] == 0xF0 and report[6] in HANDSHAKE:
        return "  (%s)" % HANDSHAKE[report[6]]
    return ""


def serve(sock, deck, poll=None, stop=lambda: False):
    """Feed the deck's datagrams to `deck`; call `poll` every 2 ms or so."""
    sock.settimeout(0.002)
    while not stop():
        try:
            deck.datagram(*sock.recvfrom(65535))
        except socket.timeout:
            pass
        if poll:
            poll()


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT,
                    help="UDP port the machine sends to (default %d; deck 2 is one higher)"
                         % DEFAULT_PORT)
    ap.add_argument("--name", default=DEFAULT_NAME,
                    help="product string the host matches the model on (default %r)"
                         % DEFAULT_NAME)
    ap.add_argument("--monitor", action="store_true",
                    help="print every report, create no HID device")
    args = ap.parse_args(argv[1:])

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", args.port))

    if args.monitor:
        def show(report, vt):
            now = time.time()
            print("%s.%03d vt %.6f  %s%s"
                  % (time.strftime("%H:%M:%S", time.localtime(now)), now % 1 * 1000,
                     vt / 1e9, report.hex(" ").upper(), describe(report)),
                  flush=True)
        print("usb_hid_port: 127.0.0.1:%d (monitor)" % args.port, flush=True)
        return serve(sock, Deck(show))

    holder = {}

    def ready(deck):
        try:
            holder["hid"] = Uhid(deck, args.name)
        except FileNotFoundError:
            sys.exit("no /dev/uhid -- creating a HID device needs a kernel "
                     "driver: Linux has /dev/uhid built in, Windows needs a "
                     "Virtual HID Framework one and macOS a DriverKit one")
        deck.on_report = holder["hid"].report
        print("usb_hid_port: 127.0.0.1:%d -> /dev/uhid %r" % (args.port, args.name),
              flush=True)

    deck = Deck(lambda report, vt: None, on_ready=ready)
    while "hid" not in holder:
        try:
            deck.datagram(*sock.recvfrom(65535))
        except socket.timeout:
            pass
    serve(sock, deck, poll=lambda: holder["hid"].pump(deck, sock))


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except KeyboardInterrupt:
        pass
