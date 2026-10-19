#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Replay-only regeneration of the model's extra regions from a private profile.
# usage: CDJ_MODEL=cdj2000nxs scripts/build/build_dsp_supplement.sh PROFILE OUT
# OUT must be a new directory; generated C and m.so stay there, never in git.
# C66X_JIT_LIBDIR selects the core build. JOBS overrides the model's six jobs.
set -euo pipefail
E="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
. "$E/scripts/cdj_model.sh"
cdj_model_load
if [ "$#" != 2 ]; then
    echo "usage: CDJ_MODEL=cdj2000nxs $0 PROFILE OUT" >&2; exit 2
fi
[ -n "${MODEL_DSP_SUPPLEMENT_ARGS:-}" ] || { echo "no DSP supplement for $MODEL_ID" >&2; exit 2; }
[ -s "$1/profile.txt" ] || { echo "missing private DSP profile: $1" >&2; exit 1; }
PROF="$(cd "$1" && pwd)"
mkdir -p "$(dirname "$2")"
mkdir "$2"
OUT="$(cd "$2" && pwd)"
LIBDIR="${C66X_JIT_LIBDIR:-$HOME/build/c6x}"
C6X="$E/hw/cdj/c6x"
native() { case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) cygpath -m "$1" ;; *) printf '%s' "$1" ;; esac; }
# Model options are whitespace-separated words, as in build_dsp_module.sh.
read -r -a ARGS <<< "$MODEL_DSP_SUPPLEMENT_ARGS"
JOBS="${JOBS:-6}"
t0=$SECONDS
make -C "$C6X" -j"$JOBS" "O=$LIBDIR" "$LIBDIR/libc66x.so"
python3 "$C6X/tools/c14_jitgen.py" "$(native "$PROF")" "$(native "$OUT")/m" \
    --lib "$(native "$LIBDIR/libc66x.so")" "${ARGS[@]}" --jobs "$JOBS" --no-cc
FLAGS=(-O2 -std=gnu11 -fPIC -fvisibility=hidden -ffp-contract=off -frounding-math
       -march=native -w -I "$(native "$C6X")")
for f in "$OUT"/m.*.c; do printf '%s\n' "$(native "$f")"; done |
    xargs -P "$JOBS" -I{} sh -c 'exec gcc "$@" -c "$0" -o "${0%.c}.o"' {} "${FLAGS[@]}"
gcc -shared -o "$(native "$OUT")/m.so" "$OUT"/*.o
echo "supplement built: $OUT/m.so ($((SECONDS - t0)) s, $JOBS jobs)"
