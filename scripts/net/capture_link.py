#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Sniff the decks' shared multicast segment and write a pcap.

QEMU's `-netdev socket,mcast=GROUP:PORT` backend puts one raw Ethernet frame
in one UDP datagram, with no length prefix and no encapsulation of its own.
So a plain multicast listener on the same group sees exactly what the decks
say to each other -- which means the whole segment is captured by ONE process,
rather than one capture per deck, and no `-object filter-dump` is needed (the
launcher passes `-netdev` only).

  usage: python3 scripts/net/capture_link.py <group:port> <out.pcap> [seconds]

Score the result with scripts/net/score_link.py. Exits cleanly on SIGTERM so the runner
can stop it, and always leaves a valid pcap behind -- an empty capture is a
result ("the MAC transmitted nothing") and must not look like a crash.
"""
import signal
import socket
import struct
import sys
import time

running = True


def stop(_signum, _frame):
    global running
    running = False


def main(argv):
    if len(argv) < 3:
        sys.exit(__doc__)
    group, _, port = argv[1].partition(':')
    port = int(port)
    out = argv[2]
    seconds = float(argv[3]) if len(argv) > 3 else 0.0

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # macOS: the decks' QEMUs bind this port with SO_REUSEPORT (see
    # patches/net_socket.c.patch), and BSD shares a port only when every
    # socket on it sets that.
    if sys.platform == 'darwin':
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind(('', port))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                    struct.pack('4s4s', socket.inet_aton(group),
                                socket.inet_aton('0.0.0.0')))
    sock.settimeout(0.5)

    n = 0
    t0 = time.time()
    # Classic pcap, Ethernet link type -- what score_link.py reads.
    with open(out, 'wb') as fh:
        fh.write(struct.pack('<IHHiIII', 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
        fh.flush()
        while running:
            if seconds and time.time() - t0 > seconds:
                break
            try:
                data, _ = sock.recvfrom(65535)
            except socket.timeout:
                continue
            except OSError:
                break
            now = time.time()
            fh.write(struct.pack('<IIII', int(now), int(now % 1 * 1e6),
                                 len(data), len(data)))
            fh.write(data)
            fh.flush()          # a killed sniffer must still leave a readable
            n += 1              # capture, so every frame is durable

    print('capture_link: %d frame(s) from %s:%d -> %s' % (n, group, port, out))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
