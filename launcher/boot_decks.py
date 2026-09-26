# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot N decks in parallel, each driven to a loaded track by load_track.py, and
print each run's summary lines.

  usage: ./scripts/run/boot_decks.sh <tag-prefix> [n]
  env:   JOBS FILMN MOTION_MS WALK KEYBYTE KEYBITS plus any CDJ_* knob
         SERVICE=1 boots into SERVICE MODE instead of loading a track
         (forces AUTOLOAD=0; see scripts/run/boot_deck.sh)
"""

import os
import re
import shutil
import time

from . import chain
from .chain import export_default, nonempty
from .layout import Layout


def batch_env(env, lay, prefix=""):
    """The batch's knobs. Returns (deck duration in s, private media, media dir)."""
    env["CDJ_ATA"] = "1"
    env["CDJ_IIC_CH"] = "both"
    env["CDJ_IIC_ADDR"] = "0x30,0x2c,0x10"
    # The DSP reply knobs are defaults only, so a caller's own values win.
    export_default(env, "CDJ_DSP_REPLY", "1")
    export_default(env, "CDJ_DSP_REPLY_ID", "0")
    export_default(env, "CDJ_DSP_REPLY_WORDS", "4")
    export_default(env, "CDJ_DSP_TAG", "1")
    # SERVICE MODE is entered before the firmware ever gets to the browse list,
    # so a load_track.py driver would just press keys the service screen
    # doesn't use.
    if nonempty(env, "SERVICE", "0") == "1" and nonempty(env, "AUTOLOAD", "1") == "1":
        chain.say("[%s] SERVICE=1: no track load in service mode (AUTOLOAD forced off)" % prefix)
        env["AUTOLOAD"] = "0"
    # AUTOLOAD=1 (the measurement default): load_track.py loads the first track
    # and presses PLAY, and a deck lives as long as its film. AUTOLOAD=0 boots
    # the decks and leaves them to the user for FILMN x MOTION_MS.
    if nonempty(env, "AUTOLOAD", "1") == "1":
        env["DRIVER"] = os.path.join(lay.run, "load_track.py")
        dur = 2
    else:
        chain.unset(env, "DRIVER")
        # SERVICE=1 needs boot_deck.py's own CDJ_PANEL_PRESS default, so only
        # wipe it here for the plain AUTOLOAD=0 case.
        if nonempty(env, "SERVICE", "0") != "1":
            env["CDJ_PANEL_PRESS"] = ""
        dur = int(nonempty(env, "FILMN", "1")) * int(nonempty(env, "MOTION_MS", "1800")) // 1000
    export_default(env, "GUI_DISPLAY", "gtk")
    export_default(env, "FILMN", "5")
    export_default(env, "MOTION_MS", "1800")
    # PRIVATE_MEDIA=1: each concurrent run gets its own copy of the USB medium,
    # since separate vvfat models writing one directory corrupt each other. An
    # image run never reads the folder, so it never copies (83 MB, ~13 s).
    img = nonempty(env, "MEDIA_MODE", "rw") == "img"
    env["PRIVATE_MEDIA"] = "0" if img else nonempty(env, "PRIVATE_MEDIA", "1")
    media = nonempty(env, "MEDIADIR", os.path.join(lay.extract, "usbmedia3"))
    return dur, env["PRIVATE_MEDIA"] == "1", media


def deck_env(env, lay, tag, private, media):
    run_media = media
    if private:
        run_media = os.path.join(lay.tmp, "media-" + tag)
        shutil.rmtree(run_media, ignore_errors=True)
        shutil.copytree(media, run_media)
    return dict(env, MEDIADIR=run_media, SHOTDIR=os.path.join(lay.tmp, tag))


def main(argv):
    if not argv:
        chain.err("usage: boot_decks.sh <tag-prefix> [n]")
        return 1
    prefix = argv[0]
    n = int(argv[1]) if len(argv) > 1 else 3
    lay = Layout()
    env = dict(os.environ)
    jobs = int(nonempty(env, "JOBS", "3"))
    dur, private, media = batch_env(env, lay, prefix)

    def launch(tag, seconds):
        os.makedirs(os.path.join(lay.tmp, tag), exist_ok=True)
        with open(os.path.join(lay.tmp, "run-%s.txt" % tag), "wb") as out:
            return chain.start_script("boot_deck", [tag, seconds], deck_env(env, lay, tag, private, media), out)

    # A warm-up wave that is thrown away: the first $JOBS runs of a batch tend
    # to fail with the panel key socket refusing connections. WARMUP=0 skips it.
    if nonempty(env, "WARMUP", "1") == "1":
        wave = [launch("%sw%d" % (prefix, w), 2) for w in range(1, jobs + 1)]
        for p in wave:
            p.wait()
        for d in os.listdir(lay.tmp):
            if d.startswith(prefix + "w") or d.startswith("media-%sw" % prefix):
                p = os.path.join(lay.tmp, d)
                shutil.rmtree(p, ignore_errors=True) if os.path.isdir(p) else chain.remove(p)

    # Read the big inputs once so the first runs do not start while the host is
    # still faulting them in.
    for f in (os.path.join(lay.extract, "main_unpacked.bin"), os.path.join(lay.extract, "gui_unpacked.bin")):
        if os.path.isfile(f):
            with open(f, "rb") as fh:
                while fh.read(1 << 22):
                    pass

    running = []
    for i in range(1, n + 1):
        running.append(launch("%s%d" % (prefix, i), dur))
        # LAUNCH_STAGGER (s): spread the launches so the QEMUs do not all race
        # to bind their sockets at once.
        chain.sleep(float(nonempty(env, "LAUNCH_STAGGER", "4")))
        while sum(p.poll() is None for p in running) >= jobs:
            time.sleep(0.3)
    for p in running:
        p.wait()

    for i in range(1, n + 1):
        tag = "%s%d" % (prefix, i)
        chain.say("=== " + tag)
        for line in _lines(os.path.join(lay.tmp, "run-%s.txt" % tag)):
            if re.match(r"^(byte|  bit|\[)", line):
                chain.say(line)
        iic = [m.group(0) for line in _lines(os.path.join(lay.tmp, "bridge-main-%s.log" % tag))
               for m in [re.search(r"iic0: .*rx=[0-9]*", line)] if m]
        if iic:
            chain.say(iic[-1])
    return 0


def _lines(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read().splitlines()
    except OSError:
        return []
