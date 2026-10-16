# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/net/usb_hid_port.py: the datagram side and the UHID event bodies."""
import socket

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


# -- monitor -------------------------------------------------------------------

def test_describe_names_the_handshake():
    # the deck's announce and ack as the firmware sends them (type at byte 6)
    assert up.describe(bytes.fromhex("00 F0 01 00 01 00 11 06 01 04 01 57")) == "  (announce)"
    assert up.describe(bytes.fromhex("00 F0 01 00 01 00 15 02")) == "  (ack)"
    assert up.describe(bytes.fromhex("00 20 00 00 00 80 60")) == ""
    assert up.describe(bytes.fromhex("00 11 00 00")) == ""
    assert up.describe(bytes(64)) == ""
