#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the portable program for this machine's OS: a folder that runs where
it is unzipped, with nothing to install, and its archive.

  usage: python packaging/package.py [--out dist] [--cache DIR] [--cc zig|mingw] [--keep-folder]
         python packaging/package.py --check <archive>...

  CDJ-Emulator/
      CDJ-Emulator.exe       Windows (packaging/cdj_launch.c); CDJ-Emulator.app
                             on macOS, a CDJ-Emulator script on Linux
      app/                   launcher/, app/, midi/, scripts/ and the DSP code
                             generator with the core headers it compiles against
      runtime/python/        python-build-standalone, with Pillow, pyfatfs,
                             mido and python-rtmidi
      runtime/qemu/          the two QEMUs from this machine's build trees,
                             their libraries, and libc66x.so
      runtime/zig/ or cc/    the C compiler the DSP's run-time JIT uses
      runtime/portable.txt   what went in (launcher.layout looks for it)

Nothing from Pioneer DJ / AlphaTheta goes in, and no DSP module either: those
are translations of the firmware's DSP program, so the user's machine builds
them on first run with the bundled compiler. The QEMUs come from the build
trees (./build.sh); the Python and zig downloads are pinned and checked
against their SHA-256.

--cc: zig on every OS (the default off Windows), or on Windows MSYS2's own
mingw gcc, which builds modules against msvcrt like the MINGW64 QEMU; a
zig-built module links the UCRT instead, so zig there wants a QEMU built in
MSYS2's UCRT64 environment. The compiler is tried once with the generator's
own flags before anything is archived.
"""

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
EMU = os.path.dirname(HERE)
sys.path.insert(0, EMU)
from launcher import conf, host  # noqa: E402
from launcher.layout import Layout  # noqa: E402

PBS_TAG, PYTHON_VERSION = "20260924", "3.12.14"
PBS_URL = "https://github.com/astral-sh/python-build-standalone/releases/download/%s/%s"
ZIG_VERSION = "0.16.0"
ZIG_INDEX = "https://ziglang.org/download/index.json"
PY_PACKAGES = ["Pillow", "pyfatfs", "setuptools<81", "mido", "python-rtmidi"]
# The parts of emulator/ the program runs. hw/cdj/c6x contributes only the
# code generator and the headers generated modules include. models/ is the
# firmware profiles firmware.py reads by name.
APP_TREES = ["launcher", "app", "midi", "scripts", "models"]
C6X = os.path.join("hw", "cdj", "c6x")
# c14_jitgen.py's flags, as it passes them to the compiler.
JIT_FLAGS = ["-O2", "-std=gnu11", "-fPIC", "-fvisibility=hidden", "-ffp-contract=off", "-frounding-math",
             "-march=native", "-w"]


def target():
    # An MSYS2 login shell drops PROCESSOR_ARCHITECTURE, which is where
    # Windows' platform.machine() looks; there is no arm64 Windows build.
    machine = platform.machine().lower() or "amd64"
    arch = {"amd64": "x86_64", "x86_64": "x86_64", "arm64": "aarch64", "aarch64": "aarch64"}[machine]
    k = host.kind()
    pbs = {host.WINDOWS: "%s-pc-windows-msvc", host.MACOS: "%s-apple-darwin",
           host.LINUX: "%s-unknown-linux-gnu"}[k] % arch
    zig = "%s-%s" % (arch, {host.WINDOWS: "windows", host.MACOS: "macos", host.LINUX: "linux"}[k])
    return k, arch, pbs, zig


def fetch(url, cache, sha256):
    path = os.path.join(cache, url.rsplit("/", 1)[-1])
    if not os.path.isfile(path) or _sha256(path) != sha256:
        print("download %s" % url, flush=True)
        with urllib.request.urlopen(url) as r, open(path + ".part", "wb") as f:
            shutil.copyfileobj(r, f)
        if _sha256(path + ".part") != sha256:
            os.remove(path + ".part")
            sys.exit("%s: SHA-256 mismatch" % url)
        os.replace(path + ".part", path)
    return path


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _read_url(url):
    with urllib.request.urlopen(url) as r:
        return r.read().decode()


def unpack(archive, dest):
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    else:
        with tarfile.open(archive) as t:
            t.extractall(dest)


def add_python(runtime, pbs, cache):
    name = "cpython-%s+%s-%s-install_only_stripped.tar.gz" % (PYTHON_VERSION, PBS_TAG, pbs)
    sums = dict(line.split()[::-1] for line in _read_url(PBS_URL % (PBS_TAG, "SHA256SUMS")).splitlines() if line)
    sha = sums[name]
    unpack(fetch(PBS_URL % (PBS_TAG, name), cache, sha), runtime)
    exe = os.path.join(runtime, "python", "python.exe" if host.is_windows() else os.path.join("bin", "python3"))
    subprocess.run([exe, "-m", "pip", "install", "--disable-pip-version-check", "--no-warn-script-location",
                    "--no-compile"] + PY_PACKAGES, check=True)
    lib = os.path.dirname(subprocess.run([exe, "-c", "import os; print(os.__file__)"], capture_output=True,
                                         text=True, check=True).stdout.strip())
    # Only for installing, which the program never does.
    for d in ("ensurepip", "idlelib", "lib2to3", "pydoc_data", os.path.join("site-packages", "pip")):
        shutil.rmtree(os.path.join(lib, d), ignore_errors=True)
    return exe


def add_zig(runtime, zig_target, cache):
    entry = json.loads(_read_url(ZIG_INDEX))[ZIG_VERSION][zig_target]
    archive = fetch(entry["tarball"], cache, entry["shasum"])
    unpack(archive, runtime)
    top = os.path.join(runtime, os.path.basename(entry["tarball"]).split(".tar")[0].replace(".zip", ""))
    os.replace(top, os.path.join(runtime, "zig"))
    prune_zig(os.path.join(runtime, "zig"), zig_target.split("-", 1)[1])
    return ["zig/zig" + host.exe_suffix(), "cc"]


def prune_zig(zig, os_name):
    """Drop the libc sources and headers of every other OS, and C++'s: they
    are most of zig's 383 MB on Windows."""
    def keep(name):
        if os_name == "windows":
            return name in ("mingw", "any-windows-any")
        if os_name == "macos":
            return name in ("darwin", "any-darwin-any")
        return name in ("glibc", "generic-glibc") or ("-linux-" in name and "musl" not in name)

    libc = os.path.join(zig, "lib", "libc")
    for d in (libc, os.path.join(libc, "include")):
        for name in os.listdir(d):
            if name != "include" and os.path.isdir(os.path.join(d, name)) and not keep(name):
                shutil.rmtree(os.path.join(d, name))
    for d in ("doc", "lib/libcxx", "lib/libcxxabi", "lib/libtsan", "lib/docs", "lib/build-web"):
        shutil.rmtree(os.path.join(zig, d), ignore_errors=True)


def add_mingw_gcc(runtime):
    """MSYS2's own gcc, cut down to compiling a module into a DLL: the drivers
    with their DLLs beside each (Windows looks for a program's DLLs in its own
    folder), the gcc lib directory, the C runtime's import libraries, and of
    its headers the ones the generated code includes."""
    cc = os.path.join(runtime, "cc")
    mingw = host.native("/mingw64")
    version = subprocess.run(["gcc", "-dumpversion"], capture_output=True, text=True, check=True).stdout.strip()
    gcclib = os.path.join("lib", "gcc", "x86_64-w64-mingw32", version)
    # gcc looks for as and ld beside cc1 before it looks on PATH.
    tools = [("bin/gcc.exe", "bin"), ("bin/as.exe", gcclib), ("bin/ld.exe", gcclib)] + [
        (os.path.join(gcclib, t), gcclib) for t in ("cc1.exe", "collect2.exe", "liblto_plugin.dll")]
    for tool, dest in tools:
        _copy(os.path.join(mingw, tool), os.path.join(cc, dest, os.path.basename(tool)))
        for dll in _dll_closure(os.path.join(mingw, tool)):
            _copy(dll, os.path.join(cc, dest, os.path.basename(dll)))
    for name in os.listdir(os.path.join(mingw, gcclib)):
        src = os.path.join(mingw, gcclib, name)
        if name == "include":
            shutil.copytree(src, os.path.join(cc, gcclib, "include"), dirs_exist_ok=True)
        elif name.endswith((".a", ".o")):
            _copy(src, os.path.join(cc, gcclib, name))
    for lib in ("crt2.o", "dllcrt2.o", "libmsvcrt.a", "libkernel32.a", "libmingw32.a", "libmingwex.a",
                "libmoldname.a", "libadvapi32.a", "libshell32.a", "libuser32.a", "libpthread.a",
                "libwinpthread.a", "libgcc_s.a"):
        _copy(os.path.join(mingw, "lib", lib), os.path.join(cc, "lib", lib))
    work = tempfile.mkdtemp(prefix="cdj-cc-")
    try:
        src = os.path.join(work, "m.c")
        with open(src, "w") as f:
            f.write(_prelude())
        deps = subprocess.run(["gcc", "-M", "-I", os.path.join(EMU, C6X), src], capture_output=True, text=True,
                              check=True).stdout.replace("\\\n", " ").split()[1:]
    finally:
        shutil.rmtree(work, ignore_errors=True)
    include = os.path.normcase(os.path.join(mingw, "include")) + os.sep
    for dep in deps:
        path = os.path.normpath(host.native(dep))
        if os.path.normcase(path).startswith(include):
            _copy(path, os.path.join(cc, os.path.relpath(path, mingw)))
    # Configured with the sysroot /mingw64, which a relocated gcc does not
    # find: point it at its own tree ({runtime} is filled in where it runs).
    return ["cc/bin/gcc.exe", "--sysroot={runtime}/cc"]


def _prelude():
    """The C every generated module starts with, its #includes above all."""
    sys.path.insert(0, os.path.join(EMU, C6X, "tools"))
    try:
        import c14_jitgen
    finally:
        sys.path.pop(0)
    return c14_jitgen.PRELUDE


def _copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def _dll_closure(exe):
    """The MSYS2 DLLs a Windows program loads (ldd names /mingw64/bin ones)."""
    out = subprocess.run(["ldd", exe], capture_output=True, text=True).stdout
    return [host.native(line.split("=>")[1].split("(")[0].strip()) for line in out.splitlines()
            if "=>" in line and "/mingw64/" in line]


def add_qemu(runtime, lay):
    values = conf.load(lay.conf)
    main_dir = os.environ.get("QEMU_BUILD") or values.get("QEMU_BUILD") or (
        "/c/qemu-build-mingw" if host.is_windows() else os.path.join(host.home(), "qemu-build"))
    eb_dir = os.environ.get("QEMU_EB_BUILD") or values.get("QEMU_EB_BUILD") or main_dir + "-eb"
    sfx = host.exe_suffix()
    bins = [os.path.join(host.native(main_dir), "qemu-system-sh4" + sfx),
            os.path.join(host.native(eb_dir), "qemu-system-sh4eb" + sfx)]
    lib = os.path.join(os.environ.get("C66X_JIT_LIBDIR") or os.path.join(host.home(), "build", "c6x"), "libc66x.so")
    for f in bins + [lib]:
        if not os.path.isfile(f):
            sys.exit("%s is missing: build it first (./build.sh main display dsp)" % f)
    q = os.path.join(runtime, "qemu")
    os.makedirs(q)
    for f in bins + [lib]:
        shutil.copy2(f, q)
    k = host.kind()
    # The build trees keep debug information: 55 MB per QEMU on Windows.
    strip = ["strip", "-x"] if k == host.MACOS else ["strip"]
    subprocess.run(strip + [os.path.join(q, os.path.basename(f)) for f in bins + [lib]], check=True)
    if k == host.WINDOWS:
        for f in bins + [lib]:
            for dll in _dll_closure(f):
                if not os.path.exists(os.path.join(q, os.path.basename(dll))):
                    shutil.copy2(dll, q)
    elif k == host.MACOS:
        _bundle_dylibs(q, [os.path.join(q, os.path.basename(f)) for f in bins + [lib]])
    else:
        _bundle_sos(q, [os.path.join(q, os.path.basename(f)) for f in bins + [lib]])
    return _source_stamp(lay)


def _bundle_dylibs(q, files):
    """Copy the Homebrew libraries into qemu/lib and point every load command
    at them, then sign ad hoc: arm64 macOS runs no binary whose signature the
    rewrite broke."""
    libdir = os.path.join(q, "lib")
    os.makedirs(libdir, exist_ok=True)
    todo, seen = list(files), set()
    while todo:
        f = todo.pop()
        out = subprocess.run(["otool", "-L", f], capture_output=True, text=True, check=True).stdout
        for line in out.splitlines()[1:]:
            dep = line.strip().split(" (")[0]
            if not dep.startswith(("/opt/homebrew/", "/usr/local/")):
                continue
            name = os.path.basename(dep)
            dst = os.path.join(libdir, name)
            if name not in seen:
                seen.add(name)
                shutil.copy2(os.path.realpath(dep), dst)
                os.chmod(dst, 0o755)
                subprocess.run(["install_name_tool", "-id", "@loader_path/" + name, dst], check=True)
                todo.append(dst)
            ref = "@loader_path/" + name if os.path.dirname(f) == libdir else "@loader_path/lib/" + name
            subprocess.run(["install_name_tool", "-change", dep, ref, f], check=True)
    for f in files + [os.path.join(libdir, n) for n in seen]:
        subprocess.run(["codesign", "--force", "-s", "-", f], check=True)


def _bundle_sos(q, files):
    """Copy the non-system shared libraries into qemu/lib and point the
    binaries' RUNPATH there (needs patchelf)."""
    if not shutil.which("patchelf"):
        sys.exit("patchelf is needed to package on Linux (apt install patchelf)")
    libdir = os.path.join(q, "lib")
    os.makedirs(libdir, exist_ok=True)
    system = ("libc.so", "libm.so", "libpthread.so", "libdl.so", "librt.so", "ld-linux", "libgcc_s.so",
              "libstdc++.so", "linux-vdso", "libasound.so", "libpulse", "libX", "libxcb", "libGL", "libEGL",
              "libwayland", "libdrm")
    for f in files:
        out = subprocess.run(["ldd", f], capture_output=True, text=True).stdout
        for line in out.splitlines():
            if "=>" not in line:
                continue
            name, path = line.split("=>")[0].strip(), line.split("=>")[1].split("(")[0].strip()
            if path and not name.startswith(system) and not os.path.exists(os.path.join(libdir, name)):
                shutil.copy2(path, os.path.join(libdir, name))
        subprocess.run(["patchelf", "--set-rpath", "$ORIGIN/lib", f], check=True)


def _source_stamp(lay):
    bash = host.find_bash()
    if not bash:
        return "unknown"
    script = '. "%s"; cdj_source_stamp "%s"' % (host.posix(os.path.join(lay.scripts, "build", "source_stamp.sh")),
                                              host.posix(lay.emu))
    return subprocess.run([bash, "-c", script], capture_output=True, text=True).stdout.strip() or "unknown"


def add_app(app):
    ignore = shutil.ignore_patterns("__pycache__", "*.pyc", "cdj.conf", "logs", ".venv")
    for d in APP_TREES:
        shutil.copytree(os.path.join(EMU, d), os.path.join(app, d), ignore=ignore)
    c6x = os.path.join(app, C6X)
    os.makedirs(os.path.join(c6x, "tools"))
    shutil.copy2(os.path.join(EMU, C6X, "tools", "c14_jitgen.py"), os.path.join(c6x, "tools"))
    for root, _, files in os.walk(os.path.join(EMU, C6X)):
        for f in files:
            if f.endswith(".h"):
                src = os.path.join(root, f)
                _copy(src, os.path.join(c6x, os.path.relpath(src, os.path.join(EMU, C6X))))
    for f in ("README.md", "requirements.txt"):
        shutil.copy2(os.path.join(EMU, f), app)


def check_compiler(runtime, cc, app):
    """Build one module the way the generator does, with the bundled compiler."""
    work = tempfile.mkdtemp(prefix="cdj-cc-")
    try:
        src = os.path.join(work, "m.c")
        with open(src, "w") as f:
            f.write(_prelude() + "\nint jit_probe(c66x_core *c) { return (int)jit_fop(c, 6, 1, 1, 2, '+'); }\n")
        argv = [os.path.join(runtime, cc[0])] + [a.format(runtime=runtime) for a in cc[1:]]
        env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(work, "zc"), ZIG_LOCAL_CACHE_DIR=os.path.join(work, "zc"))
        subprocess.run(argv + JIT_FLAGS + ["-I", os.path.join(app, C6X), "-c", src, "-o", src[:-2] + ".o"],
                       check=True, env=env)
        subprocess.run(argv + ["-shared", "-o", os.path.join(work, "m.so"), src[:-2] + ".o"], check=True, env=env)
    finally:
        shutil.rmtree(work, ignore_errors=True)


# Names only the user's own firmware, music or DSP code can have: the update
# file, the images prepare_firmware makes, a USB image, and what the DSP's JIT
# writes (modules, their generated C, the profiles holding the DSP's code).
FORBIDDEN_NAMES = ("*.upd", "*.img", "main_unpacked.bin", "gui_unpacked.bin", "flash*.bin", "settings.bin",
                   "resblob.bin", "artblob.bin", "section*.bin", "export.pdb", "m.so", "m.main.c", "m.part*.c",
                   "profile.txt")


def check_clean(folder):
    """Fail if anything derived from Pioneer's firmware or DSP program is in
    the package, by name or by the firmware images' known SHA-256."""
    import fnmatch

    from launcher import firmware

    known = {want for _, want in firmware.EXPECTED} | {firmware.UPD_SHA256}
    bad = []
    for root, _, files in os.walk(folder):
        for f in files:
            path = os.path.join(root, f)
            if any(fnmatch.fnmatch(f.lower(), pat) for pat in FORBIDDEN_NAMES) or (
                    os.path.getsize(path) > 65536 and _sha256(path) in known):
                bad.append(os.path.relpath(path, folder))
    if bad:
        sys.exit("REFUSING TO PACKAGE: firmware- or DSP-derived files in %s:\n  %s" % (folder, "\n  ".join(bad)))


LINUX_ENTRY = r"""#!/bin/sh
# The portable CDJ-2000NXS2 emulator: the bundled Python runs the launcher.
here="$(cd "$(dirname "$0")" && pwd)"
PYTHONPATH="$here/app" PYTHONNOUSERSITE=1 exec "$here/runtime/python/bin/python3" -m launcher "$@"
"""

# A double-clicked app has no terminal for setup's questions, so it opens one
# on itself.
MACOS_ENTRY = r"""#!/bin/sh
# The portable CDJ-2000NXS2 emulator: the bundled Python runs the launcher.
if [ ! -t 0 ] && [ "$#" = 0 ]; then
    exec open -a Terminal "$0"
fi
here="$(cd "$(dirname "$0")/../Resources" && pwd)"
PYTHONPATH="$here/app" PYTHONNOUSERSITE=1 exec "$here/runtime/python/bin/python3" -m launcher "$@"
"""

INFO_PLIST = """<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>CDJ-Emulator</string>
<key>CFBundleIdentifier</key><string>io.github.cdj-nxs2-qemu</string>
<key>CFBundleName</key><string>CDJ-Emulator</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>LSMinimumSystemVersion</key><string>11.0</string>
</dict></plist>
"""


def _write(path, text, mode=0o644):
    with open(path, "w", newline="\n") as f:
        f.write(text)
    os.chmod(path, mode)


def add_entry(folder, k):
    if k == host.WINDOWS:
        subprocess.run(["gcc", "-O2", "-municode", "-o", os.path.join(folder, "CDJ-Emulator.exe"),
                        os.path.join(HERE, "cdj_launch.c")], check=True)
    elif k == host.LINUX:
        _write(os.path.join(folder, "CDJ-Emulator"), LINUX_ENTRY, 0o755)
    else:
        # The folder's contents move into CDJ-Emulator.app/Contents/Resources.
        contents = os.path.join(folder, "CDJ-Emulator.app", "Contents")
        os.makedirs(os.path.join(contents, "MacOS"))
        os.makedirs(os.path.join(contents, "Resources"))
        for d in ("app", "runtime"):
            os.replace(os.path.join(folder, d), os.path.join(contents, "Resources", d))
        _write(os.path.join(contents, "MacOS", "CDJ-Emulator"), MACOS_ENTRY, 0o755)
        _write(os.path.join(contents, "Info.plist"), INFO_PLIST)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=os.path.join(EMU, "dist"))
    ap.add_argument("--cache", default=os.path.join(os.path.expanduser("~"), ".cache", "cdj-packaging"))
    ap.add_argument("--cc", choices=("zig", "mingw"))
    ap.add_argument("--keep-folder", action="store_true", help="keep the unpacked folder next to the archive")
    ap.add_argument("--check", metavar="ARCHIVE", nargs="+", help="only check finished archives, as the build does")
    a = ap.parse_args()
    if a.check:
        for archive in a.check:
            work = tempfile.mkdtemp(prefix="cdj-check-")
            try:
                unpack(archive, work)
                check_clean(work)
            finally:
                shutil.rmtree(work, ignore_errors=True)
            print("%s: clean" % archive)
        return
    k, arch, pbs, zig_target = target()
    cc_kind = a.cc or ("mingw" if k == host.WINDOWS else "zig")
    if cc_kind == "mingw" and k != host.WINDOWS:
        sys.exit("--cc mingw is for Windows")
    lay = Layout()
    name = "CDJ-Emulator-%s-%s" % ({host.WINDOWS: "windows", host.MACOS: "macos", host.LINUX: "linux"}[k], arch)
    folder = os.path.join(a.out, name)
    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(a.cache, exist_ok=True)
    runtime, app = os.path.join(folder, "runtime"), os.path.join(folder, "app")
    os.makedirs(runtime)
    add_app(app)
    add_python(runtime, pbs, a.cache)
    stamp = add_qemu(runtime, lay)
    cc = add_zig(runtime, zig_target, a.cache) if cc_kind == "zig" else add_mingw_gcc(runtime)
    check_compiler(runtime, cc, app)
    with open(os.path.join(runtime, "portable.txt"), "w", newline="\n") as f:
        f.write("python %s (python-build-standalone %s)\n" % (PYTHON_VERSION, PBS_TAG))
        f.write("qemu %s\n" % stamp)
        f.write("cc %s\n" % " ".join(cc))
        if cc_kind == "zig":
            f.write("zig %s\n" % ZIG_VERSION)
    add_entry(folder, k)
    check_clean(folder)
    archive = shutil.make_archive(folder, "zip" if k == host.WINDOWS else "gztar", a.out, name)
    if not a.keep_folder:
        shutil.rmtree(folder)
    print("%s (%d MB)" % (archive, os.path.getsize(archive) >> 20))


if __name__ == "__main__":
    main()
