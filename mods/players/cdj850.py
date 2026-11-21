# SPDX-License-Identifier: GPL-2.0-or-later
"""What the CDJ-850's mods share: the shape of its MAIN and DSP images (v1.13)."""
from dsppatch import DspShiProfile
from sigpatch import Cave, ImageProfile

# MAIN runs in place from flash 0x0C000000 on the big-endian SH7263 (SH-2A).
# Erased flash in the image is sparse, so routines go into six unused debug
# shell handlers (1,118 bytes) that only the shell command table points to.
MAIN_PROFILE = ImageProfile(
    flash_base=0x0C000000, loadtab_off=0, loadtab_n=0,
    ram_base=None, ram_end=None, byteorder='>',
    cave=Cave(
        handlers=(b'show semaphore info', b'show event flag info',
                  b'show data queue info', b'show mail box info',
                  b'show mutex info'),
        end=b'show message buffer info',
        sha256='891c18aac15fe3179e6b6f72df866a24c9bdb443359cfdd63fe2465e0ee6bba8',
    ),
)

# The DSP56371 program MAIN uploads over SHI as four contiguous big-endian
# segments (0x0C155B28..0x0C1593A1, 0x3879 bytes). Segments 1-3 are stage
# loaders that re-enter the DSP bootstrap ROM via JMP $FF0000; segment 4 holds
# the main application with unused zero vector space P:0080..P:00FE.
DSP_PROFILE = DspShiProfile(
    head=(0x000009, 0x000000, 0x300013, 0x04D03F),
    length=0x3879,
    seg_count=4,
    free=(0x0080, 0x00FE),
)
