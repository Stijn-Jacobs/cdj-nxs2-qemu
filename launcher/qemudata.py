# SPDX-License-Identifier: GPL-2.0-or-later
"""The data files QEMU looks for beside its binary, and the ones it cannot start
without.

QEMU's VNC server refuses to come up when it cannot read its keymaps, and the
virtual deck app's screen *is* that VNC server: the failure shows as a deck that
never draws, not as an error anyone reads. A QEMU run from its build tree looks
in ``qemu-bundle/``, and a development build leaves the keymaps there as links
into a source tree it never installed, so the links dangle and the screen stays
black. A packaged QEMU looks in its own data directory, relative to the binary.

`ensure_keymaps` copies real files over whatever is there, so both a development
build and a packaged one end up with keymaps QEMU can read.
"""

import glob
import os
import shutil
import subprocess
import sys


def data_dir(exe):
    """Where this QEMU looks for its data files, as it says itself: the last of
    `-L help`'s lines. That is ../share/qemu for a macOS or Linux build,
    share/ beside the .exe for a Windows one."""
    out = subprocess.run([exe, "-L", "help"], capture_output=True, text=True, check=True).stdout
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    if not lines:
        sys.exit("%s -L help names no data directory" % exe)
    return os.path.normpath(lines[-1])


def keymap_sources(eb_dir, source_tree=None):
    """Every directory that may hold keymaps, best last: the bundle's copies,
    then the source tree's ready-made ones, which stand in for any a build with
    xkbcommon has not generated yet."""
    dirs = glob.glob(os.path.join(eb_dir, "qemu-bundle", "**", "share", "qemu", "keymaps"),
                     recursive=True)
    if source_tree:
        dirs.append(os.path.join(source_tree, "pc-bios", "keymaps"))
    return dirs


def find_keymaps(eb_dir, source_tree=None):
    """The keymap files that really exist, by name. A symlink counts only when
    it resolves to a file: that is the case this module exists for."""
    maps = {}
    for d in reversed(keymap_sources(eb_dir, source_tree)):
        if not os.path.isdir(d):
            continue
        for name in os.listdir(d):
            path = os.path.join(d, name)
            if "." not in name and os.path.isfile(path):
                maps[name] = path
    return maps


def ensure_keymaps(exe, eb_dir, source_tree=None, datadir=None):
    """Copy the keymaps into the data directory `exe` reads.

    Returns the names written. Raises RuntimeError when no usable `en-us` is
    found, which is the keymap QEMU's VNC server needs to start at all.
    """
    maps = find_keymaps(eb_dir, source_tree)
    if "en-us" not in maps:
        raise RuntimeError("no QEMU keymap en-us in %s"
                           % ", ".join(keymap_sources(eb_dir, source_tree)))
    dst = os.path.join(datadir or data_dir(exe), "keymaps")
    os.makedirs(dst, exist_ok=True)
    written = []
    for name, path in maps.items():
        target = os.path.join(dst, name)
        # A dangling link is what a dev build leaves behind; unlink first so the
        # copy cannot write through it into wherever it used to point.
        if os.path.islink(target):
            os.unlink(target)
        elif os.path.exists(target) and os.path.samefile(path, target):
            written.append(name)        # already in place: nothing to do
            continue
        shutil.copyfile(path, target)
        written.append(name)
    return sorted(written)
