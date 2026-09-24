#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One headless deck, no sound, no Pro DJ Link, that loads a track, plays it and
# has MASTER TEMPO and the tempo fader swept (warm_jit.py), then stops by itself.
# Run with the auto-JIT on, it fills the DSP code cache (~/c14gen) so real
# sessions start fast; scripts/build/build_dsp_module.sh runs it to record the
# DSP instead. Ends with the auto-JIT's own count of what it built.
#
#   usage: bash scripts/run/warm_jit.sh [tag=warm] [--dry-run]
#   env:   PLAY_S=270 (virtual seconds of play after the load), plus any rig.sh
#          knob: MODULE=none, AUTOJIT=0|1, CDJ_C6X_RECORD, C66X_JIT_PROFILE, ...
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG=warm; DRY=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY=1 ;;
        -h | --help) sed -n '/^#   usage:/,/^#          knob/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) TAG="$arg" ;;
    esac
done
DECK="${TAG}1"
PLAY_S="${PLAY_S:-270}"
# The sweep starts once the track is playing and ends before the deck does.
START_S=60
SWEEP_S=$(( PLAY_S - 30 ))
[ "$SWEEP_S" -gt 0 ] || SWEEP_S=$PLAY_S
JIT_CACHE="$HOME/c14gen"
VCLOCK="/tmp/cdj-$DECK-vclock"
MAINLOG="/tmp/bridge-main-$DECK.log"

DRIVER=(python3 "$HERE/warm_jit.py" "$DECK" --vclock "$VCLOCK" --start "$START_S" --duration "$SWEEP_S")
# AUTOLOAD=1: the deck loads the first track and plays it by itself. FRAMES=1
# with MOTION_MS = the play time: the run ends that long after the load.
RIG=(env -u C66X_JIT AUTOLOAD=1 GUI_DISPLAY=none NOSOUND=1 DJLINK=0 NDECKS=1 WARM=0
     MOTION_MS=$(( PLAY_S * 1000 )) bash "$HERE/rig.sh" "$TAG" 1)
if [ "$DRY" = 1 ]; then
    printf 'rm -f %s\n' "$VCLOCK"
    printf '%q ' "${DRIVER[@]}"; printf '&\n'
    printf '%q ' "${RIG[@]}"; printf '\n'
    exit 0
fi

modules() { find "$JIT_CACHE" -maxdepth 2 -path '*/batch*/m.so' 2>/dev/null | wc -l; }
before=$(modules)
# A clock file left by an earlier run would start the sweep at once.
rm -f "$VCLOCK"
"${DRIVER[@]}" &
DRV=$!
trap 'kill "$DRV" 2>/dev/null' EXIT INT TERM
"${RIG[@]}"
rc=$?
wait "$DRV" 2>/dev/null

echo "--- DSP JIT"
grep -a 'c6x\[.*\]: jit:' "$MAINLOG" 2>/dev/null | tail -1
grep -a 'c66x jit auto:' "$MAINLOG" 2>/dev/null | tail -1
after=$(modules)
echo "warm: $(( after - before )) new auto-JIT modules, $after in the cache ($JIT_CACHE)"
if [ ! -s "$MAINLOG" ]; then
    echo "FAILED the deck wrote no log ($MAINLOG): it did not start"
    exit 1
fi
if ! grep -aq 'c66x jit auto:\|c6x\[.*\]: jit:' "$MAINLOG"; then
    echo "note: the deck did not report its DSP JIT; it may not have shut down cleanly"
fi
exit "$rc"
