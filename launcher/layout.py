# SPDX-License-Identifier: GPL-2.0-or-later
"""Where everything is, in a source checkout and in the packaged program.

A checkout keeps its places, so developers and the measurement scripts see no
change: the settings in emulator/cdj.conf, the firmware images and the USB
image in <root>/extract/, the logs in emulator/logs/, the DSP code cache in
~/c14gen and the QEMUs in their build trees.

The packaged program is this same tree as <program>/app/ beside
<program>/runtime/ (Python, the QEMUs, the C compiler; runtime/portable.txt
marks it). It writes only under one data folder: <program>/data, or the
per-user application folder when the program's own folder is read-only (an
.app in /Applications, a mounted AppImage). CDJ_DATA overrides both.
"""

import os
import shlex
import sys

from . import host


def _checkout_root(emu):
    """CDJ_ROOT as scripts/cdj_paths.sh sets it: emulator/, or the working
    tree above it when that holds lab/ too. CDJ_ROOT in the environment wins."""
    if os.environ.get("CDJ_ROOT"):
        return host.native(os.environ["CDJ_ROOT"])
    up = os.path.dirname(emu)
    if os.path.isdir(os.path.join(up, "lab")) and os.path.isdir(os.path.join(up, "emulator")):
        return up
    return emu


def _writable(d):
    try:
        os.makedirs(d, exist_ok=True)
        probe = os.path.join(d, ".write-test")
        open(probe, "w").close()
        os.remove(probe)
        return True
    except OSError:
        return False


def _data_dir(program):
    if os.environ.get("CDJ_DATA"):
        return os.environ["CDJ_DATA"]
    if os.environ.get("APPIMAGE"):
        own = os.path.splitext(os.environ["APPIMAGE"])[0] + "-data"
    else:
        own = os.path.join(program, "data")
    if _writable(own):
        return own
    k = host.kind()
    if k == host.WINDOWS:
        base = host.local_appdata()
    elif k == host.MACOS:
        base = os.path.expanduser("~/Library/Application Support")
    else:
        base = os.environ.get("XDG_DATA_HOME") or os.path.expanduser("~/.local/share")
    return os.path.join(base, "CDJ-Emulator")


class Layout:
    def __init__(self):
        self.emu = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        self.scripts = os.path.join(self.emu, "scripts")
        self.run = os.path.join(self.scripts, "run")
        self.tmp = host.rig_tmp()
        runtime = os.path.join(os.path.dirname(self.emu), "runtime")
        self.packaged = os.path.isfile(os.path.join(runtime, "portable.txt"))
        if self.packaged:
            self.runtime = runtime
            # In a macOS bundle the tree is CDJ-Emulator.app/Contents/Resources/app.
            program = os.path.dirname(self.emu)
            if program.endswith(os.path.join(".app", "Contents", "Resources")):
                program = os.path.dirname(os.path.dirname(os.path.dirname(program)))
            self.root = program
            self.data = _data_dir(program)
            self.conf = os.path.join(self.data, "cdj.conf")
            self.logs = os.path.join(self.data, "logs")
            self.extract = os.path.join(self.data, "firmware")
            self.jit_cache = os.path.join(self.data, "dsp")
        else:
            self.runtime = None
            self.root = _checkout_root(self.emu)
            self.data = self.emu
            self.conf = os.path.join(self.emu, "cdj.conf")
            self.logs = os.path.join(self.emu, "logs")
            self.extract = os.path.join(self.root, "extract")
            self.jit_cache = os.path.join(host.home(), "c14gen")

    @property
    def usb_image(self):
        return os.path.join(self.extract, "usbmedia3.img")

    def qemu_binaries(self):
        q = os.path.join(self.runtime, "qemu")
        sfx = host.exe_suffix()
        return (host.native(os.path.join(q, "qemu-system-sh4" + sfx)),
                host.native(os.path.join(q, "qemu-system-sh4eb" + sfx)))

    def runtime_env(self):
        """What the packaged program's runs need that a checkout gets from its
        build: the bundled QEMUs, and for the DSP's run-time JIT the bundled
        Python, the code generator, the core library it plans against and the
        bundled C compiler."""
        main, gui = self.qemu_binaries()
        c6x = os.path.join(self.emu, "hw", "cdj", "c6x")
        with open(os.path.join(self.runtime, "portable.txt"), encoding="utf-8") as f:
            manifest = dict(line.rstrip("\n").split(" ", 1) for line in f if " " in line)
        runtime = host.native(self.runtime)
        cc = [a.format(runtime=runtime) for a in manifest["cc"].split()]
        cc[0] = host.native(os.path.join(self.runtime, cc[0]))
        zig_cache = os.path.join(self.jit_cache, "zig-cache")
        return {"MAIN_QEMU": main, "GUI_QEMU": gui,
                "C66X_JIT_PYTHON": host.native(os.path.realpath(sys.executable)),
                "C66X_JIT_GEN": host.native(os.path.join(c6x, "tools", "c14_jitgen.py")),
                "C66X_JIT_LIB": host.native(os.path.join(self.runtime, "qemu", "libc66x.so")),
                "C66X_JIT_CC": shlex.join(cc),
                # zig keeps its build cache in the user's profile by default.
                "ZIG_GLOBAL_CACHE_DIR": zig_cache, "ZIG_LOCAL_CACHE_DIR": zig_cache}
