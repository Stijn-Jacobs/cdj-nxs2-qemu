#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# CDJ-2000NXS2 check: one headless deck loads the first track of a USB image,
# and the overview strip of its screen must be moving by the end of a short
# film (scripts/run/score_playhead.py). One PASS / FAIL line.
#
#   usage: scripts/test/nxs2_play.sh [tag]
#
#   QEMU_BUILD / QEMU_EB_BUILD   the MAIN and display builds (the rig's defaults)
#   NXS2_LOCK                    lock directory held for the run (default /tmp/cdj-emu.lock)
#   LOCK_WAIT                    seconds to wait for that lock before skipping (default 240)
#   FILMN, MOTION_MS             frames and gap in ms between them (default 4 x 2000)
#
# On Windows run it from MSYS2's MINGW64 shell.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
RUN="$HERE/../run"
tag="${1:-nxs2-$$}"
lock="${NXS2_LOCK:-/tmp/cdj-emu.lock}"
start=$(date +%s)

waited=0
until mkdir "$lock" 2> /dev/null; do
    if [ "$waited" -ge "${LOCK_WAIT:-240}" ]; then
        echo "SKIP lock $lock held by $(cat "$lock/owner" 2> /dev/null || echo '?')"
        exit 0
    fi
    sleep 5
    waited=$((waited + 5))
done
echo "nxs2_play-$$" > "$lock/owner"
trap 'rm -f "$lock/owner"; rmdir "$lock" 2> /dev/null' EXIT

[ -n "${NXS2_ROOT:-}" ] && export CDJ_ROOT="$NXS2_ROOT"
export GUI_DISPLAY=none NDECKS=1 NOSOUND=1 DJLINK="${DJLINK:-0}"
export REPORTS=0 FILMN="${FILMN:-4}" MOTION_MS="${MOTION_MS:-2000}"
bash "$RUN/rig.sh" "$tag" "$FILMN" > "/tmp/$tag.out" 2>&1

verdict="$(grep -a -m1 "\[${tag}1\] VERDICT" "/tmp/run-${tag}1.txt" | sed 's/^.*VERDICT *//' | cut -d' ' -f1)"
score="$(FRAMES_ROOT="$(cygpath -m /tmp)" python3 "$RUN/score_playhead.py" "${tag}1" 2>&1 | grep -a 'VERDICT:' | tr -d '*' | sed 's/^ *VERDICT: *//')"
if [ "$verdict" = LOADED ] && case "$score" in MOTION*) true ;; *) false ;; esac; then
    echo "PASS loaded, playhead moving ($(( $(date +%s) - start )) s)"
else
    echo "FAIL load verdict '${verdict:-none}', playhead '${score:-no frames}' (/tmp/run-${tag}1.txt)"
    exit 1
fi
