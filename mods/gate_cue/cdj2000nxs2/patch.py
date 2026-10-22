# SPDX-License-Identifier: GPL-2.0-or-later
"""gate_cue on the CDJ-2000NXS2's MAIN 1.87: a hook on the hot cue recall that
notes a pad pressed while the deck is paused, and a hook on the Player's pad
pass that sends the deck back to that cue and pauses it when the pad is let go."""
from sigpatch import Anchor, FreeEnd, Mod

# The hot cue recall 0x08435384 (r4 = the Player slot), at its entry: the
# pushes of r11-r14, pr and macl, then the frame and the first loads. The pad
# pass calls it from 0x08442D5C on the press edge of a pad whose slot is set.
RECALL_ENTRY = dict(
    sig=[0x2FB6, 0x2FC6, 0x2FD6, 0x2FE6, 0x4F22, 0x4F12, 0x7FF8, 0xD660, 0xED00, 0x6C43,
         0x6BD3, 0x6262, 0x2228, 0x8D02, 0x6ED3],
    start=0,
    end=6,
)

# The pad pass's mode switch 0x08442D96, at its entry: the pushes of r8-r14
# and pr. The pad pass 0x08443050 calls it on every Player pass, after the
# queue of pad edges is built. The span has to take the pr push with it: the
# trampoline returns through pr, so a push left after the span saves the
# trampoline's way back instead of the caller's, and the switch's rts then
# loops on itself.
PAD_MODES = dict(
    sig=[0x2F86, 0x2F96, 0x2FA6, 0x2FB6, 0x2FC6, 0x2FD6, 0x2FE6, 0x4F22, 0x4F12, 0xD60C,
         0x6062, 0x8802, 0x892B, 0x8801, 0x895E],
    start=0,
    end=8,
)

# The CUE key's return to the main cue, 0x084337EE after its checks: a flag
# cleared, the Player's playing flag cleared, the cue command 0x0842ADD2(slot 0,
# 1, *0x0B058C04), the Player's mode bookkeeping and a loop tidy-up. Gate Cue
# runs the same calls with the pad's slot.
BACK_CUE = [0xD771, 0x470B, 0x2FE2, 0x016A, 0x21E9, 0xBBD5, 0x416A, 0x046A, 0xD76E, 0xD16F,
            0xE501, 0x24E9, 0x446A, 0x6672, 0x410B, 0xE400, 0x056A, 0xD76C, 0x25E9, 0x470B,
            0x456A, 0x026A, 0xD66A, 0x22E9, 0x460B, 0x426A]
CLEAR_FLAG = (0x0844794C, Anchor(sig=BACK_CUE, index=0, literal=True))
CUE_ARG = (0x0B058C04, Anchor(sig=BACK_CUE, index=8, literal=True))
CUE_COMMAND = (0x0842ADD2, Anchor(sig=BACK_CUE, index=9, literal=True))
MODE_UPDATE = (0x0843278A, Anchor(sig=BACK_CUE, index=17, literal=True))
LOOP_TIDY = (0x0843D712, Anchor(sig=BACK_CUE, index=22, literal=True))

# 0x084330E4, which the CUE key reaches with bsr: its entry, and the Player's
# paused flag (the byte the PLAY key tests to play or pause, 1 while paused) it
# loads.
STOP_STATE = dict(sig=[0x2FE6, 0x4F22, 0xD15A, 0xDE5B, 0x410B, 0x0009, 0x056A, 0xE400, 0x2E40,
                       0xDE49, 0xD158, 0x25E9])
PLAYER_STOP = (0x084330E4, Anchor(index=0, literal=False, **STOP_STATE))
PAUSED = (0x0B058C10, Anchor(index=3, literal=True, **STOP_STATE))

# The Player state word, from its setter 0x084473F6; 0x084330E4 sets it to 7,
# stopped at a cue.
PLAYER_STATE = (0x0B058BFC, Anchor(sig=[0xD27D, 0x000B, 0x2242, 0xE209, 0x3423, 0x8901, 0xD27B, 0x2242],
                                   index=0, literal=True))

# The pad words of the deck's own panel in the Player key bank, one per slot
# from slot 0 (source 0 of 0x0B056C90, at +0x24C), as the pad test 0x08442CDA
# loads them.
PADS = (0x0B056EDC, Anchor(sig=[0xD644, 0xE504, 0x7604, 0x6262, 0x2228, 0x8B07, 0x5261], index=0,
                           literal=True))

# The slot the gate is armed with, between the press and the release.
ARMED = (0x081FFFF8, FreeEnd(-8))

MODS = {
    'gatecue': Mod(
        what='Gate Cue: back to the hot cue and paused when the pad is let go',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(ARMED, PADS, CLEAR_FLAG, PLAYER_STOP, CUE_ARG, CUE_COMMAND, MODE_UPDATE, LOOP_TIDY),
        hook=True,
        **PAD_MODES,
    ),
    'gatecuepress': Mod(
        what='Gate Cue: a hot cue pad pressed while paused',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(ARMED, PADS, PAUSED, PLAYER_STATE),
        hook=True,
        **RECALL_ENTRY,
    ),
}
