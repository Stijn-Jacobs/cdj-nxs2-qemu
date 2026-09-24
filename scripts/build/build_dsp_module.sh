#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build a curated, profile-guided DSP JIT module from your own firmware, the way
# the maintainers build theirs. About an hour, once; scripts/run/rig.sh then
# loads ~/c14gen/curated/m.so and leaves the run-time auto-JIT off.
#
#   1 tools     the core's replay tool (c6xreplay) and libc66x.so
#   2 record    one headless deck plays with MASTER TEMPO and a tempo sweep
#               (scripts/run/warm_jit.sh) with no module and no auto-JIT, so
#               the whole run is recorded (CDJ_C6X_RECORD, ~10 GB) and every
#               packet profiled (C66X_JIT_PROFILE)
#   3 generate  hw/cdj/c6x/tools/c14_jitgen.py turns the profile into C
#   4 PGO       an instrumented build, trained on the first TRAIN_CYCLES of the
#               recording, then the build that uses that profile
#   5 verify    the whole recording replayed with the module; only an EXACT
#               replay (every write and state hash as recorded) installs it
#
#   usage: scripts/build/build_dsp_module.sh [--keep-recording] [--dry-run] [--preflight]
#   env:   WORK=<dir>   recording, profile, generated C (default extract/dsp-module)
#          JOBS=<n>     parallel generator/gcc jobs (min(14, threads); ~0.8 GB RAM each)
#          PLAY_S=270   virtual seconds the recorded deck plays
#          TRAIN_CYCLES=30000000000   the training slice, in DSP cycles
#          C66X_JIT_LIBDIR   where the DSP core is built (~/build/c6x)
#
# --preflight only checks disk space and tools, and says why it would not run.
# The recording is deleted after a successful build unless --keep-recording.
set -uo pipefail
E="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$E/scripts/cdj_paths.sh"
C6X="$E/hw/cdj/c6x"
LIBDIR="${C66X_JIT_LIBDIR:-$HOME/build/c6x}"
WORK="${WORK:-$CDJ_ROOT/extract/dsp-module}"
REC="$WORK/dsp.c6rec"
PROF="$WORK/profile"
GEN="$WORK/gen"
OBJ="$WORK/pgo"
DEST="$HOME/c14gen/curated"
PLAY_S="${PLAY_S:-270}"
TRAIN_CYCLES="${TRAIN_CYCLES:-30000000000}"
NEED_MB=16000
CORES="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
JOBS="${JOBS:-$(( CORES < 14 ? CORES : 14 ))}"
WIN=0; EXE=""
case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) WIN=1; EXE=".exe" ;; esac
REPLAY="$LIBDIR/c6xreplay$EXE"

KEEP=0; DRY=0; PREFLIGHT=0
for arg in "$@"; do
    case "$arg" in
        --keep-recording) KEEP=1 ;;
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
        echo "needs ~$(( NEED_MB / 1000 )) GB free for the recording (~10 GB) and the build, and $d has $(( free / 1000 )) GB; set WORK=<a folder on a bigger disk>"
        ok=1
    fi
    for t in gcc make python3 pkg-config; do
        command -v "$t" >/dev/null 2>&1 || { echo "needs $t on PATH"; ok=1; }
    done
    for t in main_unpacked.bin gui_unpacked.bin flash.bin usbmedia3.img; do
        [ -f "$CDJ_ROOT/extract/$t" ] || { echo "needs extract/$t (./setup.sh makes it)"; ok=1; }
    done
    return "$ok"
}

if [ "$PREFLIGHT" = 1 ]; then
    preflight
    exit $?
fi
echo "curated DSP module: work in $WORK, $JOBS jobs, installs to $DEST/m.so"
[ "$DRY" = 1 ] && echo "(dry run: every command is printed, none is run)"
if ! preflight; then
    [ "$DRY" = 1 ] || die "not started (see above)"
fi
[ "$DRY" = 1 ] || mkdir -p "$WORK"
t0=$SECONDS

stage "1/5 the replay tool and the core library"
run make -C "$C6X" -j"$JOBS" "O=$LIBDIR" "$LIBDIR/c6xreplay" "$LIBDIR/libc66x.so" ||
    die "the DSP core did not build"

stage "2/5 recording one deck with no module ($PLAY_S s of play; slower than real time)"
[ "$DRY" = 1 ] || { rm -rf "$PROF" && mkdir -p "$PROF" && rm -f "$REC"; }
run env MODULE=none AUTOJIT=0 PLAY_S="$PLAY_S" \
    CDJ_C6X_RECORD="$(native "$REC")" C66X_JIT_PROFILE="$(native "$PROF")" \
    bash "$E/scripts/run/warm_jit.sh" rec
if [ "$DRY" = 0 ]; then
    [ -s "$REC" ] || die "no recording at $REC (the deck log is /tmp/bridge-main-rec1.log)"
    [ -s "$PROF/profile.txt" ] || die "no profile in $PROF: the deck did not shut down cleanly"
    echo "recorded $(( $(stat -c %s "$REC") >> 20 )) MB"
fi

stage "3/5 generating C from the profile"
[ "$DRY" = 1 ] || { rm -rf "$GEN" && mkdir -p "$GEN"; }
# The maintainers' recipe, with this run's profile as every input. --lib is
# explicit: the generator's ~ default does not expand under MSYS2's Python.
run python3 "$C6X/tools/c14_jitgen.py" "$(native "$PROF")" "$(native "$GEN")/m" \
    --lib "$(native "$LIBDIR/libc66x.so")" --no-cc \
    --roots 1300 --min 20000 --kernels 48 --idle-head 0x80076F00 \
    --loops 24 --lmin 0 --qroots 80 --qmin 30000 --cold 2000 \
    --clean-prof "$(native "$PROF")" --clean-roots 2000 --qprof "$(native "$PROF")" \
    --jobs "$JOBS" || die "the generator failed"
if [ "$DRY" = 0 ]; then
    ls "$GEN"/m.*.c > /dev/null 2>&1 || die "the generator wrote no C into $GEN"
fi

stage "4/5 profile-guided build ($JOBS parallel gcc)"
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
      for f in "$GEN"/m.*.c; do printf '%s\n' "$(native "$f")"; done |
          xargs -P "$JOBS" -I{} sh -c 'n="${0##*/}"; exec gcc "$@" -c "$0" -o "${n%.c}.o"' {} "${FLAGS[@]}" "$@" &&
      gcc -shared "$@" -o m.so ./*.o )
}
replay() {  # <module> <replay tool> <log> [max cycles]: the recording, with the module
    echo "+ C66X_JIT=$1 $2 $REC ${4:-}  > $3"
    [ "$DRY" = 1 ] && return 0
    # An instrumented module's frames are large: give Linux's main thread 64 MB
    # of stack too (Windows gets it from the trainer's link flag).
    ( ulimit -s 65536 2>/dev/null; cd "$WORK" &&
      C66X_JIT="$(native "$1")" "$2" "$(native "$REC")" ${4:-} > "$3" 2>&1 )
    local rc=$?
    grep -aE '^replayed|^verdict|c66x jit: .*(ABI|error)' "$3" | sed 's/^/  /'
    return "$rc"
}
[ "$DRY" = 1 ] || { rm -rf "$OBJ" && mkdir -p "$PGO_DIR"; }
TRAINER="$REPLAY"
if [ "$WIN" = 1 ]; then
    # Windows gives the main thread 1 MB of stack; an instrumented module
    # overflows it and the process dies before gcov writes the profile. The
    # trainer is the replay tool relinked with a 64 MB stack.
    TRAINER="$OBJ/c6xreplay-train.exe"
    # shellcheck disable=SC2046  # pkg-config prints one flag per word
    run gcc -O2 -std=gnu11 -I "$(native "$C6X")" $(pkg-config --cflags gmodule-2.0) \
        "$(native "$C6X/c6xreplay.c")" "$LIBDIR/c66x_decode.o" "$LIBDIR/c66x_image.o" \
        "$LIBDIR/c66x_core.o" -o "$TRAINER" -Wl,--stack,67108864 -lm \
        $(pkg-config --libs gmodule-2.0) || die "the trainer did not link"
fi
build_module "instrumented" -fprofile-generate -fprofile-update=single \
    "-fprofile-dir=$(native "$PGO_DIR")" || die "the instrumented build failed"
replay "$OBJ/m.so" "$TRAINER" "$OBJ/replay-train.log" "$TRAIN_CYCLES"
if [ "$DRY" = 0 ]; then
    n="$(find "$PGO_DIR" -name '*.gcda' | wc -l)"
    [ "$n" -gt 0 ] || die "the training replay wrote no profile (see $OBJ/replay-train.log)"
    echo "  $n profile files"
fi
build_module "profiled" -fprofile-use -fprofile-partial-training -fprofile-correction \
    -Wno-missing-profile "-fprofile-dir=$(native "$PGO_DIR")" || die "the profiled build failed"

stage "5/5 replaying the whole recording with the module"
replay "$OBJ/m.so" "$REPLAY" "$OBJ/replay-final.log"
if [ "$DRY" = 0 ]; then
    grep -aq '^verdict: EXACT' "$OBJ/replay-final.log" ||
        die "the module did not replay EXACT, so it is not installed (see $OBJ/replay-final.log)"
fi
run mkdir -p "$DEST"
run cp "$OBJ/m.so" "$DEST/m.so.new"
run mv -f "$DEST/m.so.new" "$DEST/m.so"
[ "$DRY" = 1 ] || rm -f "$OBJ"/*.o
if [ "$KEEP" = 1 ]; then
    echo "kept the recording: $REC"
else
    run rm -f "$REC"
fi
echo "installed $DEST/m.so in $(( (SECONDS - t0) / 60 )) min; ./start.sh loads it from now on"
