# SPDX-License-Identifier: GPL-2.0-or-later
"""What the CDJ-2000NXS's mods share: the shape of its MAIN image (v1.44)."""
from sigpatch import ImageProfile

from players import cdj2000nxs2

# MAIN is unpacked to DRAM at 0x04000000 and runs from there.
MAIN_PROFILE = ImageProfile(
    flash_base=0x04000000, loadtab_off=0, loadtab_n=0,
    ram_base=None, ram_end=None, byteorder='<')

# The RTOS keeps SR in the same routine as on the CDJ-2000NXS2, and the
# firmware stores its own MAC with the same routine, so the NXS2's anchors find
# this image's slot and MAC buffer (launcher/sites.py).
LAUNCHER = dict(sr_save_slot=cdj2000nxs2.LAUNCHER['sr_save_slot'],
                own_mac=cdj2000nxs2.LAUNCHER['own_mac'])
