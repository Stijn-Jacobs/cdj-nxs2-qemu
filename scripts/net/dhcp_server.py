#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Answer the deck's DHCP on the shared multicast segment.

Once its EtherMAC is up, the deck's first frame is a DHCP request (udp
68->67). Until it gets an address it never sends Pro DJ Link traffic, so a
link test needs something on the segment to answer.

QEMU's `-netdev socket,mcast=` backend carries one raw Ethernet frame per UDP
datagram, so any ordinary multicast socket can read and write the segment: no
root, tap or bridge needed. capture_link.py uses the read side.

Our own frames loop back, so every frame with our source MAC is dropped.

This is not a general DHCP server: it answers DISCOVER with OFFER and REQUEST
with ACK for one address per client, and ignores everything else.

  usage: python3 scripts/net/dhcp_server.py <group:port> [seconds]
  env:   DHCP_NET=192.168.50   the /24 to hand out (deck .10, server .1)

Exits cleanly on SIGTERM and always prints its counters.
"""
import os
import signal
import socket
import struct
import sys
import time

running = True

# Locally administered and outside the decks' 02:00:00:00:00:0N range, or the
# loopback filter would drop that deck's DISCOVERs. 44:48:43:50:44 is "DHCPD".
SERVER_MAC = bytes.fromhex('024448435044')
MAGIC = bytes([99, 130, 83, 99])               # DHCP options cookie

DISCOVER, OFFER, REQUEST, ACK = 1, 2, 3, 5


def stop(_signum, _frame):
    global running
    running = False


def checksum(data):
    if len(data) % 2:
        data += b'\0'
    total = 0
    for i in range(0, len(data), 2):
        total += (data[i] << 8) | data[i + 1]
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def parse_dhcp(frame):
    """Return (xid, client_mac, msg_type, requested_ip) or None.

    Deliberately strict: anything that is not IPv4/UDP/68->67 with a DHCP
    cookie is not ours, and a loose parser here would answer the deck's own
    Pro DJ Link traffic once it starts.
    """
    if len(frame) < 14 + 20 + 8 + 240:
        return None
    if frame[12:14] != b'\x08\x00':
        return None
    ihl = (frame[14] & 0x0F) * 4
    if frame[14 + 9] != 17:                     # UDP
        return None
    udp = 14 + ihl
    sport, dport = struct.unpack_from('>HH', frame, udp)
    if (sport, dport) != (68, 67):
        return None
    bootp = udp + 8
    if frame[bootp:bootp + 4] != b'\x01\x01\x06\x00':   # BOOTREQUEST, eth, 6
        return None
    xid = frame[bootp + 4:bootp + 8]
    flags = frame[bootp + 10:bootp + 12]
    chaddr = frame[bootp + 28:bootp + 34]
    ethsrc = frame[6:12]
    if frame[bootp + 236:bootp + 240] != MAGIC:
        return None

    msg_type, requested = None, None
    i = bootp + 240
    while i < len(frame):
        opt = frame[i]
        if opt == 255:
            break
        if opt == 0:
            i += 1
            continue
        if i + 1 >= len(frame):
            break
        ln = frame[i + 1]
        val = frame[i + 2:i + 2 + ln]
        if opt == 53 and ln == 1:
            msg_type = val[0]
        elif opt == 50 and ln == 4:
            requested = val
        i += 2 + ln
    if msg_type is None:
        return None
    return xid, chaddr, msg_type, requested, flags, ethsrc


def build_reply(kind, xid, client_mac, client_ip, server_ip, mask, router,
                flags=b'\x00\x00', eth_to=None):
    """One Ethernet frame carrying a BOOTREPLY.

    The IP destination is 255.255.255.255 because the client does not own an
    address yet -- a unicast IP to an address it has not accepted is the
    classic way to have an exchange that looks fine on the wire and never
    completes. The Ethernet destination is the client's own MAC unless it
    asked for a broadcast reply (BOOTP flags bit 15): the deck ignores a
    broadcast it did not ask for. Its flags are echoed rather than overridden.
    """
    # eth_to is the address that actually asked; client_mac is the
    # chaddr the firmware wrote, which is not unique across decks.
    eth_dst = (b'\xff' * 6 if (flags[0] & 0x80)
               else (eth_to or client_mac))
    opts = bytearray(MAGIC)
    opts += bytes([53, 1, kind])
    opts += bytes([54, 4]) + server_ip          # server identifier
    opts += bytes([51, 4]) + struct.pack('>I', 86400)
    opts += bytes([1, 4]) + mask
    opts += bytes([3, 4]) + router
    opts += bytes([6, 4]) + server_ip           # DNS: ourselves, harmlessly
    opts += bytes([255])
    while len(opts) < 64:                       # keep BOOTP's minimum shape
        opts += b'\0'

    bootp = bytearray(236)
    bootp[0:4] = bytes([2, 1, 6, 0])            # BOOTREPLY, eth, hlen 6
    bootp[4:8] = xid
    bootp[10:12] = flags                        # echoed, not chosen by us
    bootp[16:20] = client_ip                    # yiaddr
    bootp[20:24] = server_ip                    # siaddr
    bootp[28:34] = client_mac
    payload = bytes(bootp) + bytes(opts)

    udp_len = 8 + len(payload)
    pseudo = server_ip + b'\xff\xff\xff\xff' + bytes([0, 17]) + \
        struct.pack('>H', udp_len)
    udp = struct.pack('>HHHH', 67, 68, udp_len, 0) + payload
    ck = checksum(pseudo + udp)
    udp = udp[:6] + struct.pack('>H', ck or 0xFFFF) + udp[8:]

    total = 20 + udp_len
    ip = bytearray(struct.pack('>BBHHHBBH', 0x45, 0, total, 0, 0, 64, 17, 0))
    ip += server_ip + b'\xff\xff\xff\xff'
    ip[10:12] = struct.pack('>H', checksum(bytes(ip)))

    return eth_dst + SERVER_MAC + b'\x08\x00' + bytes(ip) + udp


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    group, _, port = argv[1].partition(':')
    port = int(port)
    seconds = float(argv[2]) if len(argv) > 2 else 0.0

    net = os.environ.get('DHCP_NET', '192.168.50')
    server_ip = socket.inet_aton('%s.1' % net)
    # MAC -> address. Handing every client the same address is fine for one
    # deck and useless for two, which is the whole point of a shared segment.
    leases = {}
    next_host = [10]

    def lease_for(mac):
        if mac not in leases:
            leases[mac] = socket.inet_aton('%s.%d' % (net, next_host[0]))
            next_host[0] += 1
        return leases[mac]
    router = server_ip
    mask = socket.inet_aton('255.255.255.0')

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
    sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 1)
    sock.settimeout(0.5)
    dest = (group, port)

    seen = offers = acks = 0
    t0 = time.time()
    while running:
        if seconds and time.time() - t0 > seconds:
            break
        try:
            frame, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue
        except OSError:
            break
        if len(frame) >= 12 and frame[6:12] == SERVER_MAC:
            continue                            # our own, looped back
        got = parse_dhcp(frame)
        if got and got[5] == SERVER_MAC:
            # Only reachable if SERVER_MAC is inside the deck range.
            print('dhcp_server: a CLIENT is using the server MAC %s -- it will '
                  'be ignored as loopback' % SERVER_MAC.hex(':'), flush=True)
        if not got:
            continue
        xid, chaddr, msg_type, _requested, flags, ethsrc = got
        seen += 1
        # Key on the Ethernet source, not chaddr: the firmware puts the same
        # 00:00:00:00:00:01 in chaddr on every deck. The reply still echoes the
        # client's own xid and chaddr.
        client_ip = lease_for(ethsrc)
        if msg_type == DISCOVER:
            sock.sendto(build_reply(OFFER, xid, chaddr, client_ip, server_ip,
                                    mask, router, flags, ethsrc), dest)
            offers += 1
            print('dhcp_server: DISCOVER from %s (chaddr %s) -> OFFER %s'
                  % (ethsrc.hex(':'), chaddr.hex(':'),
                     socket.inet_ntoa(client_ip)), flush=True)
        elif msg_type == REQUEST:
            sock.sendto(build_reply(ACK, xid, chaddr, client_ip, server_ip,
                                    mask, router, flags, ethsrc), dest)
            acks += 1
            print('dhcp_server: REQUEST from %s (chaddr %s) -> ACK %s'
                  % (ethsrc.hex(':'), chaddr.hex(':'),
                     socket.inet_ntoa(client_ip)), flush=True)

    print('dhcp_server: %d client message(s), %d OFFER, %d ACK on %s:%d'
          % (seen, offers, acks, group, port))
    for mac, ip in leases.items():
        print('dhcp_server:   %s -> %s' % (mac.hex(':'), socket.inet_ntoa(ip)))
    if len(leases) == 1:
        print('dhcp_server:   ONE client only. Two decks sharing a hardware '
              'address arrive here as one -- see CDJ_ETHER_MAC.')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
