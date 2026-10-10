#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Observe, measure and score the Ableton Link traffic of an emulated deck.

Only the wire format is reimplemented, from Ableton's Link headers
(github.com/Ableton/link, include/ableton/): discovery/v1/Messages.hpp (the
`_asdp_v` header, ALIVE / RESPONSE / BYEBYE, the TLV payload),
link/PeerState.hpp (the `mep4` endpoint), link/Timeline.hpp (`tmln`),
link/v1/Messages.hpp and link/Measurement.hpp (`_link_v`, PING / PONG, the
50 ms reply window, the session check), link/PingResponder.hpp (the PONG is the
PING's payload behind `sess` and `__gt`). All integers are big-endian.

The deck does not send on the host's network. Its frames travel inside QEMU's
`-netdev socket,mcast=GROUP:PORT` segment as raw Ethernet frames (see
capture_link.py), and its ALIVE goes to the subnet broadcast address on UDP
20808, never to 224.76.78.75. So every mode that talks to the deck does it on
that segment with --segment, the way dhcp_server.py does.

  usage: link_probe.py --listen [--segment G:P] [--seconds S]
         link_probe.py --ping DECK_IP:PORT [--segment G:P] [-n ROUND_TRIPS] [--measurements M]
         link_probe.py --score capture.pcap|SECONDS [--segment G:P] [--measurements M]
                       [--bpm B --pitch PERCENT] [--rate-tol T] [--expect-none]

--listen prints every Link message it sees. --ping runs Link's own measurement
loop against a peer's measurement endpoint (PING, wait for the PONG, repeat)
and reports round trips and the (host time, ghost time) pairs. --score reads a
pcap written by capture_link.py, or captures the segment for SECONDS, and
prints one PASS / FAIL / INFO line per criterion; it exits 1 on any FAIL.
"""
import argparse
import bisect
import socket
import statistics
import struct
import sys
import time
from collections import namedtuple

from dhcp_server import checksum
from score_link import MAGIC, parse

LINK_GROUP = '224.76.78.75'
LINK_PORT = 20808
DISCOVERY_HEADER = b'_asdp_v\x01'
MEASURE_HEADER = b'_link_v\x01'
ALIVE, RESPONSE, BYEBYE = 1, 2, 3
PING, PONG = 1, 2
KINDS = {ALIVE: 'ALIVE', RESPONSE: 'RESPONSE', BYEBYE: 'BYEBYE'}
MEASURE_KINDS = {PING: 'PING', PONG: 'PONG'}

PONG_WINDOW = 0.050
MEASUREMENT_ROUND_TRIPS = 51           # Measurement.hpp: done at more than 100 data points, two per round trip
MAX_TIMEOUTS = 5
ALIVE_SIZE = 107                       # tmln + sess + stst + mep4 behind the 20-byte header
BEAT_SIZE = 0x60
BEAT_TYPE = 0x28

# Outside the decks' 02:00:00:00:00:0N range and the DHCP server's MAC.
PROBE_MAC = bytes.fromhex('024c4e4b0001')

Discovery = namedtuple('Discovery', 'kind ttl group node entries')
Measurement = namedtuple('Measurement', 'kind entries')
Sample = namedtuple('Sample', 'sent received ghost session')


def entry(key, value):
    return key + struct.pack('>I', len(value)) + value


def decode_entries(payload):
    found = {}
    at = 0
    while at < len(payload):
        if at + 8 > len(payload):
            raise ValueError('truncated entry header')
        key = payload[at:at + 4]
        size = struct.unpack_from('>I', payload, at + 4)[0]
        if at + 8 + size > len(payload):
            raise ValueError('entry %r runs past the datagram' % key)
        found.setdefault(key, payload[at + 8:at + 8 + size])
        at += 8 + size
    return found


def decode_discovery(data):
    if data[:8] != DISCOVERY_HEADER or len(data) < 20:
        raise ValueError('not a Link discovery message')
    kind, ttl, group = struct.unpack_from('>BBH', data, 8)
    return Discovery(kind, ttl, group, data[12:20], decode_entries(data[20:]))


def decode_measurement(data):
    if data[:8] != MEASURE_HEADER or len(data) < 9:
        raise ValueError('not a Link measurement message')
    return Measurement(data[8], decode_entries(data[9:]))


def timeline(message):
    """(microseconds per beat, beat origin in micro-beats, time origin in
    ghost microseconds)."""
    return struct.unpack('>qqq', message.entries[b'tmln'])


def endpoint(message):
    ip, port = struct.unpack('>IH', message.entries[b'mep4'])
    return socket.inet_ntoa(struct.pack('>I', ip)), port


def start_stop(message):
    return struct.unpack('>?qq', message.entries[b'stst'])


def node_name(node):
    if node[4:7] == b'NXS':
        return '%s/NXS%d' % (socket.inet_ntoa(node[:4]), node[7])
    return node.hex()


def encode_ping(host_time, previous_ghost=None):
    message = MEASURE_HEADER + bytes([PING]) + entry(b'__ht', struct.pack('>q', host_time))
    if previous_ghost is not None:
        message += entry(b'_pgt', struct.pack('>q', previous_ghost))
    return message


def describe(data):
    """One line for a Link datagram, or None when it is not one."""
    try:
        message = decode_discovery(data)
    except ValueError:
        pass
    else:
        text = '%s ttl=%d group=%d node=%s' % (KINDS.get(message.kind, message.kind),
                                               message.ttl, message.group, node_name(message.node))
        if b'tmln' in message.entries:
            us_per_beat, origin, time_origin = timeline(message)
            text += ' bpm=%.3f beat_origin=%.3f time_origin=%d' % (
                60e6 / us_per_beat, origin / 1e6, time_origin)
        if b'sess' in message.entries:
            text += ' session=%s' % node_name(message.entries[b'sess'])
        if b'stst' in message.entries:
            playing, beats, stamp = start_stop(message)
            text += ' playing=%d beats=%.3f stamp=%d' % (playing, beats / 1e6, stamp)
        if b'mep4' in message.entries:
            text += ' mep4=%s:%d' % endpoint(message)
        return text
    try:
        message = decode_measurement(data)
    except ValueError:
        return None
    text = MEASURE_KINDS.get(message.kind, str(message.kind))
    for key, name in ((b'sess', 'session'), (b'__ht', 'ht'), (b'_pgt', 'pgt'), (b'__gt', 'gt')):
        if key in message.entries:
            value = message.entries[key]
            text += ' %s=%s' % (name, node_name(value) if key == b'sess'
                                else struct.unpack('>q', value)[0])
    return text


# -- raw segment -----------------------------------------------------------------

def read_pcap(path):
    """(timestamp, frame) from a classic pcap, the format capture_link.py writes."""
    with open(path, 'rb') as fh:
        head = fh.read(24)
        if head[:4] == b'\xd4\xc3\xb2\xa1':
            endian = '<'
        elif head[:4] == b'\xa1\xb2\xc3\xd4':
            endian = '>'
        else:
            raise ValueError('%s: not a pcap' % path)
        while True:
            record = fh.read(16)
            if len(record) < 16:
                return
            sec, usec, caplen, _ = struct.unpack(endian + 'IIII', record)
            frame = fh.read(caplen)
            if len(frame) < caplen:
                return
            yield sec + usec / 1e6, frame


def join_group(group, port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # macOS: the decks' QEMUs bind this port with SO_REUSEPORT, and BSD
    # shares a port only when every socket on it sets that.
    if sys.platform == 'darwin':
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind(('', port))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                    struct.pack('4s4s', socket.inet_aton(group), socket.inet_aton('0.0.0.0')))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    return sock


def split_group(text):
    group, _, port = text.partition(':')
    return group, int(port)


def segment_frames(group, seconds):
    """(timestamp, frame) from the segment for `seconds`, or until interrupted
    when it is 0."""
    sock = join_group(*split_group(group))
    sock.settimeout(0.5)
    end = time.time() + seconds
    try:
        while not seconds or time.time() < end:
            try:
                frame, _ = sock.recvfrom(65535)
            except socket.timeout:
                continue
            yield time.time(), frame
    except KeyboardInterrupt:
        pass


def udp_frame(src_mac, dst_mac, src_ip, dst_ip, sport, dport, payload):
    src, dst = socket.inet_aton(src_ip), socket.inet_aton(dst_ip)
    udp_len = 8 + len(payload)
    udp = struct.pack('>HHHH', sport, dport, udp_len, 0) + payload
    pseudo = src + dst + bytes([0, 17]) + struct.pack('>H', udp_len)
    udp = udp[:6] + struct.pack('>H', checksum(pseudo + udp) or 0xFFFF) + udp[8:]
    ip = bytearray(struct.pack('>BBHHHBBH', 0x45, 0, 20 + udp_len, 0, 0, 64, 17, 0))
    ip += src + dst
    ip[10:12] = struct.pack('>H', checksum(bytes(ip)))
    return dst_mac + src_mac + b'\x08\x00' + bytes(ip) + udp


def arp_reply(request, our_ip, our_mac=PROBE_MAC):
    """The reply to an ARP who-has for our_ip, or None."""
    if (len(request) < 42 or request[12:14] != b'\x08\x06' or request[20:22] != b'\x00\x01'
            or request[38:42] != socket.inet_aton(our_ip)):
        return None
    body = struct.pack('>HHBBH', 1, 0x0800, 6, 4, 2) + our_mac + socket.inet_aton(our_ip) \
        + request[22:28] + request[28:32]
    return request[6:12] + our_mac + b'\x08\x06' + body


class SegmentSocket:
    """The bit of a UDP socket the measurement loop uses, on the raw segment:
    sendto wraps the datagram in Ethernet/IPv4/UDP to the peer's MAC (learned
    from any frame it sent), recvfrom unwraps what comes back and answers ARP
    for our own address, since the deck's stack needs it to reply."""

    def __init__(self, group, our_ip, port, macs):
        self.dest = split_group(group)
        self.sock = join_group(*self.dest)
        self.ip, self.port, self.macs = our_ip, port, macs
        self.timeout = None

    def settimeout(self, seconds):
        self.timeout = seconds

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def next_frame(self, deadline):
        """Read one frame: answer ARP, learn the sender's MAC, and return
        (payload, source) when it is a datagram for our port."""
        left = deadline - time.time()
        if left <= 0:
            raise socket.timeout
        self.sock.settimeout(left)
        frame, _ = self.sock.recvfrom(65535)
        if frame[6:12] == PROBE_MAC:
            return None
        reply = arp_reply(frame, self.ip)
        if reply:
            self.sock.sendto(reply, self.dest)
            return None
        got = parse(frame)
        if not got or got['kind'] != 'udp':
            return None
        self.macs[socket.inet_ntoa(got['sip'])] = got['src']
        if socket.inet_ntoa(got['dip']) == self.ip and got['dport'] == self.port:
            return got['payload'], (socket.inet_ntoa(got['sip']), got['sport'])
        return None

    def sendto(self, payload, peer):
        deadline = time.time() + 5
        while peer[0] not in self.macs:
            self.next_frame(deadline)
        frame = udp_frame(PROBE_MAC, self.macs[peer[0]], self.ip, peer[0], self.port, peer[1], payload)
        self.sock.sendto(frame, self.dest)

    def recvfrom(self, _size):
        deadline = time.time() + (self.timeout or 3600)
        while True:
            got = self.next_frame(deadline)
            if got:
                return got


# -- measurement -----------------------------------------------------------------

def host_us():
    return time.monotonic_ns() // 1000


def measure(sock, peer, round_trips):
    """Link's measurement loop: PING, wait up to 50 ms for the PONG, repeat.
    Returns (samples, reason); reason is None when `round_trips` completed."""
    samples, session, previous = [], None, None
    timeouts = 0
    sock.settimeout(PONG_WINDOW)
    while len(samples) < round_trips:
        sent = host_us()
        sock.sendto(encode_ping(sent, previous), peer)
        deadline = time.monotonic() + PONG_WINDOW
        reply = None
        while reply is None:
            sock.settimeout(max(deadline - time.monotonic(), 0.001))
            try:
                data, _ = sock.recvfrom(512)
            except (socket.timeout, ConnectionResetError):      # Windows reports a closed peer port this way
                break
            received = host_us()
            try:
                message = decode_measurement(data)
            except ValueError:
                continue
            if message.kind == PONG and message.entries.get(b'__ht') == struct.pack('>q', sent):
                reply = message
        if reply is None:
            timeouts += 1
            if timeouts >= MAX_TIMEOUTS:
                return samples, '%d PINGs without a PONG in 50 ms' % timeouts
            continue
        if session is None:
            session = reply.entries[b'sess']
        elif reply.entries[b'sess'] != session:
            return samples, 'session id changed'
        previous = struct.unpack('>q', reply.entries[b'__gt'])[0]
        samples.append(Sample(sent, received, previous, session))
    return samples, None


def report_measurement(number, samples, reason):
    if not samples:
        print('measurement %d: FAILED, no PONG (%s)' % (number, reason))
        return
    trips = [(s.received - s.sent) / 1000 for s in samples]
    offsets = [s.ghost - (s.sent + s.received) // 2 for s in samples]
    print('measurement %d: %s, %d round trips, rtt ms min %.2f median %.2f max %.2f, '
          'ghost - host median %d us' % (
              number, 'complete' if reason is None else 'FAILED (%s)' % reason, len(samples),
              min(trips), statistics.median(trips), max(trips), statistics.median(offsets)))
    if len(samples) > 1:
        slope = statistics.linear_regression([s.sent for s in samples],
                                             [s.ghost for s in samples]).slope
        print('  ghost us per host us: %.6f' % slope)
    for s in samples[:3] + samples[-1:]:
        print('  host %d..%d us -> ghost %d us' % (s.sent, s.received, s.ghost))


def ping_peer(args):
    host, _, port = args.ping.rpartition(':')
    peer = (host, int(port))
    macs = {}
    failed = 0
    for number in range(1, args.measurements + 1):
        if args.segment:
            ours = args.our_ip or host.rsplit('.', 1)[0] + '.254'
            sock = SegmentSocket(args.segment, ours, 41808 + number, macs)
        else:
            sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        with sock:
            samples, reason = measure(sock, peer, args.n)
        report_measurement(number, samples, reason)
        failed += reason is not None
    print('%d of %d measurements complete' % (args.measurements - failed, args.measurements))
    return 1 if failed else 0


# -- listen ----------------------------------------------------------------------

def listen(args):
    if args.segment:
        for stamp, frame in segment_frames(args.segment, args.seconds):
            got = parse(frame)
            if got and got['kind'] == 'udp':
                text = describe(got['payload'])
                if text:
                    print('%.3f %s:%d -> %s:%d %s' % (
                        stamp, socket.inet_ntoa(got['sip']), got['sport'],
                        socket.inet_ntoa(got['dip']), got['dport'], text), flush=True)
        return 0
    sock = join_group(LINK_GROUP, LINK_PORT)
    sock.settimeout(0.5)
    end = time.time() + args.seconds
    try:
        while not args.seconds or time.time() < end:
            try:
                data, src = sock.recvfrom(1500)
            except socket.timeout:
                continue
            text = describe(data)
            if text:
                print('%.3f %s:%d %s' % (time.time(), src[0], src[1], text), flush=True)
    except KeyboardInterrupt:
        pass
    return 0


# -- score -----------------------------------------------------------------------

Packet = namedtuple('Packet', 'stamp sip sport dip dport payload')


def packets(frames):
    for stamp, frame in frames:
        got = parse(frame)
        if got and got['kind'] == 'udp':
            yield Packet(stamp, socket.inet_ntoa(got['sip']), got['sport'],
                         socket.inet_ntoa(got['dip']), got['dport'], got['payload'])


def max_gap(stamps):
    return max((b - a for a, b in zip(stamps, stamps[1:])), default=0.0)


def beat_fields(payload):
    """(bpm x 100, pitch, beat in bar, player) of a stock beat packet."""
    return (struct.unpack_from('>H', payload, 0x5A)[0], struct.unpack_from('>I', payload, 0x54)[0],
            payload[0x5C], payload[0x5F])


def check_alive(alives, beats, args):
    results = []
    sizes = {len(p.payload) for p in alives}
    results.append((sizes == {ALIVE_SIZE}, 'alive_size', 'sizes %s, want {%d}' % (sorted(sizes), ALIVE_SIZE)))
    gap = len(alives) - len(beats)
    results.append((abs(gap) <= 1, 'alive_per_beat', '%d ALIVE, %d beat packets' % (len(alives), len(beats))))

    beat_stamps = [p.stamp for p in beats]
    bad, origins, tempo_error, earlier = [], [], 0.0, None
    for p in alives:
        message = decode_discovery(p.payload)
        player = beats[0].payload[0x5F] if beats else message.node[7]
        want_node = socket.inet_aton(p.sip) + b'NXS' + bytes([player])
        us_per_beat, origin, time_origin = timeline(message)
        problems = []
        if message.kind != ALIVE or message.ttl != 10 or message.group != 0:
            problems.append('header')
        if message.node != want_node or message.entries.get(b'sess') != want_node:
            problems.append('node/sess')
        if endpoint(message) != (p.sip, 50000):
            problems.append('mep4')
        if origin % 1000000:
            problems.append('origin not whole beats')
        if origins and origin <= origins[-1]:
            problems.append('origin not increasing')
        index = bisect.bisect_right(beat_stamps, p.stamp) - 1
        if index >= 0:
            bpm_x100, pitch, in_bar, _ = beat_fields(beats[index].payload)
            want = 6e9 * 0x100000 / (bpm_x100 * pitch)
            tempo_error = max(tempo_error, abs(us_per_beat - want))
            if abs(us_per_beat - want) > 1:
                problems.append('tempo %d us, beat packet says %.1f' % (us_per_beat, want))
            if origin // 1000000 % 4 != in_bar - 1:
                problems.append('bar phase')
        origins.append(origin)
        if problems:
            bad.append('%.3f %s' % (p.stamp, ', '.join(problems)))
    results.append((not bad, 'alive_fields', '%d of %d ALIVEs wrong%s' % (
        len(bad), len(alives), ': ' + '; '.join(bad[:3]) if bad else
        ', max tempo error %.2f us against the beat packets' % tempo_error)))

    if args.bpm and alives:
        bpm = 60e6 / timeline(decode_discovery(alives[-1].payload))[0]
        want = args.bpm * (1 + args.pitch / 100)
        results.append((abs(bpm - want) <= args.bpm_tol, 'alive_bpm',
                        '%.3f BPM, want %.3f +-%.2f' % (bpm, want, args.bpm_tol)))
    return results


def check_clock(alives, args):
    """The ghost clock against the beats: between two ALIVEs the ghost time
    must advance by the beats the timeline covered times its beat length."""
    if len(alives) < 2:
        return [(False, 'ghost_rate', 'fewer than two ALIVEs')]
    messages = [decode_discovery(p.payload) for p in alives]
    lines = [timeline(m) for m in messages]
    span_beats = 0.0
    for (us_per_beat, origin, _), (_, next_origin, _) in zip(lines, lines[1:]):
        span_beats += (next_origin - origin) / 1e6 * us_per_beat
    ghost_span = lines[-1][2] - lines[0][2]
    host_span = (alives[-1].stamp - alives[0].stamp) * 1e6
    rate = ghost_span / span_beats
    return [(abs(rate - 1) <= args.rate_tol, 'ghost_rate',
             'ghost us per beat-clock us %.5f over %.1f s, want 1 +-%g' % (
                 rate, ghost_span / 1e6, args.rate_tol)),
            (True, 'ghost_vs_host', 'INFO ghost us per capture us %.4f' % (ghost_span / host_span))]


def check_pongs(pings, pongs, nodes, args):
    answered, late, wrong, used = 0, 0, [], set()
    rtts, gts, per_source = [], [], {}
    for ping in pings:
        source = (ping.sip, ping.sport)
        per_source.setdefault(source, [0, 0])[0] += 1
        for k, pong in enumerate(pongs):
            if k in used or pong.stamp < ping.stamp or (pong.dip, pong.dport) != source:
                continue
            tail = pong.payload[9:]
            if tail[32:] != ping.payload[9:]:
                continue
            used.add(k)
            answered += 1
            per_source[source][1] += 1
            rtts.append(pong.stamp - ping.stamp)
            late += rtts[-1] > PONG_WINDOW
            sess = tail[:16]
            gts.append((pong.stamp, struct.unpack('>q', tail[24:32])[0]))
            if len(pong.payload) != len(ping.payload) + 32 or sess != entry(b'sess', nodes.get(pong.sip, b'')):
                wrong.append(pong.stamp)
            break
    complete = sum(1 for _, got in per_source.values() if got >= MEASUREMENT_ROUND_TRIPS)
    results = [
        (bool(pings) and answered == len(pings), 'pong_answered', '%d of %d PINGs answered' % (answered, len(pings))),
        (not late and bool(rtts), 'pong_in_50ms', 'max round trip %.1f ms, %d late' % (max(rtts, default=0) * 1000, late)),
        (not wrong, 'pong_form', '%d PONGs with the wrong length or session' % len(wrong)),
        (all(b[1] > a[1] for a, b in zip(gts, gts[1:])) and bool(gts), 'pong_ghost_increasing',
         '%d __gt values' % len(gts)),
        (complete >= args.measurements, 'measurements_complete',
         '%d measurements with %d or more answered round trips, want %d' % (
             complete, MEASUREMENT_ROUND_TRIPS, args.measurements)),
    ]
    return results


def score(frames, args):
    everything = list(packets(frames))
    alives = [p for p in everything if p.dport == LINK_PORT and p.payload[:8] == DISCOVERY_HEADER
              and len(p.payload) > 8 and p.payload[8] == ALIVE]
    if args.expect_none:
        link = [p for p in everything if p.payload[:8] in (DISCOVERY_HEADER, MEASURE_HEADER)]
        return [(not link, 'no_link_traffic', '%d Link datagrams' % len(link))]
    if not alives:
        return [(False, 'alive_seen', 'no ALIVE on UDP %d in %d UDP datagrams' % (LINK_PORT, len(everything)))]
    deck = alives[0].sip
    alives = [p for p in alives if p.sip == deck]
    beats = [p for p in everything if p.sip == deck and p.dport == 50001 and len(p.payload) == BEAT_SIZE
             and p.payload[:10] == MAGIC and p.payload[10] == BEAT_TYPE]
    pings = [p for p in everything if p.dip == deck and p.dport == 50000 and p.payload[:9] == MEASURE_HEADER + bytes([PING])]
    pongs = [p for p in everything if p.sip == deck and p.sport == 50000 and p.payload[:9] == MEASURE_HEADER + bytes([PONG])]
    nodes = {deck: decode_discovery(alives[0].payload).node}

    results = check_alive(alives, beats, args) + check_clock(alives, args)
    pong_results = check_pongs(pings, pongs, nodes, args)
    results += pong_results
    last = max((p.stamp for p in pings + pongs), default=None)
    if last is not None:
        after = lambda group: sum(1 for p in group if p.stamp > last)
        results.append((after(alives) > 0 and after(beats) > 0, 'streams_continue',
                        '%d ALIVE, %d beat packets after the last PING/PONG' % (after(alives), after(beats))))
        results.append((bool(pongs) and pongs[-1].stamp >= pings[-1].stamp and len(pongs) >= len(pings),
                        'last_ping_answered', 'last PING %.3f, last PONG %.3f' % (pings[-1].stamp, pongs[-1].stamp)))
    for port, name in ((50000, 'announce'), (50001, 'beat'), (50002, 'status')):
        stamps = [p.stamp for p in everything if p.sip == deck and p.dport == port and p.payload[:10] == MAGIC]
        results.append((True, 'count_' + name, 'INFO %d packets, longest gap %.2f s' % (len(stamps), max_gap(stamps))))
    return results


def score_main(args):
    if args.score.replace('.', '', 1).isdigit():
        if not args.segment:
            sys.exit('--score SECONDS needs --segment GROUP:PORT')
        frames = segment_frames(args.segment, float(args.score))
    else:
        frames = read_pcap(args.score)
    results = score(frames, args)
    for ok, name, detail in results:
        print('%s %s: %s' % ('INFO' if detail.startswith('INFO') else 'PASS' if ok else 'FAIL',
                             name, detail.replace('INFO ', '', 1)))
    failed = [name for ok, name, detail in results if not ok]
    print('RESULT %s' % ('FAIL (%s)' % ', '.join(failed) if failed else 'PASS'))
    return 1 if failed else 0


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--listen', action='store_true')
    mode.add_argument('--ping', metavar='IP:PORT')
    mode.add_argument('--score', metavar='PCAP|SECONDS')
    parser.add_argument('--segment', metavar='GROUP:PORT',
                        help='the decks\' multicast segment (launcher GROUP, default 239.77.77.1:45000)')
    parser.add_argument('--our-ip', help='address to use on the segment (default: .254 of the deck\'s /24)')
    parser.add_argument('--seconds', type=float, default=0, help='--listen: stop after this long')
    parser.add_argument('-n', type=int, default=MEASUREMENT_ROUND_TRIPS, help='--ping: round trips per measurement')
    parser.add_argument('--measurements', type=int, default=1, help='--ping: how many; --score: how many must complete')
    parser.add_argument('--bpm', type=float, help='--score: the deck\'s track BPM')
    parser.add_argument('--pitch', type=float, default=0.0, help='--score: pitch in percent')
    parser.add_argument('--bpm-tol', type=float, default=0.1)
    parser.add_argument('--rate-tol', type=float, default=0.001)
    parser.add_argument('--expect-none', action='store_true', help='--score: pass only if no Link datagram is seen')
    args = parser.parse_args(argv[1:])
    if args.ping:
        return ping_peer(args)
    if args.score:
        return score_main(args)
    return listen(args)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
