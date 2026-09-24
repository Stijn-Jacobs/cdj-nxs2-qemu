#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Print the six RAMSNAP words per run, per snapshot, next to the load verdict.
#
#   0x0B059C90  PlayerTASK's task id -- the value the reply builder compares
#               the requester against at 0x082FFEBC
#   0x0A35DE24  the sender-filter id, written once at init from a name lookup.
#               A NEGATIVE value here is an RTOS error code, and would mean no
#               0x411/0x412/0x413 message ever dispatches at all.
#   0x099471E0  0x0AC9F0DC  0x0B058BFC  0x0B058C28   state words
#
#   usage: ./scripts/run/report_snapshots.sh <tag-prefix> <n>
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${1:?usage: report_snapshots.sh <prefix> <n>}"
N="${2:?}"
# The base must follow the region that was actually dumped, or the words are
# read at the wrong origin.
BASE="${SNAPBASE:-${RAMSNAP%%:*}}"
BASE="${BASE:-0x09947000}"
WORDS="${WORDS:-0x0B059C90 0x099471E0 0x0A35DE24 0x0AC9F0DC 0x0B058BFC 0x0B058C28}"

# A WORDS entry outside the dumped region would print as dashes, which look like
# an unreadable word; warn once before the table. Checks the declared RAMSNAP.
SPAN="${RAMSNAP#*:}"
if [ -n "${SPAN:-}" ] && [ "$SPAN" != "${RAMSNAP:-}" ]; then
    for w in $WORDS; do
        if ! python3 -c "import sys; b=int('$BASE',0); s=int('$SPAN',0); a=int('$w',0); sys.exit(0 if b <= a < b+s else 1)" 2>/dev/null; then
            printf '  !! %s IS OUTSIDE THE SNAPSHOT (%s + %s) -- its column is NOT a measurement
'                 "$w" "$BASE" "$SPAN" >&2
        fi
    done
fi

printf '%-9s %-11s %-7s' run verdict snap
for w in $WORDS; do printf ' %9s' "$w"; done
printf '\n'

for i in $(seq 1 "$N"); do
    tag="$PREFIX$i"
    v=$(grep -ao 'VERDICT \(LOADED\|not-loaded\)' "/tmp/run-$tag.txt" 2>/dev/null \
        | tail -1 | awk '{print $2}')
    for s in idle0 idle1 after; do
        f="/tmp/$tag/ram-$tag-$s-01.bin"
        printf '%-9s %-11s %-7s' "$tag" "${v:--}" "$s"
        if [ -f "$f" ]; then
            python3 "$HERE/snapshot_peek.py" "$BASE" "$f" $WORDS
        else
            printf '   <snapshot missing>\n'
        fi
    done
done
