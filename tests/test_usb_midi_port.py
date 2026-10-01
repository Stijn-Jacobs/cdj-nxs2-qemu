# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/net/usb_midi_port.py: USB-MIDI event packets <-> MIDI messages, and
the datagram loop on a loopback socket."""
import io
import socket

import pytest

import usb_midi_port as up


# -- decode ----------------------------------------------------------------------

def test_decode_single_byte_realtime():
    assert up.decode_usb_midi(bytes.fromhex("0F F8 00 00")) == [(0, b"\xf8")]


def test_decode_note_on_off_and_cc():
    payload = bytes.fromhex("09 90 3C 7F  08 80 3C 00  0B B0 07 64")
    assert up.decode_usb_midi(payload) == [
        (0, b"\x90\x3c\x7f"), (0, b"\x80\x3c\x00"), (0, b"\xb0\x07\x64")]


def test_decode_two_byte_messages():
    payload = bytes.fromhex("0C C1 05 00  0D D2 40 00  02 F1 21 00")
    assert up.decode_usb_midi(payload) == [
        (0, b"\xc1\x05"), (0, b"\xd2\x40"), (0, b"\xf1\x21")]


def test_decode_sysex_split_over_packets():
    payload = bytes.fromhex("04 F0 7E 00  04 09 01 02  06 03 F7 00")
    assert up.decode_usb_midi(payload) == [(0, bytes.fromhex("F0 7E 00 09 01 02 03 F7"))]


@pytest.mark.parametrize("end, message", [("05 F7 00 00", "F0 01 02 F7"),
                                          ("07 03 04 F7", "F0 01 02 03 04 F7")])
def test_decode_sysex_end_lengths(end, message):
    payload = bytes.fromhex("04 F0 01 02  " + end)
    assert up.decode_usb_midi(payload) == [(0, bytes.fromhex(message))]


def test_decode_sysex_continues_across_transfers():
    pending = {}
    assert up.decode_usb_midi(bytes.fromhex("04 F0 01 02"), pending) == []
    assert up.decode_usb_midi(bytes.fromhex("05 F7 00 00"), pending) == [
        (0, bytes.fromhex("F0 01 02 F7"))]


def test_decode_skips_padding():
    payload = bytes.fromhex("00 00 00 00  0F FA 00 00  00 00 00 00")
    assert up.decode_usb_midi(payload) == [(0, b"\xfa")]


def test_decode_two_cables():
    payload = bytes.fromhex("09 90 3C 7F  19 91 40 20  14 F0 01 02  24 F0 05 06")
    assert up.decode_usb_midi(payload) == [(0, b"\x90\x3c\x7f"), (1, b"\x91\x40\x20")]
    pending = {}
    up.decode_usb_midi(bytes.fromhex("14 F0 01 02  24 F0 05 06"), pending)
    assert sorted(pending) == [1, 2]


# -- encode ----------------------------------------------------------------------

@pytest.mark.parametrize("message", [
    "F8", "FA", "FC", "90 3C 7F", "80 3C 00", "B0 07 64", "C1 05", "D2 40",
    "E0 00 40", "F1 21", "F2 10 20", "F3 05", "F6",
    "F0 F7", "F0 01 F7", "F0 01 02 F7", "F0 01 02 03 F7", "F0 7E 00 09 01 02 03 04 05 F7",
])
def test_encode_round_trip(message):
    raw = bytes.fromhex(message)
    packets = up.encode_usb_midi(raw, cable=2)
    assert len(packets) % 4 == 0
    assert up.decode_usb_midi(packets) == [(2, raw)]


def test_encode_packet_layout():
    assert up.encode_usb_midi(b"\x90\x3c\x7f") == bytes.fromhex("09 90 3C 7F")
    assert up.encode_usb_midi(b"\xf8") == bytes.fromhex("0F F8 00 00")
    assert up.encode_usb_midi(bytes.fromhex("F0 01 02 03 F7")) == bytes.fromhex(
        "04 F0 01 02  06 03 F7 00")


def stamped(endpoint, vt_ns, packets):
    return bytes([endpoint]) + vt_ns.to_bytes(8, "little") + bytes.fromhex(packets)


# -- the datagram loop -----------------------------------------------------------

def test_serve_counts_midi_in_and_follows_the_status_line():
    rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rx.bind(("127.0.0.1", 0))
    tx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    counted = io.StringIO()
    counter = up.Counter(counted)
    deck = up.Deck(counter.add)
    try:
        tx.sendto(b"\x00configured vid=2b73 pid=0005 midi_in=86 midi_out=05", rx.getsockname())
        tx.sendto(stamped(0x86, 5_000_000, "0F F8 00 00  0F F8 00 00  0F FA 00 00  09 90 3C 7F"),
                  rx.getsockname())
        tx.sendto(stamped(0x85, 6_000_000, "0F FC 00 00"), rx.getsockname())
        up.serve(rx, deck, stop=lambda: deck.midi_in == 0x86 and counter.seen.get(0x90))
        assert counter.seen == {0xF8: 2, 0xFA: 1, 0x90: 1}
        assert deck.midi_out == 0x05

        deck.send(rx, b"\xf8")
        assert tx.recvfrom(64)[0] == bytes.fromhex("05 0F F8 00 00")
    finally:
        rx.close()
        tx.close()


def test_deck_hands_over_the_virtual_time_stamp():
    seen = []
    deck = up.Deck(lambda cable, message, vt: seen.append((message, vt)))
    deck.datagram(stamped(0x85, 1_234_567_890, "0F F8 00 00  09 90 3C 7F"), ("127.0.0.1", 1))
    assert seen == [(b"\xf8", 1_234_567_890), (b"\x90\x3c\x7f", 1_234_567_890)]


# -- replay at virtual time ------------------------------------------------------

class Wall:
    def __init__(self):
        self.t = 100.0

    def __call__(self):
        return self.t


def new_replay(latency=0.05):
    wall, sent = Wall(), []
    return wall, sent, up.Replay(lambda m: sent.append((wall.t, m)), latency, now=wall)


def run_until(wall, replay, end, tick=0.0005):
    while wall.t < end:
        replay.step()
        wall.t += tick


def test_replay_evens_out_bursty_arrivals():
    wall, sent, replay = new_replay()
    start = wall.t
    # a clock every 1/82 s of virtual time, handed over in lumps 10-40 ms apart
    delivered, next_lump, lumps = 0, 0.0, 0
    while wall.t < start + 8:
        elapsed = wall.t - start
        if elapsed >= next_lump:
            while delivered / 82 <= elapsed:
                replay.add(int(delivered / 82 * 1e9), b"\xf8")
                delivered += 1
            next_lump = elapsed + (0.01, 0.04, 0.025)[lumps % 3]
            lumps += 1
        replay.step()
        wall.t += 0.0005
    gaps = [b - a for (a, _), (b, _) in zip(sent, sent[1:])]
    assert len(sent) > 600
    assert max(gaps) - min(gaps) < 0.001
    assert sum(gaps) / len(gaps) == pytest.approx(1 / 82, rel=0.01)


def test_replay_keeps_start_and_stop_in_order_with_the_clocks():
    wall, sent, replay = new_replay()
    for i, message in enumerate([b"\xfa", b"\xf8", b"\xf8", b"\xfc"]):
        replay.add(i * 12_000_000, message)
    run_until(wall, replay, wall.t + 0.2)
    assert [m for _, m in sent] == [b"\xfa", b"\xf8", b"\xf8", b"\xfc"]
    assert [round(t - sent[0][0], 3) for t, _ in sent] == [0, 0.012, 0.024, 0.036]


def test_replay_pins_again_after_a_stall():
    wall, sent, replay = new_replay()
    replay.add(0, b"\xf8")
    wall.t += 0.2
    replay.add(12_000_000, b"\xf8")
    replay.add(24_000_000, b"\xf8")
    run_until(wall, replay, wall.t + 0.2)
    assert [round(t - sent[1][0], 3) for t, _ in sent[1:]] == [0, 0.012]
    assert sent[1][0] == pytest.approx(100.2 + 0.05, abs=0.001)


def test_replay_pins_again_when_the_emulator_runs_fast():
    wall, sent, replay = new_replay()
    replay.add(0, b"\xf8")
    replay.add(1_000_000_000, b"\xf8")
    run_until(wall, replay, wall.t + 0.3)
    assert sent[1][0] - sent[0][0] < 0.2
