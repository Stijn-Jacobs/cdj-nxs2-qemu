# SPDX-License-Identifier: GPL-2.0-or-later
"""tablet_peer: the TOUR1 MODE frame, byte for byte, and the ARP answer for the
tablet's own address."""
import socket
import struct

import link_probe
import tablet_peer

# The frame as the tablet's Utility screen sends it with TOUR1 MODE off.
TOUR1_MODE_OFF = bytes.fromhex(
    '5173707431576d4a4f4c' '5d' '544f5552312d5441424c4554' + '00' * 8 + '0100' '31' '0010'
    '08000000' + '00' * 6 + '80' + '00' * 5)


def test_the_mode_off_frame_is_the_tablets():
    assert tablet_peer.cdj_utility(tablet_peer.TOUR1_MODE_OFF) == TOUR1_MODE_OFF


def test_the_frame_header_names_the_sender_and_the_parameter_length():
    frame = tablet_peer.cdj_utility(0x80, player=2)
    assert frame[:10] == b'Qspt1WmJOL'
    assert frame[10] == 0x5d
    assert frame[0x21] == 0x32
    assert struct.unpack('>H', frame[0x22:0x24])[0] == 0x10
    assert frame[0x24] == 8
    assert frame[0x2e] == 0x80


def test_it_answers_arp_for_its_own_address_with_its_own_mac():
    asker = bytes.fromhex('020000000001')
    request = (b'\xff' * 6 + asker + b'\x08\x06' + struct.pack('>HHBBH', 1, 0x0800, 6, 4, 1)
               + asker + socket.inet_aton('192.168.50.10') + bytes(6) + socket.inet_aton(tablet_peer.TABLET_IP))
    reply = link_probe.arp_reply(request, tablet_peer.TABLET_IP, tablet_peer.TABLET_MAC)
    assert reply[:6] == asker
    assert reply[6:12] == tablet_peer.TABLET_MAC
    assert reply[22:28] == tablet_peer.TABLET_MAC
