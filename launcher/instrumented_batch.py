# SPDX-License-Identifier: GPL-2.0-or-later
"""A batch of boot_decks.sh runs with firmware taps, a RAMSNAP region and the
load poke, followed by the verdict, error, tap and snapshot reports.

Default taps (TAPS overrides):
  0x082FBA6E  manager dispatch reached the `cmd == 8` test
  0x082FBAB6  the cmd-8 arm executed -> save slot filled
  0x082FBAD8  the cmd-8 arm's reply-builder call
  0x082FBE56  alternative reply-builder call site
  0x082FC216  alternative reply-builder call site
  0x082FFEBC  reply builder reached the requester compare
  0x082FFEC0  compare passed -> snd_mbx
  0x0842B218  a command 8 emitted by the standby-SET builder
  0x0842A160  the single command-post function (positive control)

FWTRACE reports absence by printing nothing, so keep a tap known to fire as a
positive control. RAMSNAP is a single region (SNAP=<addr>:<len>); three 23 MB
snapshots per run, so clean /tmp afterwards.

  usage: ./scripts/run/instrumented_batch.sh <tag-prefix> [n]
"""

import os
import subprocess

from . import chain, host
from .chain import export_default, nonempty
from .layout import Layout

TAPS = "0x082FBA6E,0x082FBAB6,0x082FBAD8,0x082FBE56,0x082FC216,0x082FFEBC,0x082FFEC0,0x0842B218,0x0842A160"


def batch_env(env):
    chain.unset(env, "CDJ_MWATCH", "CDJ_FWDUMP", "CDJ_MPOKE", "CDJ_PPOKE_N")
    # DUMP, WATCH and MPOKE pass CDJ_FWDUMP, CDJ_MWATCH and CDJ_MPOKE through;
    # the unset above only drops stale exports.
    for knob, var in (("DUMP", "CDJ_FWDUMP"), ("WATCH", "CDJ_MWATCH"), ("MPOKE", "CDJ_MPOKE")):
        if env.get(knob):
            env[var] = env[knob]
    # PPOKEX appends a spec to the load poke (CDJ_PPOKE holds up to four).
    # NOPOKE=1 omits the load poke; W1=0 does not, it forces the branch the
    # other way.
    ppokex = env.get("PPOKEX", "")
    if nonempty(env, "NOPOKE", "0") == "1":
        env["CDJ_PPOKE"] = ppokex
    else:
        env["CDJ_PPOKE"] = "0x082f04d8:0x0994CC18:%s:0%s" % (nonempty(env, "W1", "1"),
                                                             "," + ppokex if ppokex else "")
    env["CDJ_FWLOG"] = nonempty(env, "LOGPCS", "0x0853410C,0x084933F4")
    env["CDJ_FWTRACE"] = nonempty(env, "TAPS", TAPS)
    env["MAIN_MON"] = "1"
    # SNAP=none takes no snapshots (each memsave holds the machine while it copies).
    env["RAMSNAP"] = nonempty(env, "SNAP", "0x09947000:0x01713000")
    if env["RAMSNAP"] == "none":
        env["RAMSNAP"] = ""
    # Defaults only, so a caller's key is the key that gets pressed.
    export_default(env, "KEYBYTE", "0x10")
    export_default(env, "KEYBITS", "0x01")
    # RETRIES: load retries in load_track.py (default 0).
    export_default(env, "FILMN", "3")
    export_default(env, "JOBS", "3")
    env["PRIVATE_MEDIA"] = "1"
    export_default(env, "RETRIES", "0")


def main(argv):
    if not argv:
        chain.err("usage: instrumented_batch.sh <prefix> [n]")
        return 1
    prefix = argv[0]
    n = argv[1] if len(argv) > 1 else "9"
    lay = Layout()
    env = dict(os.environ)
    batch_env(env)

    # A checkout may vet the tap list before a batch: $CDJ_ROOT/.cdj/preflight,
    # if present, gets the taps as its argument. Advisory only; PREFLIGHT=0
    # silences it.
    preflight = os.path.join(lay.root, ".cdj", "preflight")
    bash = host.find_bash()
    if nonempty(env, "PREFLIGHT", "1") == "1" and os.path.isfile(preflight) and bash:
        chain.say("--- preflight (prior art for these taps)")
        if subprocess.run([bash, host.posix(preflight), env["CDJ_FWTRACE"]], env=env).returncode:
            chain.say("  (preflight flagged the taps above -- read it before believing this batch)")

    # Warn before the batch when a word in WORDS lies outside RAMSNAP, since
    # its column would otherwise be dashes.
    if env.get("WORDS") and env["RAMSNAP"] and ":" in env["RAMSNAP"]:
        sb, ss = env["RAMSNAP"].split(":", 1)
        for w in env["WORDS"].split():
            try:
                inside = int(sb, 0) <= int(w, 0) < int(sb, 0) + int(ss, 0)
            except ValueError:
                inside = False
            if not inside:
                chain.say("  !! %s IS OUTSIDE RAMSNAP (%s + %s) -- widen SNAP or drop it; its column will "
                          "be dashes, NOT a measurement" % (w, sb, ss))

    with open(os.path.join(lay.tmp, prefix + ".txt"), "wb") as out:
        chain.run_script("boot_decks", [prefix, n], env, out)

    chain.say("--- verdicts (ppoke = %s)" % env["CDJ_PPOKE"])
    chain.bash_report("report_load.sh", [prefix, n], env)
    chain.bash_report("report_errors.sh", [prefix, n], env)
    chain.say("--- taps (fwtrace = %s)" % env["CDJ_FWTRACE"])
    chain.bash_report("report_taps.sh", [prefix, n], env)
    chain.say("--- ramsnap (region = %s)" % env["RAMSNAP"])
    return chain.bash_report("report_snapshots.sh", [prefix, n], env)
