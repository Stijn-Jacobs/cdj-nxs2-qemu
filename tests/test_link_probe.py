# SPDX-License-Identifier: GPL-2.0-or-later
"""link_probe: the Link wire format against the bytes the Link mods' own tests
build, the pcap scoring on a synthetic capture (the file capture_link.py
writes), and the measurement loop over a real socket against a fake peer."""
import argparse
import socket
import struct
import threading

import link_probe
from test_main_abletonlink import GHOST_OFFSET, IP, NODE_ID, alive_message, entry, ping, pong

DECK = '192.168.1.20'
APP = '192.168.1.5'
PLAYER = NODE_ID[7]
US_PER_BEAT = 468750                    # 128 BPM, no pitch


def args(**changes):
    base = dict(bpm=None, pitch=0.0, bpm_tol=0.1, rate_tol=0.001, measurements=1, expect_none=False)
    return argparse.Namespace(**{**base, **changes})


# -- wire format -----------------------------------------------------------------

def test_the_alive_the_mod_builds_decodes_to_its_fields():
    data = alive_message(US_PER_BEAT, 2000000, GHOST_OFFSET + 1000)
    message = link_probe.decode_discovery(data)
    assert (message.kind, message.ttl, message.group, message.node) == (link_probe.ALIVE, 10, 0, NODE_ID)
    assert message.entries[b'sess'] == NODE_ID
    assert link_probe.timeline(message) == (US_PER_BEAT, 2000000, GHOST_OFFSET + 1000)
    assert link_probe.start_stop(message) == (False, 0, 0)
    assert link_probe.endpoint(message) == (DECK, 50000)
    assert len(data) == link_probe.ALIVE_SIZE


def test_a_ping_encodes_to_the_bytes_the_mod_tests_answer():
    assert link_probe.encode_ping(123456789, 9876543210) == ping(123456789, 9876543210)
    assert link_probe.encode_ping(55) == ping(55)


def test_the_pong_the_mod_builds_decodes_to_the_pings_payload_and_a_ghost_time():
    message = link_probe.decode_measurement(pong(ping(123456789, 9876543210), 777))
    assert message.kind == link_probe.PONG
    assert message.entries[b'sess'] == NODE_ID
    assert struct.unpack('>q', message.entries[b'__gt'])[0] == 777
    assert struct.unpack('>q', message.entries[b'__ht'])[0] == 123456789
    assert struct.unpack('>q', message.entries[b'_pgt'])[0] == 9876543210


def test_entries_are_read_in_any_order_and_unknown_keys_are_skipped():
    payload = entry(b'xxxx', b'\x01\x02') + entry(b'sess', NODE_ID)
    assert link_probe.decode_entries(payload) == {b'xxxx': b'\x01\x02', b'sess': NODE_ID}


def test_truncated_and_foreign_datagrams_are_refused():
    data = alive_message(US_PER_BEAT, 0, 0)
    for bad in (data[:-1], data[:10], b'_link_v\x01\x01', b'hello'):
        try:
            link_probe.decode_discovery(bad)
        except ValueError:
            continue
        raise AssertionError(bad)
    assert link_probe.describe(b'hello') is None


def test_describe_names_the_tempo_in_bpm_and_the_node():
    text = link_probe.describe(alive_message(US_PER_BEAT, 2000000, 5))
    assert 'ALIVE ttl=10' in text and 'bpm=128.000' in text
    assert 'node=192.168.1.20/NXS2' in text and 'mep4=192.168.1.20:50000' in text
    assert link_probe.describe(pong(ping(9), 10)).startswith('PONG session=192.168.1.20/NXS2')


# -- frames ----------------------------------------------------------------------

def test_a_datagram_wrapped_for_the_segment_unwraps_with_the_shared_parser():
    mac = bytes.fromhex('020000000001')
    frame = link_probe.udp_frame(link_probe.PROBE_MAC, mac, APP, DECK, 41809, 50000, b'payload')
    got = link_probe.parse(frame)
    assert (got['kind'], got['dport'], got['sport'], got['payload']) == ('udp', 50000, 41809, b'payload')
    assert got['dst'] == mac and got['src'] == link_probe.PROBE_MAC
    assert link_probe.checksum(frame[14:34]) == 0


def test_an_arp_request_for_our_address_is_answered_and_others_are_not():
    request = (b'\xff' * 6 + bytes.fromhex('020000000001') + b'\x08\x06'
               + struct.pack('>HHBBH', 1, 0x0800, 6, 4, 1) + bytes.fromhex('020000000001')
               + socket.inet_aton(DECK) + bytes(6) + socket.inet_aton(APP))
    reply = link_probe.arp_reply(request, APP)
    assert reply[:6] == bytes.fromhex('020000000001') and reply[6:12] == link_probe.PROBE_MAC
    assert reply[20:22] == b'\x00\x02' and reply[22:28] == link_probe.PROBE_MAC
    assert link_probe.arp_reply(request, '192.168.1.99') is None


# -- pcap scoring ----------------------------------------------------------------

def beat_packet(bpm_x100, pitch, in_bar):
    packet = bytearray(0x60)
    packet[:10] = link_probe.MAGIC
    packet[10] = link_probe.BEAT_TYPE
    struct.pack_into('>I', packet, 0x54, pitch)
    struct.pack_into('>H', packet, 0x5A, bpm_x100)
    packet[0x5C], packet[0x5F] = in_bar, PLAYER
    return bytes(packet)


def deck_frame(dip, sport, dport, payload):
    return link_probe.udp_frame(bytes.fromhex('020000000001'), b'\xff' * 6, DECK, dip, sport, dport, payload)


def app_frame(sport, dport, payload):
    return link_probe.udp_frame(bytes.fromhex('020000000009'), bytes.fromhex('020000000001'),
                                APP, DECK, sport, dport, payload)


def capture(beats=60, pings=51, pong_delay=0.002, pitch=0x100000):
    """A deck playing 128 BPM for `beats` beats with one ALIVE behind each
    beat packet, and an app measuring it with `pings` round trips."""
    frames = []
    for k in range(beats):
        stamp = 100 + k * US_PER_BEAT / 1e6
        origin = (k + 2) * 1000000
        frames.append((stamp, deck_frame('255.255.255.255', 1, 50001, beat_packet(12800, pitch, (k + 2) % 4 + 1))))
        frames.append((stamp + 0.001, deck_frame('192.168.1.255', 1, link_probe.LINK_PORT,
                                                 alive_message(US_PER_BEAT, origin, GHOST_OFFSET + k * US_PER_BEAT))))
    for k in range(pings):
        stamp = 110 + k * 0.01
        message = ping(1000 + k, 500 + k if k else None)
        frames.append((stamp, app_frame(54321, 50000, message)))
        frames.append((stamp + pong_delay, deck_frame(APP, 50000, 54321, pong(message, GHOST_OFFSET + 5000 + k * 100))))
    return sorted(frames, key=lambda f: f[0])


def write_pcap(path, frames):
    with open(path, 'wb') as fh:
        fh.write(struct.pack('<IHHiIII', 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        for stamp, frame in frames:
            fh.write(struct.pack('<IIII', int(stamp), int(stamp % 1 * 1e6), len(frame), len(frame)))
            fh.write(frame)


def outcome(results):
    return {name: ok for ok, name, _ in results}


def test_a_capture_of_a_healthy_deck_passes_every_criterion(tmp_path):
    path = str(tmp_path / 'good.pcap')
    write_pcap(path, capture())
    frames = list(link_probe.read_pcap(path))
    assert len(frames) == 120 + 102
    results = link_probe.score(frames, args())
    assert all(outcome(results).values()), [r for r in results if not r[0]]
    assert 'pong_answered' in outcome(results) and 'measurements_complete' in outcome(results)


def test_the_bpm_cross_check_uses_the_pitch():
    frames = capture(pitch=0x10A3D7)
    assert not outcome(link_probe.score(frames, args()))['alive_fields']      # the capture's tempo is at 0 %
    frames = capture()
    assert outcome(link_probe.score(frames, args(bpm=128, pitch=0)))['alive_bpm']
    assert not outcome(link_probe.score(frames, args(bpm=128, pitch=4)))['alive_bpm']


def test_a_pong_after_50_ms_fails():
    results = outcome(link_probe.score(capture(pong_delay=0.08), args()))
    assert not results['pong_in_50ms'] and results['pong_answered']


def test_a_missing_alive_and_an_unanswered_ping_fail():
    frames = capture()
    alives = [f for f in frames if f[1][36:38] == struct.pack('>H', link_probe.LINK_PORT)]
    pongs = [f for f in frames if f[1][34:36] == struct.pack('>H', 50000)]
    results = outcome(link_probe.score([f for f in frames if f is not alives[10] and f is not alives[11]
                                        and f is not pongs[-1]], args()))
    assert not results['alive_per_beat'] and not results['pong_answered']
    assert not results['last_ping_answered']


def test_a_ghost_clock_that_runs_slow_fails():
    frames = []
    for stamp, frame in capture():
        got = link_probe.parse(frame)
        if got['dport'] == link_probe.LINK_PORT:
            origin = struct.unpack_from('>q', got['payload'], 36)[0]
            slow = GHOST_OFFSET + int((origin / 1e6 - 2) * US_PER_BEAT * 0.99)
            payload = got['payload'][:44] + struct.pack('>q', slow) + got['payload'][52:]
            frame = deck_frame('192.168.1.255', 1, link_probe.LINK_PORT, payload)
        frames.append((stamp, frame))
    assert not outcome(link_probe.score(frames, args()))['ghost_rate']


def test_a_control_capture_has_no_link_traffic():
    quiet = [f for f in capture() if f[1][36:38] != struct.pack('>H', link_probe.LINK_PORT)
             and f[1][34:36] != struct.pack('>H', 50000) and f[1][36:38] != struct.pack('>H', 50000)]
    assert link_probe.score(quiet, args(expect_none=True))[0][0]
    assert not link_probe.score(capture(), args(expect_none=True))[0][0]


# -- the measurement loop over a socket ------------------------------------------

def fake_peer(replies):
    """A UDP socket that answers `replies` PINGs with the mod's PONG bytes and
    then goes quiet; returns (socket address, thread)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', 0))
    sock.settimeout(2)
    ghost = [GHOST_OFFSET]

    def serve():
        with sock:
            for _ in range(replies):
                try:
                    data, src = sock.recvfrom(512)
                except socket.timeout:
                    return
                ghost[0] += 1000
                sock.sendto(pong(data, ghost[0]), src)

    thread = threading.Thread(target=serve)
    thread.start()
    return sock.getsockname(), thread


def test_a_measurement_against_a_peer_completes_with_the_ghost_times():
    address, thread = fake_peer(51)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        samples, reason = link_probe.measure(sock, address, 51)
    thread.join()
    assert reason is None and len(samples) == 51
    assert [s.ghost for s in samples[:2]] == [GHOST_OFFSET + 1000, GHOST_OFFSET + 2000]
    assert all(s.sent <= s.received and s.session == NODE_ID for s in samples)


def test_a_peer_that_stops_answering_fails_the_measurement_after_five_timeouts():
    address, thread = fake_peer(3)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        samples, reason = link_probe.measure(sock, address, 51)
    thread.join()
    assert len(samples) == 3 and '5 PINGs' in reason
