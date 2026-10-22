# SPDX-License-Identifier: GPL-2.0-or-later
"""beat_jump_long on the CDJ-2000NXS2's MAIN 1.87: the 2 and 4 beat jumps
become 32 and 64 beat jumps.

A beat jump key sets one of 14 slots per deck, idx = 2*log2(beats) + 1 for a
forward jump and + 0 for a reverse one. The Player's beat jump pass 0x084414A0
turns the newly pressed slot into the jump it posts through the table at
0xA80EA3D4, one (beats, reverse) pair per slot. Rows 10..13 already hold 32
and 64 beats, but the pass drops any slot above 9, and the touch row's routine
that releases its keys clears slots 0..9 only; that routine is reached from the
panel task, so it stays as it is. Rewriting the beat counts of rows 2..5
instead leaves every key on its own slot.
"""
from sigpatch import DataPatch, Edit

# Rows 6..13 of the table: 8, 16, 32 and 64 beats, each back then forward.
LONG_ROWS = bytes.fromhex('0800000001000000080000000000000010000000010000001000000000000000'
                          '2000000001000000200000000000000040000000010000004000000000000000')

MODS = {
    'beatjumplong': DataPatch(
        what='beat jumps of 2 and 4 beats lengthened to 32 and 64',
        target='main',
        fw_versions=('1.87',),
        table=LONG_ROWS,
        before=0x20,
        reach=0x20,
        edits=(Edit(bytes.fromhex('0200000001000000020000000000000004000000010000000400000000000000'),
                    bytes.fromhex('2000000001000000200000000000000040000000010000004000000000000000')),),
    ),
}
