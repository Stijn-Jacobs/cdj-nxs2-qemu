#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply code mods to the unpacked display (GUI) firmware image.

    usage: patch_gui.py [--model <id>] <gui_unpacked.bin> <patched.bin> <mod> [<mod>...]
           patch_gui.py --assemble     rebuild every mod's .bin from its .s
           patch_gui.py --check        fail if a committed .bin is not its .s assembled
           patch_gui.py --list

Each mod is a routine written in SH-2A assembly (gui_<mod>.s, assembled to
gui_<mod>.bin, which is kept next to it so patching needs no cross tools) and
a stretch of firmware code it replaces with a call to that routine, declared
beside them in mods/<mod>/<model>/patch.py; the display image's profile is in
mods/players/<model>.py. The patch is made to the image, the way a firmware
update would carry it:

  1. The routines go into the erased flash right after the on-chip RAM
     segments' source, and one new load-table record copies them to on-chip
     RAM past the end of the zero-filled area, where nothing in the image
     points.
  2. Each mod's call site is found by its instructions, not its address, and
     the image is left alone unless every site occurs exactly once.

A mod whose site is not found, or whose space is taken, is left out and
reported; the rest still go in, and an image that already carries a mod gets
it left as it is. --model picks the player (default cdj2000nxs2); a model
with no patch of a mod reports it as not applied.

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
import modelfw
import sigpatch

# The display firmware patches of each model: the routines and byte rewrites
# its mod folders declare. Only the CDJ-2000NXS2's display processor is
# covered; the older decks' displays are not.
PATCH_SETS = {model: sigpatch.patch_set(modelfw.player(model).GUI_PROFILE, mods)
              for model, mods in modelfw.declared_mods('gui').items()}
MODS = PATCH_SETS[modelfw.DEFAULT_MODEL].mods
TABLES = PATCH_SETS[modelfw.DEFAULT_MODEL].tables


def mods_for(model):
    """Every patch of `model`: its routines, and the rewrites of bytes the firmware already has."""
    return {**PATCH_SETS[model].mods, **PATCH_SETS[model].tables} if model in PATCH_SETS else {}


def every_mod():
    return {name for model in PATCH_SETS for name in mods_for(model)}


def source(name, model=modelfw.DEFAULT_MODEL):
    """Each routine sits in its mod's folder for its player, mods/<mod>/<model>/."""
    return modelfw.routine_source('gui', name, model)


def blob_path(name, model=modelfw.DEFAULT_MODEL):
    return source(name, model)[:-2] + '.bin'


def patch(data, names, model=modelfw.DEFAULT_MODEL, report=None, spans=None):
    return sigpatch.patch_model(PATCH_SETS, blob_path, data, names, model, 'patch_gui', report, spans)


def _assemble(name, model, tmp, out):
    obj = os.path.join(tmp, name + '.o')
    subprocess.run(['sh4-linux-gnu-as', '--isa=sh2a', '-big',
                    '-o', obj, source(name, model)], check=True)
    subprocess.run(['sh4-linux-gnu-objcopy', '-O', 'binary', obj, out], check=True)


def routines():
    return [(model, name) for model, patches in PATCH_SETS.items() for name in patches.mods]


def assemble():
    with tempfile.TemporaryDirectory() as tmp:
        for model, name in routines():
            _assemble(name, model, tmp, blob_path(name, model))
            print('patch_gui: wrote %s (%d bytes)'
                  % (blob_path(name, model), os.path.getsize(blob_path(name, model))))


def check():
    """Every committed .bin is what its .s assembles to now: a .s edited
    without --assemble would otherwise ship its old routine."""
    stale = []
    with tempfile.TemporaryDirectory() as tmp:
        for model, name in routines():
            out = os.path.join(tmp, name + '.bin')
            _assemble(name, model, tmp, out)
            with open(out, 'rb') as f, open(blob_path(name, model), 'rb') as g:
                same = f.read() == g.read()
            print('patch_gui: %s %s' % (os.path.basename(blob_path(name, model)),
                                        'matches its .s' if same else 'is STALE'))
            if not same:
                stale.append(name)
    if stale:
        raise SystemExit('patch_gui: rebuild with patch_gui.py --assemble: %s' % ' '.join(stale))


def main():
    model, args = modelfw.model_id(sys.argv[1:])
    if args == ['--assemble']:
        assemble()
        return
    if args == ['--check']:
        check()
        return
    if args == ['--list']:
        for name, mod in mods_for(model).items():
            print('%-8s %s' % (name, mod.what))
        return
    if len(args) < 3 or any(n not in every_mod() for n in args[2:]):
        raise SystemExit(__doc__)
    with open(args[0], 'rb') as f:
        data = f.read()
    out = patch(data, args[2:], model)
    with open(args[1], 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    main()
