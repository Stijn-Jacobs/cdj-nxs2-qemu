#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Sweep one running deck's tempo fader and MASTER TEMPO on its virtual clock.

A deck that only plays at 0 % runs a small part of the DSP program; MASTER
TEMPO and a fader off centre run its time-stretch and resampling code. This
driver makes one session cover all of it, so the run-time JIT compiles it
(./setup.sh's warm-up, through scripts/run/warm_jit.sh) or a recording
profiles it (scripts/build/build_dsp_module.sh).

The deck loads and starts playing by itself. From --start virtual seconds on,
PLAN repeats until --duration has passed; then the fader goes back to centre,
MASTER TEMPO off, and the driver exits. It also exits when the deck's clock
stops, i.e. the deck ended first.

usage: warm_jit.py <deck tag> [--start S] [--duration S] [--vclock FILE]
                   [--keys NAME] [--stale S] [--boot-timeout S] [--print-plan]
"""
import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdj_panelsock  # noqa: E402

MT_KEY = (0x15, 0x10)
FADER = "0x04:%d:0:lvl"          # the tempo fader byte; about 127 is centre
CENTRE = 127

# (virtual seconds into one pass, action): MASTER TEMPO on, the fader across
# both sides of centre, MASTER TEMPO off for a stretch so plain resampling is
# covered too, and off again at the end so every pass starts the same way.
PLAN = [(0, "mt"), (5, 100), (20, 150), (35, 90), (50, 170), (65, CENTRE),
        (75, "mt"), (80, 110), (95, 160), (110, "mt"), (115, 60), (130, 200),
        (145, CENTRE), (150, "mt")]
PERIOD = 160


def schedule(start, duration):
    """PLAN repeated from `start` for `duration` virtual seconds."""
    out, base = [], start
    while True:
        for at, act in PLAN:
            if base + at >= start + duration:
                return out
            out.append((base + at, act))
        base += PERIOD


def tmp_path(name):
    """/tmp/<name> as this Python sees it. A native Windows Python does not
    know MSYS2's /tmp, so ask cygpath where it is."""
    if os.name == "nt":
        try:
            tmp = subprocess.check_output(["cygpath", "-m", "/tmp"], text=True).strip()
            return tmp + "/" + name
        except (OSError, subprocess.CalledProcessError):
            pass
    return "/tmp/" + name


class VClock:
    """The deck's virtual clock: MAIN writes its virtual milliseconds to a file."""

    def __init__(self, path, stale, boot_timeout):
        self.path, self.stale, self.boot_timeout = path, stale, boot_timeout
        self.t0 = time.time()
        self.last, self.changed = None, self.t0

    def read(self):
        try:
            with open(self.path, "rb") as f:
                v = int(f.read(20)) / 1000.0
        except (OSError, ValueError):
            return None
        if v != self.last:
            self.last, self.changed = v, time.time()
        return v

    def wait_until(self, t, poll=0.2):
        """True once the clock reaches t; False if it never starts or stops."""
        while True:
            v = self.read()
            now = time.time()
            if v is not None and v >= t:
                return True
            if self.last is None and now - self.t0 > self.boot_timeout:
                print("the deck's clock never appeared (%s)" % self.path, flush=True)
                return False
            if self.last is not None and now - self.changed > self.stale:
                print("the deck's clock stopped at %.1f s: the deck has ended" % self.last, flush=True)
                return False
            time.sleep(poll)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("tag", help="the deck's tag, e.g. warm1")
    ap.add_argument("--start", type=float, default=60.0,
                    help="virtual seconds after boot to begin; the track is playing by then (60)")
    ap.add_argument("--duration", type=float, default=240.0, help="virtual seconds of sweeping (240)")
    ap.add_argument("--vclock", help="the deck's clock file (default /tmp/cdj-<tag>-vclock)")
    ap.add_argument("--keys", help="the deck's panel key socket (default /tmp/cdj-panel-keys-<tag>.sock)")
    ap.add_argument("--stale", type=float, default=60.0,
                    help="wall seconds without a clock tick that mean the deck has ended (60)")
    ap.add_argument("--boot-timeout", type=float, default=900.0,
                    help="wall seconds to wait for the clock to appear (900)")
    ap.add_argument("--print-plan", action="store_true", help="print the schedule and exit")
    a = ap.parse_args()

    plan = schedule(a.start, a.duration)
    if a.print_plan:
        for t, act in plan:
            print("%7.1f  %s" % (t, "MASTER TEMPO" if act == "mt" else "fader %d" % act))
        return 0

    keys = a.keys or "/tmp/cdj-panel-keys-%s.sock" % a.tag
    clock = VClock(a.vclock or tmp_path("cdj-%s-vclock" % a.tag), a.stale, a.boot_timeout)
    mt_on, fader = False, CENTRE

    def act(what):
        nonlocal mt_on, fader
        try:
            if what == "mt":
                cdj_panelsock.press(keys, MT_KEY[0], MT_KEY[1], 200)
                mt_on = not mt_on
            else:
                cdj_panelsock.send(keys, FADER % what)
                fader = what
        except OSError as e:
            print("cannot reach %s: %s" % (keys, e), flush=True)
            return
        print("v=%6.1f  MASTER TEMPO %s  fader %d" % (clock.last or 0, "on" if mt_on else "off", fader),
              flush=True)

    print("%s: %d steps from %.0f s to %.0f s virtual" % (a.tag, len(plan), a.start, a.start + a.duration),
          flush=True)
    done = 0
    for t, what in plan:
        if not clock.wait_until(t):
            break
        act(what)
        done += 1
    # Leave the deck as it started, whether the plan finished or the deck did.
    if fader != CENTRE:
        act(CENTRE)
    if mt_on:
        act("mt")
    print("%s: %d of %d steps done" % (a.tag, done, len(plan)), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
