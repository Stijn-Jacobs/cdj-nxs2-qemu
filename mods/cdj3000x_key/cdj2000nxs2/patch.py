# SPDX-License-Identifier: GPL-2.0-or-later
"""cdj3000x_key on the CDJ-2000NXS2's MAIN 1.87: a hook on the host-report
router that hands Serato's key of the loaded track to the report framework as
the musical key message the deck already takes."""
from players.cdj2000nxs2 import HS_AUTH, REPORT_FRAMEWORK, REPORT_ROUTER_ENTRY
from sigpatch import Mod

MODS = {
    # Runs after x3000hs, which hooks the same entry and answers the handshake
    # first.
    'x3000key': Mod(
        what='track key from Serato as the musical key report',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(HS_AUTH, REPORT_FRAMEWORK),
        hook=True,
        after=('x3000hs',),
        **REPORT_ROUTER_ENTRY,
    ),
}
