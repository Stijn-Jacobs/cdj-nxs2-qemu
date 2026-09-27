#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# A batch of boot_decks.sh runs with firmware taps, a RAMSNAP region and the
# load poke, followed by the verdict, error, tap and snapshot reports.
#
# Default taps (TAPS overrides):
#   0x082FBA6E  manager dispatch reached the `cmd == 8` test
#   0x082FBAB6  the cmd-8 arm executed -> save slot filled
#   0x082FBAD8  the cmd-8 arm's reply-builder call
#   0x082FBE56  alternative reply-builder call site
#   0x082FC216  alternative reply-builder call site
#   0x082FFEBC  reply builder reached the requester compare
#   0x082FFEC0  compare passed -> snd_mbx
#   0x0842B218  a command 8 emitted by the standby-SET builder
#   0x0842A160  the single command-post function (positive control)
#
# FWTRACE reports absence by printing nothing, so keep a tap known to fire as a
# positive control. RAMSNAP is a single region (SNAP=<addr>:<len>); three 23 MB
# snapshots per run, so clean /tmp afterwards.
#
#   usage: ./scripts/run/instrumented_batch.sh <tag-prefix> [n]
#
# The work is done by launcher/instrumented_batch.py.
. "$(dirname "${BASH_SOURCE[0]}")/../cdj_python.sh"
cdj_launcher_script instrumented_batch "$@"
