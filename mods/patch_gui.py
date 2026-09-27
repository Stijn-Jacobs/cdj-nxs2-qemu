#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply code mods to the unpacked display (GUI) firmware image.

    usage: patch_gui.py <gui_unpacked.bin> <patched.bin> <mod> [<mod>...]
           patch_gui.py --assemble     rebuild every mod's .bin from its .s
           patch_gui.py --list

Each mod is a routine written in SH-2A assembly (gui_<mod>.s, assembled to
gui_<mod>.bin, which is kept next to it so patching needs no cross tools) and
a stretch of firmware code it replaces with a call to that routine. The patch
is made to the image, the way a firmware update would carry it:

  1. The routines go into the erased flash right after the on-chip RAM
     segments' source, and one new load-table record copies them to on-chip
     RAM past the end of the zero-filled area, where nothing in the image
     points.
  2. Each mod's call site is found by its instructions, not its address, and
     the image is left alone unless every site occurs exactly once.

Naming several mods patches all of them in one pass, sharing one load-table
record between them: pass every mod you want in a single call. Patching them
one at a time and combining the results is not supported.

The load table at 0x10 is twenty 12-byte records in groups of four (that
shape, and everything else about how a call site is found and spliced in, is
generic -- see sigpatch.py). That reading fits every record of v1.80 and
v1.81, but the boot loader that walks the table is not in the image, so it is
inferred, not read.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sigpatch
from sigpatch import Mod

GUI_PROFILE = sigpatch.ImageProfile(
    flash_base=0x30000, loadtab_off=0x10, loadtab_n=20,
    ram_base=0x1C000000, ram_end=0x1C280000)

MODS = {
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
}


def source(name):
    return os.path.join(HERE, 'gui_%s.s' % name)


def blob_path(name):
    return os.path.join(HERE, 'gui_%s.bin' % name)


def patch(data, names):
    return sigpatch.patch(GUI_PROFILE, blob_path, data, MODS, names, tag='patch_gui')


def assemble():
    with tempfile.TemporaryDirectory() as tmp:
        for name in MODS:
            obj = os.path.join(tmp, name + '.o')
            subprocess.run(['sh4-linux-gnu-as', '--isa=sh2a', '-big',
                            '-o', obj, source(name)], check=True)
            subprocess.run(['sh4-linux-gnu-objcopy', '-O', 'binary',
                            obj, blob_path(name)], check=True)
            print('patch_gui: wrote %s (%d bytes)'
                  % (blob_path(name), os.path.getsize(blob_path(name))))


def main():
    args = sys.argv[1:]
    if args == ['--assemble']:
        assemble()
        return
    if args == ['--list']:
        for name, mod in MODS.items():
            print('%-8s %s' % (name, mod.what))
        return
    if len(args) < 3 or any(n not in MODS for n in args[2:]):
        raise SystemExit(__doc__)
    with open(args[0], 'rb') as f:
        data = f.read()
    out = patch(data, args[2:])
    with open(args[1], 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    main()
