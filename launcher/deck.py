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
import time

from . import chain, dsp_module, host, model, mods, sites, snapshot
from .boot_deck import _monsock, window_display
from .chain import nonempty, say, warn_banner
from .layout import Layout
from .rig import DEFAULT_GROUP, default_audiodev

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


def dsp_env(lay, m, env, found=None):
    """The DSP knobs a model's profile sets: the busy-wait loop the core skips,
    the interrupt fast path, and the module built by build_dsp_module.sh (when its
    stamp fits) unless MODULE=none or C66X_JIT names one by hand. `found` is the
    sites of the MAIN image that runs (sites.find), the installed one's when None."""
    knobs = {}
    if (found or sites.find(lay, m)).pinned:
        knobs[m.dsp_idle_knob] = nonempty(env, m.dsp_idle_knob, m.dsp_idle)
        if m.dsp_isr_fast:
            knobs["C66X_IDLE_ISR_FAST"] = nonempty(env, "C66X_IDLE_ISR_FAST", m.dsp_isr_fast)
    if m.dsp_mhz:
        knobs["CDJ_C6747_MHZ"] = nonempty(env, "CDJ_C6747_MHZ", m.dsp_mhz)
    if env.get("C66X_JIT") or env.get("MODULE") == "none":
        return knobs
    module = dsp_module.check(lay, m)
    if module.path:
        knobs["C66X_JIT"] = host.native(module.path)
    if module.notice:
        warn_banner(*module.notice)
    return knobs


def link_group(env):
    """The Pro DJ Link segment (ip:port) when DJLINK=1, else an empty string."""
    if env.get("DJLINK") != "1":
        return ""
    ip, _, port = nonempty(env, "GROUP", DEFAULT_GROUP).rpartition(":")
    return "%s:%d" % (ip, host.pick_udp_port(int(port)))


def patched_kernel(lay, m, env, kernel):
    """MAIN with the mods cdj.conf switched on patched in, as a copy under tmp;
    the image itself when none is on."""
    names = [knob[len("CDJ_MAIN_"):].lower() for row in mods.load(lay, m.id)
             for knob in mods.env_names(row.env)
             if knob.startswith("CDJ_MAIN_") and env.get(knob) == "1"]
    if not names:
        return kernel
    patched = os.path.join(lay.tmp, "cdj-%s-main.bin" % m.id)
    subprocess.run(host.python_argv() + [os.path.join(lay.emu, "mods", "patch_main.py"),
                                         "--model", m.id, kernel, patched] + names, check=True)
    return patched


def command(lay, m, env, main=None):
    """(argv, env) of the player's QEMU. The stick is a snapshot, so the
    firmware's writes to it never reach the image. `main` runs another MAIN
    image in place of the installed one, with the sites found in it."""
    folder = model.extract_dir(lay, m)
    env = dict(env)
    env["CDJ_ATA"] = "1"
    found = sites.find(lay, m, main)
    sr_seed_slot = found.sr_seed_slot()
    if sr_seed_slot is not None:
        env["CDJ_SR_SEED_SLOT"] = sr_seed_slot
    if m.has_dsp_module:
        env.update(dsp_env(lay, m, env, found))
    if m.display_upd:
        env["CDJ_BF531_UPD"] = host.native(os.path.join(folder, model.DISPLAY_UPD_IMAGE))
    if m.display_flash:
        env["CDJ_M16C_GUI"] = host.native(os.path.join(folder, m.display_flash))
    env["CDJ_PANEL_KEYSOCK"] = nonempty(env, "CDJ_PANEL_KEYSOCK", str(host.pick_udp_port(PANEL_KEY_PORT)))
    argv = [main_qemu(lay, env), "-M", m.main_machine, "-name", m.title,
            "-kernel", host.native(patched_kernel(lay, m, env, main or os.path.join(folder, "main_unpacked.bin"))),
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
    group = link_group(env)
    if group:
        # QEMU's socket backend carries one frame per datagram; every deck on
        # the group sees the others. The firmware reads its MAC from flash,
        # which is empty here, so each deck on one segment needs its own.
        argv += ["-netdev", "socket,id=djlink,mcast=" + group]
        env["GROUP"] = group
        env["CDJ_ETHER_MAC"] = nonempty(env, "CDJ_ETHER_MAC", "02:00:00:00:00:01")
    argv += shlex.split(env.get("MAIN_ARGS", ""))
    return argv, env


def point_dir(tmp, argv, env, media, point, left_out=()):
    """Where a deck started with (argv, env) from command() keeps a saved
    point: keyed by its QEMU, MAIN and display images, stick, DSP module,
    MAIN_ARGS and sound device, and the knobs of command() a caller left out."""
    kernel = argv[argv.index("-kernel") + 1]
    files = [argv[0], kernel] + [env[k] for k in ("CDJ_BF531_UPD", "CDJ_M16C_GUI") if k in env] + [media]
    # The sound device and the DSP module shape the machine as well.
    if os.path.isfile(env.get("C66X_JIT", "")):
        files.append(env["C66X_JIT"])
    audio = argv[argv.index("-audio") + 1] if "-audio" in argv else ""
    options = env.get("MAIN_ARGS", "") + " " + audio + "".join(" -" + k for k in sorted(left_out))
    return os.path.join(snapshot.root(tmp, files, options), point)


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
    where = point_dir(lay.tmp, argv, env, lay.usb_image, point)
    if snapshot.saved(where, ("main.vm",)):
        argv += snapshot.restore_args(where)
        say("starting from the saved '%s' point %s" % (point, host.native(where)))
        return None
    mon = "%s/cdj-deck-%s-mon.sock" % (lay.tmp, m.id)
    argv += ["-monitor", _monsock(lay).spec(mon)] + snapshot.SAVE_ARGS
    env.update({"MODEL_IDLE_S": m.idle_s, "MODEL_LOAD_STEPS": m.load_steps})
    say("no saved '%s' point yet: the deck boots and saves it on the way" % point)
    return host.python_argv() + [os.path.join(lay.run, "snapshot_deck.py"), "--reach", point, mon, where]


def start(m, env, dry, app_cmd=()):
    """Run the player until its window closes. With app_cmd, the virtual deck
    app is the window (the player's screen is on VNC) and closing it stops the
    player."""
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
    skipped = "A second deck, MIDI controllers and mods" if app_cmd else (
        "A second deck, MIDI controllers, mods and the virtual deck app")
    say("%s: one window with the USB stick. %s are CDJ-2000NXS2 features and are skipped." % (m.title, skipped))
    if dry:
        knobs = ["CDJ_ATA", "CDJ_BF531_UPD", "CDJ_M16C_GUI", "CDJ_PANEL_KEYSOCK", "CDJ_DSP_AUDIO", "CDJ_C6747_IDLE",
                 "CDJ_C6727_IDLE", "C66X_IDLE_ISR_FAST", "C66X_JIT"]
        say("would run:  %s %s" % (" ".join("%s=%s" % (k, env[k]) for k in knobs if k in env),
                                   " ".join(shlex.quote(a) for a in argv)))
        if app_cmd:
            say("with:       GUI_DISPLAY=%s, then %s" % (env["GUI_DISPLAY"], " ".join(shlex.quote(a) for a in app_cmd)))
        return 0
    os.makedirs(lay.logs, exist_ok=True)
    say("the first boot takes a minute; close the window or press Ctrl-C to stop.")
    dhcp = None
    if env.get("DJLINK") == "1":
        # The deck announces itself only once it holds a lease.
        with open(os.path.join(lay.tmp, "cdj-%s-dhcpd.log" % m.id), "wb") as log:
            dhcp = subprocess.Popen(host.python_argv() + [os.path.join(lay.scripts, "net", "dhcp_server.py"),
                                                          env["GROUP"]], stdout=log, stderr=subprocess.STDOUT)
    qemu = subprocess.Popen(argv, env=env)
    driver = subprocess.Popen(reach, env=env) if reach else None
    window = subprocess.Popen(app_cmd, env=env) if app_cmd else None
    try:
        while window and window.poll() is None and qemu.poll() is None:
            time.sleep(0.5)
        if window and qemu.poll() is None:
            qemu.terminate()
        qemu.wait()
    except KeyboardInterrupt:
        qemu.terminate()
        qemu.wait()
    for helper in (driver, dhcp, window):
        if helper:
            helper.terminate()
    say("stopped.")
    return 0
