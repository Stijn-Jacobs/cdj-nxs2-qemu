# SPDX-License-Identifier: GPL-2.0-or-later
"""key_shift on the CDJ-2000NXS2's MAIN 1.87: a semitone shift of the deck's
pitch that leaves its tempo alone, stepped with the pads while MASTER TEMPO
is held, sent to the DSP in a field of the id-block that the stock program
never reads, cleared on every track load and shown in the title bar's key;
and on the DSP program MAIN carries, the resampler in front of the MASTER
TEMPO vocoder that turns it into a pitch shift (dsp_keyshiftdsp.s)."""
from dsppatch import DspMod
from players.cdj2000nxs2 import DSP_PROFILE, PAD_MODES, PADS
from sigpatch import Anchor, FreeEnd, Mod

# DSPoutCTL's compare of its speed block against the mirror it last sent: the
# pool load of the mirror 0x09944184 into r13 through the shll2 of the first
# frame offset, between halfwords that make the signature unique. Every mode
# arm of the block builder joins here; r15 still points at the block.
BLOCK_COMPARE = dict(
    sig=[0xE002, 0x4018, 0x7038, 0x06FE, 0x70A0, 0x0F66, 0xDD8E, 0xE0E0, 0x600C, 0x06DE,
         0xE075, 0x4008, 0x02FE, 0x3620, 0x8B37, 0xE0E4],
    start=6,
    end=12,
)

# The load's request for the track's audio, 0x08300254 called from 0x0830BBFE
# with its delay slot; the bf.s before the span lands on its first halfword.
AUDIO_REQUEST = dict(
    sig=[0x8F02, 0x426A, 0xA75B, 0x0009, 0xE062, 0xE700, 0x4008, 0x64C3, 0xD36B, 0x430B,
         0x0C76, 0xE003, 0xE500, 0x4018],
    start=4,
    end=11,
)

# FUN_082DFCC6(speed, mt, &speed out, &mt out), the speed snap DSPoutCTL runs
# with the MASTER TEMPO flag, at its entry.
SPEED_SNAP = (0x082DFCC6, Anchor(sig=[0x2558, 0x2642, 0x8D1D, 0x2752, 0xE214, 0x4218, 0x727A, 0x3422],
                                 index=0, literal=False))

# MASTER TEMPO's key state, 1 while the key is down: 0x10 past the MASTER
# key's, which the Ui loads at 0x084DFD00.
MT_KEY = (0x0B54CB0C, Anchor(sig=[0xED05, 0x4A18, 0xDE40, 0x7ACC, 0xDC40, 0xDB41, 0x60E2, 0x8806, 0x893C,
                                  0x0A07], index=4, literal=True, delta=0x10))

# The pad words of the pass before, slot n at +4n, as the mode 2 arm of the
# mode switch loads them.
PAD_COPIES = (0x0B057CD8, Anchor(sig=[0xD17F, 0x6212, 0x2228, 0x8B79, 0xDD7E, 0xEC01, 0xDE7E, 0xEB09],
                                 index=6, literal=True))

# The pad mode, 0 for hot cues, as the pad pass's next routine 0x08442F26 loads it.
PAD_MODE = (0x0B05651C, Anchor(sig=[0xD638, 0x6062, 0x8806, 0x8912, 0x2008, 0x8910, 0xD643, 0x6062],
                               index=6, literal=True))

# The queue of pad edges, as the pad pass loads it after the mode switch.
PAD_QUEUE = (0x0B056BB4, Anchor(sig=[0xD40C, 0x016A, 0x21E9, 0x6242, 0x2228, 0x8D07, 0x416A, 0xD609, 0x6062],
                                index=7, literal=True))

# The queue head mode 0 handled last, as the mode switch passes it to 0x08442CFA.
PAD_LAST = (0x10DE00F8, Anchor(sig=[0xD144, 0x6212, 0x2228, 0x8B03, 0xD54F, 0xD44E, 0xBEFB, 0x0009],
                               index=4, literal=True))

# The display task's dispatch lock around its copy of the display model into
# the screen message: 0x08404A38 disables dispatching and returns 1 when it
# already was, 0x08404A5E(that) enables it again; as the task loads them.
MODEL_LOCK = (0x08404A38, Anchor(sig=[0x8804, 0x8903, 0x8801, 0x8901, 0xA0B7, 0x0009, 0xD17A, 0xEC76],
                                 index=6, literal=True))
MODEL_UNLOCK = (0x08404A5E, Anchor(sig=[0x21C2, 0x6150, 0x056A, 0xDC6F, 0x611C, 0x361C, 0x25E9, 0x456A],
                                   index=3, literal=True))

# The key's text field in the display model: 0x132C past the title's, which
# the task's flag clearing 0x0838F336 loads.
KEY_FIELD = (0x0B540ED0, Anchor(sig=[0x604C, 0x8801, 0x8B54, 0xDB38, 0xEE02, 0xE704, 0xE113, 0x4E18],
                                index=3, literal=True, delta=0x132C))

# The shift, 0x100 | (n & 0xFF) for n semitones; and the pads A..C that were
# down on the pass before, in bits 0-2.
SHIFT = (0x081FFFEC, FreeEnd(-20))
PADS_DOWN = (0x081FFFE8, FreeEnd(-24))
# The track's own key and the text the readout last wrote, each a length and
# its UTF-16 units; below cdj3000x's words, which reach down to -0x4AE0.
KEY_TEXT = (0x081FB500, FreeEnd(-0x4B00))

# The DSP code goes at the start of the program's cave; its first six
# branches are the entries of ks_mt, ks_hop_a, ks_hop_b, ks_pull, ks_append
# and ks_mt_play, two words each.
CODE = DSP_PROFILE.cave[0]
KS_MT, KS_HOP_A, KS_HOP_B, KS_PULL, KS_APPEND, KS_MT_PLAY = (CODE + 8 * k for k in range(6))


def displacement(pc, target):
    """The 21-bit field of a b or callp at pc, counted in words from its fetch packet."""
    return ((target - (pc & ~31)) >> 2 & 0x1FFFFF) << 7


def callp(pc, target):
    return 0x10000012 | displacement(pc, target)


def branch_s1(pc, target):
    return 0x00000010 | displacement(pc, target)


def branch_s2(pc, target):
    return 0x00000012 | displacement(pc, target)


DSP_EDITS = (
    # state handler: the setter's MT argument, ldw *+a12(0),b4
    (0x80005EB0, 0x02300266, callp(0x80005EB0, KS_MT)),
    # the play-path setter's one read of its MT argument, cmpeq .L2 0,b13,b4
    (0x8000D424, 0x02340A5A, callp(0x8000D424, KS_MT_PLAY)),
    # engine A and B hop: the nop 3 after r is formed
    (0x80046CF0, 0x00004000, callp(0x80046CF0, KS_HOP_A)),
    (0x8004F3B8, 0x00004000, callp(0x8004F3B8, KS_HOP_B)),
    # the stream puller's ldw *+b14(1076),b4 and the nop 4 after it
    (0x8003E4E0, 0x02010D6E, branch_s2(0x8003E4E0, KS_PULL)),
    (0x8003E4E4, 0x00006000, 0x00008000),
    # engine A and B append: the b .S1 0x8007D0B0, in parallel with what follows
    (0x80046C2C, 0x006C9211, branch_s1(0x80046C2C, KS_APPEND) | 1),
    (0x8004F2EC, 0x005BBA11, branch_s1(0x8004F2EC, KS_APPEND) | 1),
)

MODS = {
    'keyshift': Mod(
        what='key shift: the shift sent to the DSP in the id-block',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(SHIFT, SPEED_SNAP),
        hook=True,
        **BLOCK_COMPARE,
    ),
    'keyshiftpad': Mod(
        what='key shift: pads A, B and C step it while MASTER TEMPO is held',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(PADS, PAD_COPIES, MT_KEY, PAD_MODE, PAD_QUEUE, PAD_LAST, PADS_DOWN, SHIFT),
        hook=True,
        **PAD_MODES,
    ),
    'keyshiftload': Mod(
        what='key shift: back to no shift on every track load',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(SHIFT,),
        hook=True,
        **AUDIO_REQUEST,
    ),
    'keyshiftkey': Mod(
        what='key shift: the title bar shows the shifted key',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(KEY_FIELD, KEY_TEXT, SHIFT, MODEL_LOCK, MODEL_UNLOCK),
        hook=True,
        after=('keyshiftpad',),
        **PAD_MODES,
    ),
    'keyshiftdsp': DspMod(
        what='key shift: the DSP program resamples into MASTER TEMPO',
        target='dsp',
        fw_versions=('1.87',),
        edits=DSP_EDITS,
        code=((CODE, 'keyshiftdsp'),),
    ),
}
