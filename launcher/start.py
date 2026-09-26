# SPDX-License-Identifier: GPL-2.0-or-later
"""Start the decks the way setup configured them (cdj.conf): one or two
CDJ-2000NXS2 windows, the controller relay and, if you chose a MIDI
controller, the bridge. Ctrl-C stops everything.

  usage: ./start.sh             start
         ./start.sh --app       start with the virtual deck app as the window
         ./start.sh stop        stop a running rig from another shell
         ./start.sh --dry-run   show what would be started
  env:   every knob of scripts/run/rig.sh still works (AUDIODEV=, NOSOUND=1,
         GUI_DISPLAY=, PRIO=, TBFAST=0, ...).
"""

import glob
import os
import shlex
import shutil
import signal
import subprocess
import sys
import time

from . import chain, conf, host
from .chain import nonempty, say
from .console import stdin_is_tty
from .layout import Layout


def _python_cmd(value, default=()):
    """A recorded Python: an absolute path (possibly with spaces, possibly an
    MSYS2 /c/... one) or a command such as "py -3"."""
    if not value:
        return list(default)
    if host.is_file(host.native(value)):
        return [host.native(value)]
    argv = shlex.split(value)
    return argv if argv and shutil.which(argv[0]) else []


def _stale_builds(lay, env):
    """Build trees older than the board sources, judged by
    scripts/build/source_stamp.sh, which build.sh stamps them with."""
    bash = host.find_bash()
    if lay.packaged or not bash:
        return []
    script = '. "%s"; CDJ_EMU_DIR="%s"; cdj_stale_builds' % (
        host.posix(os.path.join(lay.scripts, "build", "source_stamp.sh")), host.posix(lay.emu))
    out = subprocess.run([bash, "-c", script], env=env, capture_output=True, text=True).stdout
    return [line for line in out.splitlines() if line.strip()]


def stop(lay):
    """Ask every running rig to stop its decks in order, then end any QEMU
    that is still there."""
    requests = []
    for f in glob.glob(os.path.join(lay.tmp, "cdj-stop-*")):
        open(f, "a").close()
        requests.append(f)
    deadline = time.time() + 75
    while any(os.path.exists(f) for f in requests) and time.time() < deadline:
        time.sleep(0.5)
    if host.is_windows():
        subprocess.run(["taskkill", "/F", "/IM", "qemu-system-sh4.exe", "/IM", "qemu-system-sh4eb.exe"],
                       capture_output=True)
    else:
        subprocess.run(["pkill", "-x", "qemu-system-sh4"], capture_output=True)
        subprocess.run(["pkill", "-f", "qemu-system-sh4eb"], capture_output=True)
    say("stopped.")
    return 0


def main(argv):
    dry, app = False, None
    for a in argv:
        if a == "stop":
            return stop(Layout())
        if a == "--dry-run":
            dry = True
        elif a == "--app":
            app = True
        elif a == "--no-app":
            app = False
        elif a in ("-h", "--help"):
            sys.stdout.write(__doc__[__doc__.index("  usage:"):])
            return 0
        else:
            chain.err("unknown argument: %s (./start.sh --help)" % a)
            return 2
    lay = Layout()
    if not os.path.isfile(lay.conf):
        chain.err("no cdj.conf yet -- run ./setup.sh first (it builds, prepares the firmware")
        chain.err("and the USB stick, and asks how you want the decks set up).")
        return 1
    values = conf.load(lay.conf, warn=chain.err)
    c = conf.with_start_defaults(values)
    if app is None:
        # The packaged program's window is the virtual deck app unless the
        # settings say otherwise.
        app = c["CDJ_APP"] == "1" or (lay.packaged and "CDJ_APP" not in values)
    env = dict(os.environ)
    for k in ("QEMU_BUILD", "QEMU_EB_BUILD"):
        if c[k]:
            env[k] = c[k]
    if lay.packaged:
        env["MAIN_QEMU"], env["GUI_QEMU"] = lay.qemu_binaries()
    env.update(RELAY_PORT=c["CDJ_RELAY_PORT"], DJLINK=c["CDJ_DJLINK"], GROUP=c["CDJ_GROUP"])
    if c["CDJ_AUDIO"] != "1":
        env["NOSOUND"] = "1"
    # One frame of 24 hours: the rig lives until Ctrl-C and writes nothing per frame.
    for k, v in (("FRAMES", "1"), ("MOTION_MS", "86400000"), ("AUTOLOAD", "0")):
        env[k] = nonempty(env, k, v)
    decks = "2" if c["CDJ_DECKS"] == "2" else "1"
    launch = chain.script_argv("live_linked" if decks == "2" else "live", [c["CDJ_NAME"], decks])

    # The boards and patches are compiled into the QEMU binaries, so after a
    # pull that changed them the decks would run the old code. STALE_CHECK=0
    # skips the check.
    if env.get("STALE_CHECK", "1") == "1":
        stale = _stale_builds(lay, env)
        if stale:
            say("the emulator's source has changed since it was last built:")
            for s in stale:
                say("    " + s)
            if not dry and stdin_is_tty():
                ans = input("rebuild now (a few minutes)? [Y/n] ").strip() or "y"
                if ans[:1] in "Yy":
                    if subprocess.run([host.find_bash(), host.posix(os.path.join(lay.emu, "build.sh")),
                                       "main", "display"]).returncode:
                        return 1
                else:
                    say("starting the old build (./build.sh main display rebuilds it)")
            else:
                say("run ./build.sh main display to pick the changes up")
    missing = [f for f in ("main_unpacked.bin", "gui_unpacked.bin", "flash.bin")
               if not os.path.isfile(os.path.join(lay.extract, f))]
    if missing:
        chain.err("missing firmware images: %s -- run ./setup.sh --firmware <C2KNXS2.UPD>"
                  % " ".join("extract/" + f for f in missing))
        return 1
    if nonempty(env, "MEDIA_MODE", "img") == "img" and not env.get("MEDIA_IMG_SRC") \
            and not os.path.isfile(lay.usb_image):
        chain.err("no USB image yet -- run ./setup.sh --music <your rekordbox USB folder>")
        return 1

    # The virtual deck app (app/virtual_deck.py) replaces the display board's
    # QEMU window: each deck's screen goes to a VNC server on loopback and a
    # frame file, and the app draws the player around it.
    app_cmd = []
    if app:
        # setup installs Pillow into .venv/, so the app runs there when it
        # exists; this interpreter is for setups without one (MSYS2's Pillow).
        venv = os.path.join(lay.emu, ".venv", "bin", "python")
        app_py = _python_cmd(c["CDJ_APP_PYTHON"],
                             [venv] if os.access(venv, os.X_OK) else host.python_argv())
        if not lay.packaged and subprocess.run(app_py + ["-c", "import tkinter, PIL.ImageTk"],
                                               capture_output=True).returncode:
            chain.err("the virtual deck app needs tkinter and Pillow's ImageTk in %s:" % " ".join(app_py))
            chain.err("  MSYS2:  pacman -S --needed mingw-w64-x86_64-tk mingw-w64-x86_64-python-pillow")
            chain.err("  Debian/Ubuntu:  sudo apt install python3-tk python3-pil.imagetk")
            chain.err("  macOS (Homebrew):  brew install python-tk   (Pillow in .venv/)")
            chain.err("or run without --app (the plain deck windows).")
            return 1
        env["GUI_DISPLAY"] = "vnc"
        env["CDJ_APP_VNC_BASE"] = nonempty(env, "CDJ_APP_VNC_BASE", "5920")
        # Both QEMU and a Windows Python want a native path here.
        env["CDJ_APP_FRAME_DIR"] = env.get("CDJ_APP_FRAME_DIR") or (
            host.native(env["TMPDIR"]) if env.get("TMPDIR") else lay.tmp)
        app_cmd = app_py + [os.path.join(lay.emu, "app", "virtual_deck.py"), "--decks", decks,
                            "--prefix", c["CDJ_NAME"], "--relay", "127.0.0.1:" + c["CDJ_RELAY_PORT"],
                            "--vnc-base", env["CDJ_APP_VNC_BASE"], "--frame-dir", env["CDJ_APP_FRAME_DIR"]]

    # The bridge runs on the Python setup recorded: on Windows a native one.
    bridge = []
    if c["CDJ_CONTROLLER"] != "none":
        py = _python_cmd(c["CDJ_MIDI_PYTHON"], host.python_argv() if lay.packaged else ())
        if not py:
            say("controller '%s' is configured but the Python with mido +" % c["CDJ_CONTROLLER"])
            say("python-rtmidi (%s) is not there; starting" % (c["CDJ_MIDI_PYTHON"] or "none recorded"))
            say("without it (install them, then ./setup.sh)")
        else:
            bridge = py + ["-u", os.path.join(lay.emu, "midi", "bridge.py"), "--controller", c["CDJ_CONTROLLER"],
                           "--relay", "127.0.0.1:" + c["CDJ_RELAY_PORT"], "--prefix", c["CDJ_NAME"]]

    say("decks: %s (%s)   Pro DJ Link: %s   sound: %s   controller: %s   window: %s" % (
        decks, c["CDJ_NAME"], "on " + env["GROUP"] if env["DJLINK"] == "1" else "off",
        "on" if c["CDJ_AUDIO"] == "1" else "off", c["CDJ_CONTROLLER"], "virtual deck app" if app else "QEMU"))
    if dry:
        say("would run:  RELAY_PORT=%s DJLINK=%s GROUP=%s%s%s %s" % (
            env["RELAY_PORT"], env["DJLINK"], env["GROUP"], " NOSOUND=1" if env.get("NOSOUND") == "1" else "",
            " QEMU_BUILD=" + env["QEMU_BUILD"] if env.get("QEMU_BUILD") else "", _shown(launch)))
        if bridge:
            say("and:        %s > logs/bridge.log" % " ".join(shlex.quote(a) for a in bridge))
        if app_cmd:
            say("with:       GUI_DISPLAY=vnc CDJ_APP_VNC_BASE=%s, then %s"
                % (env["CDJ_APP_VNC_BASE"], " ".join(shlex.quote(a) for a in app_cmd)))
        return 0
    return run(lay, env, launch, bridge, app_cmd)


def _shown(argv):
    if os.path.basename(argv[0]).startswith("bash"):
        argv = ["bash"] + argv[1:]
    return " ".join(argv)


def run(lay, env, launch, bridge, app_cmd):
    os.makedirs(lay.logs, exist_ok=True)
    env["LAUNCHER_STOP_FILE"] = "%s/cdj-stop-%d" % (lay.tmp, os.getpid())
    env = chain.launcher_env(env)
    signal.signal(signal.SIGINT, lambda *_: open(env["LAUNCHER_STOP_FILE"], "a").close())
    helpers = []
    try:
        if bridge:
            # The bridge reconnects on its own, so it can start before the relay is up.
            with open(os.path.join(lay.logs, "bridge.log"), "wb") as log:
                helpers.append(subprocess.Popen(bridge, stdout=log, stderr=subprocess.STDOUT))
            say("controller bridge running (log: logs/bridge.log)")
        say("the first boot takes a minute; then press USB (or LINK) to browse, load a track and play.")
        if not app_cmd:
            return subprocess.Popen(launch, env=env).wait()
        # With the app, the rig runs behind it and lives as long as its window:
        # closing the window or Ctrl-C stops the decks.
        with open(os.path.join(lay.logs, "rig.log"), "wb") as log:
            rig = subprocess.Popen(launch, env=env, stdout=log, stderr=subprocess.STDOUT)
        say("decks starting (log: logs/rig.log); the virtual deck window opens now.")
        window = subprocess.Popen(app_cmd, env=env)
        while window.poll() is None and rig.poll() is None and not os.path.exists(env["LAUNCHER_STOP_FILE"]):
            time.sleep(0.5)
        open(env["LAUNCHER_STOP_FILE"], "a").close()
        if window.poll() is None:
            window.terminate()
        rig.wait()
        say("stopped.")
        return 0
    finally:
        for p in helpers:
            p.terminate()
        chain.remove(env["LAUNCHER_STOP_FILE"])
