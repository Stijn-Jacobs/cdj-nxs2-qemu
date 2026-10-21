# SPDX-License-Identifier: GPL-2.0-or-later
"""phrase on the CDJ-2000NXS2: two MAIN routines (1.87) that read the track's
PSSI and put it into the overview payload, and the display routine (1.81) that
draws the band."""
from players.cdj2000nxs2 import (FREE, MALLOC, MEMSET, OVERVIEW_PUBLISH, OVERVIEW_SLOT,
                                 PREVIEW_READ, TAG_FETCH)
from sigpatch import Anchor, FreeEnd, Mod

# The display mod's merge point and the context around it (see its entry below).
PHRASE_SITE = [
    0xE627, 0x60F2, 0x3658, 0x59F1, 0x4680, 0x4600, 0x36BC, 0x369C,
    0x2611, 0x52F2, 0x7801, 0x72FF, 0x2228, 0x8D02, 0x1F22, 0xAECE,
    0x0009, 0xDB34, 0x6DB3,
]

# The phrase table phrasefetch keeps for phrasedata.
PHRASE_TABLE = (0x081FFFF0, FreeEnd(-16))

# What the display routine imports. The two code addresses are fixed distances
# from the phrase site (the loop top 0x242 bytes before the signature, the
# renderer's marker pass where the replaced pair ends) because the loop top
# has no unique signature of its own that stays clear of the replaced span.
GUI_IMPORTS = (
    (0x0E5BB45C, Anchor(sig=[0x0009, 0x3DF1, 0x6194, 0xD141, 0xD241, 0x0000], index=3, literal=True)),  # the 600 overview records
    (0x1C007B82, Anchor(PHRASE_SITE, 0, False, None, -0x242)),
    (0x1C007DE6, Anchor(PHRASE_SITE, 17, False)),
)

MODS = {
    # Hooks PREVIEW_READ: reads the track's PSSI and beat grid and reduces them
    # to the 600-column phrase table phrasedata writes.
    'phrasefetch': Mod(
        what='phrase analysis (PSSI) read and reduced to overview columns',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(PHRASE_TABLE, FREE, MALLOC, MEMSET, TAG_FETCH),
        hook=True,
        **PREVIEW_READ,
    ),
    # Hooks OVERVIEW_PUBLISH: writes the phrase table phrasefetch kept into the
    # overview records' spare bits (see main_phrasedata.s).
    'phrasedata': Mod(
        what='phrase colours in the overview payload',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(PHRASE_TABLE, OVERVIEW_SLOT),
        hook=True,
        **OVERVIEW_PUBLISH,
    ),
    # The colour-preview overview strip's per-column loop-bottom merge point:
    # every fill style the renderer can take for one column rejoins here,
    # right before the column counter is bumped (r8 = the just-finished
    # column, 0..599). Hooking here instead of the colour lookup means the
    # waveform's own fill is already painted before this call runs, so
    # gui_phrase.s draws its band on top rather than the fill overwriting it,
    # and the waveform's own colours are never touched by this mod at all.
    #
    # sig carries 9 words of context before the replaced pair and 2 after
    # (neither replaced) purely so this site matches uniquely; the two
    # replaced words are sig[9] (the counter load) and sig[10] (the column
    # increment). gui_phrase.s reproduces both, then the branch they lead
    # into, itself -- see that file for why (sigpatch's own auto-resume can
    # only express one target, and this merge point has two).
    'phrase': Mod(
        what='overview strip phrase band',
        target='gui',
        fw_versions=('1.81',),
        sig=PHRASE_SITE,
        imports=GUI_IMPORTS,
        start=9,
        end=17,
        args=[],
        literal=False,
    ),
}
