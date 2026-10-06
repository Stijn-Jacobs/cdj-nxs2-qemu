#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The quick check: everything that runs in a couple of minutes, all at once.
#
#   unit    the offline tests in tests/ (pytest)
#   c6x     every DSP recording in CDJ_TEST_C6REC replayed for its first
#           C6REC_CYCLES core cycles by a c6xreplay built from this tree; the
#           verdict must be EXACT (recorded state hashes and bus writes match)
#   decks   every deck in CDJ_TEST_DECKS booted headless to its first stable
#           screen (scripts/test/deck_smoke.py; record its frames with
#           `deck_smoke.py <ini> --record` on a known-good build first)
#
# --tier2 is the longer check, a few minutes more, still all side by side:
#
#   decks2  as decks, then each deck's key presses up to a loaded track, which
#           must reach its second reference frame (deck_smoke.py --tier2);
#           a deck with `motion = <model>` must then also move its playhead
#           (scripts/run/score_playhead.py)
#   nxs2    CDJ_TEST_NXS2=1: one CDJ-2000NXS2 loads a track and its waveform
#           strip must move (scripts/test/nxs2_play.sh)
#
# A part with nothing to run (no recordings, no deck table) is skipped.
#
#   usage: scripts/test/quick.sh [--tier2] [unit|c6x|decks|decks2|nxs2 ...]
#
#   CDJ_TEST_OUT      logs and frames (default $TMPDIR/cdj-quick)
#   CDJ_TEST_PYTHON   a Python with pytest (default: the first that has it)
#   UNIT_SHARDS       pytest processes side by side (default 4)
#   CDJ_TEST_C6REC    .c6rec files, space separated
#   C6REC_CYCLES      default 200000000
#   CDJ_TEST_DECKS    a deck_smoke.py table
#   CDJ_TEST_NXS2     1 to include the NXS2 part (needs the rig's builds)
#   CDJ_TEST_LOCK_WAIT  seconds a deck waits for its lane lock before it is skipped (default 20)
#
# On Windows run it from MSYS2's MINGW64 shell, which has the compiler and
# the DLLs the QEMU builds need.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EMU="$(cd "$HERE/../.." && pwd)"
OUT="${CDJ_TEST_OUT:-${TMPDIR:-/tmp}/cdj-quick}"
mkdir -p "$OUT"
if [ "${1:-}" = --tier2 ]; then
    shift
    PARTS="${*:-unit c6x decks2 nxs2}"
else
    PARTS="${*:-unit c6x decks}"
fi

pick_python() {
    for py in "${CDJ_TEST_PYTHON:-}" python3 python py; do
        [ -n "$py" ] && "$py" -c 'import pytest' > /dev/null 2>&1 && { echo "$py"; return; }
    done
}

run_unit() {
    local py n i f files rc=0
    py="$(pick_python)"
    [ -n "$py" ] || { echo "SKIP no Python with pytest (pip install -r requirements-dev.txt)"; return; }
    # The suite is mostly process start-up and small compiles, so it shards
    # well: each shard takes every n-th test file.
    n="${UNIT_SHARDS:-4}"
    for i in $(seq 0 $((n - 1))); do
        files=""
        for f in $(cd "$EMU" && ls tests/test_*.py | awk -v n="$n" -v i="$i" 'NR % n == i'); do files="$files $f"; done
        (cd "$EMU" && "$py" -m pytest $files -q -p no:cacheprovider) > "$OUT/unit-$i.log" 2>&1 &
    done
    for i in $(seq 0 $((n - 1))); do wait -n || rc=1; done
    local passed failed
    passed=$(cat "$OUT"/unit-*.log | grep -aoE '[0-9]+ passed' | awk '{s += $1} END {print s + 0}')
    failed=$(cat "$OUT"/unit-*.log | grep -aoE '[0-9]+ (failed|error)' | awk '{s += $1} END {print s + 0}')
    if [ $rc = 0 ]; then echo "PASS $passed tests"; else echo "FAIL $failed of $((passed + failed)) tests (unit-*.log)"; fi
}

run_c6x() {
    local recs="" r
    for r in ${CDJ_TEST_C6REC:-}; do [ -f "$r" ] && recs="$recs $r"; done
    [ -n "$recs" ] || { echo "SKIP no recordings in CDJ_TEST_C6REC"; return; }
    make -s -C "$EMU/hw/cdj/c6x" O="$OUT/c6x" "$OUT/c6x/c6xreplay" > "$OUT/c6x-build.log" 2>&1 \
        || { echo "FAIL c6xreplay did not build (c6x-build.log)"; return; }
    for r in $recs; do
        "$OUT/c6x/c6xreplay" "$r" "${C6REC_CYCLES:-200000000}" > "$OUT/c6x-$(basename "$r" .c6rec).log" 2>&1 &
    done
    wait
    local bad=""
    for r in $recs; do
        grep -aq '^verdict: EXACT' "$OUT/c6x-$(basename "$r" .c6rec).log" || bad="$bad $(basename "$r")"
    done
    if [ -z "$bad" ]; then echo "PASS $(echo $recs | wc -w | tr -d ' ') recordings EXACT"; else echo "FAIL not EXACT:$bad"; fi
}

run_decks() {
    [ -n "${CDJ_TEST_DECKS:-}" ] || { echo "SKIP no deck table in CDJ_TEST_DECKS"; return; }
    python3 "$HERE/deck_smoke.py" "$CDJ_TEST_DECKS" ${1:+"$1"} --lock-wait "${CDJ_TEST_LOCK_WAIT:-20}" --out "$OUT/${2:-decks}" > "$OUT/${2:-decks}.log" 2>&1
    local rc=$?
    grep -q ' PASS \| FAIL ' "$OUT/${2:-decks}.log" || { echo "FAIL deck_smoke.py did not run (${2:-decks}.log)"; return; }
    if [ $rc = 0 ]; then echo "PASS $(grep -c ' PASS ' "$OUT/${2:-decks}.log") decks"; else echo "FAIL see below"; fi
}

run_decks2() { run_decks --tier2 decks2; }

run_nxs2() {
    [ "${CDJ_TEST_NXS2:-}" = 1 ] || { echo "SKIP set CDJ_TEST_NXS2=1 to run the NXS2 deck"; return; }
    bash "$HERE/nxs2_play.sh" | tail -1
}

start=$(date +%s)
for p in $PARTS; do
    case "$p" in
        unit | c6x | decks | decks2 | nxs2) "run_$p" > "$OUT/$p.result" 2>&1 & ;;
        *) echo "unknown part '$p' (unit, c6x, decks, decks2, nxs2)" >&2; exit 2 ;;
    esac
done
wait

fail=0
for p in $PARTS; do
    r="$(cat "$OUT/$p.result")"
    printf '%-6s %s\n' "$p" "$r"
    case "$r" in FAIL*) fail=1 ;; esac
    case "$p" in decks | decks2) [ -f "$OUT/$p.log" ] && sed 's/^/         /' "$OUT/$p.log" ;; esac
done
echo "quick check: $(( $(date +%s) - start )) s, logs in $OUT"
exit $fail
