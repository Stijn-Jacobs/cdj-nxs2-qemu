# SPDX-License-Identifier: GPL-2.0-or-later
"""Start one older player: a model whose profile says MODEL_LAUNCH=deck. The
CDJ-2000 and the CDJ-2000NXS run their display board inside the MAIN
emulator; the XDJ-1000, XDJ-700 and CDJ-900NXS have none, MAIN draws the
screen. Either way a single QEMU process makes the window; there is no second
board, no DSP rig and no relay.
"""

import os
import shlex
import subprocess

from . import chain, host, model
from .chain import nonempty, say
from .layout import Layout

# The panel key socket's UDP port, which the key scripts send to.
PANEL_KEY_PORT = 40124


def main_qemu(lay, env):
    """The MAIN emulator: MAIN_QEMU, else the one in QEMU_BUILD. The older
    boards are machines of the same qemu-system-sh4 as the NXS2's MAIN."""
    if lay.packaged:
        return lay.qemu_binaries()[0]
    if env.get("MAIN_QEMU"):
        return host.native(env["MAIN_QEMU"])
    build = env.get("QEMU_BUILD") or ("/c/qemu-build-mingw" if host.is_windows()
                                      else os.path.join(host.home(), "qemu-build"))
    return os.path.join(host.native(build), "qemu-system-sh4" + host.exe_suffix())


def missing_images(lay, m):
    folder = model.extract_dir(lay, m)
    return [m.extract + "/" + n for n in m.images if not os.path.isfile(os.path.join(folder, n))]


def command(lay, m, env):
    """(argv, env) of the player's QEMU. The stick is a snapshot, so the
    firmware's writes to it never reach the image."""
    folder = model.extract_dir(lay, m)
    env = dict(env)
    env["CDJ_ATA"] = "1"
    if m.display_upd:
        env["CDJ_BF531_UPD"] = host.native(os.path.join(folder, model.DISPLAY_UPD_IMAGE))
    env["CDJ_PANEL_KEYSOCK"] = nonempty(env, "CDJ_PANEL_KEYSOCK", str(host.pick_udp_port(PANEL_KEY_PORT)))
    argv = [main_qemu(lay, env), "-M", m.main_machine, "-name", m.title,
            "-kernel", host.native(os.path.join(folder, "main_unpacked.bin")),
            "-serial", "file:" + os.path.join(lay.logs, m.id + ".log"),
            "-drive", "if=none,id=usbstk,file=%s,format=raw,snapshot=on" % host.native(lay.usb_image),
            "-device", "usb-storage,drive=usbstk,port=1"]
    if env.get("GUI_DISPLAY"):
        argv += ["-display", env["GUI_DISPLAY"]]
    argv += shlex.split(env.get("MAIN_ARGS", ""))
    return argv, env


def start(m, env, dry):
    """Run the player until its window closes."""
    lay = Layout()
    missing = missing_images(lay, m)
    if missing:
        chain.err("missing firmware images: %s -- run ./setup.sh --model %s --firmware <the update file>"
                  % (" ".join(missing), m.id))
        return 1
    if not os.path.isfile(lay.usb_image):
        chain.err("no USB image yet -- run ./setup.sh --music <your rekordbox USB folder>")
        return 1
    argv, env = command(lay, m, env)
    say("%s: one window with the USB stick. Sound, Pro DJ Link, a second deck, MIDI controllers, mods and "
        "the virtual deck app are CDJ-2000NXS2 features and are skipped." % m.title)
    if dry:
        knobs = ["CDJ_ATA", "CDJ_BF531_UPD", "CDJ_PANEL_KEYSOCK"]
        say("would run:  %s %s" % (" ".join("%s=%s" % (k, env[k]) for k in knobs if k in env),
                                   " ".join(shlex.quote(a) for a in argv)))
        return 0
    os.makedirs(lay.logs, exist_ok=True)
    say("the first boot takes a minute; close the window or press Ctrl-C to stop.")
    qemu = subprocess.Popen(argv, env=env)
    try:
        qemu.wait()
    except KeyboardInterrupt:
        qemu.terminate()
        qemu.wait()
    say("stopped.")
    return 0
