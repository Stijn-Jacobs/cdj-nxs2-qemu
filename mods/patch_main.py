#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply code mods to the unpacked MAIN firmware image.

    usage: patch_main.py <main_unpacked.bin> <patched.bin> <mod> [<mod>...]
           patch_main.py --flash <flash.bin> <patched-flash.bin> <mod> [<mod>...]
           patch_main.py --assemble     rebuild every mod's .bin from its .s
           patch_main.py --list

Same shape as patch_gui.py (see that file for the general mechanism), on the
MAIN image instead of the display one. Two differences follow from MAIN being
executed in place rather than scattered to RAM by a load table:

  1. There is no scatter-load record to add: a mod's routine is written
     directly into a run of erased flash within MAIN's own address space
     (see sigpatch.place_inplace()), which is both its storage and its
     execution address.
  2. --flash patches the MAIN application's packed image directly inside a
     flat NOR/flash buffer (extract/flash.bin, or a deck's own copy), at the
     flash offset make_flash.py places it -- for booting a patched MAIN
     through Pioneer's own bootloader (MAIN_BOOT=flash) rather than via
     -kernel. The window each update is allowed to write is erased first, the
     same way the real updater erases before it writes, so a shorter or
     longer re-encoded stream never leaves stale bytes past its own end.

MAIN is a little-endian SH-4 (the GUI's SH7269 is big-endian), so every mod's
sig/args/literal is still given as the instruction's logical value -- see
sigpatch.ImageProfile's byteorder field, which does the translation.

MAIN mods are hooks (Mod.hook): several of them can sit on one firmware
site, which is defined once below and shared by every mod on it. The patch
engine moves the site's span into one trampoline that runs it once and then
calls each selected mod's routine in registry order -- see
sigpatch.trampoline() for the code and the contract a hook routine follows.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, '..', 'scripts', 'firmware')
sys.path.insert(0, HERE)
sys.path.insert(0, FIRMWARE)
import sigpatch
from sigpatch import Mod
import main_decode
import main_encode
from make_flash import MAIN_END as FLASH_WINDOW_END

# The 938,112-byte run of erased flash at file offset 0x11AF80 (address
# 0x0811AF80-0x08200000), found by the same erased-flash scan
# that found the GUI board's cave. Confirmed here, independently of that
# scan, by decoding every mov.l/mov.w @(disp,PC) instruction in the whole
# image (the same check sigpatch.literal_pool_reads() runs before every
# patch) and finding none of their targets inside this range.
MAIN_PROFILE = sigpatch.ImageProfile(
    flash_base=0x08000000, loadtab_off=0, loadtab_n=0,
    ram_base=0x0811AF80, ram_end=0x08200000, byteorder='<')

# The beat loop's call to the UDP send wrapper: the wrapper's literal, the
# port store into the send descriptor at r15+0x150, the descriptor pointer
# into r5 and the call itself with its delay slot. The beat packet the call
# sends stays reachable after it at *(r15+0x218), r12 still holds the
# wrapper and r10 the fpscr mask the firmware applies before every call.
BEAT_SEND = dict(
    sig=[0x0FC4, 0x7004, 0xDC2E, 0x0F26, 0xE254, 0x4208, 0x352C, 0x026A,
         0x22A9, 0x4C0B, 0x426A, 0x026A, 0xD62A],
    start=2,
    end=11,
)

MODS = {
    'oscbeat': Mod(
        what='OSC beat output (UDP 50010)',
        target='main',
        fw_versions=('1.87',),
        args=[],
        literal=False,
        hook=True,
        **BEAT_SEND,
    ),
}


def source(name):
    return os.path.join(HERE, 'main_%s.s' % name)


def blob_path(name):
    return os.path.join(HERE, 'main_%s.bin' % name)


def patch(data, names):
    return sigpatch.patch(MAIN_PROFILE, blob_path, data, MODS, names, tag='patch_main')


def patch_flash(flash, names):
    """Patch the packed MAIN application directly inside a flat flash image
    (flash.bin's own bytes, not the unpacked one -kernel loads)."""
    _, image = main_decode.decode_image(flash)
    patched = patch(image, names)
    packed = main_encode.encode_image(patched)
    if len(packed) > FLASH_WINDOW_END - main_decode.MAIN_IMG_OFF:
        raise SystemExit('patch_main: patched image (%d bytes) does not fit '
                         'the flash window' % len(packed))
    out = bytearray(flash)
    lo, hi = main_decode.MAIN_IMG_OFF, FLASH_WINDOW_END
    out[lo:hi] = b'\xff' * (hi - lo)      # erase the window, as the real updater does
    out[lo:lo + len(packed)] = packed
    return bytes(out)


def assemble():
    with tempfile.TemporaryDirectory() as tmp:
        for name in MODS:
            obj = os.path.join(tmp, name + '.o')
            subprocess.run(['sh4-linux-gnu-as', '--isa=sh4', '-little',
                            '-o', obj, source(name)], check=True)
            subprocess.run(['sh4-linux-gnu-objcopy', '-O', 'binary',
                            obj, blob_path(name)], check=True)
            print('patch_main: wrote %s (%d bytes)'
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
    if args[:1] == ['--flash']:
        args = args[1:]
        flasher = True
    else:
        flasher = False
    if len(args) < 3 or any(n not in MODS for n in args[2:]):
        raise SystemExit(__doc__)
    with open(args[0], 'rb') as f:
        data = f.read()
    out = patch_flash(data, args[2:]) if flasher else patch(data, args[2:])
    with open(args[1], 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    main()
