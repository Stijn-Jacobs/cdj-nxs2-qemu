# SPDX-License-Identifier: GPL-2.0-or-later
"""Finding the Pythons setup records in cdj.conf.

The rig itself needs only the standard library; requirements.txt says which
Python needs what else:
  pyfatfs         the USB image builder; any Python 3
  mido, rtmidi    midi/bridge.py. On Windows a native Python (python.org or the
                  Microsoft Store): the bridge opens the USB MIDI device, and
                  python-rtmidi does not build for MSYS2's Python, which pip
                  refuses to install into anyway (EXTERNALLY-MANAGED).
  Pillow          the playhead scorer, run by the rig's own python3
A candidate is either a file (a recorded absolute path, possibly with spaces)
or a command such as "py -3".
"""

import glob
import os
import shlex
import shutil
import subprocess

from . import host


def argv_of(cand):
    if host.is_file(host.native(cand)):
        return [host.native(cand)]
    return shlex.split(cand)


def has(cand, *mods):
    """Does this candidate run, and import every one of `mods`?"""
    argv = argv_of(cand)
    if not argv:
        return False
    code = "; ".join(["import sys"] + ["import " + m for m in mods])
    try:
        return subprocess.run(argv + ["-c", code], capture_output=True).returncode == 0
    except OSError:
        return False


def _msc_python(argv):
    """sys.executable of a native Windows CPython 3 (built with MSC, which
    tells it apart from MSYS2's GCC-built one), else None."""
    try:
        r = subprocess.run(argv + ["-c", "import sys; assert 'MSC' in sys.version and "
                                   "sys.version_info[0] == 3; print(sys.executable)"],
                           capture_output=True, text=True)
    except OSError:
        return None
    out = r.stdout.strip()
    return host.native(out) if r.returncode == 0 and out else None


class Pythons:
    """The candidates on this machine, found once."""

    def __init__(self, emu, midi_python, tools_python):
        self.emu = emu
        self.windows_pythons = []
        self.py_launcher = None
        if host.is_windows():
            self._find_windows([os.environ.get("PYTHON", ""), midi_python, tools_python])
        cands = [c for c in (os.environ.get("PYTHON"), tools_python) if c]
        venv = os.path.join(emu, ".venv", "bin", "python")
        if os.access(venv, os.X_OK):
            cands.append(venv)
        self.candidates = cands + self.windows_pythons + ["python3", "python"]

    def _add_windows(self, argv):
        p = _msc_python(argv)
        if p and p not in self.windows_pythons:
            self.windows_pythons.append(p)

    def _find_windows(self, recorded):
        # MSYS2's default PATH has neither the py launcher's folder nor the
        # Store's aliases, so their usual places are searched too.
        local = host.local_appdata()
        for c in recorded:
            if c and host.is_file(host.native(c)):
                self._add_windows([host.native(c)])
        for c in ("py.exe", r"C:\Windows\py.exe",
                  os.path.join(local, "Programs", "Python", "Launcher", "py.exe")):
            found = shutil.which(c) or (c if host.is_file(c) else None)
            if found:
                self.py_launcher = found
                self._add_windows([found, "-3"])
                break
        for d in os.environ.get("PATH", "").split(os.pathsep):
            low = d.replace("\\", "/").lower()
            if "/msys64/" in low or low.startswith(("/mingw", "/ucrt64", "/clang", "/usr", "/bin")):
                continue
            for name in ("python.exe", "python3.exe"):
                c = os.path.join(d, name)
                if host.is_file(c):
                    self._add_windows([c])
        for c in (glob.glob(os.path.join(local, "Programs", "Python", "Python3*", "python.exe"))
                  + glob.glob(r"C:\Program Files\Python3*\python.exe")
                  + [os.path.join(local, "Microsoft", "WindowsApps", "python3.exe")]):
            if host.is_file(c):
                self._add_windows([c])

    def first_with(self, *mods):
        for c in self.candidates:
            if has(c, *mods):
                return c
        return None

    def prefer_venv(self):
        """After a fix made .venv, which now holds the packages, look there first."""
        venv = os.path.join(self.emu, ".venv", "bin", "python")
        if os.access(venv, os.X_OK) and venv not in self.candidates:
            self.candidates.insert(0, venv)
        return venv if os.access(venv, os.X_OK) else None

    def midi(self):
        if host.is_windows():
            return next((c for c in self.windows_pythons if has(c, "mido", "rtmidi")), None)
        return self.first_with("mido", "rtmidi")

    def pip_fix(self, what):
        """The command that installs `what` into a suitable Python ('' if none)."""
        k = host.kind()
        if k == host.MACOS:
            # Homebrew's Python refuses pip installs outside a venv (PEP 668),
            # so everything goes into this folder's .venv, which setup looks in.
            return "{ [ -x .venv/bin/python ] || python3 -m venv .venv; } && .venv/bin/python -m pip install " + what
        if k != host.WINDOWS:
            if what.startswith("-r"):
                return "python3 -m venv .venv && .venv/bin/python -m pip install " + what
            return "python3 -m pip install --user " + what
        if self.py_launcher:
            return "py -3 -m pip install --user " + what
        if self.windows_pythons:
            return "%s -m pip install --user %s" % (shlex.quote(self.windows_pythons[0]), what)
        return ""
