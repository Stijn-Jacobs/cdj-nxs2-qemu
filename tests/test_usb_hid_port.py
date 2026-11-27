# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/net/usb_hid_port.py: the datagram side and the UHID event bodies."""
import socket
import struct

import usb_hid_port as up


def stamped(endpoint, vt_ns, payload):
    return bytes([endpoint]) + vt_ns.to_bytes(8, "little") + bytes.fromhex(payload)


# -- the datagram side ---------------------------------------------------------

def test_deck_routes_hid_in_and_follows_the_status_line():
    seen = []
    deck = up.Deck(lambda report, vt: seen.append((report, vt)))
    deck.datagram(b"\x00configured vid=2b73 pid=0005 midi_in=85 midi_out=04 hid_in=83 hid_out=02",
                  ("127.0.0.1", 1))
    assert (deck.hid_in, deck.hid_out) == (0x83, 0x02)
    deck.datagram(stamped(0x83, 1_234_567_890, "11 04 01 04 01 05"), ("127.0.0.1", 1))
    assert seen == [(bytes.fromhex("11 04 01 04 01 05"), 1_234_567_890)]


def test_deck_ignores_midi_endpoints():
    seen = []
    deck = up.Deck(lambda report, vt: seen.append(report))
    deck.datagram(stamped(0x85, 0, "0F F8 00 00"), ("127.0.0.1", 1))
    assert seen == []


def test_deck_collects_the_report_descriptor():
    deck = up.Deck(lambda report, vt: None)
    deck.datagram(b"\x00hid_desc=05010906a101", ("127.0.0.1", 1))
    assert deck.hid_desc == bytes.fromhex("05010906a101")


def test_deck_send_goes_to_the_output_endpoint():
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        deck = up.Deck(lambda report, vt: None)
        deck.datagram(b"\x00configured hid_in=83 hid_out=02", rx.getsockname())
        deck.send(tx, bytes.fromhex("AA BB"))
        assert rx.recvfrom(64)[0] == bytes.fromhex("02 AA BB")
    finally:
        rx.close()
        tx.close()


# -- UHID event bodies ---------------------------------------------------------

def test_create2_carries_the_descriptor_and_ids():
    body = up.create2(bytes.fromhex("0501"), 0x2B73, 0x0005, 0x0100, "CDJ-2000NXS2")
    name, phys, uniq, rd_size, bus, vendor, product, version, country = (
        __import__("struct").unpack_from("<128s64s64sHHIIII", body))
    assert name.rstrip(b"\0") == b"CDJ-2000NXS2"
    assert (rd_size, bus) == (2, up.BUS_USB)
    assert (vendor, product, version) == (0x2B73, 0x0005, 0x0100)
    assert body[-up.UHID_DATA_SIZE:-up.UHID_DATA_SIZE + 2] == bytes.fromhex("0501")


def test_input2_prefixes_the_length():
    body = up.input2(bytes.fromhex("11 04"))
    assert body[:2] == b"\x02\x00"
    assert body[2:4] == bytes.fromhex("11 04")
    assert len(body) == 2 + up.UHID_DATA_SIZE


def test_event_numbers_follow_the_kernel_header():
    assert (up.UHID_DESTROY, up.UHID_START, up.UHID_STOP, up.UHID_OPEN,
            up.UHID_CLOSE, up.UHID_OUTPUT) == (1, 2, 3, 4, 5, 6)
    assert (up.UHID_GET_REPORT, up.UHID_GET_REPORT_REPLY, up.UHID_CREATE2,
            up.UHID_INPUT2, up.UHID_SET_REPORT, up.UHID_SET_REPORT_REPLY) == (9, 10, 11, 12, 13, 14)


class FakeKernel:
    """Stands in for /dev/uhid: hands pump() one event, keeps what it writes."""

    def __init__(self, event, body):
        self.event = struct.pack("<I", event) + body.ljust(up.EVENT_SIZE - 4, b"\0")
        self.written = []

    def read(self, fd, size):
        return self.event


def pump(event, body):
    kernel = FakeKernel(event, body)
    hid = up.Uhid.__new__(up.Uhid)
    hid.os, hid.fd = kernel, None
    hid.send = lambda ev, reply: kernel.written.append((ev, reply))
    sent = []
    deck = up.Deck(lambda report, vt: None)
    deck.send = lambda sock, report: sent.append(report)
    hid.pump(deck, None)
    return kernel.written, sent


def test_output_report_keeps_its_trailing_zeros():
    report = bytes.fromhex("01 02 00 00")
    body = report.ljust(up.UHID_DATA_SIZE, b"\0") + struct.pack("<HB", len(report), 1)
    assert pump(up.UHID_OUTPUT, body) == ([], [report])


def test_report_requests_are_answered_with_their_own_id():
    written, _ = pump(up.UHID_GET_REPORT, struct.pack("<IBB", 0x1234, 0, 0))
    assert written == [(up.UHID_GET_REPORT_REPLY, struct.pack("<IHH", 0x1234, 0, 0))]
    written, _ = pump(up.UHID_SET_REPORT, struct.pack("<IBBH", 0x99, 0, 0, 0))
    assert written == [(up.UHID_SET_REPORT_REPLY, struct.pack("<IH", 0x99, 0))]


# -- monitor -------------------------------------------------------------------

def test_describe_names_the_handshake():
    # the deck's announce and ack as the firmware sends them (type at byte 6)
    assert up.describe(bytes.fromhex("00 F0 01 00 01 00 11 06 01 04 01 57")) == "  (announce)"
    assert up.describe(bytes.fromhex("00 F0 01 00 01 00 15 02")) == "  (ack)"
    assert up.describe(bytes.fromhex("00 20 00 00 00 80 60")) == ""
    assert up.describe(bytes.fromhex("00 11 00 00")) == ""
    assert up.describe(bytes(64)) == ""
