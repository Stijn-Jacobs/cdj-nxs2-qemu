#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Tap census as a matrix: one row per run, one column per tap, with that run's
# own load verdict on the row. Every tap gets a column even when it is zero.
#
#   usage: ./scripts/run/report_taps.sh <tag-prefix> <n> [tap,tap,...]
set -uo pipefail
PREFIX="${1:?usage: report_taps.sh <prefix> <n> [taps]}"
N="${2:?}"
TAPS="${3:-${CDJ_FWTRACE:-}}"
[ -n "$TAPS" ] || { echo "no taps given and CDJ_FWTRACE unset" >&2; exit 1; }

LIST=$(echo "$TAPS" | tr ',' ' ')

printf '%-9s %-11s' run verdict
for t in $LIST; do printf ' %10s' "$(printf '%08x' "$t")"; done
printf '\n'

for i in $(seq 1 "$N"); do
    tag="$PREFIX$i"
    log="/tmp/bridge-main-$tag.log"
    v=$(grep -ao 'VERDICT \(LOADED\|not-loaded\)' "/tmp/run-$tag.txt" 2>/dev/null \
        | tail -1 | awk '{print $2}')
    printf '%-9s %-11s' "$tag" "${v:--}"
    if [ ! -f "$log" ]; then
        for t in $LIST; do printf ' %10s' "?"; done
        printf '   (log missing)\n'
        continue
    fi
    for t in $LIST; do
        h=$(printf '%08x' "$t")
        printf ' %10s' "$(grep -ac "FWTRACE $h" "$log" || true)"
    done
    printf '\n'
done
