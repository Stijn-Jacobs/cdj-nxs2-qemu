# SPDX-License-Identifier: GPL-2.0-or-later
"""tempo_wide on the CDJ-850 MAIN 1.13: TEMPO cycles ±6 -> ±10 -> ±16 -> WIDE on USB."""
from sigpatch import DataPatch, Edit, Mod

# The TEMPO button handler's range step (0x0C0D8DC0..0x0C0D8DDE): after
# loading the flag *0x0CE6D25A, the current range address 0x0CE6D0E1 -> r5, and
# *r5 -> r6 (1=±6, 2=±10, 3=±16, 4=WIDE), right before testing the mode and r6.
# The routine clears saved r0 (the flag) so USB takes the CD step table
# 1->2->3->4->1 instead of USB's 1->2->3->1 (and 4->3).
# Hook span 0x0C0D8DC2..0x0C0D8DCE (12 bytes) includes the PC-relative loads and
# the SH-2A 32-bit movu.b @(0,r5),r6 that sets r6 before the trampoline saves r0-r7.
TEMPO_RANGE = dict(
    sig=[0x8B2F, 0xD776, 0xD576, 0x6070, 0xE200, 0x3651, 0x8000, 0xD175, 0x8801],
    start=1,
    end=7,
)

# In FUN_0c0d59b8, both pitch-scaling paths guard the overflow-safe WIDE
# multiplier (0x51EB851F) on *0x0CE6D25A != 1, assuming USB/data never uses WIDE.
# Rewriting those two conditional branches keeps WIDE scaling accurate on USB
# without altering *0x0CE6D25A for display/panel code.
WIDE_FALLBACK_SPAN = bytes.fromhex('e713dd786243eef4427d3d2d050a45ec60534004')
WIDE_RAMP_SPAN = bytes.fromhex('d225e7fced13eefa326d050a457c60534004c901350c45dd')

MODS = {
    'tempowide': Mod(
        what='tempo wide: USB TEMPO steps ±16% -> WIDE (CD step table)',
        target='main',
        fw_versions=('1.13',),
        args=[],
        literal=False,
        hook=True,
        **TEMPO_RANGE,
    ),
    'tempowidemath1': DataPatch(
        what='tempo wide: enable overflow-safe WIDE pitch multiplier on USB',
        target='main',
        fw_versions=('1.13',),
        table=WIDE_FALLBACK_SPAN,
        before=6,
        reach=6,
        edits=(Edit(bytes.fromhex('88018b0d'), bytes.fromhex('8801a00d')),),
    ),
    'tempowidemath2': DataPatch(
        what='tempo wide: enable overflow-safe WIDE pitch ramp on USB',
        target='main',
        fw_versions=('1.13',),
        table=WIDE_RAMP_SPAN,
        before=6,
        reach=6,
        edits=(Edit(bytes.fromhex('88018913'), bytes.fromhex('88010009')),),
    ),
}
