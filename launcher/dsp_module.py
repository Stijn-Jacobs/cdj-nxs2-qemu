# SPDX-License-Identifier: GPL-2.0-or-later
"""The stamp beside an installed DSP module (m.stamp) and the one comparison
setup and start share. The stamp names what the module was built from:

  generator  c14_jitgen.py's GENERATOR_VERSION; the generated C changed meaning
  abi        C66X_JIT_ABI, the core structs the module is compiled against
  firmware   the sha256 of the player's main_unpacked.bin, the DSP program's source
  model      the player

The core refuses a module of another ABI by itself and every compiled region
checks its code bytes, so a module of another firmware only loses speed. A
module of another generator version is the one that could compute wrongly, and
the core cannot tell. Start-up therefore loads none of the three kinds of
mismatch, and loads a module with no stamp (built before stamps existed) with a
banner.

  python -m launcher dsp-stamp <folder>   write the stamp of CDJ_MODEL's module
"""

import collections
import os
import re
import sys

from . import model
from .layout import Layout

STAMP = "m.stamp"

Module = collections.namedtuple("Module", "path notice")


def _constant(path, pattern):
    with open(path, encoding="utf-8") as f:
        return re.search(pattern, f.read(), re.MULTILINE).group(1)


def current(lay, m):
    """What a module built now would be stamped with."""
    c6x = os.path.join(lay.emu, "hw", "cdj", "c6x")
    return {"generator": _constant(os.path.join(c6x, "tools", "c14_jitgen.py"), r"^GENERATOR_VERSION = (\d+)"),
            "abi": _constant(os.path.join(c6x, "c66x_core_int.h"), r"^#define C66X_JIT_ABI (\d+)"),
            "firmware": next(sha for rel, sha in m.expected if rel.endswith("main_unpacked.bin")),
            "model": m.id}


def write(folder, stamp):
    with open(os.path.join(folder, STAMP), "w", encoding="utf-8", newline="\n") as f:
        f.writelines("%s=%s\n" % kv for kv in stamp.items())


def differences(folder, stamp):
    """The stamp keys that differ between the module in `folder` and `stamp`;
    None when it has no stamp."""
    try:
        with open(os.path.join(folder, STAMP), encoding="utf-8") as f:
            built = dict(line.rstrip("\n").split("=", 1) for line in f if "=" in line)
    except OSError:
        return None
    return [k for k in stamp if built.get(k) != stamp[k]]


def build_command(lay, m):
    return "the program's setup" if lay.packaged else "./setup.sh --model %s" % m.id


def check(lay, m):
    """The model's installed module: its path when start-up may load it, and
    the banner lines to show, if any."""
    folder = os.path.join(lay.jit_cache, m.module_dir)
    path = os.path.join(folder, "m.so")
    build = "Rebuild it with %s (about 20-40 minutes)." % build_command(lay, m)
    if not os.path.isfile(path):
        return Module("", ("No DSP module for the %s yet: its DSP runs far below real time" % m.title,
                           "and the sound will gap. Build it once with %s" % build_command(lay, m),
                           "(about 20-40 minutes); mods do not affect it."))
    differ = differences(folder, current(lay, m))
    if differ is None:
        return Module(path, ("The DSP module for the %s was built before modules were stamped, so it" % m.title,
                             "cannot be checked against this code. " + build))
    if differ:
        return Module("", ("The DSP module for the %s was built from another %s and is not loaded;"
                           % (m.title, ", ".join(differ)), "its DSP runs far below real time. " + build))
    return Module(path, ())


def main(argv):
    if len(argv) != 1 or not os.path.isdir(argv[0]):
        sys.stderr.write("usage: python -m launcher dsp-stamp <folder with the module>\n")
        return 2
    write(argv[0], current(Layout(), model.load()))
    return 0
