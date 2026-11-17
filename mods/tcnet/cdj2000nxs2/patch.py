# SPDX-License-Identifier: GPL-2.0-or-later
"""tcnet on the CDJ-2000NXS2's MAIN 1.87: Opt-IN and Status once a second,
Time every 20 ms, and the listener that answers Metrics and MetaData."""
from players.cdj2000nxs2 import (DEVICE, PLAYER_RECORDS, STATUS_SEND, TICK, TICK_GETTER,
                                 UDP_SND_DAT, UDP_WRAP)
from sigpatch import Anchor, Mod

# The DJcont output task's loop (the task at 0x082D86AC), after the call that
# recomputes the play position word 0x09947488 from the DSP's frame: the call
# of its DSP-output step with its delay slot and the fpscr reads around it.
# The loop branches back to its wait right after the span, and nothing
# branches into it. r13 holds the DJcont record, r14 the fpscr mask; r6, the
# mask the loop applies on the way back, comes from the block.
DJCONT_PASS = dict(
    sig=[0xB015, 0x64D3, 0x056A, 0x64D3, 0x25E9, 0xD38F, 0x430B, 0x456A,
         0x066A, 0x26E9, 0xAFD2, 0x466A],
    start=2,
    end=10,
)

UDP_VCRE_CEP = (0x08233A22, Anchor(sig=[0xEE03, 0xD477, 0x4E18, 0xD777, 0x7EFC, 0xE014], index=3, literal=True))  # udp_vcre_cep
UDP_RCV_DAT = (0x0823415E, Anchor(sig=[0x0009, 0x6EF3, 0xDD2C, 0xEA00, 0x2FA6, 0x65E3], index=2, literal=True))   # udp_rcv_dat
TRACK_LENGTH = (0x0B5125E8, Anchor(sig=[0x0009, 0xE200, 0x2522, 0xD55F, 0x6052, 0x88FF], index=3, literal=True))
# data the firmware reaches only through a base a little before it
PLAY_POSITION = (0x09947488, Anchor(sig=[0x420B, 0x0009, 0x2008, 0xD577, 0x8B02, 0x4F26], index=3, literal=True, delta=0x10))
TITLE = (0x0994DE40, Anchor(sig=[0xE10A, 0xE709, 0x0DB6, 0xDB5C, 0x24E2, 0x1411], index=3, literal=True, delta=0x20C))
ARTIST = (0x0994E040, Anchor(sig=[0xE10A, 0xE709, 0x0DB6, 0xDB5C, 0x24E2, 0x1411], index=3, literal=True, delta=0x40C))

TCNET_COMMON = (TICK, TICK_GETTER, UDP_WRAP, DEVICE)
TCNET_RECORDS = (PLAYER_RECORDS, PLAY_POSITION, TRACK_LENGTH)

MODS = {
    'tcnet': Mod(
        what='TCNet Opt-IN and Status, once a second (UDP 60000)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=TCNET_COMMON,
        hook=True,
        **STATUS_SEND,
    ),
    'tcnettime': Mod(
        what='TCNet Time packets, every 20 ms (UDP 60001)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=TCNET_RECORDS + TCNET_COMMON,
        hook=True,
        **DJCONT_PASS,
    ),
    'tcnetdata': Mod(
        what='TCNet listener (UDP 65023): Metrics and MetaData to requesting nodes',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=TCNET_RECORDS + TCNET_COMMON + (TITLE, ARTIST, UDP_VCRE_CEP, UDP_RCV_DAT, UDP_SND_DAT),
        hook=True,
        **DJCONT_PASS,
    ),
}
