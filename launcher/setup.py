# SPDX-License-Identifier: GPL-2.0-or-later
"""From a fresh clone to a running, customised CDJ-2000NXS2, in one command.

  ./setup.sh                 walk through everything, asking as it goes
  ./setup.sh --dry-run       show every step and command, change nothing

Six steps, each one safe to re-run and each one skipped when its result is
already there:

  1 prerequisites   compilers, libraries and Python packages, with the exact
                    pacman / apt / brew command for whatever is missing
  2 build           QEMU 9.1.0 fetched and patched, the MAIN and display-board
                    emulators, the DSP core library (logs in logs/)
  3 firmware        your own C2KNXS2.UPD (v1.87) turned into the images the
                    emulator boots, each checked against a known SHA-256
  4 USB stick       a disk image made from a folder of your own music: either
                    a rekordbox USB export, or a plain folder of music files
                    that baken (github.com/M-Igashi/baken, MIT) analyses --
                    no rekordbox needed
  5 DSP code        one headless deck plays for a few minutes so the DSP JIT
                    compiles its hot code into ~/c14gen; with --curated-jit,
                    a profile-guided module built from a recording instead
  6 your setup      one deck or two, Pro DJ Link, audio, a MIDI controller,
                    saved to cdj.conf -- which ./start.sh then uses

options:
  --dry-run              print what would happen; run and write nothing
  -y, --yes              never ask: take the defaults and the options below
  --skip-build           leave step 2 out (a build tree you made yourself)
  --rebuild              run step 2 even when the emulators are already built
  --no-warm              leave step 5 out
  --warm                 run step 5's warm-up even when the cache is warm
  --curated-jit          step 5 builds the profile-guided DSP module (~1 h,
                         ~16 GB free disk while it runs)
  --keep-recording       keep that build's DSP recording (~10 GB) afterwards
  --reconfigure          ask the step 6 questions again
  --firmware <file>      the C2KNXS2.UPD to use (re-installs the images)
  --music <folder>       the rekordbox USB export to image (re-makes the stick)
  --tracks <folder>      a plain folder of music to image instead, analysed by baken
  --decks 1|2            --name <deck name>    --djlink on|off   --audio on|off
  --controller none|<profile>|learn            --relay-port <port>
  --build-dir <dir>      where the two QEMU build trees go
  -h, --help

Nothing from Pioneer DJ / AlphaTheta is in this repository: the firmware file
and the music are yours, and they stay in extract/ on your machine.
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

from . import baken, chain, conf, firmware, host, pythons
from .console import Console, dropped_path, stdin_is_tty
from .layout import Layout

USAGE = __doc__[__doc__.index("  ./setup.sh  "):__doc__.index("  -h, --help") + len("  -h, --help")] + "\n"

VALUE_OPTS = {"--firmware": ("firmware", "a file"), "--music": ("music", "a folder"),
              "--tracks": ("tracks", "a folder"),
              "--decks": ("decks", "1 or 2"), "--name": ("name", "a word"),
              "--djlink": ("djlink", "on or off"), "--audio": ("audio", "on or off"),
              "--controller": ("controller", "a name"), "--relay-port": ("relay", "a number"),
              "--build-dir": ("build_dir", "a directory")}


class Options:
    def __init__(self):
        self.dry = self.yes = self.skip_build = self.rebuild = self.reconfigure = False
        self.warm = "auto"
        self.curated = self.keep_recording = False
        self.firmware = self.music = self.tracks = self.decks = self.name = self.djlink = ""
        self.audio = self.controller = self.relay = self.build_dir = ""


def parse_args(argv):
    o = Options()
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--dry-run":
            o.dry = True
        elif a in ("-y", "--yes", "--non-interactive"):
            o.yes = True
        elif a == "--skip-build":
            o.skip_build = True
        elif a == "--rebuild":
            o.rebuild = True
        elif a == "--reconfigure":
            o.reconfigure = True
        elif a == "--no-warm":
            o.warm = "off"
        elif a == "--warm":
            o.warm = "force"
        elif a == "--curated-jit":
            o.curated = True
        elif a == "--keep-recording":
            o.keep_recording = True
        elif a in VALUE_OPTS:
            attr, what = VALUE_OPTS[a]
            if i + 1 >= len(argv) or not argv[i + 1]:
                sys.stderr.write("%s needs %s\n" % (a, what))
                raise SystemExit(2)
            setattr(o, attr, argv[i + 1])
            i += 1
        elif a in ("-h", "--help"):
            sys.stdout.write(USAGE)
            raise SystemExit(0)
        else:
            sys.stderr.write("unknown option: %s (see ./setup.sh --help)\n" % a)
            raise SystemExit(2)
        i += 1
    return o


def launcher_cmd(*args):
    """argv and environment that run this launcher as a child."""
    env = {k: v for k, v in os.environ.items() if k not in ("LAUNCHER_TTY_IN", "LAUNCHER_TTY_OUT")}
    return chain.launcher_argv() + list(args), chain.launcher_env(env)


def _mb(path):
    """du -sm: the folder's size in MB, rounded up."""
    total = 0
    for d, _, files in os.walk(path):
        for f in files:
            try:
                total += os.path.getsize(os.path.join(d, f))
            except OSError:
                pass
    return -(-total // (1 << 20))


def _onoff(v):
    return "1" if v in ("on", "1", "yes", "y") else "0"


def _yesno(v):
    return "on" if v == "1" else "off"


def _modules(cache):
    return len(glob.glob(os.path.join(cache, "batch*", "m.so")))


class Setup:
    def __init__(self, o):
        self.o = o
        self.lay = Layout()
        self.con = Console(dry=o.dry, interactive=not o.yes and stdin_is_tty())
        self.platform = host.kind()
        self.ts = time.strftime("%Y%m%d-%H%M%S")

    def run(self):
        con, lay, o = self.con, self.lay, self.o
        self.pkg = self._package_manager()
        self.cores = os.cpu_count() or 4
        self.wsl = self.platform == host.LINUX and _is_wsl()
        self.values = conf.load(lay.conf, warn=con.warn)
        c = dict({k: "" for k in conf.KEYS}, **self.values)
        if o.build_dir:
            c["QEMU_BUILD"] = ""
        # The build trees are a checkout's; the packaged program brings its QEMUs.
        if not lay.packaged:
            base = o.build_dir or self._default_build_base()
            default_build = os.path.join(base, "qemu-build-mingw" if self.platform == host.WINDOWS else "qemu-build")
            c["QEMU_BUILD"] = c["QEMU_BUILD"] or default_build.replace("\\", "/")
            c["QEMU_EB_BUILD"] = c["QEMU_EB_BUILD"] or c["QEMU_BUILD"] + "-eb"
            self.jit_libdir = os.environ.get("C66X_JIT_LIBDIR") or os.path.join(host.home(), "build", "c6x")
            os.environ.update({"QEMU_BUILD": c["QEMU_BUILD"], "QEMU_EB_BUILD": c["QEMU_EB_BUILD"],
                               "C66X_JIT_LIBDIR": self.jit_libdir})
        self.c = c

        con._p("%sCDJ-2000NXS2 emulator setup%s" % (con.B, con.N))
        con.dim("the real firmware of a Pioneer CDJ-2000NXS2, on emulated hardware")
        con.info("platform: %s%s, %s CPU threads%s" % (self.platform, " (WSL)" if self.wsl else "", self.cores,
                                                       ", packages via " + self.pkg if self.pkg else ""))
        con.info("this folder: %s" % host.posix(lay.emu if not lay.packaged else lay.data))
        if o.dry:
            con.warn("dry run: nothing is installed, built or written")
        if self.platform == host.UNKNOWN:
            con.die("only Windows (MSYS2), Linux and macOS are supported.")

        self.step_prerequisites()
        self.step_build()
        self.step_firmware()
        self.step_usb()
        self.step_dsp()
        self.step_config()
        return self.summary()

    def _package_manager(self):
        if self.platform == host.WINDOWS:
            return "pacman" if shutil.which("pacman") else ""
        if self.platform == host.MACOS:
            return "brew" if shutil.which("brew") else ""
        return next((p for p in ("apt-get", "dnf", "pacman", "zypper") if shutil.which(p)), "")

    def _default_build_base(self):
        # On Windows QEMU's tracetool cannot relate paths across drives, so
        # the build trees must share this source's drive (and be on NTFS).
        if self.platform == host.WINDOWS:
            drive = host.native(self.lay.emu)[:1].lower() or "c"
            return "/" + drive
        return host.home()

    def step_prerequisites(self):
        con, o = self.con, self.o
        con.step(1, "Prerequisites")
        if self.lay.packaged:
            con.good("the emulators, the compiler and Python come with this program")
            self.py = None
            self.py_tools = self.py_midi = self.py_any = sys.executable
            self.midi_fix = ""
            return
        if self.platform == host.WINDOWS:
            if not self.pkg:
                con.fail("this shell has no pacman, so it is not MSYS2 (Git Bash, perhaps).")
                con.info("Install MSYS2 from https://www.msys2.org, open %sMSYS2 MINGW64%s from" % (con.B, con.N))
                con.info("the Start menu, and run ./setup.sh from there.")
                if not o.dry:
                    raise SystemExit(1)
            elif os.environ.get("MSYSTEM") != "MINGW64":
                con.fail("this is the MSYS2 %s shell; the emulator is built in %sMSYS2 MINGW64%s."
                         % (os.environ.get("MSYSTEM", "?"), con.B, con.N))
                if not o.dry:
                    raise SystemExit(1)
        if self.platform == host.MACOS:
            if not self.pkg:
                con.fail("no Homebrew: the build's libraries and tools come from it.")
                con.info("Install it from https://brew.sh, then run ./setup.sh again.")
                if not o.dry:
                    raise SystemExit(1)
            # /usr/bin/gcc is a stub until the Command Line Tools are installed.
            if subprocess.run(["xcode-select", "-p"], capture_output=True).returncode != 0:
                con.fail("no Xcode Command Line Tools (the C compiler, make, git).")
                con.info("install them:  %sxcode-select --install%s   and run ./setup.sh again" % (con.B, con.N))
                if not o.dry:
                    raise SystemExit(1)

        missing = missing_prerequisites(self.platform)
        if not missing:
            con.good("build tools and libraries")
        else:
            con.fail("missing: %s" % " ".join(missing))
            pk = " ".join(packages_for(self.platform, missing)) + " "
            if self.platform == host.WINDOWS:
                con.info("install them:  %spacman -S --needed %s%s" % (con.B, pk, con.N))
            elif self.platform == host.MACOS:
                con.info("install them:  %sbrew install %s%s" % (con.B, pk, con.N))
            elif self.pkg == "apt-get":
                con.info("install them:  %ssudo apt-get install -y %s%s" % (con.B, pk, con.N))
            else:
                con.info("install the equivalents of: %s(Debian package names)" % pk)
            if not o.skip_build and not o.dry:
                con.die("install the packages above, then run ./setup.sh again.")

        c = self.c
        self.py = pythons.Pythons(self.lay.emu, c["CDJ_MIDI_PYTHON"], c["CDJ_TOOLS_PYTHON"])
        self.py_any = self.py.first_with()
        if not self.py_any:
            con.die("no working Python 3 found (see above for the package).")
        if self.platform == host.WINDOWS:
            if self.py.windows_pythons:
                con.info("Windows Python: %s" % host.posix(self.py.windows_pythons[0]))
            else:
                con.info("no Windows Python found (python.org or the Microsoft Store)")
        win_hint = "install Python 3 for Windows (python.org, or the Microsoft Store), then run:"

        self.py_tools = self.py.first_with("pyfatfs")
        if self.py_tools:
            con.good("Python for the USB image builder: %s (pyfatfs)" % host.posix(self.py_tools))
        else:
            con.warn("no Python with pyfatfs yet -- step 4 (the USB image) needs it")
            fix = self.py.pip_fix("-r requirements.txt")
            if not fix:
                con.info(win_hint)
                con.info("  %spy -3 -m pip install -r requirements.txt%s   and ./setup.sh again" % (con.B, con.N))
            else:
                con.info("fix:  %s%s%s" % (con.B, fix, con.N))
                if o.dry:
                    con.would(fix)
                elif con.ask_yn("run that now?", "y"):
                    if _run_fix(fix, self.lay.emu):
                        venv = self.py.prefer_venv()
                        self.py_tools = self.py.first_with("pyfatfs") or venv
                        if self.py_tools:
                            con.good("installed")

        # A Python set up before mutagen joined requirements.txt has pyfatfs, so
        # the offer above never ran for it: make the same offer here.
        if self.py_tools and not pythons.has(self.py_tools, "mutagen"):
            fix = self.py.pip_fix("-r requirements.txt")
            if fix:
                con.warn("no mutagen in %s -- a folder-of-music USB step needs it" % host.posix(self.py_tools))
                con.info("fix:  %s%s%s" % (con.B, fix, con.N))
                if o.dry:
                    con.would(fix)
                elif con.ask_yn("run that now?", "y") and _run_fix(fix, self.lay.emu):
                    # The fix installs into .venv, which may not be py_tools.
                    self.py.prefer_venv()
                    self.py_tools = self.py.first_with("pyfatfs", "mutagen") or self.py_tools
        if self.py_tools and pythons.has(self.py_tools, "mutagen"):
            con.good("mutagen, for a folder-of-music USB step (tags, and BPM tags when present)")
        else:
            con.dim("no mutagen yet: same fix as above (-r requirements.txt) -- until then, a")
            con.dim("folder-of-music USB step reads no tags at all")
        if shutil.which("ffmpeg") and self.py_tools and pythons.has(self.py_tools, "numpy"):
            con.good("ffmpeg + numpy for a BPM estimate (a folder-of-music track with no BPM tag)")
        else:
            con.dim("no ffmpeg and/or numpy: such a track still loads, just with no beat grid")

        self.py_midi = self.py.midi()
        midi_fix = self.py.pip_fix("mido python-rtmidi") or "py -3 -m pip install mido python-rtmidi"
        self.midi_fix = midi_fix
        if self.py_midi:
            con.good("Python for a MIDI controller: %s (mido, python-rtmidi)" % host.posix(self.py_midi))
        else:
            con.dim("no Python with mido + python-rtmidi: fine unless you want a MIDI controller")
            if self.platform == host.WINDOWS and not self.py.windows_pythons:
                con.dim(win_hint)
            con.dim("for one:  %s" % midi_fix)

        if pythons.has("python3", "PIL"):
            con.good("Pillow for the playhead scorer (python3)")
        else:
            fix = {host.WINDOWS: "pacman -S --needed mingw-w64-x86_64-python-pillow",
                   # The scorer runs on python3 itself, not .venv; Homebrew's
                   # refuses a plain --user install (PEP 668).
                   host.MACOS: "python3 -m pip install --user --break-system-packages Pillow"}.get(
                self.platform, "sudo apt-get install -y python3-pil" if self.pkg == "apt-get"
                else "python3 -m pip install --user Pillow")
            con.dim("no Pillow for python3: the rig runs, only its end-of-run playhead score fails (%s)" % fix)

        # The build trees and the DSP code cache take about 3 GB.
        where = os.path.dirname(host.native(c["QEMU_BUILD"]))
        try:
            free_mb = shutil.disk_usage(where).free // (1 << 20)
        except OSError:
            free_mb = None
        if free_mb is not None and free_mb < 4000:
            con.warn("only %d MB free where the build goes (%s); it needs ~3 GB" % (free_mb, where))
        if self.cores < 4:
            con.warn("%d CPU threads: one deck may not keep up with real time" % self.cores)

    def binaries(self):
        sfx = host.exe_suffix()
        if self.lay.packaged:
            main, gui = self.lay.qemu_binaries()
            return main, gui, os.path.join(self.lay.runtime, "qemu", "libc66x.so")
        return (os.path.join(host.native(self.c["QEMU_BUILD"]), "qemu-system-sh4" + sfx),
                os.path.join(host.native(self.c["QEMU_EB_BUILD"]), "qemu-system-sh4eb" + sfx),
                os.path.join(self.jit_libdir, "libc66x.so"))

    def step_build(self):
        con, o, lay = self.con, self.o, self.lay
        con.step(2, "Build the emulators")
        main, gui, dsp = self.binaries()
        if lay.packaged:
            con.good("prebuilt: %s" % os.path.dirname(main))
            return
        con.info("build trees: %s, %s" % (self.c["QEMU_BUILD"], self.c["QEMU_EB_BUILD"]))
        if o.skip_build:
            con.dim("skipped (--skip-build)")
            return
        bash = host.find_bash() or "bash"
        phases = (
            (1, "source", os.path.join(lay.root, "qemu-src", "hw", "sh4"),
             "needs git and network access to gitlab.com; delete a half-cloned qemu-src/ and retry",
             "QEMU 9.1.0 source (a shallow git clone, ~100 MB)"),
            (2, "patches", "",
             "a patch that does not apply means qemu-src/ is not a clean v9.1.0: delete it and re-run",
             "this project's patches to QEMU"),
            (3, "main", main,
             "a missing library shows as a meson 'Dependency ... not found' above; step 1 lists the package",
             "the MAIN emulator (SH-4 + the CDJ-2000NXS2 board; the long one, 10-40 min)"),
            (4, "display", gui,
             "same toolchain as 2.3; if that worked, look for a disk-full or gtk3 error above",
             "the display-board emulator (SH-2A)"),
            (5, "dsp", dsp, "the DSP library needs only gcc and make",
             "the DSP core library for the run-time JIT"),
        )
        for n, name, artifact, hint, label in phases:
            if artifact and os.path.exists(artifact) and not o.rebuild:
                con.good("%s  %s(already built: %s)%s" % (label, con.D, artifact, con.N))
                continue
            con._p("  %s2.%d%s %s" % (con.B, n, con.N, label))
            log = os.path.join(lay.logs, "build-%s-%d-%s.log" % (self.ts, n, name))
            env = dict(os.environ, JOBS=str(self.cores))
            if not con.run_phase(label, log, hint, [bash, host.posix(os.path.join(lay.emu, "build.sh")), name],
                                 rel=lay.emu, env=env):
                con.die("the build stopped at phase 2.%d" % n)

    def step_firmware(self):
        con, o, lay = self.con, self.o, self.lay
        con.step(3, "Firmware (your own update file)")
        shown = os.path.relpath(lay.extract, lay.root) if not lay.packaged else lay.extract
        if firmware.installed(lay.extract) and not o.firmware:
            con.good("firmware images in %s/ (checked when they were installed)" % shown)
            return
        con.info("The emulator boots the CDJ-2000NXS2's own firmware, and this repository")
        con.info("contains none of it. You need Pioneer DJ's public update file for the")
        con.info("CDJ-2000NXS2, %sversion 1.87%s (C2KNXS2.UPD), from their support site." % (con.B, con.N))
        upd = o.firmware
        while not upd or not os.path.isfile(upd):
            if not con.interactive:
                if o.dry:
                    con.would("scripts/firmware/prepare_firmware.sh --install <your C2KNXS2.UPD>")
                    return
                con.die("no firmware: pass --firmware /path/to/C2KNXS2.UPD")
            if upd:
                con.warn("no such file: %s" % upd)
            upd = dropped_path(con.ask("path to C2KNXS2.UPD (drag the file here):", ""),
                               self.platform == host.WINDOWS)
        force = any(os.path.exists(os.path.join(lay.extract, f)) for f in firmware.IMAGES)
        label = "unpacking and verifying %s" % os.path.basename(upd)
        log = os.path.join(lay.logs, "firmware.log")
        argv, env = launcher_cmd("firmware", "--install", *(["--force"] if force else []), upd)
        if not con.run_phase(label, log, "only the v1.87 update (sha256 f211191a...) is supported; "
                             "the log says which image differed", argv, rel=lay.emu, env=env):
            con.die("the firmware was not installed; nothing in extract/ changed")
        if firmware.installed(lay.extract) and not o.dry:
            con.good("six images installed in %s/" % shown)

    def make_image(self, folder, expect_pioneer=True):
        con, lay = self.con, self.lay
        if expect_pioneer and not os.path.isdir(os.path.join(folder, "PIONEER")):
            con.warn("no PIONEER/ folder in %s: the deck browses rekordbox's database, so\n"
                     "      loose files will not show up. Export a playlist to a folder with rekordbox (Export\n"
                     "      mode, 'USB') and use that folder." % folder)
        mb = _mb(folder)
        size = max(256, (mb * 115 // 100 + 64 + 63) // 64 * 64)
        if size > 2000:
            con.fail("%s holds %d MB; the stick image is FAT16, at most 2 GB. Export fewer tracks." % (folder, mb))
            return False
        py = self.py_tools
        if not py:
            if not self.o.dry:
                con.fail("no Python with pyfatfs (see step 1)")
                return False
            py = "<a python with pyfatfs>"
        con.info("%d MB of music -> a %d MB FAT16 image (copied fresh for every run)" % (mb, size))
        argv = (pythons.argv_of(py) if py != sys.executable else host.python_argv()) + [
            os.path.join(lay.scripts, "media", "make_usb_image.py"), folder, lay.usb_image, str(size)]
        return con.run_phase("USB image from %s" % os.path.basename(folder.rstrip("/\\")),
                             os.path.join(lay.logs, "usb-image.log"),
                             "pyfatfs failing on import is usually setuptools>=81: pip install 'setuptools<81'",
                             argv, rel=lay.emu)

    def make_image_from_tracks(self, tracks):
        """Read tags (and estimate a missing BPM) with collection_xml.py, hand
        the result to baken's `expressport --generate-analysis` (no rekordbox
        needed), and image what it writes."""
        con, o, lay = self.con, self.o, self.lay
        baken_bin = baken.ensure(con, lay, o)
        if not baken_bin:
            if o.dry:
                baken_bin = "<baken>"
            else:
                con.warn("no baken: imaging %s as plain files instead -- the deck will still browse" % tracks)
                con.warn("and play them (a real NXS2 falls back to a plain folder browse when a stick")
                con.warn("has no rekordbox database at all), just with no waveform, beat grid or BPM")
                return self.make_image(tracks, expect_pioneer=False)
        py = self.py_tools
        if not py:
            if not o.dry:
                con.fail("no Python with pyfatfs (see step 1)")
                return False
            py = "<a python with pyfatfs>"
        shown = os.path.relpath(lay.usb_image, lay.root) if not lay.packaged else lay.usb_image
        stage = tempfile.mkdtemp(prefix="cdj-baken-")
        try:
            xml_path = os.path.join(stage, "collection.xml")
            argv = (pythons.argv_of(py) if py != sys.executable else host.python_argv()) + [
                os.path.join(lay.scripts, "media", "collection_xml.py"), tracks, xml_path]
            log = os.path.join(lay.logs, "collection-xml.log")
            if not con.run_phase("reading tags and estimating any missing BPM", log,
                                 "no audio files in that folder, or no mutagen -- the log says which",
                                 argv, rel=lay.emu):
                return False
            if not o.dry:
                for line in _read(log).splitlines():
                    if line.startswith("note: "):
                        con.warn(line[len("note: "):])
            device = os.path.join(stage, "device")
            os.makedirs(device, exist_ok=True)
            argv = [baken_bin, "expressport", "--device", device, "--generate-analysis",
                    "--no-settings", xml_path]
            if not con.run_phase("baken: writing the device export from %s"
                                 % os.path.basename(tracks.rstrip("/\\")),
                                 os.path.join(lay.logs, "baken-expressport.log"),
                                 "baken's own log above says which track failed and why", argv, rel=lay.emu):
                return False
            if o.dry:
                con.would("scripts/media/make_usb_image.py %s %s" % (device, shown))
                return True
            return self.make_image(device)
        finally:
            shutil.rmtree(stage, ignore_errors=True)

    def _choose_usb_source(self):
        con = self.con
        con.info("Two ways to fill the stick: a folder already exported by rekordbox, or a")
        con.info("plain folder of your own music (mp3, FLAC, AAC/M4A, WAV, AIFF, ALAC), which")
        con.info("baken (github.com/M-Igashi/baken, MIT) analyses instead -- no rekordbox")
        con.info("needed. Nothing is uploaded anywhere.")
        which = con.choose("rekordbox export, or a folder of music?", "rekordbox", "rekordbox", "folder")
        prompt = "folder of music:" if which == "folder" else "folder of your rekordbox USB export:"
        path = dropped_path(con.ask(prompt, ""), self.platform == host.WINDOWS)
        return (None, path) if which == "folder" else (path, None)

    def step_usb(self):
        con, o, lay = self.con, self.o, self.lay
        con.step(4, "USB stick (your own music)")
        shown = os.path.relpath(lay.usb_image, lay.root) if not lay.packaged else lay.usb_image
        if o.music and o.tracks:
            con.die("--music and --tracks name two different sources; pass only one")
        music, tracks = o.music, o.tracks

        if os.path.isfile(lay.usb_image) and not music and not tracks:
            con.good("USB image: %s (%d MB)" % (shown, -(-os.path.getsize(lay.usb_image) // (1 << 20))))
            if not (con.interactive and con.ask_yn("make a new one from another folder?", "n")):
                return
            music, tracks = self._choose_usb_source()

        if not music and not tracks:
            if not con.interactive:
                if o.dry:
                    con.would("scripts/media/make_usb_image.py <your rekordbox export> %s" % shown)
                    con.would("collection_xml.py <a folder of music> collection.xml, baken expressport "
                              "--generate-analysis, then make_usb_image.py %s" % shown)
                    return
                con.die("no USB image: pass --music <a rekordbox export folder>, or --tracks <a folder of music>")
            music, tracks = self._choose_usb_source()

        os.makedirs(lay.extract, exist_ok=True)
        if tracks:
            while not os.path.isdir(tracks):
                if not con.interactive:
                    con.die("no such folder: %s" % tracks)
                con.warn("no such folder: %s" % tracks)
                tracks = dropped_path(con.ask("folder of music:", ""), self.platform == host.WINDOWS)
            if not self.make_image_from_tracks(tracks):
                con.die("no USB image was made")
            return

        while not music or not os.path.isdir(music):
            if not con.interactive:
                con.die("no USB image: pass --music /path/to/your/rekordbox/export")
            con.info("Point me at a folder with your own music exported by rekordbox (the")
            con.info("folder that holds PIONEER/ and the tracks). Nothing is uploaded anywhere.")
            if music:
                con.warn("no such folder: %s" % music)
            music = dropped_path(con.ask("music folder:", ""), self.platform == host.WINDOWS)
        if not self.make_image(music):
            con.die("no USB image was made")

    def deck_ready(self):
        main, gui, dsp = self.binaries()
        return all(os.path.exists(p) for p in (main, gui, dsp)) and firmware.installed(self.lay.extract) \
            and os.path.isfile(self.lay.usb_image)

    def _stop_background(self):
        # Ctrl-C reached the headless deck too, which stops in order.
        self.con._p("")
        self.con.die("stopped; the headless deck was shut down")

    def step_dsp(self):
        con, o, lay = self.con, self.o, self.lay
        cache = lay.jit_cache
        shown = "~/c14gen" if not lay.packaged else cache
        curated_so = os.path.join(cache, "curated", "m.so")
        con.step(5, "DSP code (the JIT's cache)")
        con.info("The DSP program runs through a JIT that compiles its hot code to native")
        con.info("modules, cached in %s. Until they are there a deck runs slower than" % shown)
        con.info("real time (audio gaps), so setup builds them now.")
        done = False
        if o.curated:
            if lay.packaged:
                con.warn("the curated module is a maintainer build; the packaged program uses the warm-up")
            else:
                done = self._curated(curated_so)
            if not done:
                # Without this the "already warm" line below reads as if the curated build ran.
                con.warn("--curated-jit did NOT build a module (the reason is above);")
                con.warn("check it with: bash scripts/build/build_dsp_module.sh --preflight")
        if done:
            return
        n = _modules(cache)
        warm, env = launcher_cmd("run", "warm_jit", "warm")
        log = os.path.join(lay.logs, "warm-jit-%s.log" % self.ts)
        if o.warm == "off":
            con.dim("warm-up skipped (--no-warm): the first minutes of your first sessions will be slow")
        elif os.path.isfile(curated_so) and o.warm != "force":
            con.good("curated module in %s/curated: nothing to warm" % shown)
        elif n > 0 and o.warm != "force":
            con.good("already warm: %d compiled modules in %s (--warm adds more)" % (n, shown))
        elif not o.dry and not self.deck_ready():
            con.warn("warm-up skipped: it needs the emulators, the firmware and the USB stick;")
            con.warn("run ./setup.sh --warm once they are there")
        elif o.warm == "auto" and not con.ask_yn(
                "warm it now? (one headless deck plays for about 15 minutes)", "y"):
            con.dim("skipped; ./setup.sh --warm does it later")
        else:
            try:
                ok = con.run_phase("warming the DSP JIT: a headless deck plays with a tempo sweep (~15 min)",
                                   log, "the deck's own log is %s; ./setup.sh --warm tries again"
                                   % os.path.join(lay.tmp, "bridge-main-warm1.log"), warm, rel=lay.emu, env=env)
            except KeyboardInterrupt:
                self._stop_background()
            if ok and o.dry:
                subprocess.run(warm + ["--dry-run"], env=env)
            elif ok:
                text = _read(log)
                built = re.findall(r"^warm: (\d+) new", text, re.M)
                auto = [ln for ln in text.splitlines() if "c66x jit auto:" in ln]
                if auto:
                    con.dim(auto[-1].split("c66x jit ", 1)[-1])
                if built and int(built[-1]) > 0:
                    con.good("%s new modules compiled; %d in %s" % (built[-1], _modules(cache), shown))
                else:
                    con.warn("the JIT compiled nothing new; the end of %s says why" % os.path.relpath(log, lay.emu))

    def _curated(self, curated_so):
        con, o, lay = self.con, self.o, self.lay
        bash = host.find_bash() or "bash"
        cmd = [bash, host.posix(os.path.join(lay.scripts, "build", "build_dsp_module.sh"))]
        if o.keep_recording:
            cmd.append("--keep-recording")
        log = os.path.join(lay.logs, "dsp-module-%s.log" % self.ts)
        if o.dry:
            con.would("%s > logs/dsp-module-%s.log, which runs:" % (" ".join(cmd), self.ts))
            r = subprocess.run(cmd + ["--dry-run"], capture_output=True, text=True)
            for line in (r.stdout + r.stderr).splitlines():
                con._p("      " + line)
            return True
        if not self.deck_ready():
            con.warn("the curated module needs the emulators, the firmware and the USB stick first")
            return False
        r = subprocess.run(cmd + ["--preflight"], capture_output=True, text=True)
        if r.returncode != 0:
            con.warn("no curated module: %s" % "; ".join((r.stdout + r.stderr).strip().splitlines()))
            return False
        try:
            ok = con.run_phase("curated DSP module: record, generate, profile-guided build (about an hour)", log,
                               "the deck's own log is %s; a module that does not replay EXACT is not installed"
                               % os.path.join(lay.tmp, "bridge-main-rec1.log"), cmd, rel=lay.emu)
        except KeyboardInterrupt:
            self._stop_background()
        if ok:
            con.good("installed ~/c14gen/curated/m.so: ./start.sh loads it from now on")
            return True
        con.warn("no curated module; falling back to the quick warm-up")
        return False

    def step_config(self):
        con, o, c, lay = self.con, self.o, self.c, self.lay
        con.step(6, "Your setup")
        asked = bool(o.decks or o.name or o.djlink or o.audio or o.controller or o.relay)
        if os.path.isfile(lay.conf) and not o.reconfigure and not asked:
            con.good("keeping your setup in cdj.conf (./setup.sh --reconfigure to change it)")
            self.ask5 = False
        else:
            self.ask5 = True
        if self.ask5:
            self._questions()
        # A kept setup still takes the Pythons found now, e.g. after installing mido.
        if not self.ask5 and not lay.packaged and (
                (self.py_midi and not _same(self.py_midi, c["CDJ_MIDI_PYTHON"]))
                or (self.py_tools and not _same(self.py_tools, c["CDJ_TOOLS_PYTHON"]))):
            c["CDJ_MIDI_PYTHON"] = host.posix(self.py_midi or c["CDJ_MIDI_PYTHON"])
            c["CDJ_TOOLS_PYTHON"] = host.posix(self.py_tools or c["CDJ_TOOLS_PYTHON"])
            self.ask5 = True
        if self.ask5:
            values = dict(self.values)
            values.update({k: c[k] for k in conf.KEYS})
            if o.dry:
                con.info("(dry run) would write cdj.conf:")
                for line in conf.render(values).splitlines():
                    con._p("      " + line)
            else:
                conf.save(lay.conf, values)
                con.good("saved to cdj.conf")

    def _questions(self):
        con, o, c, lay = self.con, self.o, self.c, self.lay
        con.info("Two decks run on a shared Pro DJ Link network and SYNC to each other, but")
        con.info("need roughly twice the CPU of one (this machine: %d threads)." % self.cores)
        if o.decks not in ("", "1", "2"):
            con.die("--decks takes 1 or 2")
        c["CDJ_DECKS"] = o.decks or con.choose("how many decks?", c["CDJ_DECKS"] or "1", "1", "2")
        name = o.name or con.ask("a name for the deck windows and logs:", c["CDJ_NAME"] or "show")
        c["CDJ_NAME"] = re.sub(r"[^A-Za-z0-9]", "", name) or "show"
        if o.djlink:
            c["CDJ_DJLINK"] = _onoff(o.djlink)
        else:
            c["CDJ_DJLINK"] = _onoff(con.choose("Pro DJ Link network?",
                                                "on" if c["CDJ_DECKS"] == "2" else "off", "on", "off"))
        if o.audio:
            c["CDJ_AUDIO"] = _onoff(o.audio)
        else:
            c["CDJ_AUDIO"] = _onoff(con.choose("sound?", "on" if (c["CDJ_AUDIO"] or "1") == "1" else "off",
                                               "on", "off"))

        # The controller: a profile from midi/controllers/, none, or learn one now.
        profiles = sorted(os.path.splitext(os.path.basename(f))[0]
                          for f in glob.glob(os.path.join(lay.emu, "midi", "controllers", "*.json")))
        detected = ""
        if self.py_midi:
            r = subprocess.run(pythons.argv_of(self.py_midi) + [
                "-c", "import mido; print('\\n'.join(mido.get_input_names()))"],
                capture_output=True, text=True)
            inputs = r.stdout.lower()
            for p in profiles:
                m = re.search(r'"match":\s*"([^"]*)"', _read(os.path.join(lay.emu, "midi", "controllers",
                                                                          p + ".json")))
                if m and m.group(1).lower() in inputs:
                    detected = p
                    break
            if detected:
                con.good("found a connected controller: %s" % detected)
        con.info("MIDI controller: 'none', one of: %s, or 'learn' to teach it a new one" % " ".join(profiles))
        ctl = o.controller or con.ask("controller?", c["CDJ_CONTROLLER"] or detected or "none")
        if ctl == "learn":
            if not self.py_midi and not o.dry:
                con.warn("learning needs a Python with mido + python-rtmidi (%s);" % self.midi_fix)
                con.warn("no controller for now -- run ./setup.sh --reconfigure after installing them")
                ctl = "none"
            else:
                cname = con.ask("a short name for it (e.g. ddj-400):", "my-controller")
                cname = re.sub(r"[^a-z0-9-]", "", cname.lower().replace(" ", "-"))
                py = self.py_midi or "python"
                if o.dry:
                    con.would("%s midi/learn.py new --controller %s; %s midi/learn.py map --controller %s"
                              % (py, cname, py, cname))
                else:
                    argv = pythons.argv_of(self.py_midi)
                    ok = subprocess.run(argv + ["midi/learn.py", "new", "--controller", cname, "--name", cname],
                                        cwd=lay.emu).returncode == 0 and subprocess.run(
                        argv + ["midi/learn.py", "map", "--controller", cname, "--prefix", c["CDJ_NAME"]],
                        cwd=lay.emu).returncode == 0
                    if not ok:
                        con.warn("the learn session did not finish; the controller is saved as far as it got")
                ctl = cname
        elif ctl != "none" and not os.path.isfile(os.path.join(lay.emu, "midi", "controllers", ctl + ".json")):
            con.warn("no profile midi/controllers/%s.json -- no controller for now" % ctl)
            ctl = "none"
        c["CDJ_CONTROLLER"] = ctl
        if ctl != "none" and not self.py_midi:
            con.warn("the bridge needs a Python with mido + python-rtmidi; until then ./start.sh runs without it")
            con.warn("  %s" % self.midi_fix)

        # Windows reserves whole port ranges for Hyper-V and WSL, and a bind
        # inside one fails (the relay's TCP port, Pro DJ Link's UDP one).
        want = int(o.relay or c["CDJ_RELAY_PORT"] or 7202)
        port = host.pick_tcp_port(want)
        c["CDJ_RELAY_PORT"] = str(port)
        if port != want:
            con.warn("TCP %d is reserved or in use here; the controller relay uses %d" % (want, port))
        gport = int((c["CDJ_GROUP"] or "239.77.77.1:45000").rsplit(":", 1)[-1] or 45000)
        gfree = host.pick_udp_port(gport)
        if gfree != gport and c["CDJ_DJLINK"] == "1":
            con.warn("UDP %d is reserved by Windows; Pro DJ Link uses %d" % (gport, gfree))
        c["CDJ_GROUP"] = "239.77.77.1:%d" % gfree
        if not lay.packaged:
            c["CDJ_MIDI_PYTHON"] = host.posix(self.py_midi or "")
            c["CDJ_TOOLS_PYTHON"] = host.posix(self.py_tools or "")

    def summary(self):
        con, c, lay, o = self.con, self.c, self.lay, self.o
        con._p("\n%sReady.%s" % (con.B + con.G, con.N))
        decks = c["CDJ_DECKS"] or "1"
        name = c["CDJ_NAME"] or "show"
        con.info("decks        %s (%s1%s)" % (decks, name, ", %s2" % name if decks == "2" else ""))
        djl = c["CDJ_DJLINK"] or "0"
        con.info("Pro DJ Link  %s%s" % (_yesno(djl), " (%s)" % c["CDJ_GROUP"] if djl == "1" else ""))
        con.info("sound        %s" % _yesno(c["CDJ_AUDIO"] or "1"))
        ctl = c["CDJ_CONTROLLER"] or "none"
        con.info("controller   %s%s" % (ctl, " (relay on 127.0.0.1:%s)" % c["CDJ_RELAY_PORT"] if ctl != "none" else ""))
        cache = lay.jit_cache
        shown = "~/c14gen" if not lay.packaged else cache
        if os.path.isfile(os.path.join(cache, "curated", "m.so")):
            con.info("DSP JIT      curated module (%s/curated)" % shown)
        elif _modules(cache) > 0:
            con.info("DSP JIT      %d cached modules (%s)" % (_modules(cache), shown))
        else:
            con.info("DSP JIT      cold: the first minutes of your first sessions will be slow")
        run = "CDJ-Emulator" if lay.packaged else "./start.sh"
        con.info("start it:    %s%s%s      stop: Ctrl-C (or %s stop from another shell)" % (con.B, run, con.N, run))
        main = self.binaries()[0]
        if (not o.dry and con.interactive and firmware.installed(lay.extract) and os.path.isfile(lay.usb_image)
                and os.path.exists(main) and con.ask_yn("start the deck now?", "y")):
            from . import start

            return start.main([])
        return 0


def ready(lay):
    return os.path.isfile(lay.conf) and firmware.installed(lay.extract) and os.path.isfile(lay.usb_image)


def missing_prerequisites(platform):
    """The tools and pkg-config modules building the emulator needs that this
    machine lacks."""
    tools = ["gcc", "make", "ninja", "meson", "pkg-config", "flex", "bison", "git", "patch"]
    tools += ["python", "diff"] if platform == host.WINDOWS else ["python3"]
    # The window: GTK on Windows and Linux, macOS's own Cocoa there. The sound:
    # SDL2 (WASAPI) on Windows, PulseAudio on Linux, Core Audio on macOS.
    libs = {host.WINDOWS: ["glib-2.0", "pixman-1", "zlib", "gtk+-3.0", "sdl2"],
            host.MACOS: ["glib-2.0", "pixman-1", "zlib"]}.get(
        platform, ["glib-2.0", "pixman-1", "zlib", "gtk+-3.0", "libpulse"])
    missing = [t for t in tools if not shutil.which(t)]
    # build.sh needs bash 4; macOS's own is 3.2.
    if platform == host.MACOS and not host.find_bash():
        missing.append("bash")
    if not shutil.which("pkg-config"):
        return missing + libs
    return missing + [lib for lib in libs
                      if subprocess.run(["pkg-config", "--exists", lib], capture_output=True).returncode]


def packages_for(platform, missing):
    """The packages that provide them, in this OS's package manager's names
    (Debian's on Linux)."""
    return sorted({p for x in missing for p in _pkg_for(platform, x).split()})


def _pkg_for(platform, x):
    """The package that provides a tool or pkg-config module here."""
    if platform == host.WINDOWS:
        return {"gcc": "mingw-w64-x86_64-gcc", "ninja": "mingw-w64-x86_64-ninja",
                "meson": "mingw-w64-x86_64-meson", "pkg-config": "mingw-w64-x86_64-pkgconf",
                "python": "mingw-w64-x86_64-python", "glib-2.0": "mingw-w64-x86_64-glib2",
                "pixman-1": "mingw-w64-x86_64-pixman", "zlib": "mingw-w64-x86_64-zlib",
                "gtk+-3.0": "mingw-w64-x86_64-gtk3", "sdl2": "mingw-w64-x86_64-SDL2",
                "diff": "diffutils"}.get(x, x)
    if platform == host.MACOS:
        return {"pkg-config": "pkgconf", "python3": "python", "glib-2.0": "glib",
                "pixman-1": "pixman"}.get(x, x)
    return {"gcc": "build-essential", "make": "build-essential", "ninja": "ninja-build",
            "python3": "python3 python3-venv python3-pip", "glib-2.0": "libglib2.0-dev",
            "pixman-1": "libpixman-1-dev", "zlib": "zlib1g-dev", "gtk+-3.0": "libgtk-3-dev libepoxy-dev",
            "libpulse": "libpulse-dev"}.get(x, x)


def _is_wsl():
    try:
        with open("/proc/version") as f:
            return "microsoft" in f.read().lower()
    except OSError:
        return False


def _same(a, b):
    return bool(a) and bool(b) and host.native(a) == host.native(b)


def _read(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def _run_fix(fix, cwd):
    """Run a pip_fix command the way the shell did (it may use shell syntax)."""
    if host.is_windows():
        import shlex

        return subprocess.run(shlex.split(fix), cwd=cwd).returncode == 0
    return subprocess.run(["/bin/sh", "-c", fix], cwd=cwd).returncode == 0


def main(argv):
    o = parse_args(argv)
    try:
        return Setup(o).run()
    except KeyboardInterrupt:
        sys.stderr.write("\n")
        return 130
