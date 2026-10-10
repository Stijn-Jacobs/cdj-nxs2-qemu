# SPDX-License-Identifier: GPL-2.0-or-later
"""One headless deck, no sound, no Pro DJ Link, that loads a track, plays it and
stops by itself: the profile and training runs of scripts/build/build_dsp_module.sh.
The CDJ-2000NXS2 also has MASTER TEMPO and the tempo fader swept (scripts/run/warm_jit.py).

  usage: bash scripts/run/warm_jit.sh [tag=warm] [--dry-run]
  env:   PLAY_S=45 (virtual seconds of play after the load; 30 for a
         one-window model, which has no sweep), plus any rig.sh
         knob: MODULE=none, AUTOJIT=0|1, C66X_JIT_PROFILE, ...
         DSP_TRAIN_MODULE=<m.so>: run with that module (an instrumented build
         collecting its profile-guided counts) instead of none
"""

import os
import re
import shlex
import subprocess
import sys

from . import chain, host, model
from .boot_deck import _monsock
from .chain import nonempty
from .deck import command as deck_command
from .layout import Layout

# An idle deck executes about 29 M DSP cycles per virtual second, one that has
# loaded and plays 39 (the CDJ-2000NXS) to 70 M; a run below this never got
# past the browse list.
MIN_PLAYING_MCYCLES = 35

# One pass of the tempo and MASTER TEMPO sweep (scripts/run/warm_jit.py), seconds.
PASS_S = 30


def main(argv):
    tag, dry = "warm", False
    for a in argv:
        if a == "--dry-run":
            dry = True
        elif a in ("-h", "--help"):
            sys.stdout.write(__doc__[__doc__.index("  usage:"):])
            return 0
        else:
            tag = a
    lay = Layout()
    try:
        m = model.load()
    except model.ModelError as e:
        chain.err(str(e))
        return 1
    if m.has_dsp_module:
        return record_deck(lay, m, tag, dry)
    deck = tag + "1"
    play_s = int(nonempty(os.environ, "PLAY_S", "45"))
    # The sweep starts once the track is playing and is one pass of its plan,
    # which has to end before the deck does.
    sweep_s = PASS_S
    vclock = "%s/cdj-%s-vclock" % (lay.tmp, deck)
    driver = host.python_argv() + [os.path.join(lay.run, "warm_jit.py"), deck, "--vclock", vclock,
                                   "--start", "60", "--duration", str(sweep_s)]
    # AUTOLOAD=1: the deck loads the first track and plays it by itself.
    # FRAMES=1 with MOTION_MS = the play time: the run ends that long after the load.
    rig_env = {"AUTOLOAD": "1", "GUI_DISPLAY": "none", "NOSOUND": "1", "DJLINK": "0", "NDECKS": "1",
               "WARM": "0", "MOTION_MS": str(play_s * 1000)}
    if dry:
        chain.say("rm -f %s" % vclock)
        chain.say(" ".join(shlex.quote(a) for a in driver) + " &")
        chain.say("env -u C66X_JIT %s %s" % (" ".join("%s=%s" % kv for kv in rig_env.items()),
                                            " ".join(shlex.quote(a) for a in chain.script_argv("rig", [tag, 1]))))
        return 0
    # A clock file left by an earlier run would start the sweep at once.
    chain.remove(vclock)
    drv = subprocess.Popen(driver)
    env = dict(os.environ, **rig_env)
    _train_module(env)
    try:
        return chain.run_script("rig", [tag, 1], env)
    finally:
        drv.terminate()
        drv.wait()


def _train_module(env):
    """The run's own module is DSP_TRAIN_MODULE or none, never a leftover C66X_JIT."""
    env.pop("C66X_JIT", None)
    if env.get("DSP_TRAIN_MODULE"):
        env["C66X_JIT"] = host.native(env["DSP_TRAIN_MODULE"])


def _lines(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return []


def record_deck(lay, m, tag, dry):
    """The same run for a one-window model: power on, load the first track,
    play it for PLAY_S wall seconds and quit, so the profile is written
    (C66X_JIT_PROFILE). The core skips its idle loop as it does in a session.
    Returns 1 unless the deck played."""
    play_s = nonempty(os.environ, "PLAY_S", "30")
    log = os.path.join(lay.tmp, "bridge-main-%s1.log" % tag)
    mon = "%s/cdj-deck-%s-%s-mon.sock" % (lay.tmp, m.id, tag)
    env = dict(os.environ, MODULE="none", AUDIODEV="none", GUI_DISPLAY="none", CDJ_REPORT="1",
               MODEL_IDLE_S=m.idle_s, MODEL_LOAD_STEPS=m.load_steps)
    _train_module(env)
    argv, env = deck_command(lay, m, env)
    argv += ["-monitor", _monsock(lay).spec(mon)]
    driver = host.python_argv() + [os.path.join(lay.run, "play_deck.py"), mon, play_s]
    if dry:
        chain.say(" ".join(shlex.quote(a) for a in argv) + " > " + log + " &")
        chain.say(" ".join(shlex.quote(a) for a in driver))
        return 0
    os.makedirs(lay.logs, exist_ok=True)
    with open(log, "wb") as f:
        qemu = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT, env=env)
        try:
            subprocess.run(driver, env=env)
            qemu.wait(120)
        finally:
            if qemu.poll() is None:
                qemu.kill()
                qemu.wait()
    text = " ".join(_lines(log))
    ran = re.search(r"(c67[24]7): running, pc \S+, (\d+) cycles, .*?(\d+) cycles skipped idle", text)
    if not ran:
        chain.say("FAILED the deck wrote no DSP report (%s)" % log)
        return 1
    cycles, skipped = int(ran.group(2)), int(ran.group(3))
    hz = float(nonempty(env, "CDJ_%s_MHZ" % ran.group(1).upper(), "300")) * 1e6
    rate = (cycles - skipped) / (cycles / hz) / 1e6
    chain.say("deck ran %.0f s of DSP time, %.0f M executed cycles per second" % (cycles / hz, rate))
    if rate < MIN_PLAYING_MCYCLES:
        chain.say("FAILED the deck never played the track (see %s)" % log)
        return 1
    return 0
