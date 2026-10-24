# SPDX-License-Identifier: GPL-2.0-or-later
"""cdj3000x_beat on the CDJ-2000NXS2's MAIN 1.87: one hook after the CDJ-3000X
report translation that fills the beat segment fields of the control report."""
from players.cdj2000nxs2 import BEAT_GRID, HIDO_BUFFER, HIDO_READ_DONE, HIDO_STATE, HS_AUTH
from sigpatch import Mod

MODS = {
    # Reads the control report once x3000rxview (cdj3000x) has put it in the
    # NXS2 layout.
    'x3000segments': Mod(
        what='beat segment fields of the control report from the beat grid',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(HIDO_BUFFER, HIDO_STATE, HS_AUTH, BEAT_GRID),
        hook=True,
        after=('x3000rxview',),
        **HIDO_READ_DONE,
    ),
}
