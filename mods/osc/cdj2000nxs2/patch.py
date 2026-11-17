# SPDX-License-Identifier: GPL-2.0-or-later
"""osc on the CDJ-2000NXS2's MAIN 1.87: the deck's state from its status
record, and a message on every beat."""
from players.cdj2000nxs2 import BEAT_SEND, DEVICE, STATUS_SEND, TICK, UDP_WRAP
from sigpatch import Anchor, Mod

MASTER = (0x0A35F39C, Anchor(sig=[0x0E24, 0x2228, 0x8906, 0xD421, 0x6640, 0x626C], index=3, literal=True))  # sync master word

MODS = {
    'osc': Mod(
        what='OSC deck state and load/play/stop/cue/loop events (UDP 50010)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(DEVICE, MASTER, TICK, UDP_WRAP),
        hook=True,
        **STATUS_SEND,
    ),
    'oscbeat': Mod(
        what='OSC per-beat output (UDP 50010)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(TICK,),
        hook=True,
        **BEAT_SEND,
    ),
}
