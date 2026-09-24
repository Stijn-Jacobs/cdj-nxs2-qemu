#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Stop a live rig: its wrapper scripts (live, live_linked, rig, play_real_dsp,
# boot_decks, boot_deck, midi_relay) and every qemu, without pkill -f matching this shell.
for pat in run/live.sh run/live_linked.sh run/rig.sh run/play_real_dsp.sh run/boot_decks.sh run/boot_deck.sh run/midi_relay.py; do
  for p in $(pgrep -f "$pat"); do [ "$p" != "$$" ] && [ "$p" != "$PPID" ] && kill "$p" 2>/dev/null; done
done
for p in $(pgrep -x qemu-system-sh4) $(pgrep -f qemu-system-sh4eb); do kill "$p" 2>/dev/null; done
sleep 3; echo "qemu left: $(pgrep -f qemu-system | wc -l)"; pgrep -fa "run/live|run/rig|midi_relay" | grep -v pgrep
