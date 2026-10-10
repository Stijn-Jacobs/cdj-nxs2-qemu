#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a profile-guided DSP JIT module from your own firmware, the way the
# maintainers build theirs. scripts/run/rig.sh and the one-window players load
# ~/c14gen/curated[-<model>]/m.so and leave the run-time auto-JIT off.
#
# About half an hour, no recording. One headless deck boots, loads a track and
# plays PLAY_S (default 30; 45 for the rig player, whose play also sweeps MASTER TEMPO
# and the tempo fader) seconds with no module; its machine writes the
# profile at exit. After the instrumented build a second such run, with that
# module loaded, collects the profile-guided counts. A short play is enough
# because the hot code is the decoder every play runs: on the CDJ-2000 fw 4.33 a
# module from boot plus 15 s of play compiles the same 8.4 G of a 10.57 G-cycle
# play recording as one from 270 s, and replays it EXACT and faster.
#
# The module is installed with an m.stamp beside it naming what it was built
# from (launcher/dsp_module.py); ./start.sh compares it with the current values.
#
#   1 tools     the DSP core library (libc66x.so) the generator plans against
#   2 profile   one headless deck plays with no module (scripts/run/warm_jit.sh)
#               and writes C66X_JIT_PROFILE
#   3 generate  hw/cdj/c6x/tools/c14_jitgen.py turns the profile into C
#   4 PGO       an instrumented build, a second play with it for the
#               profile-guided counts, then the build that uses them
#
#   usage: scripts/build/build_dsp_module.sh [--reuse-profile] [--dry-run] [--preflight]
#   env:   CDJ_MODEL=<id>  the player (default cdj2000nxs2). The module installs to
#                  ~/c14gen/curated[-<id>]/m.so
#          WORK=<dir>   profile and generated C (default extract/dsp-module)
#          JOBS=<n>     parallel generator/gcc jobs (min(14, threads); ~0.8 GB RAM each)
#          PLAY_S=30|45 seconds the deck plays, each time
#          DSP_MODULE_DIR=<dir>   install there instead of ~/c14gen/<module>
#          C66X_JIT_LIBDIR   where the DSP core is built (~/build/c6x)
#
# --preflight only checks disk space and tools, and says why it would not run.
# --reuse-profile skips the profile run and builds from the one in WORK.
set -uo pipefail
E="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$E/scripts/cdj_paths.sh"
. "$E/scripts/cdj_model.sh"
cdj_model_load || exit 2
C6X="$E/hw/cdj/c6x"
LIBDIR="${C66X_JIT_LIBDIR:-$HOME/build/c6x}"
# The default model's module is "curated"; launcher/model.py names the others alike.
CURATED=curated; [ "$MODEL_ID" = cdj2000nxs2 ] || CURATED="curated-$MODEL_ID"
export CDJ_MODEL="$MODEL_ID"
IDLE_SPEC="${MODEL_DSP_IDLE:-}"
IDLE_HEAD="${IDLE_SPEC%%:*}"; IDLE_HEAD="${IDLE_HEAD:-0x80076F00}"
# A one-window deck's machine profiles differently from the rig's;
# a rig player with its own DSP program (the CDJ-TOUR1) names the main loop it
# skips in the same field and is built the way the default player is.
OLDER=0; [ -z "$IDLE_SPEC" ] || [ "${MODEL_LAUNCH:-rig}" = rig ] || OLDER=1
WORK="${WORK:-$CDJ_ROOT/extract/dsp-module${CURATED#curated}}"
PROF="$WORK/profile"
GEN="$WORK/gen"
OBJ="$WORK/pgo"
DEST="${DSP_MODULE_DIR:-$HOME/c14gen/$CURATED}"
# The rig player's profile play also sweeps MASTER TEMPO and the tempo fader
# (scripts/run/warm_jit.py), which takes longer than the plain play of a
# one-window model.
if [ "$OLDER" = 1 ]; then PLAY_S="${PLAY_S:-30}"; else PLAY_S="${PLAY_S:-45}"; fi
NEED_MB=2000
CORES="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
JOBS="${JOBS:-$(( CORES < 14 ? CORES : 14 ))}"
WIN=0
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) WIN=1 ;; esac
# gcc's profile-guided build writes .gcda files; clang's (macOS's gcc is clang)
# writes .profraw files that llvm-profdata merges into one .profdata.
CLANG=0; PROFDATA=()
if gcc --version 2>/dev/null | grep -q clang; then
    CLANG=1
    if command -v xcrun >/dev/null 2>&1 && xcrun --find llvm-profdata >/dev/null 2>&1; then
        PROFDATA=(xcrun llvm-profdata)
    elif command -v llvm-profdata >/dev/null 2>&1; then
        PROFDATA=(llvm-profdata)
    fi
fi

DRY=0; PREFLIGHT=0; REUSE=0
for arg in "$@"; do
    case "$arg" in
        --reuse-profile) REUSE=1 ;;
        --dry-run) DRY=1 ;;
        --preflight) PREFLIGHT=1 ;;
        -h | --help) sed -n '/^#   usage:/,/^#          C66X_JIT_LIBDIR/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg (--help)" >&2; exit 2 ;;
    esac
done

# Native Windows tools (gcc, python, the replay) get C:/... paths.
native() { if [ "$WIN" = 1 ]; then cygpath -m "$1"; else printf '%s' "$1"; fi; }
stage() { printf '\n=== %s  (%s)\n' "$1" "$(date +%H:%M:%S)"; }
# "FAILED " at the start of a line is what ./setup.sh's log check looks for.
die() { echo "FAILED $*"; exit 1; }
run() {  # print a command, then run it unless --dry-run
    printf '+'; printf ' %q' "$@"; printf '\n'
    [ "$DRY" = 1 ] || "$@"
}

preflight() {
    local d="$WORK" free t ok=0
    while [ ! -d "$d" ]; do d="$(dirname "$d")"; done
    free="$(df -Pm "$d" 2>/dev/null | awk 'NR==2 {print $4}')"
    if [ -n "$free" ] && [ "$free" -lt "$NEED_MB" ]; then
        echo "needs ~$(( NEED_MB / 1000 )) GB free for the build, and $d has $(( free / 1000 )) GB; set WORK=<a folder on a bigger disk>"
        ok=1
    fi
    for t in gcc make python3 pkg-config; do
        command -v "$t" >/dev/null 2>&1 || { echo "needs $t on PATH"; ok=1; }
    done
    if [ "$CLANG" = 1 ] && [ "${#PROFDATA[@]}" -eq 0 ]; then
        echo "gcc here is clang, and its profile-guided build needs llvm-profdata (Xcode's, via xcrun)"
        ok=1
    fi
    if [ "$OLDER" = 1 ]; then
        needs=("$MODEL_EXTRACT/main_unpacked.bin" extract/usbmedia3.img)
        [ -z "${MODEL_DISPLAY_UPD:-}" ] || needs+=("$MODEL_EXTRACT/display.upd")
    else
        needs=("$MODEL_EXTRACT"/{main_unpacked,gui_unpacked,flash}.bin extract/usbmedia3.img)
    fi
    for t in "${needs[@]}"; do
        [ -f "$CDJ_ROOT/$t" ] || { echo "needs $t (./setup.sh makes it)"; ok=1; }
    done
    return "$ok"
}

if [ "$PREFLIGHT" = 1 ]; then
    preflight
    exit $?
fi
echo "DSP module: work in $WORK, $JOBS jobs, installs to $DEST/m.so"
[ "$DRY" = 1 ] && echo "(dry run: every command is printed, none is run)"
if ! preflight; then
    [ "$DRY" = 1 ] || die "not started (see above)"
fi
[ "$DRY" = 1 ] || mkdir -p "$WORK"
t0=$SECONDS

stage "1/4 the core library"
run make -C "$C6X" -j"$JOBS" "O=$LIBDIR" "$LIBDIR/libc66x.so" ||
    die "the DSP core did not build"

# A deck whose auto-load missed sits on the browse list: its profile is then the
# idle DSP, and a module built from it leaves all of playback to the
# interpreter -- it replays EXACT and runs no faster than no module.
played() {  # <warm_jit exit code> <tag>
    if [ "$OLDER" = 1 ]; then
        [ "$1" = 0 ] || die "the deck never played the track (see /tmp/bridge-main-${2}1.log); run it again"
    else
        pcm="$(grep -a 'nonzero PCM words' "/tmp/bridge-main-${2}1.log" 2>/dev/null | tail -1 |
               sed -n 's/.*nonzero PCM words \([0-9]*\).*/\1/p')"
        [ "${pcm:-0}" -gt 0 ] ||
            die "the deck never played the track (no PCM in /tmp/bridge-main-${2}1.log; see /tmp/run-${2}1.txt); run it again"
    fi
}

if [ "$REUSE" = 1 ] && [ -s "$PROF/profile.txt" ]; then
    stage "2/4 profile"
    echo "using the profile already in $WORK"
else
    stage "2/4 profiling one deck with no module ($PLAY_S s of play; slower than real time)"
    [ "$DRY" = 1 ] || { rm -rf "$PROF" && mkdir -p "$PROF"; }
    run env MODULE=none AUTOJIT=0 PLAY_S="$PLAY_S" C66X_JIT_PROFILE="$(native "$PROF")" \
        bash "$E/scripts/run/warm_jit.sh" prof
    prof_rc=$?
    [ "$DRY" = 1 ] || played "$prof_rc" prof
fi
if [ "$DRY" = 0 ]; then
    [ -s "$PROF/profile.txt" ] || die "no profile in $PROF: the deck did not shut down cleanly"
fi

stage "3/4 generating C from the profile"
[ "$DRY" = 1 ] || { rm -rf "$GEN" && mkdir -p "$GEN"; }
# The maintainers' recipe, with this run's profile as every input. --lib is
# explicit: the generator's ~ default does not expand under MSYS2's Python.
# A short play counts few queued-branch entries: at --qmin 30000 only 76 of
# those regions qualify on the CDJ-2000.
GEN_ARGS=(--roots 1300 --min 20000 --kernels 48 --loops 24 --lmin 0 --qroots 160 --qmin 4000 --cold 2000)
if [ "$OLDER" = 1 ]; then
    # shellcheck disable=SC2206  # the profile's options are words
    GEN_ARGS+=(--idle-head "$IDLE_HEAD" $MODEL_DSP_GEN_ARGS)
else
    GEN_ARGS+=(--idle-head "$IDLE_HEAD" --clean-prof "$(native "$PROF")" --clean-roots 2000
               --qprof "$(native "$PROF")")
fi
run python3 "$C6X/tools/c14_jitgen.py" "$(native "$PROF")" "$(native "$GEN")/m" \
    --lib "$(native "$LIBDIR/libc66x.so")" --no-cc "${GEN_ARGS[@]}" \
    --jobs "$JOBS" || die "the generator failed"
if [ "$DRY" = 0 ]; then
    ls "$GEN"/m.*.c > /dev/null 2>&1 || die "the generator wrote no C into $GEN"
fi

stage "4/4 profile-guided build ($JOBS parallel gcc)"
PGO_DIR="$OBJ/prof"
# The flags c14_jitgen.py compiles with. -ffp-contract=off and -frounding-math
# keep the DSP's arithmetic exact; -I is for the generated C's core header.
FLAGS=(-O2 -std=gnu11 -fPIC -fvisibility=hidden -ffp-contract=off -frounding-math
       -march=native -w -I "$(native "$C6X")")
build_module() {  # <label> <extra gcc flags...>: every generated file, then m.so
    local label="$1"; shift
    echo "+ gcc ${FLAGS[*]} $* -c <each of $GEN/m.*.c>, then gcc -shared -o $OBJ/m.so  ($label)"
    [ "$DRY" = 1 ] && return 0
    rm -f "$OBJ"/*.o "$OBJ/m.so"
    # Relative object names, compiled from inside $OBJ: gcc names each .gcda
    # after its object, and mingw's gcov cannot join an absolute name onto
    # -fprofile-dir. Both builds use the same names, so the profile matches.
    ( cd "$OBJ" &&
      for f in "$GEN"/m.*.c; do printf '%s
' "$(native "$f")"; done |
          xargs -P "$JOBS" -I{} sh -c 'n="${0##*/}"; exec gcc "$@" -c "$0" -o "${n%.c}.o"' {} "${FLAGS[@]}" "$@" &&
      gcc -shared "$@" -o m.so ./*.o )
}
[ "$DRY" = 1 ] || { rm -rf "$OBJ" && mkdir -p "$PGO_DIR"; }
if [ "$CLANG" = 1 ]; then
    build_module "instrumented" "-fprofile-generate=$PGO_DIR" || die "the instrumented build failed"
else
    build_module "instrumented" -fprofile-generate -fprofile-update=single \
        "-fprofile-dir=$(native "$PGO_DIR")" || die "the instrumented build failed"
fi
# The counts are written when the deck quits and the module is unloaded.
run env MODULE=none AUTOJIT=0 PLAY_S="$PLAY_S" DSP_TRAIN_MODULE="$(native "$OBJ/m.so")" \
    bash "$E/scripts/run/warm_jit.sh" train
train_rc=$?
[ "$DRY" = 1 ] || played "$train_rc" train
if [ "$DRY" = 0 ]; then
    n="$(find "$PGO_DIR" -name "$([ "$CLANG" = 1 ] && echo '*.profraw' || echo '*.gcda')" | wc -l)"
    [ "$n" -gt 0 ] || die "the training run wrote no profile (see /tmp/bridge-main-train1.log)"
    echo "  $n profile files"
fi
if [ "$CLANG" = 1 ]; then
    run "${PROFDATA[@]}" merge -o "$OBJ/m.profdata" "$PGO_DIR" || die "llvm-profdata could not merge the profile"
    build_module "profiled" "-fprofile-use=$OBJ/m.profdata" || die "the profiled build failed"
else
    build_module "profiled" -fprofile-use -fprofile-partial-training -fprofile-correction \
        -Wno-missing-profile "-fprofile-dir=$(native "$PGO_DIR")" || die "the profiled build failed"
fi

run mkdir -p "$DEST"
run rm -f "$DEST/m.stamp"
run cp "$OBJ/m.so" "$DEST/m.so.new"
run mv -f "$DEST/m.so.new" "$DEST/m.so"
run env PYTHONPATH="$(native "$E")" python3 -m launcher dsp-stamp "$(native "$DEST")" ||
    die "the module was installed but its stamp was not written"
[ "$DRY" = 1 ] || rm -f "$OBJ"/*.o
echo "installed $DEST/m.so in $(( (SECONDS - t0) / 60 )) min; ./start.sh loads it from now on"
