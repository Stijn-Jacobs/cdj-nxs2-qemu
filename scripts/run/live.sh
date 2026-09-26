#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The MIDI relay in the background and the real-DSP rig in the foreground, so
# both live exactly as long as this script (background processes do not survive
# a `wsl -- ...` call). Start midi\bridge.py on Windows once the deck window is
# up; Ctrl-C stops both.
#
#   usage: bash scripts/run/live.sh [prefix=show] [decks=2]
#   The rig lives as long as play_real_dsp.sh films: FRAMES (default 240) x 5 s = ~20 min.
#
# The work is done by launcher/live.py.
. "$(dirname "${BASH_SOURCE[0]}")/../cdj_python.sh"
cdj_launcher_script live "$@"
