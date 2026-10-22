#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply code mods to the unpacked MAIN firmware image.

    usage: patch_main.py [--model <id>] <main_unpacked.bin> <patched.bin> <mod> [<mod>...]
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

Each mod declares its routines in its own folder, mods/<mod>/<model>/patch.py
(a MODS dict of sigpatch.Mod), next to their assembly; what several mods of a
player share (the image profile, the hook sites and imports more than one of
them use) is in mods/players/<model>.py. Most MAIN mods are hooks (Mod.hook):
several of them can sit on one firmware site, which is defined once in the
player's file and shared by every mod on it. The patch engine moves the
site's span into one trampoline that runs it once and then calls each selected
mod's routine in registry order -- see sigpatch.trampoline() for the code and
the contract a hook routine follows.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.join(HERE, '..', 'scripts', 'firmware')
sys.path.insert(0, HERE)
sys.path.insert(0, FIRMWARE)
import modelfw
import sigpatch
import main_decode
import main_encode

# The MAIN patches of each model: the routines its mod folders declare, placed
# the way its player's profile says.
PATCH_SETS = {model: sigpatch.patch_set(modelfw.player(model).MAIN_PROFILE, mods)
              for model, mods in modelfw.declared_mods('main').items()}
MODS = PATCH_SETS[modelfw.DEFAULT_MODEL].mods
TABLES = PATCH_SETS[modelfw.DEFAULT_MODEL].tables


def mods_for(model):
    """Every patch of `model`: its routines, and the rewrites of bytes the firmware already has."""
    return {**PATCH_SETS[model].mods, **PATCH_SETS[model].tables} if model in PATCH_SETS else {}


def every_mod():
    return {name for model in PATCH_SETS for name in mods_for(model)}


def source(name, model=modelfw.DEFAULT_MODEL):
    """Each routine sits in its mod's folder for its player, mods/<mod>/<model>/."""
    return modelfw.routine_source('main', name, model)


def blob_path(name, model=modelfw.DEFAULT_MODEL):
    return source(name, model)[:-2] + '.bin'


def patch(data, names, model=modelfw.DEFAULT_MODEL, report=None, spans=None):
    return sigpatch.patch_model(PATCH_SETS, blob_path, data, names, model, 'patch_main', report, spans)


def patch_flash(flash, names, model=modelfw.DEFAULT_MODEL, report=None):
    """Patch the packed MAIN application directly inside a flat flash image
    (flash.bin's own bytes, not the unpacked one -kernel loads)."""
    off = modelfw.load(model).main_off
    window_end = modelfw.update_window_end(model)
    if window_end is None:
        raise SystemExit('patch_main: the update window of %s is not known' % model)
    _, image = main_decode.decode_image(flash, off)
    patched = patch(image, names, model, report)
    packed = main_encode.encode_image(patched)
    if len(packed) > window_end - off:
        raise SystemExit('patch_main: patched image (%d bytes) does not fit '
                         'the flash window' % len(packed))
    out = bytearray(flash)
    lo, hi = off, window_end
    out[lo:hi] = b'\xff' * (hi - lo)      # erase the window, as the real updater does
    out[lo:lo + len(packed)] = packed
    return bytes(out)


def assemble():
    with tempfile.TemporaryDirectory() as tmp:
        for model, patches in PATCH_SETS.items():
            for name in patches.mods:
                obj = os.path.join(tmp, name + '.o')
                subprocess.run(['sh4-linux-gnu-as', '--isa=sh4', '-little',
                                '-I', os.path.dirname(source(name, model)),
                                '-o', obj, source(name, model)], check=True)
                subprocess.run(['sh4-linux-gnu-objcopy', '-O', 'binary',
                                obj, blob_path(name, model)], check=True)
                print('patch_main: wrote %s (%d bytes)'
                      % (blob_path(name, model), os.path.getsize(blob_path(name, model))))


def main():
    model, args = modelfw.model_id(sys.argv[1:])
    if args == ['--assemble']:
        assemble()
        return
    if args == ['--list']:
        for name, mod in mods_for(model).items():
            print('%-8s %s' % (name, mod.what))
        return
    if args[:1] == ['--flash']:
        args = args[1:]
        flasher = True
    else:
        flasher = False
    if len(args) < 3 or any(n not in every_mod() for n in args[2:]):
        raise SystemExit(__doc__)
    with open(args[0], 'rb') as f:
        data = f.read()
    out = patch_flash(data, args[2:], model) if flasher else patch(data, args[2:], model)
    with open(args[1], 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    main()
