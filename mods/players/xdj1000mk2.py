# SPDX-License-Identifier: GPL-2.0-or-later
"""What the XDJ-1000MK2's mods share: the shape of its MAIN image (v1.45)."""
from sigpatch import ImageProfile

from players import cdj2000nxs2

# MAIN runs in place, as on the CDJ-2000NXS2.
MAIN_PROFILE = ImageProfile(
    flash_base=0x08000000, loadtab_off=0, loadtab_n=0,
    ram_base=None, ram_end=None, byteorder='<')

# The firmware stores its own MAC with the same routine as the CDJ-2000NXS2, so
# the NXS2's anchor finds this image's MAC buffer (launcher/sites.py).
LAUNCHER = dict(own_mac=cdj2000nxs2.LAUNCHER['own_mac'])
