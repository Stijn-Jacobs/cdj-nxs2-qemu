#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The MIDI relay in the background and the real-DSP rig in the foreground, so
# both live exactly as long as this script (background processes do not survive
# a `wsl -- ...` call). Start midi\bridge.py on Windows once the deck window is
# up; Ctrl-C stops both.
#
#   usage: bash scripts/run/live.sh [prefix=show] [decks=2]
#   The rig lives as long as play_real_dsp.sh films: FRAMES (default 240) x 5 s = ~20 min.
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="${1:-show}"
N="${2:-2}"
RELAY_PORT="${RELAY_PORT:-7202}"
TAGS=$(seq -s, -f "${TAG}%g" 1 "$N")

python3 "$HERE/midi_relay.py" --tags "$TAGS" --port "$RELAY_PORT" &
RELAY=$!
trap 'kill $RELAY 2>/dev/null' EXIT
NDECKS="$N" bash "$HERE/rig.sh" "$TAG" "${FRAMES:-240}"
