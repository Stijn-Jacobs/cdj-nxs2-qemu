# SPDX-License-Identifier: GPL-2.0-or-later
"""What the XDJ-700's mods share: the shape of its MAIN image (v1.15)."""
from dsppatch import DspProfile
from sigpatch import Cave, ImageProfile

from players import cdj2000nxs2

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

# The firmware stores its own MAC with the same routine as the CDJ-2000NXS2, so
# the NXS2's anchor finds this image's MAC buffer (launcher/sites.py).
LAUNCHER = dict(own_mac=cdj2000nxs2.LAUNCHER['own_mac'])

# The DSP program MAIN copies to the C6747's SDRAM at 0xC0000000 (0x5F4C0 bytes
# at 0xE770 of the unpacked image), found by its first eight words. The run of
# 0xFF at 0xC0051E00 is erased and the program does not point into the part of it a mod may use.
DSP_PROFILE = DspProfile(
    load=0xC0000000, length=0x5F4C0,
    head=(0x000ED02A, 0x0008C06A, 0x00000362, 0x004003E2, 0x028003A2, 0x0C6E4C6E, 0x0C6E0C6E, 0xEC000000),
    free=(0xC0051E00, 0xC0058000))
