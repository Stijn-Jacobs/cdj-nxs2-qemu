# SPDX-License-Identifier: GPL-2.0-or-later
"""Start one older player: a model whose profile says MODEL_LAUNCH=deck. The
CDJ-2000 and the CDJ-2000NXS run their display board inside the MAIN
emulator; the XDJ-1000, XDJ-700 and CDJ-900NXS have none, MAIN draws the
screen. Either way a single QEMU process makes the window; there is no second
board, no DSP rig and no relay.

SNAPSHOT=<point> (idle or loaded) starts the deck from that saved point; when
it is not saved yet the deck boots as usual and scripts/run/snapshot_deck.py
drives it there and saves it (the profile's MODEL_IDLE_S and MODEL_LOAD_STEPS).
Points are cached per build, firmware, medium and MAIN_ARGS (snapshot.py).
"""

import os
import shlex
import subprocess

from . import chain, host, model, snapshot
from .boot_deck import _monsock, window_display
from .chain import nonempty, say
from .layout import Layout
from .rig import default_audiodev

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


def dsp_env(lay, m, env):
    """The DSP knobs a model's profile sets: the busy-wait loop the core skips,
    the interrupt fast path, and the module built by build_dsp_module.sh unless
    MODULE=none or C66X_JIT names one by hand."""
    knobs = {"CDJ_C6747_IDLE": nonempty(env, "CDJ_C6747_IDLE", m.dsp_idle)}
    if m.dsp_isr_fast:
        knobs["C66X_IDLE_ISR_FAST"] = nonempty(env, "C66X_IDLE_ISR_FAST", m.dsp_isr_fast)
    module = os.path.join(lay.jit_cache, m.module_dir, "m.so")
    if env.get("C66X_JIT") or env.get("MODULE") == "none":
        return knobs
    if os.path.isfile(module):
        knobs["C66X_JIT"] = host.native(module)
    else:
        say("no DSP module for the %s yet: its DSP runs interpreted and the sound will gap "
            "(./setup.sh --model %s builds it)" % (m.title, m.id))
    return knobs


def command(lay, m, env):
    """(argv, env) of the player's QEMU. The stick is a snapshot, so the
    firmware's writes to it never reach the image."""
    folder = model.extract_dir(lay, m)
    env = dict(env)
    env["CDJ_ATA"] = "1"
    if m.has_dsp_module:
        env.update(dsp_env(lay, m, env))
    if m.display_upd:
        env["CDJ_BF531_UPD"] = host.native(os.path.join(folder, model.DISPLAY_UPD_IMAGE))
    env["CDJ_PANEL_KEYSOCK"] = nonempty(env, "CDJ_PANEL_KEYSOCK", str(host.pick_udp_port(PANEL_KEY_PORT)))
    argv = [main_qemu(lay, env), "-M", m.main_machine, "-name", m.title,
            "-kernel", host.native(os.path.join(folder, "main_unpacked.bin")),
            "-serial", "file:" + os.path.join(lay.logs, m.id + ".log"),
            "-drive", "if=none,id=usbstk,file=%s,format=raw,snapshot=on" % host.native(lay.usb_image),
            "-device", "usb-storage,drive=usbstk,port=1"]
    notes = []
    display = window_display(nonempty(env, "GUI_DISPLAY", "gtk"), m.id, notes)
    for _level, text in notes:
        say(text)
    # The window has an absolute pointer (so the host never grabs the mouse),
    # and QEMU then hides the host cursor over it unless show-cursor is on.
    argv += ["-display", display]
    if nonempty(env, "NOSOUND", "0") != "1":
        # The same ring, prefill and latency cap (ms) as the NXS2's rig.
        env["CDJ_DSP_AUDIO"] = nonempty(env, "CDJ_DSP_AUDIO", "1:3000:150:450")
        argv += ["-audio", nonempty(env, "AUDIODEV", default_audiodev())]
    argv += shlex.split(env.get("MAIN_ARGS", ""))
    return argv, env


def plan_snapshot(lay, m, argv, env):
    """Adds what SNAPSHOT asks for to argv. Returns the argv of the driver that
    reaches and saves the point, or None when there is nothing to save."""
    point = env.get("SNAPSHOT", "")
    if not point:
        return None
    if point not in snapshot.POINTS:
        raise SystemExit("SNAPSHOT point %r: known points are %s" % (point, ", ".join(snapshot.POINTS)))
    if not (m.idle_s and m.load_steps):
        raise SystemExit("SNAPSHOT: models/%s.conf sets no MODEL_IDLE_S and MODEL_LOAD_STEPS to reach a point" % m.id)
    kernel = argv[argv.index("-kernel") + 1]
    files = [argv[0], kernel] + ([env["CDJ_BF531_UPD"]] if m.display_upd else []) + [lay.usb_image]
    point_dir = os.path.join(snapshot.root(lay.tmp, files, env.get("MAIN_ARGS", "")), point)
    if snapshot.saved(point_dir, ("main.vm",)):
        argv += ["-incoming", "file:%s/main.vm" % host.native(point_dir)]
        say("starting from the saved '%s' point %s" % (point, host.native(point_dir)))
        return None
    mon = "%s/cdj-deck-%s-mon.sock" % (lay.tmp, m.id)
    # A state that records "paused" comes back paused: the save stops the deck.
    argv += ["-monitor", _monsock(lay).spec(mon), "-global", "migration.store-global-state=off"]
    env.update({"MODEL_IDLE_S": m.idle_s, "MODEL_LOAD_STEPS": m.load_steps})
    say("no saved '%s' point yet: the deck boots and saves it on the way" % point)
    return host.python_argv() + [os.path.join(lay.run, "snapshot_deck.py"), "--reach", point, mon, point_dir]


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
    reach = plan_snapshot(lay, m, argv, env)
    say("%s: one window with the USB stick. Pro DJ Link, a second deck, MIDI controllers, mods and "
        "the virtual deck app are CDJ-2000NXS2 features and are skipped." % m.title)
    if dry:
        knobs = ["CDJ_ATA", "CDJ_BF531_UPD", "CDJ_PANEL_KEYSOCK", "CDJ_DSP_AUDIO", "CDJ_C6747_IDLE",
                 "C66X_IDLE_ISR_FAST", "C66X_JIT"]
        say("would run:  %s %s" % (" ".join("%s=%s" % (k, env[k]) for k in knobs if k in env),
                                   " ".join(shlex.quote(a) for a in argv)))
        return 0
    os.makedirs(lay.logs, exist_ok=True)
    say("the first boot takes a minute; close the window or press Ctrl-C to stop.")
    qemu = subprocess.Popen(argv, env=env)
    driver = subprocess.Popen(reach, env=env) if reach else None
    try:
        qemu.wait()
    except KeyboardInterrupt:
        qemu.terminate()
        qemu.wait()
    if driver:
        driver.terminate()
    say("stopped.")
    return 0
