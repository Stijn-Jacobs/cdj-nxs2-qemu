#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One line per run: the load verdict and the key-press film, side by side.
#   usage: ./scripts/run/report_load.sh <tag-prefix> <n>
set -uo pipefail
PREFIX="${1:?usage: report_load.sh <prefix> <n>}"
N="${2:?}"
for i in $(seq 1 "$N"); do
    f="/tmp/run-$PREFIX$i.txt"
    [ -f "$f" ] || continue
    # Anchor to the verdict words; the driver also prints a NOTE line
    # containing VERDICT.
    v=$(grep -ao 'VERDICT \(LOADED\|not-loaded\)' "$f" | tail -1)
    r=$(grep -ao 'POSITION \(yes\|no\) (title-right bright px = [0-9-]*, REMAIN px = [0-9-]*' "$f" \
        | sed 's/ (title-right bright px = /\/t/; s/, REMAIN px = /\/r/' | tail -1)
    [ -n "$r" ] || r=$(grep -ao 'REMAIN px = [0-9-]*' "$f" | tail -1)
    p=$(grep -a '^byte' "$f" | tail -1)
    # Matches both the '  bit 0x10' and the KEYSEQ '  key 0x10:0x01' lines.
    b=$(grep -a '^  bit\|^  key' "$f" | tail -1)
    printf '%-8s %-20s %-16s %-22s %s\n' \
        "$PREFIX$i" "${v:--}" "${r:--}" "${p:--}" "${b:--}"
done
