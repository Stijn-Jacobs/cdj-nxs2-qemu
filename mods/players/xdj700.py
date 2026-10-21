# SPDX-License-Identifier: GPL-2.0-or-later
"""What the XDJ-700's mods share: the shape of its MAIN image (v1.15)."""
from sigpatch import Cave, ImageProfile

# MAIN runs in place, as on the CDJ-2000NXS2, but no run of erased flash in it
# is free: the 0xFF runs are bitmap data. Its room is four handlers of the
# kernel's debug shell (semaphore, event flag, data queue, mail box: 1,168
# bytes), which nothing calls but the shell's command table, found by their
# name strings.
MAIN_PROFILE = ImageProfile(
    flash_base=0x08000000, loadtab_off=0, loadtab_n=0,
    ram_base=None, ram_end=None, byteorder='<',
    cave=Cave(handlers=(b'show semaphore info', b'show event flag info',
                        b'show data queue info', b'show mail box info'),
              end=b'show mutex info',
              sha256='2e4dc6aa5e8400152e828e99fe4df3116d0c2f7dd5e24223df2dc31e6121e376'))

# The launcher pokes nothing in this player's MAIN (launcher/sites.py).
LAUNCHER = {}
