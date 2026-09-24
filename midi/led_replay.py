#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Replay a recorded deck-state log through leds.py, with no controller.

The log holds one relay state line per row, prefixed with its time in seconds
and without the tag ("12.5 frm beat=3 ..." or "12.5 pnl <hex>"). Prints what
each LED role would show, per second of the log, so the LED rules can be
checked offline.

    python midi/led_replay.py <log>
"""
import os
import sys
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import leds  # noqa: E402

ROLES = {"beat": ["b1", "b2", "b3", "b4"], "play": "play", "cue": "cue"}
CONTROLS = {n: {"type": "note", "channel": 0, "number": i}
            for i, n in enumerate(["b1", "b2", "b3", "b4", "play", "cue"])}

surface = leds.LedSurface(CONTROLS, None, dry_run=True)
deck = leds.DeckLeds("deck", "tag", ROLES, surface, [])
clock = [0.0]

with mock.patch.object(leds.time, "time", lambda: clock[0]):
    events = []
    for line in open(sys.argv[1], encoding="ascii", errors="replace"):
        t, _, rest = line.strip().partition(" ")
        if not rest.startswith(("frm", "pnl")):
            continue
        events.append((float(t), rest))
    last_row = None
    i = 0
    t = 0.0
    end = events[-1][0] if events else 0
    while t <= end:
        while i < len(events) and events[i][0] <= t:
            clock[0] = events[i][0]
            kind, value = leds.parse_state(events[i][1])
            if kind:
                deck.update(kind, value)
            i += 1
        clock[0] = t
        deck.render(t)
        lit = {n for (_, num), v in surface.lit.items() if v
               for n, c in CONTROLS.items() if c["number"] == num}
        beat = "".join("#" if f"b{k}" in lit else "." for k in range(1, 5))
        row = (f"play={'on' if 'play' in lit else '--'} "
               f"cue={'on' if 'cue' in lit else '--'} fx={beat}")
        if row != last_row:
            print(f"{t:7.1f}  {row}")
            last_row = row
        t += 0.25
