# SPDX-License-Identifier: GPL-2.0-or-later
"""The questions the shell scripts answered with `uname -s`, `cygpath` and
$HOME. A native Windows Python sees the machine differently (backslashes,
USERPROFILE instead of the MSYS2 home, no /tmp), so it asks them here and gets
the scripts' answers."""

import functools
import os
import shutil
import socket
import subprocess
import sys
import tempfile

WINDOWS, MACOS, LINUX, UNKNOWN = "windows", "macos", "linux", "unknown"


def kind():
    p = sys.platform
    if p.startswith(("win", "cygwin", "msys")):
        return WINDOWS
    if p == "darwin":
        return MACOS
    if p.startswith("linux"):
        return LINUX
    return UNKNOWN


def is_windows():
    return kind() == WINDOWS


def exe_suffix():
    return ".exe" if is_windows() else ""


@functools.lru_cache(maxsize=None)
def _msys2_root():
    """The MSYS2 this project's QEMUs are built under (C:\\msys64), if it is
    actually there. Not whatever cygpath a shell happens to have on PATH:
    Git for Windows ships its own, and answers for a tree with none of the
    project's mingw64 runtime in it."""
    root = r"C:\msys64"
    return root if os.path.isfile(os.path.join(root, "usr", "bin", "cygpath.exe")) else None


@functools.lru_cache(maxsize=None)
def cygpath_tool():
    """MSYS2's cygpath: there, as in the scripts, paths for a native program
    are converted with it."""
    if not is_windows():
        return None
    root = _msys2_root()
    return os.path.join(root, "usr", "bin", "cygpath.exe") if root else shutil.which("cygpath")


@functools.lru_cache(maxsize=None)
def _cygpath(flag, path):
    try:
        out = subprocess.run([cygpath_tool(), flag, path], capture_output=True, text=True, check=True).stdout
    except (OSError, subprocess.CalledProcessError):
        return None
    return out.strip() or None


def native(path):
    """A path QEMU or a Windows Python can open: `cygpath -m` as the scripts
    did, forward slashes on plain Windows, unchanged elsewhere."""
    path = os.fspath(path)
    if not is_windows():
        return path
    if cygpath_tool():
        return _cygpath("-m", path) or path
    return path.replace("\\", "/")


def posix(path):
    """The MSYS2 spelling (/c/...) of a Windows path, for bash and for show."""
    path = os.fspath(path)
    if cygpath_tool():
        return _cygpath("-u", path) or path
    return path


def is_file(path):
    # A Microsoft Store app alias (the Store's python.exe) cannot be stat'ed.
    return os.path.isfile(path) or (os.path.lexists(path) and not os.path.isdir(path))


def local_appdata():
    """%LOCALAPPDATA% from Windows itself (CSIDL 28, as `cygpath -F 28`): an
    MSYS2 login shell does not pass the variable on."""
    import ctypes

    buf = ctypes.create_unicode_buffer(260)
    if ctypes.windll.shell32.SHGetFolderPathW(None, 28, None, 0, buf) == 0:
        return buf.value
    return os.environ.get("LOCALAPPDATA", "")


def home():
    """The scripts' $HOME. Under MSYS2 that is C:\\msys64\\home\\<user>, where
    ~/c14gen and the build trees are, while Python's expanduser() on Windows
    reads USERPROFILE."""
    h = os.environ.get("HOME")
    if h:
        return native(h)
    return os.path.expanduser("~")


def rig_tmp():
    """Where the rig's sockets, logs and throwaway images go: the scripts'
    literal /tmp (macOS's $TMPDIR is a per-user folder), MSYS2's own /tmp, or
    on Windows without MSYS2 (the packaged program) the OS temp directory."""
    if not is_windows():
        return "/tmp"
    if cygpath_tool():
        return native("/tmp")
    return tempfile.gettempdir().replace("\\", "/")


def qemu_dll_dir():
    """Where MAIN and GUI's runtime DLLs (SDL2, glib, ...) live: MSYS2's
    mingw64/bin. The build trees under QEMU_BUILD do not carry copies of
    them, so a shell whose PATH never had this directory on it (any shell
    other than MSYS2's own) starts a QEMU that dies before it opens its log."""
    root = _msys2_root()
    return os.path.join(root, "mingw64", "bin") if root else None


def windows_reserved_ranges(proto):
    """The (start, end) port blocks Windows keeps for Hyper-V and WSL, chosen
    at boot. A bind inside one fails, for QEMU with only "Unknown error"."""
    if not is_windows():
        return []
    try:
        out = subprocess.run(["netsh", "int", "ipv4", "show", "excludedportrange", "protocol=" + proto],
                             capture_output=True, text=True).stdout
    except OSError:
        return []
    return [(int(f[0]), int(f[1])) for f in (line.split() for line in out.splitlines())
            if len(f) >= 2 and f[0].isdigit() and f[1].isdigit()]


def in_ranges(port, ranges):
    return any(lo <= port <= hi for lo, hi in ranges)


def _unreserved(port, step, ranges):
    for _ in range(2000):
        if not in_ranges(port, ranges):
            return port
        port += step
    return -1


def pick_tcp_port(wanted):
    """`wanted`, or the next port that is neither reserved nor in use."""
    ranges = windows_reserved_ranges("tcp")
    p = wanted
    for _ in range(8):
        p = _unreserved(p, 1, ranges)
        if p == -1:
            break
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            return p
        except OSError:
            p += 1
        finally:
            s.close()
    return wanted


def pick_udp_port(wanted):
    """`wanted`, or the next one BELOW it: Windows reserves the high ranges."""
    p = _unreserved(wanted, -1, windows_reserved_ranges("udp"))
    return wanted if p == -1 else p


def pick_port_block(base, count):
    """`base`, or the next one where base .. base+count-1 are all free and
    unreserved: the app's one VNC port per deck."""
    ranges = windows_reserved_ranges("tcp")
    for base in range(base, base + 64):
        if any(in_ranges(base + i, ranges) for i in range(count)):
            continue
        socks = [socket.socket() for _ in range(count)]
        try:
            for i, s in enumerate(socks):
                s.bind(("127.0.0.1", base + i))
            return base
        except OSError:
            continue
        finally:
            for s in socks:
                s.close()
    return base


@functools.lru_cache(maxsize=None)
def find_bash():
    """A bash 4 or newer, for the shell scripts that remain (build.sh, the run
    reports). macOS's /bin/bash is 3.2, so Homebrew's is looked for first, as
    scripts/cdj_bash.sh does; Windows' System32 bash.exe is WSL's, another
    machine. None when there is none."""
    cands = ["/opt/homebrew/bin/bash", "/usr/local/bin/bash"] if kind() == MACOS else []
    b = shutil.which("bash")
    if b and not (is_windows() and os.path.normcase(b).startswith(
            os.path.normcase(os.environ.get("SystemRoot", r"C:\Windows")))):
        cands.append(b)
    for c in cands:
        if os.path.exists(c) and subprocess.run([c, "-c", '[ "${BASH_VERSINFO[0]}" -ge 4 ]'],
                                                capture_output=True).returncode == 0:
            return c
    return None


def python_argv():
    """How the project's scripts are run: on this interpreter, which in the
    packaged program is the bundled one."""
    return [sys.executable]
