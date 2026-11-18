# SPDX-License-Identifier: GPL-2.0-or-later
"""dsp_probe on the CDJ-850 MAIN 1.13: minimal DSP56371 SHI segment edit."""
from dsppatch import DspMod

# Segment 4 main loop has a NOP (0x000000) at P:01EA; P:0080..P:00FE is zeroed
# unused vector space. Patching P:01EA to JSR $0080 and placing `nop; nop; rts`
# at P:0080 exercises DSP56371 segment editing while keeping boot and audio intact.
MODS = {
    'dspprobe': DspMod(
        what='DSP56371 probe: main-loop JSR into free vector space',
        target='dsp',
        fw_versions=('1.13',),
        edits=((0x01EA, 0x000000, 0x0D0080),),
        code=((0x0080, 'dspprobe'),),
    ),
}
