# SPDX-License-Identifier: GPL-2.0-or-later
"""cdj3000x_wave on the CDJ-2000NXS2's display 1.81: keep the centre waveform
on screen while a new detail waveform is published, and keep it moving while
the publish is on the link.

The renderer draws as many points as the count at 0x0E5BC26C holds. The
detail-waveform message (type 0x37) handler 0x0E502056 sets that count to 0 on
its first chunk and again on its last, and only the beat grid's handler (type
0x2B) sets it again once its last chunk is in. The points the renderer draws
from are not written by 0x37, so the old picture stays valid; only the count
hides it, for the whole of the publish. With Serato a publish comes every
4,500 points and on a seek, so the strip goes black each time. Leaving the two
stores out keeps the old count until the beat grid brings the new one.

The publish then stops the playhead instead: its link frames all carry the
position from before it went out, and the display drains them for 100 to
120 ms. x3000trainpos, a hook at the renderer's start, moves the position on
at the deck's own speed from the display's clock for as long as the publish
is on the link (see its routine).
"""
from sigpatch import Anchor, DataPatch, Edit, Mod

# The 0x37 handler from its entry to just before the chunk-1 count store.
DETAIL_HANDLER_ENTRY = bytes.fromhex('2fb62fc62fd62fe64f22dc6f32c19003602388018b0cd16d'
                                     '6012d56d4008005e88018b01d66c2602e600')

# The system time call the display's UI task makes to time each frame: it
# fills a short and a u32 of milliseconds at +4 (called with r4 = the buffer).
SYSTEM_TIME = (0x0E52E9FC, Anchor(sig=[0xDD26, 0xDC27, 0x4610, 0x2252, 0x1251, 0x8FFB, 0x7208, 0xE82B],
                                  index=1, literal=True))
# The beat tick handler (0x38) setting the tick enable to 1 and the tick count
# next to it.
TICK_ENABLE = (0x0E5BC274, Anchor(sig=[0x6C73, 0x7CFF, 0xE601, 0x37C6, 0xDD40, 0x2D62, 0xE100, 0x1D71],
                                  index=4, literal=True))

MODS = {
    'x3000keepwave': DataPatch(
        what='centre waveform kept on screen during a detail waveform publish',
        target='gui',
        fw_versions=('1.81',),
        table=DETAIL_HANDLER_ENTRY,
        before=0,
        reach=0xA0,
        edits=(Edit(bytes.fromhex('d16b21621162'), bytes.fromhex('d16b00091162')),    # mov.l r6,@r1, chunk 1
               Edit(bytes.fromhex('d75327026503'), bytes.fromhex('d75300096503'))),   # mov.l r0,@r7, last chunk
    ),
    # The renderer's start: the position word 0xFFF88070 (1/150 s) is loaded
    # into r4 with the zoom, and the address of the end clamp follows; the
    # hook runs after those loads and may change r4 in the saved block. The
    # name sorts after the other display routines, whose placement it would
    # otherwise move (the registry is laid out in alphabetical order).
    'x3000trainpos': Mod(
        what='centre waveform kept moving while a detail waveform update is on the link',
        target='gui',
        fw_versions=('1.81',),
        sig=[0x8B01, 0xA405, 0x0009, 0xD560, 0x3B51, 0x601C, 0xE205, 0x6452, 0xDE5F, 0x32B8, 0x66E2, 0x72FF, 0xEC01],
        start=3,            # `mov.l @(96,pc),r5`, the first of the six replaced
        end=9,              # up to `sub r11,r2`
        args=[],
        literal=False,
        imports=(SYSTEM_TIME, TICK_ENABLE),
        hook=True,
    ),
}
