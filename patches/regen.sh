#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Regenerate every patch in this directory from the live qemu-src/ tree.
#
# qemu-src/ is an untracked v9.1.0 checkout, so these patches are the record of
# the QEMU changes. Run this after any change under qemu-src/. The board
# sources (hw/cdj/) and cdj-pcring.h are copied whole, not patched.
#
#   usage: ./patches/regen.sh [path-to-qemu-src]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../scripts/cdj_paths.sh"
SRC="${1:-$CDJ_ROOT/qemu-src}"

[ -d "$SRC/hw/sh4" ] || { echo "no QEMU source at $SRC" >&2; exit 1; }

BASE="$(git -C "$SRC" describe --tags --abbrev=0)"
echo "regenerating against $BASE"

# The modified files, listed explicitly. `git diff --name-only` also reports
# vendored blobs and, on some checkouts, thousands of spurious changes.
FILES=(
    accel/tcg/cpu-exec.c
    accel/tcg/cputlb.c
    accel/tcg/tb-hash.h
    accel/tcg/tb-jmp-cache.h
    accel/tcg/tb-maint.c
    chardev/char-win-stdio.c
    hw/char/sh_serial.c
    hw/intc/sh_intc.c
    hw/sh4/Kconfig
    hw/sh4/meson.build
    hw/timer/sh_timer.c
    include/hw/sh4/sh_intc.h
    net/socket.c
    system/runstate.c
    target/sh4/cpu.c
    target/sh4/cpu.h
    target/sh4/helper.c
    target/sh4/helper.h
    target/sh4/op_helper.c
    target/sh4/translate.c
    ui/cocoa.m
)

# Warn about edits to files not on the list: they would be built but never
# captured.
mapfile -t SEEN < <(git -C "$SRC" diff --name-only -- \
    'accel/tcg/**' 'chardev/**' 'hw/char/**' 'hw/intc/**' 'hw/sh4/**' 'hw/timer/**' \
    'include/hw/sh4/**' 'net/**' 'system/**' 'target/sh4/**' 'ui/**' 2>/dev/null \
    | grep -E '\.(c|h|m|build)$|Kconfig$' || true)
for f in "${SEEN[@]:-}"; do
    [ -n "$f" ] || continue
    printf '%s\n' "${FILES[@]}" | grep -qxF "$f" \
        || echo "  UNTRACKED EDIT: $f -- modified but not in regen.sh's list" >&2
done

# core.fileMode=false: a checkout on a Windows filesystem reports every file as
# 100755, which would put a spurious mode change at the top of each patch.
WRITTEN=()
for f in "${FILES[@]}"; do
    out="${f//\//_}.patch"
    git -c core.fileMode=false -C "$SRC" diff --no-color --ignore-cr-at-eol \
        -- "$f" > "$HERE/$out"
    WRITTEN+=("$out")
    printf '  %-44s %5s lines\n' "$f" "$(wc -l < "$HERE/$out")"
done

# A leftover patch for a file no longer modified would reapply a reverted
# change. Compared by generated name, since mapping a name back to a path is
# ambiguous (underscores in hw_char_sh_serial.c).
for p in "$HERE"/*.patch; do
    printf '%s\n' "${WRITTEN[@]}" | grep -qxF "$(basename "$p")" \
        || echo "  STALE: $(basename "$p") -- no longer modified in $SRC" >&2
done

echo
echo "${#FILES[@]} patches written. Verify with: ./patches/verify.sh"
