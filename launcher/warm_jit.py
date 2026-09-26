# SPDX-License-Identifier: GPL-2.0-or-later
"""One headless deck, no sound, no Pro DJ Link, that loads a track, plays it and
has MASTER TEMPO and the tempo fader swept (scripts/run/warm_jit.py), then
stops by itself. Run with the auto-JIT on, it fills the DSP code cache
(~/c14gen) so real sessions start fast; scripts/build/build_dsp_module.sh runs
it to record the DSP instead. Ends with the auto-JIT's own count of what it
built.

  usage: bash scripts/run/warm_jit.sh [tag=warm] [--dry-run]
  env:   PLAY_S=270 (virtual seconds of play after the load), plus any rig.sh
         knob: MODULE=none, AUTOJIT=0|1, CDJ_C6X_RECORD, C66X_JIT_PROFILE, ...
"""

import glob
import os
import shlex
import subprocess
import sys

from . import chain, host
from .chain import nonempty
from .layout import Layout


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
    deck = tag + "1"
    play_s = int(nonempty(os.environ, "PLAY_S", "270"))
    # The sweep starts once the track is playing and ends before the deck does.
    sweep_s = play_s - 30 if play_s > 30 else play_s
    cache = lay.jit_cache
    vclock = "%s/cdj-%s-vclock" % (lay.tmp, deck)
    main_log = os.path.join(lay.tmp, "bridge-main-%s.log" % deck)
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

    def modules():
        return len(glob.glob(os.path.join(cache, "batch*", "m.so")))

    before = modules()
    # A clock file left by an earlier run would start the sweep at once.
    chain.remove(vclock)
    drv = subprocess.Popen(driver)
    env = dict(os.environ, **rig_env)
    env.pop("C66X_JIT", None)
    try:
        rc = chain.run_script("rig", [tag, 1], env)
    finally:
        drv.terminate()
        drv.wait()

    chain.say("--- DSP JIT")
    lines = _lines(main_log)
    for pat in ("]: jit:", "c66x jit auto:"):
        hits = [ln for ln in lines if pat in ln and (pat != "]: jit:" or "c6x[" in ln)]
        if hits:
            chain.say(hits[-1])
    after = modules()
    chain.say("warm: %d new auto-JIT modules, %d in the cache (%s)" % (after - before, after, host.posix(cache)))
    if not lines:
        chain.say("FAILED the deck wrote no log (%s): it did not start" % main_log)
        return 1
    if not any("c66x jit auto:" in ln or ("c6x[" in ln and "]: jit:" in ln) for ln in lines):
        chain.say("note: the deck did not report its DSP JIT; it may not have shut down cleanly")
    return rc


def _lines(path):
    try:
        with open(path, "rb") as f:
            return f.read().decode("utf-8", "replace").splitlines()
    except OSError:
        return []
