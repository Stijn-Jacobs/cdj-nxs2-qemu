# SPDX-License-Identifier: GPL-2.0-or-later
"""Load and play on the real DSP with every DSP stand-in switched off, so the
decoder's buffer level (status 0x158), the play position and the audio can
only come from IC301's own program. MAIN/GUI-side pokes are kept: the PPOKE
loader (instrumented_batch.sh), the play-key hold 0x0B056E38, the colour
capability byte, the edge RPOKE 0x08443182 and the GUI link thinning.

C6X=0 is the control: the same arm on the stand-in peer, which does not play.

  usage: bash scripts/run/play_real_dsp.sh <tag> [n]
  env:   C6X=1 MHZ=250 LOCKSTEP=1 JOBS=3 FILMN=8 MOTION_MS=1500
"""

import os
import subprocess

from . import chain, host
from .chain import export_default, nonempty
from .layout import Layout


def batch_env(env):
    chain.unset(env, "CDJ_AUDIO_DRAIN", "CDJ_DSP_ENGINE", "CDJ_DSP_ENGINE_GATE", "CDJ_DSP_ENGINE_POS",
                "CDJ_CUEKEY", "CDJ_JOGDRIVE", "CDJ_BEATDRIVE", "CDJ_BEATGRID", "CDJ_PITCHDRIVE",
                "CDJ_PSTORE", "CDJ_DMA1_COPY", "CDJ_PCALL")
    c6x = nonempty(env, "C6X", "1") == "1"
    lockstep = nonempty(env, "LOCKSTEP", "1") == "1"
    if c6x:
        if lockstep:
            # MAIN under -icount and the DSP stepped against it, so MAIN waits
            # for the DSP rather than the DSP dropping time. DSPTHREAD=2: the
            # DSP on its own host thread, synchronised one quantum at a time.
            export_default(env, "ICOUNT", "shift=1,sleep=off")
            env["CDJ_C6X_THREAD"] = nonempty(env, "DSPTHREAD", "0")
            export_default(env, "CDJ_C6X_CATCHUP_MS", "100000000")
            env["CDJ_C6X_MHZ"] = nonempty(env, "MHZ", "250")
        else:
            env["CDJ_C6X_MHZ"] = nonempty(env, "MHZ", "30")
            export_default(env, "CDJ_C6X_THREAD", "1")
        env["CDJ_C6X"] = "1"
        # W1=0 (default for the real DSP): the load poke writes 0, so the
        # loader takes bsr 0x082F118C and the file layer keeps filling. W1=1
        # forces the file state machine and leaves the real deck in prebuffer.
        export_default(env, "W1", "0")
        if env.get("PCM"):
            env["CDJ_C6X_PCM"] = env["PCM"]
    else:
        chain.unset(env, "CDJ_C6X")
    env["CDJ_AREA6"] = "1"
    # Heartbeat thinning paces a free-running MAIN; under LOCKSTEP it starves
    # the GUI link, so it defaults off there.
    if c6x and lockstep:
        env["THIN"] = nonempty(env, "THIN", "0")
    env["CDJ_SPILINK_THIN3"] = nonempty(env, "THIN", "8")
    env["SPILINK_KEEP_FRAMES"] = nonempty(env, "KEEPF", "1024")
    env["CDJ_SPILINK_DEDUP"] = "1"
    holds0 = nonempty(env, "HOLDS", "0") == "0"
    ladder = nonempty(env, "LADDER", "1") == "1"
    # LADDER=1: MAIN-side transport forces -- the edge RPOKEs and the start-arm
    # hold anchored at the edge detector's load 0x0844323A, armed once the
    # waveform publisher 0x084E294E has run. No CDJ_PCALL: the load poke is the
    # loader, and two loaders give a title bar with no track.
    if ladder:
        export_default(env, "CDJ_RPOKE", "0x08443182:0:3,0x08443232:0:1")
        env["CDJ_PPOKE_ARM"] = nonempty(env, "ARM", "0x084E294E")
        env["PPOKEX"] = nonempty(env, "PPOKEX", "%s:0x0B058C10:0x00010000:0xFF00FFFF:0:1"
                                 % nonempty(env, "EDGEPC", "0x0844323A"))
    elif c6x and holds0:
        # With the real DSP the deck mode reaches 3 unforced.
        export_default(env, "CDJ_RPOKE", "none")
    else:
        export_default(env, "CDJ_RPOKE", "0x08443182:0:3")
    # HOLDS=0 (default for C6X=1): no play-key holds. 0x0B058C00=1 sends the
    # panel PLAY handler down its MAIN-only arm and the Player never sees the
    # key. The real deck starts playing at the end of the load, so PLAY would
    # pause it: the no-holds real-DSP arm presses nothing by default.
    pokex = "," + env["POKEX"] if env.get("POKEX") else ""
    if c6x and holds0 and not ladder:
        env["MPOKE"] = nonempty(env, "MPOKE", "0x0A35FAAA:3:1" + pokex)
        env["KEYBITS"] = nonempty(env, "KEYBITS", "0x00")
    else:
        env["MPOKE"] = nonempty(env, "MPOKE", "0x0A35FAAA:3:1,0x0B056E38:1:4,0x0B058C00:1:4" + pokex)
    export_default(env, "KEYBYTE", "0x10")
    export_default(env, "KEYBITS", "0x01")
    export_default(env, "KEYDUR", "150")
    export_default(env, "TAPS", "0x08443252,0x08443276,0x0842A160,0x084E294E")
    export_default(env, "WORDS", "0x0B054D44 0x0B5125E8 0x09944170 0x09947478")
    export_default(env, "SNAP", "0x09944000:0x01C40000")
    export_default(env, "FILMN", "8")
    export_default(env, "MOTION_MS", "1500")
    # What load_track.py sees as the settings modal here is first the real MY
    # SETTINGS modal, which dismisses itself about 5 s after it appears, and
    # then the browse list behind it, which never clears; waiting for that cost
    # ~30 s a run. The cap has to outlast the real modal, which swallows keys:
    # at 2 s the walk's first presses were lost and the deck never loaded.
    export_default(env, "MODAL_CLEAR", "7")
    export_default(env, "GUI_DISPLAY", "none")
    export_default(env, "PREFLIGHT", "0")
    export_default(env, "JOBS", "3")


def main(argv):
    if not argv:
        chain.err("usage: play_real_dsp.sh <tag> [n]")
        return 1
    prefix = argv[0]
    n = int(argv[1]) if len(argv) > 1 else 4
    lay = Layout()
    env = dict(os.environ)
    batch_env(env)
    chain.say("=== %s  C6X=%s MHZ=%s LOCKSTEP=%s ICOUNT=%s  qemu before: %d  load: %s" % (
        prefix, nonempty(env, "C6X", "1"), env.get("CDJ_C6X_MHZ", ""), nonempty(env, "LOCKSTEP", "1"),
        env.get("ICOUNT", ""), chain.pgrep_count("qemu-system"), chain.loadavg()))
    chain.run_script("instrumented_batch", [prefix, n], env)
    chain.say("--- DSP (c6x exit lines)")
    chain.bash_report("report_dsp.sh", [prefix, n], env)
    chain.say("--- minimap playhead")
    # The scorer needs Pillow, which setup installs into .venv/, not into the
    # python3 running this; the packaged program's own interpreter has it.
    venv = os.path.join(lay.emu, ".venv", "bin", "python")
    score_py = [venv] if not lay.packaged and os.access(venv, os.X_OK) else host.python_argv()
    for i in range(1, n + 1):
        r = subprocess.run(score_py + [os.path.join(lay.run, "score_playhead.py"), "%s%d" % (prefix, i)],
                           env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        for line in r.stdout.splitlines()[-2:]:
            chain.say(line)
    chain.say("=== host load after: %s" % chain.loadavg())
    return 0
