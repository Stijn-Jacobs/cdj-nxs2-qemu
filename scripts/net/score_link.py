#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Score a QEMU filter-dump capture for Pro DJ Link traffic.

This is the oracle for the EtherMAC work. Screen-scoring cannot see a link
that half works -- a deck that announces but is never answered looks exactly
like a deck that never sent a byte -- whereas the wire says which of the two
happened, and says it on the first run.

  usage: python3 scripts/net/score_link.py /tmp/djlink-show1.pcap [more.pcap ...]

What it deliberately does NOT do is claim to understand the Pro DJ Link
protocol beyond its framing. The 10-byte magic and the type byte are stable
and easy to verify; the meaning of each type is not something this project has
measured yet, so types are counted and dumped, never named. Name them here
only once a capture has proved what they are.
"""
import struct
import sys

# Every Pro DJ Link packet opens with this, on all three ports.
MAGIC = bytes([0x51, 0x73, 0x70, 0x74, 0x31, 0x57, 0x6D, 0x4A, 0x4F, 0x4C])

PORTS = {50000: 'announce', 50001: 'beat', 50002: 'status'}


def read_pcap(path):
    """Yield raw link-layer frames from a classic (non-ng) pcap file.

    QEMU's filter-dump writes exactly this format, so a full pcap library
    would be a dependency for nothing.
    """
    with open(path, 'rb') as fh:
        hdr = fh.read(24)
        if len(hdr) < 24:
            raise ValueError('%s: too short to be a pcap' % path)
        magic = hdr[:4]
        if magic == b'\xd4\xc3\xb2\xa1':
            endian = '<'
        elif magic == b'\xa1\xb2\xc3\xd4':
            endian = '>'
        else:
            raise ValueError('%s: not a pcap (magic %s)' % (path, magic.hex()))
        while True:
            rec = fh.read(16)
            if len(rec) < 16:
                return
            _, _, caplen, _ = struct.unpack(endian + 'IIII', rec)
            data = fh.read(caplen)
            if len(data) < caplen:
                return
            yield data


def parse(frame):
    """Pull out the bits we can be sure of: MACs, IPs, UDP ports, payload.

    Returns None for anything that is not IPv4/UDP, which is most of what a
    quiet segment carries (ARP, and whatever the firmware's stack emits).
    """
    if len(frame) < 14:
        return None
    dst, src = frame[0:6], frame[6:12]
    ethertype = struct.unpack('>H', frame[12:14])[0]
    if ethertype != 0x0800:
        return {'kind': 'eth-0x%04x' % ethertype, 'src': src, 'dst': dst}
    ip = frame[14:]
    if len(ip) < 20:
        return None
    ihl = (ip[0] & 0x0F) * 4
    proto = ip[9]
    sip, dip = ip[12:16], ip[16:20]
    if proto != 17 or len(ip) < ihl + 8:
        return {'kind': 'ip-proto-%d' % proto, 'src': src, 'dst': dst,
                'sip': sip, 'dip': dip}
    udp = ip[ihl:]
    sport, dport = struct.unpack('>HH', udp[0:4])
    return {'kind': 'udp', 'src': src, 'dst': dst, 'sip': sip, 'dip': dip,
            'sport': sport, 'dport': dport, 'payload': udp[8:]}


def mac(b):
    return ':'.join('%02x' % x for x in b)


def ip(b):
    return '.'.join(str(x) for x in b)


def report(path):
    frames = list(read_pcap(path))
    print('=== %s: %d frame(s)' % (path, len(frames)))
    if not frames:
        print('    NOTHING ON THE WIRE -- the MAC transmitted no frame at all.')
        return

    kinds = {}
    senders = {}
    djlink = {}          # (port, type byte) -> count
    other_udp = {}
    samples = {}

    for f in frames:
        p = parse(f)
        if not p:
            continue
        kinds[p['kind']] = kinds.get(p['kind'], 0) + 1
        senders[mac(p['src'])] = senders.get(mac(p['src']), 0) + 1
        if p['kind'] != 'udp':
            continue
        pay = p['payload']
        if pay.startswith(MAGIC) and len(pay) > 10:
            # Keyed by sender too, so two decks do not fold into one row.
            key = (p['dport'], pay[10], mac(p['src']))
            djlink[key] = djlink.get(key, 0) + 1
            samples.setdefault(key, (p, pay))
        else:
            key = (p['sport'], p['dport'])
            other_udp[key] = other_udp.get(key, 0) + 1

    print('    frame kinds : %s' % ', '.join(
        '%s=%d' % kv for kv in sorted(kinds.items())))
    print('    source MACs : %s' % ', '.join(
        '%s=%d' % kv for kv in sorted(senders.items())))

    if not djlink:
        print('    NO PRO DJ LINK PACKET. UDP seen: %s' % (', '.join(
            '%d->%d=%d' % (k[0], k[1], v)
            for k, v in sorted(other_udp.items())) or 'none'))
        return

    players = {}
    print('    PRO DJ LINK:')
    for key, n in sorted(djlink.items()):
        port, typ, _src = key
        p, pay = samples[key]
        # Announce port (50000) header:
        #   magic(10) type(1) 00(1) name(20) 01 02 00 len(1) payload
        # and the byte after `len` is the device number (the player number).
        # Beat (50001) and status (50002) packets have no zero after the type,
        # so the name starts at 11.
        start = 12 if port == 50000 else 11
        name = pay[start:start + 20].split(b'\x00')[0].decode('ascii', 'replace')
        dev = pay[36] if len(pay) > 36 else None
        # Byte 36 is the device number only for these types on the announce
        # port; elsewhere it is a packet counter, an IP byte, or something else.
        numbered = port == 50000 and typ in (0x04, 0x05, 0x06, 0x08)
        print('      port %-5d (%-8s) type 0x%02x  x%-5d  from %s / %s  '
              'len %-3d player %s  name %r'
              % (port, PORTS.get(port, '?'), typ, n, ip(p['sip']),
                 mac(p['src']), len(pay),
                 dev if dev is not None else '?', name))
        if dev is not None and numbered:
            players.setdefault(dev, set()).add(mac(p['src']))
    if other_udp:
        print('    other UDP   : %s' % ', '.join(
            '%d->%d=%d' % (k[0], k[1], v)
            for k, v in sorted(other_udp.items())))

    # A player number claimed by two MACs is a device-number conflict; a real
    # CDJ flashes its player number for it.
    for dev, macs in sorted(players.items()):
        if len(macs) > 1:
            print('    !! PLAYER NUMBER %d is claimed by %d decks: %s'
                  % (dev, len(macs), ', '.join(sorted(macs))))
            print('       That is a device-number CONFLICT, and a real '
                  'CDJ flashes its player number for exactly this.')

    # Two announcing MACs means both decks are on the link.
    talkers = sorted({k[2] for k in djlink})
    print('    VERDICT     : %d deck(s) put Pro DJ Link on the wire: %s'
          % (len(talkers), ', '.join(talkers)))
    if len(talkers) < 2:
        print('                  (a second deck would appear here as a second '
              'MAC -- one talker is not two decks seeing each other)')


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    for path in argv[1:]:
        try:
            report(path)
        except (OSError, ValueError) as exc:
            print('=== %s: %s' % (path, exc))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
