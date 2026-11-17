# SPDX-License-Identifier: GPL-2.0-or-later
"""ableton_link on the CDJ-2000NXS2's MAIN 1.87: the announcements on the
beat and the replies to Link's measurement pings."""
from players.cdj2000nxs2 import BEAT_SEND, DEVICE, TICK, TICK_GETTER, UDP_SND_DAT
from sigpatch import Anchor, Mod

# The Pro DJ Link receive task, right after udp_rcv_dat has filled its 128-byte
# buffer (r10) and the call that takes a memory block for the datagram: the
# block call with its delay slot and the fpscr read that follows it. r0 still
# holds the block call's result, r14 the datagram length, *(r15) the block and
# r15+12 the sender's address; the stock test of r0 after the span loops back
# to the receive when it is non-zero, so a hook that sets r0 swallows the
# datagram.
LINK_RECEIVE = dict(
    sig=[0x26C9, 0xAFE4, 0x466A, 0xD25C, 0x65F3, 0xD75C, 0xE6FF, 0x470B,
         0x6422, 0x016A, 0x2008, 0x21C9, 0x8FD9, 0x416A, 0xD758, 0xE500,
         0x64F2, 0x470B],
    start=3,
    end=10,
)

LINK_CEP = (0x0AB84CE8, Anchor(sig=[0x5BA4, 0x7B01, 0x1AB4, 0xD222, 0x22E2, 0x4F26], index=3, literal=True))      # Pro DJ Link endpoint
LINK_POOL = (0x0AB84CDC, Anchor(sig=[0x2279, 0x8F2B, 0x426A, 0xD146, 0x65F3, 0xD746], index=3, literal=True))     # Pro DJ Link datagram pool
LINK_RELEASE = (0x08518DA8, Anchor(sig=[0x1F21, 0xDC8D, 0xDB8D, 0xDD8E, 0xDE8E, 0xD98F], index=3, literal=True))  # frees a Pro DJ Link datagram block
LINK_COMMON = (TICK, TICK_GETTER, DEVICE)

MODS = {
    'abletonlink': Mod(
        what='Ableton Link announcements (UDP 20808)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=LINK_COMMON,
        hook=True,
        **BEAT_SEND,
    ),
    'abletonlinkpong': Mod(
        what='Ableton Link measurement replies (UDP 50000)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(LINK_CEP, LINK_POOL, UDP_SND_DAT, LINK_RELEASE) + LINK_COMMON,
        hook=True,
        **LINK_RECEIVE,
    ),
}
