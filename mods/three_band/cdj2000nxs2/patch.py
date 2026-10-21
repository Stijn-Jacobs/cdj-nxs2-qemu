# SPDX-License-Identifier: GPL-2.0-or-later
"""three_band on the CDJ-2000NXS2: four MAIN routines (1.87) that fetch the
track's PWV7/PWV6 and write them into what MAIN sends the display, and two
display routines (1.81) that draw them."""
from players.cdj2000nxs2 import (FREE, MALLOC, MEMSET, OVERVIEW_PUBLISH, OVERVIEW_SLOT,
                                 PREVIEW_READ, TAG_FETCH)
from sigpatch import Anchor, FreeEnd, Mod

# The waveform requester's success path after the firmware has re-encoded the
# builder's PWV5 record: the free of the builder's record, then the stores of
# the new record and its size into the shared context (0x0B531EB8/0x0B531EBC).
# The hooks run after the stores and before the record is published. The only
# branch into the span is the `bf` just before it, which lands on its first
# halfword.
DETAIL_CONVERTED = dict(
    sig=[0x8B08, 0xD67E, 0x4A0B, 0x2F66, 0x096A, 0x57F7, 0x2979, 0x496A, 0xA00C,
         0x7F04, 0xD178, 0xDA7A, 0x4A0B, 0x5417, 0xD276, 0x0B6A, 0x5CF4, 0x5DF3,
         0x2BE9, 0x12C7, 0x4B6A, 0x12D8, 0x65F2, 0xD675, 0x460B],
    start=10,
    end=22,
)

ANLZ_PWV5_READ = (0x08289E98, Anchor(sig=[0x1F12, 0xD128, 0x66F3, 0x55FC, 0xE400, 0x1F41], index=1, literal=True))  # PWV5/EXT reader
ANLZ_CLOSE = (0x0827A34E, Anchor(sig=[0x4D0B, 0x456A, 0x076A, 0xD119, 0x64C3, 0x27E9], index=3, literal=True))      # ANLZ close-handle call
TAG_HELPERS = ((0x085336DC, Anchor(sig=[0xE0FE, 0xD777, 0x65D3, 0x64F3, 0x7504, 0x7404], index=1, literal=True)),
               (0x085341D4, Anchor(sig=[0x0009, 0xD79E, 0x64B3, 0xD59E, 0x470B, 0xE604], index=1, literal=True)))
DETAIL_RECORD = (0x0B531EB8, Anchor(sig=[0x4E0B, 0x476A, 0x0A6A, 0xD46B, 0x1BD3, 0x1BD4], index=3, literal=True, delta=0x8))

# The words handed from one routine to the other: wave3data's buffer to
# wave3detail, wave3ovfetch's to wave3ovdata.
DETAIL_BUFFER = (0x081FFFF4, FreeEnd(-12))
OVERVIEW_BUFFER = (0x081FFFFC, FreeEnd(-4))

MODS = {
    # The detail-waveform builder's own PWV5 fetch, right after it returns:
    # r15+8/+12 still hold the &out_ptr/&out_size the fetch wrote into (read
    # again a few instructions later, once this call site resumes, to fill
    # the shared ctx's own RGB fields), r10 still holds the fetch's hash
    # argument, and the ANLZ handle this call opened is at r15+16 -- all of
    # it live only in this one span, which is why the hook sits exactly
    # here rather than earlier or later in the same function. r6/r7/r10 and
    # the bsr are reproduced first, so the RGB path is untouched whether or
    # not this mod's own fetches below succeed.
    'wave3data': Mod(
        what='3-band detail waveform data fetch (PWV7)',
        target='main',
        fw_versions=('1.87',),
        sig=[0x67F3, 0x66F3, 0x7708, 0x65A3, 0x760C, 0xDA18, 0xBC7B, 0x1FE5],
        start=0,
        end=8,
        args=[],
        literal=False,
        imports=(ANLZ_PWV5_READ, ANLZ_CLOSE, MALLOC, FREE, DETAIL_BUFFER, TAG_FETCH, MEMSET) + TAG_HELPERS,
    ),
    # Hooks DETAIL_CONVERTED: writes the words the wave3data mod packed into the
    # re-encoded waveform record, reading its buffer through two words at fixed
    # addresses (see main_wave3data.s).
    'wave3detail': Mod(
        what='3-band detail waveform data (PWV7 in the waveform record)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(DETAIL_BUFFER, DETAIL_RECORD),
        hook=True,
        **DETAIL_CONVERTED,
    ),
    # Hooks OVERVIEW_PUBLISH: writes the PWV6 bands the wave3ovfetch mod kept
    # into the overview payload, reading that mod's buffer through a word at a
    # fixed address (see main_wave3ovfetch.s).
    'wave3ovdata': Mod(
        what='3-band overview data (PWV6 in the overview payload)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(OVERVIEW_BUFFER, OVERVIEW_SLOT),
        hook=True,
        **OVERVIEW_PUBLISH,
    ),
    # Hooks PREVIEW_READ: reads and packs the track's PWV6 for wave3ovdata.
    'wave3ovfetch': Mod(
        what='3-band overview data fetch (PWV6)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        imports=(MALLOC, FREE, OVERVIEW_BUFFER, TAG_FETCH, MEMSET) + TAG_HELPERS,
        hook=True,
        **PREVIEW_READ,
    ),
    # The RGB branch of the centre-waveform renderer, from the height clamp to
    # the end of the column fill. r5 = peak sample index, r8 = PWV5 array,
    # r12 = height, @(44,r15) = screen column; the canvas is the literal.
    'wave3': Mod(
        what='3-band centre waveform',
        target='gui',
        fw_versions=('1.81',),
        sig=[
            0x2CC8, 0x8B00, 0xEC01, 0x4500, 0xEB07, 0x6053, 0x058D, 0x0A00,
            0xE000, 0x4B18, 0x605D, 0xE9FB, 0x6103, 0xE400, 0x4121, 0x4121,
            0x21B9, 0x6603, 0x26A9, 0x409C, 0x67C3, 0x77FF, 0x316C, 0xC91C,
            0x3C77, 0x8F22, 0x310C, 0x4715, 0x8B12, 0x56FB, 0xD28A, 0x4600,
            0x0510, 0x9E00, 0x0910, 0xA400, 0x362C, 0x356C, 0x369C, 0x7402,
            0xE2F4, 0x4218, 0x3473, 0x2611, 0x362C, 0x2511, 0x8FF7, 0x352C,
            0x34C3, 0x891A, 0x60E3, 0x0710, 0xA400, 0x4480, 0x50FB, 0xD27E,
            0x4000, 0x3748, 0x307C, 0xA010, 0x0215, 0x4C15, 0x8B0D, 0x52FB,
            0x66C3, 0xD479, 0x4200, 0x0710, 0xA400, 0x324C, 0x327C, 0xE7FA,
            0x4610, 0x4718, 0x2211, 0x8FFA, 0x327C, 0x5BFA, 0xE00C,
        ],
        start=3,            # `shll r5`, after the height clamp
        end=77,             # up to `mov.l @(40,r15),r11`
        # mov.l @(44,r15),r4; mov r12,r6; mov r8,r7 (r5 is already the index)
        args=[0x54FB, 0x66C3, 0x6783],
        literal=True,
    ),
    # The colour-preview strip renderer's per-column record decode (r5 = the
    # record, r8 = the column, r6 = -4): the two heights read from record
    # bytes 4-5, up to the `tst r2,r2` that tests the first. The hook decodes
    # the same way and returns, or paints a 3-band record and returns r2 = 0,
    # which sends the renderer to the end of the column.
    'wave3ov': Mod(
        what='3-band overview strip',
        target='gui',
        fw_versions=('1.81',),
        sig=[
            0xE006, 0x6583, 0x4580, 0xD421, 0xE6FC, 0x354C, 0x3751, 0x8004,
            0x3051, 0x9002, 0x4709, 0x406D, 0x627C, 0xC93F, 0x2228, 0x8F02,
        ],
        start=6,            # `movu.b @(4,r5),r7`
        end=14,             # up to `tst r2,r2`
        args=[],
        literal=False,
    ),
}
