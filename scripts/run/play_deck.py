#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Load a track on a one-board deck (launcher/deck.py), let it play, and quit.

    usage: play_deck.py <monitor> <play seconds>

Waits for MAIN's monitor, gives the firmware MODEL_IDLE_S to settle, runs
MODEL_LOAD_STEPS (the steps snapshot_deck.py describes), plays on for the
given seconds and sends 'quit', so QEMU's exit handlers write the DSP
recording's profile and print the core's report.
"""

import os
import sys
import time

import cdj_monsock
from snapshot_deck import run_steps


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    mon, play_s = argv[0], float(argv[1])
    if not cdj_monsock.wait_up(mon):
        print("play_deck: the deck's monitor never came up")
        return 1
    time.sleep(float(os.environ.get("MODEL_IDLE_S", "60")))
    run_steps(mon, os.environ.get("MODEL_LOAD_STEPS", ""), int(os.environ.get("CDJ_PANEL_KEYSOCK", "0")))
    time.sleep(play_s)
    cdj_monsock.command(mon, "quit")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
