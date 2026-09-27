#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Boot N decks in parallel, each driven to a loaded track by load_track.py, and
# print each run's summary lines.
#
#   usage: ./scripts/run/boot_decks.sh <tag-prefix> [n]
#   env:   JOBS FILMN MOTION_MS WALK KEYBYTE KEYBITS plus any CDJ_* knob
#
# The work is done by launcher/boot_decks.py.
. "$(dirname "${BASH_SOURCE[0]}")/../cdj_python.sh"
cdj_launcher_script boot_decks "$@"
