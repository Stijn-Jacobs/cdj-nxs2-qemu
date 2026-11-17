# SPDX-License-Identifier: GPL-2.0-or-later
"""What the mod tools need to know about one model's firmware, read from its
profile (models/<id>.conf): which sections of its update hold the images a mod
can patch, and where the packed MAIN application starts in the update's
address space; and which routines the mod folders declare for it.
"""
import collections
import functools
import glob
import importlib
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
from launcher import model as cdj_model

DEFAULT_MODEL = cdj_model.DEFAULT


def update_window_end(model):
    """Where the updater of `model` stops erasing and programming the MAIN
    region of a public update (label byte 31 = 0), from its profile; None while
    the updater has not been read. The end is the first sector the updater's
    erase routine does not reach: it loops over a table of sector offsets in
    the flash driver, starting at the entry of the packed image (main_lzss - 4),
    for a fixed count, and the count is a "mov #n" in front of the table
    pointer. The sectors are 64 KiB, except 128 KiB on the XDJ-1000 and MK2."""
    return cdj_model.load(model).update_window_end


Firmware = collections.namedtuple('Firmware', 'sections main_off bare', defaults=(False,))


def parse_manifest(data):
    """(section sizes, header length) of an update file: the manifest is one CRLF
    decimal per section, giving its size."""
    sizes, pos = [], 0
    while True:
        m = re.match(rb'(\d+)\r\n', data[pos:pos + 32])
        if not m:
            break
        sizes.append(int(m.group(1)))
        pos += m.end()
    return sizes, pos


def model_id(args):
    """(model id, args without --model <id>)."""
    if '--model' not in args:
        return DEFAULT_MODEL, args
    i = args.index('--model')
    if i + 1 >= len(args):
        raise SystemExit('--model needs a model id')
    return args[i + 1], args[:i] + args[i + 2:]


def main_base(model):
    """Address the unpacked MAIN image of `model` starts at, from its profile."""
    return cdj_model.load(model).main_base


def other_files(model):
    """[(file name, sha256)] of the files a one-file-per-section update has
    besides MAIN's; none for a player whose update is one file."""
    m = cdj_model.load(model)
    if len(m.upd) < 2:
        return []
    main = int(m.main_section) - 1
    return [(name, digest) for k, (name, digest) in enumerate(zip(m.upd, m.upd_sha256)) if k != main]


def main_file(model):
    """(file name, sha256) of the official file that holds MAIN."""
    m = cdj_model.load(model)
    k = int(m.main_section) - 1 if len(m.upd) > 1 else 0
    return m.upd[k], m.upd_sha256[k]


def load(model):
    """Firmware of `model`. A GUI section is listed only for a model whose
    display firmware runs on its own board. A model whose update is one file
    per section (the CDJ-2000 and CDJ-900) is `bare`: the MAIN file is a
    single section with no manifest in front of it."""
    try:
        m = cdj_model.load(model)
    except cdj_model.ModelError as e:
        raise SystemExit(str(e))
    if len(m.upd) > 1:
        return Firmware({'main': 1}, int(m.main_lzss, 0) - 4, bare=True)
    sections = {'main': int(m.main_section)}
    if m.gui_machine:
        sections['gui'] = int(m.gui_section)
    return Firmware(sections, int(m.main_lzss, 0) - 4)


def player(model):
    """What the mods of `model` share: mods/players/<model>.py."""
    return importlib.import_module('players.' + model)


def dsp_profile(model):
    """Where MAIN of `model` carries its DSP program (dsppatch.DspProfile), or
    None for a player this is not known for."""
    try:
        return getattr(player(model), 'DSP_PROFILE', None)
    except ModuleNotFoundError:
        return None


@functools.lru_cache(maxsize=None)
def mod_patch(mod, model):
    """The routines `mod` patches into `model`: mods/<mod>/<model>/patch.py."""
    path = os.path.join(HERE, mod, model, 'patch.py')
    spec = importlib.util.spec_from_file_location('%s_%s_patch' % (mod, model), path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def declared_mods(target):
    """{model: {routine name: Mod}} of every routine a mod folder declares for
    `target` ('main' or 'gui')."""
    found = {}
    for path in sorted(glob.glob(os.path.join(HERE, '*', '*', 'patch.py'))):
        folder = os.path.dirname(path)
        mod, model = os.path.basename(os.path.dirname(folder)), os.path.basename(folder)
        routines = found.setdefault(model, {})
        for name, routine in mod_patch(mod, model).MODS.items():
            if routine.target != target:
                continue
            if name in routines:
                raise SystemExit('%s: routine %s is declared by two mods' % (model, name))
            routines[name] = routine
    return {model: routines for model, routines in found.items() if routines}


def routine_source(prefix, name, model):
    """The assembly of one mod's routine for `model`: mods/<mod>/<model>/<prefix>_<name>.s."""
    return glob.glob(os.path.join(HERE, '*', model, '%s_%s.s' % (prefix, name)))[0]
