#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The CDJ-TOUR1's tablet, reduced to the one message the deck acts on.

The TOUR1 player is a CDJ-2000NXS2 whose browse keys are forwarded to a tablet
while its TOUR1 MODE setting is on (the default). Without a tablet nothing
answers those keys, so no list is ever navigated. The tablet turns the setting
off with a Pro DJ Link frame: type 0x5d (general operation), kind 8 (CDJ
utility), byte 0x2e = 0x80, sent from player id 0x31..0x35 to the deck's UDP
port 50002. MAIN's own handler (0x084C5870 -> 0x084C4136) then calls the
setter 0x083FD9C6(0), which lowers the word at 0x0A35FB4C.

The deck drops datagrams until it has claimed its player number, and its first
keep-alives can come ahead of that (a slow boot lost five of them), so the
frame goes out with each of the deck's first keep-alives (type 0x06 on UDP
50000, about every 1.5 s). It has to stop early: the deck acts on every copy,
and one that arrives after the deck has opened the USB device in its browse
view throws the browse view back to the player screen, so no track loads
(the tenth keep-alive did, the fifth is still well ahead of it). Like
dhcp_server.py it lives on the rig's multicast segment, which carries raw
Ethernet frames, and answers ARP for its address.

  usage: python3 scripts/net/tablet_peer.py <group:port> [seconds]
"""
import signal
import socket
import sys
import time

from link_probe import arp_reply, join_group, split_group, udp_frame
from score_link import parse

MAGIC = b'Qspt1WmJOL'
NAME = b'TOUR1-TABLET'.ljust(20, b'\0')
KEEP_ALIVE, GENERAL_OPERATION = 0x06, 0x5d
LINK_PORT, CONTROL_PORT = 50000, 50002
CDJ_UTILITY = 8
TOUR1_MODE_OFF = 0x80       # bit 7: the field is valid; low bits: the new mode

SENDS = 5                   # keep-alives answered; the deck drops what comes before its claim
TABLET_PLAYER = 1           # sender id 0x31, the tablet paired with player 1
TABLET_IP = '192.168.50.60'
# Outside the decks' 02:00:00:00:00:0N range and the DHCP and probe MACs.
TABLET_MAC = bytes.fromhex('024241540001')

running = True


def stop(_signum, _frame):
    global running
    running = False


def header(frame_type, player, length_field):
    return (MAGIC + bytes([frame_type]) + NAME + bytes([1, 0, 0x30 + player])
            + length_field.to_bytes(2, 'big'))


def cdj_utility(tour1_mode, player=TABLET_PLAYER):
    """A GeneralOperationReq of kind 8 that sets only TOUR1 MODE."""
    param = bytes(6) + bytes([tour1_mode]) + bytes(5)
    return header(GENERAL_OPERATION, player, len(param) + 4) + bytes([CDJ_UTILITY, 0, 0, 0]) + param


def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    group = argv[1]
    seconds = float(argv[2]) if len(argv) > 2 else 0.0
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    sock = join_group(*split_group(group))
    sock.settimeout(0.5)
    dest = split_group(group)
    sent = 0
    t0 = time.time()
    while running and not (seconds and time.time() - t0 > seconds):
        try:
            frame, _ = sock.recvfrom(65535)
        except socket.timeout:
            continue
        except OSError:
            break
        if frame[6:12] == TABLET_MAC:
            continue
        reply = arp_reply(frame, TABLET_IP, TABLET_MAC)
        if reply:
            sock.sendto(reply, dest)
            continue
        got = parse(frame)
        if (not got or got['kind'] != 'udp' or got['dport'] != LINK_PORT
                or got['payload'][:10] != MAGIC or got['payload'][10] != KEEP_ALIVE):
            continue
        if sent == SENDS:
            continue
        deck_ip = socket.inet_ntoa(got['sip'])
        sock.sendto(udp_frame(TABLET_MAC, got['src'], TABLET_IP, deck_ip, CONTROL_PORT, CONTROL_PORT,
                              cdj_utility(TOUR1_MODE_OFF)), dest)
        sent += 1
        print('tablet_peer: TOUR1 MODE off sent to %s (%s), %d of %d' % (deck_ip, got['src'].hex(':'), sent, SENDS),
              flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
