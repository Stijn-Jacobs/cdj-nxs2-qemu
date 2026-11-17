# SPDX-License-Identifier: GPL-2.0-or-later
"""track_filter on the XDJ-700's MAIN 1.15: the long press of INFO that
switches the firmware's own BPM filter."""
from sigpatch import Anchor, Mod

# The UI task's call to the key table's initialisation (the table at
# 0x0BA39730, 16 bytes a key), with the first instructions after it: the loads
# of the init and the fpscr mask, the call with its delay slot, the first
# fpscr read and the load after it. The hook runs after the table is filled
# in, which is when the INFO key's slot can be replaced.
KEY_TABLE_INIT = dict(
    sig=[0x2FE6, 0x4F22, 0xD12F, 0xDE30, 0x410B, 0x0009, 0x046A, 0xD22F,
         0x24E9, 0x420B, 0x446A, 0x056A, 0xD72D, 0x25E9],
    start=2,
    end=8,
)

KEY_TABLE = (0x0BA39730, Anchor(sig=[0x08CF, 0xD576, 0xE28C, 0xD176, 0x602C, 0xD476], index=1, literal=True))      # 12 keys, 16 bytes each, the handler at +12
BROWSE_MODE = (0x08DB0DF4, Anchor(sig=[0xD141, 0x410B, 0x0009, 0x046A, 0xD640, 0x24E9], index=4, literal=True))    # browse mode of the UI
UPDATE_LIST = (0x08DAB83A, Anchor(sig=[0x8905, 0xD121, 0x410B, 0x0009, 0x026A, 0x22E9], index=1, literal=True))    # BROWSE_CMD UPDATE_LIST
SET_FILTER_ONOFF = (0x08B34CC0, Anchor(sig=[0xD266, 0x2118, 0x8D11, 0x656C, 0x420B, 0xE601], index=0, literal=True))  # the DB server's filter switch
SET_FILTER_CONDITION = (0x08B34D5A, Anchor(sig=[0x56E4, 0x2F66, 0xD93B, 0x57E3, 0x56E2, 0x55E1], index=2, literal=True))
LIST_REFRESH_CONDITIONS = (0x08CE4FD0, Anchor(sig=[0xD237, 0x65E2, 0x6D03, 0xD431, 0x1F98, 0x2199], index=0, literal=True))
LIST_REFRESH_SWITCH = (0x08CE5068, Anchor(sig=[0xD163, 0x6D03, 0x65E2, 0xD463, 0x2929, 0x1F2A], index=0, literal=True))
LIST_CONTEXT = (0x0B0E0E98, Anchor(sig=[0xD44E, 0x470B, 0xE600, 0x096A, 0xD14D, 0x64F3], index=0, literal=True))   # the list server's context
# the USB filter's state block is at 0x09603674; the routine uses its field at +0xE8
FILTER_STATE = (0x0960375C, Anchor(sig=[0xD16F, 0xE805, 0xE900, 0xDA71, 0x6613, 0x717F], index=0, literal=True, delta=0xE8))
DECK_BPM = (0x0C0E6B74, Anchor(sig=[0xD79C, 0x0A1A, 0x36A8, 0x5472, 0x666C, 0x3E45], index=0, literal=True, delta=8))  # the deck's BPM x 10

MODS = {
    'trackfilter': Mod(
        what='BPM filter on a long press of INFO',
        target='main',
        fw_versions=('1.15',),
        args=[],
        literal=False,
        imports=(KEY_TABLE, BROWSE_MODE, UPDATE_LIST, SET_FILTER_ONOFF, SET_FILTER_CONDITION,
                 LIST_REFRESH_CONDITIONS, LIST_REFRESH_SWITCH, LIST_CONTEXT, FILTER_STATE, DECK_BPM),
        hook=True,
        **KEY_TABLE_INIT,
    ),
}
