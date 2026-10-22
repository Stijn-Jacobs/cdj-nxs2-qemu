#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Load a track on a one-board deck (launcher/deck.py), let it play, and quit.

    usage: play_deck.py <monitor> <play seconds> <screen file>

Waits for MAIN's monitor, waits for the deck to settle (MODEL_IDLE_S at least,
then a screen that stops changing), runs MODEL_LOAD_STEPS (the steps
snapshot_deck.py describes), plays on for the given seconds, running
MODEL_PROFILE_STEPS (the same kind of steps) at the start of that time, and sends 'quit',
so QEMU's exit handlers write the DSP recording's profile and print the
core's report. The screen file is left holding the last picture of the deck.
"""

import os
import sys
import time

import cdj_monsock
from snapshot_deck import SETTLE_TIMEOUT_S, run_steps, screendump, wait_settled


def main(argv):
    if len(argv) != 3:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    mon, play_s, screen = argv[0], float(argv[1]), argv[2]
    if not cdj_monsock.wait_up(mon):
        print("play_deck: the deck's monitor never came up")
        return 1
    idle_s = float(os.environ.get("MODEL_IDLE_S", "60"))
    if not wait_settled(mon, idle_s, screen):
        print("play_deck: the deck's screen was still changing %d s after its monitor came up (%s)"
              % (SETTLE_TIMEOUT_S, screen))
        return 1
    run_steps(mon, os.environ.get("MODEL_LOAD_STEPS", ""), int(os.environ.get("CDJ_PANEL_KEYSOCK", "0")))
    played = time.time()
    run_steps(mon, os.environ.get("MODEL_PROFILE_STEPS", ""), int(os.environ.get("CDJ_PANEL_KEYSOCK", "0")))
    time.sleep(max(0.0, play_s - (time.time() - played)))
    screendump(mon, screen)
    cdj_monsock.command(mon, "quit")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
