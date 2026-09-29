#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Hand the decks' OSC beat messages (the osc_beat mod) to desktop OSC apps.

On a real network the deck's OSC datagrams reach every machine on its subnet.
Here they travel inside QEMU's `-netdev socket,mcast=GROUP:PORT` segment as raw
Ethernet frames, which no OSC app can read, so this unwraps every UDP datagram
sent to the OSC port and sends its payload on to a normal UDP address.

  usage: python3 scripts/net/osc_relay.py [group:port] [to-host:to-port]
         defaults 239.77.77.1:45000 and 127.0.0.1:50010
"""
import socket
import struct
import sys

OSC_PORT = 50010


def main(argv):
    group, _, port = (argv[1] if len(argv) > 1 else '239.77.77.1:45000').partition(':')
    to_host, _, to_port = (argv[2] if len(argv) > 2 else '127.0.0.1:%d' % OSC_PORT).partition(':')
    dest = (to_host, int(to_port))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    # macOS: the decks' QEMUs bind this port with SO_REUSEPORT, and BSD
    # shares a port only when every socket on it sets that.
    if sys.platform == 'darwin':
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEPORT, 1)
    sock.bind(('', int(port)))
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_ADD_MEMBERSHIP,
                    struct.pack('4s4s', socket.inet_aton(group),
                                socket.inet_aton('0.0.0.0')))
    out = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    print('osc_relay: %s:%s port %d -> %s:%d' % (group, port, OSC_PORT, *dest))

    while True:
        frame, _ = sock.recvfrom(65535)
        # IPv4 carrying UDP, addressed to the OSC port
        if len(frame) < 42 or frame[12:14] != b'\x08\x00' or frame[23] != 17:
            continue
        udp = 14 + (frame[14] & 0x0F) * 4
        if struct.unpack_from('>H', frame, udp + 2)[0] == OSC_PORT:
            out.sendto(frame[udp + 8:], dest)


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv))
    except KeyboardInterrupt:
        pass
