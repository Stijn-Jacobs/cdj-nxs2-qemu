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
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../cdj_paths.sh"
PREFIX="${1:?usage: instrumented_batch.sh <prefix> [n]}"
N="${2:-9}"

unset CDJ_MWATCH CDJ_FWDUMP CDJ_MPOKE CDJ_PPOKE_N
# DUMP=<pc>:<reg>:<off>:<len> passes CDJ_FWDUMP through; the unset above only
# drops stale exports.
[ -n "${DUMP:-}" ] && export CDJ_FWDUMP="$DUMP"
# WATCH=<addr>[,<addr>...] passes CDJ_MWATCH through.
[ -n "${WATCH:-}" ] && export CDJ_MWATCH="$WATCH"
# MPOKE=<addr>:<value>:<size>[,...] passes CDJ_MPOKE through.
[ -n "${MPOKE:-}" ] && export CDJ_MPOKE="$MPOKE"
# PPOKEX appends a spec to the load poke (CDJ_PPOKE holds up to four).
# NOPOKE=1 omits the load poke; W1=0 does not, it forces the branch the other
# way.
if [ "${NOPOKE:-0}" = "1" ]; then
    export CDJ_PPOKE="${PPOKEX:-}"
else
    export CDJ_PPOKE="0x082f04d8:0x0994CC18:${W1:-1}:0${PPOKEX:+,$PPOKEX}"
fi
export CDJ_FWLOG="${LOGPCS:-0x0853410C,0x084933F4}"
export CDJ_FWTRACE="${TAPS:-0x082FBA6E,0x082FBAB6,0x082FBAD8,0x082FBE56,0x082FC216,0x082FFEBC,0x082FFEC0,0x0842B218,0x0842A160}"
export MAIN_MON=1 RAMSNAP="${SNAP:-0x09947000:0x01713000}"
# Defaults only, so a caller's key is the key that gets pressed.
export KEYBYTE="${KEYBYTE:-0x10}" KEYBITS="${KEYBITS:-0x01}"
# RETRIES: load retries in load_track.py (default 0).
export FILMN="${FILMN:-3}" JOBS="${JOBS:-3}" PRIVATE_MEDIA=1 RETRIES="${RETRIES:-0}"

# ---- preflight --------------------------------------------------------------
# Prior art for the taps from the knowledge graph, if present. Advisory only;
# PREFLIGHT=0 silences it.
if [ "${PREFLIGHT:-1}" = "1" ] && [ -f "$CDJ_ROOT/graph/gq.py" ]; then
    echo "--- preflight (prior art for these taps)"
    python3 "$CDJ_ROOT/graph/gq.py" preflight "$CDJ_FWTRACE" 2>/dev/null         || echo "  (preflight found dead nodes above -- read them before believing this batch)"
fi

# ---- snapshot coverage -------------------------------------------------------
# Warn before the batch when a word in WORDS lies outside RAMSNAP, since its
# column would otherwise be dashes.
if [ -n "${WORDS:-}" ] && [ -n "${RAMSNAP:-}" ]; then
    _sb="${RAMSNAP%%:*}"; _ss="${RAMSNAP#*:}"
    if [ "$_ss" != "$RAMSNAP" ]; then
        for _w in $WORDS; do
            python3 -c "import sys;b=int('$_sb',0);s=int('$_ss',0);a=int('$_w',0);sys.exit(0 if b<=a<b+s else 1)" 2>/dev/null                 || echo "  !! $_w IS OUTSIDE RAMSNAP ($_sb + $_ss) -- widen SNAP or drop it; its column will be dashes, NOT a measurement"
        done
    fi
fi

bash "$HERE/boot_decks.sh" "$PREFIX" "$N" > "/tmp/$PREFIX.txt" 2>&1

echo "--- verdicts (ppoke = $CDJ_PPOKE)"
bash "$HERE/report_load.sh" "$PREFIX" "$N"
bash "$HERE/report_errors.sh" "$PREFIX" "$N"
echo "--- taps (fwtrace = $CDJ_FWTRACE)"
bash "$HERE/report_taps.sh" "$PREFIX" "$N"
echo "--- ramsnap (region = $RAMSNAP)"
bash "$HERE/report_snapshots.sh" "$PREFIX" "$N"
