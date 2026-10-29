#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Regenerate every patch in this directory from the live qemu-src/ tree.
#
# qemu-src/ is an untracked v11.1.2 checkout, so these patches are the record of
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
    accel/tcg/tcg-accel-ops-icount.c
    accel/tcg/cputlb.c
    accel/tcg/tb-hash.h
    accel/tcg/tb-jmp-cache.h
    accel/tcg/tb-maint.c
    chardev/char-win-stdio.c
    configure
    hw/char/sh_serial.c
    hw/intc/sh_intc.c
    hw/sh4/Kconfig
    hw/sh4/meson.build
    hw/timer/sh_timer.c
    include/accel/tcg/getpc.h
    include/hw/sh4/sh_intc.h
    include/qemu/event_notifier.h
    include/system/cpu-timers.h
    include/tcg/helper-info.h
    include/tcg/tcg.h
    io/channel-file.c
    meson.build
    meson_options.txt
    migration/file.c
    net/socket.c
    scripts/meson-buildoptions.sh
    scripts/symlink-install-tree.py
    system/cpus.c
    system/main.c
    system/runstate.c
    target/sh4/cpu.c
    target/sh4/cpu.h
    target/sh4/helper.c
    target/sh4/helper.h
    target/sh4/op_helper.c
    target/sh4/translate.c
    tcg/meson.build
    tcg/region.c
    tcg/tcg-op-ldst.c
    tcg/tcg.c
    tcg/wasm64.c
    tcg/wasm64.h
    tcg/wasm64/tcg-target-con-set.h
    tcg/wasm64/tcg-target-con-str.h
    tcg/wasm64/tcg-target-has.h
    tcg/wasm64/tcg-target-mo.h
    tcg/wasm64/tcg-target-opc.h.inc
    tcg/wasm64/tcg-target.c.inc
    tcg/wasm64/tcg-target.h
    ui/cocoa.m
    util/event_notifier-posix.c
    util/qemu-thread-posix.c
    util/qemu-timer.c
)

# Warn about edits to files not on the list: they would be built but never
# captured. New files count too, except the two that are copied whole.
WATCHED=('configure' 'meson.build' 'meson_options.txt' 'accel/tcg/**' 'chardev/**' 'hw/char/**' 'hw/intc/**' 'hw/sh4/**' 'hw/timer/**'
    'include/**' 'io/**' 'migration/**' 'net/**' 'scripts/**' 'system/**' 'target/sh4/**' 'tcg/**' 'ui/**' 'util/**')
mapfile -t SEEN < <({ git -C "$SRC" diff --name-only -- "${WATCHED[@]}"
                      git -C "$SRC" ls-files --others --exclude-standard -- "${WATCHED[@]}"; } 2>/dev/null \
    | grep -vE '^hw/sh4/cdj/|^include/exec/cdj-pcring\.h$' \
    | grep -E '\.(c|h|m|inc|build|py|sh)$|Kconfig$|^configure$|^meson_options\.txt$' || true)
for f in "${SEEN[@]:-}"; do
    [ -n "$f" ] || continue
    printf '%s\n' "${FILES[@]}" | grep -qxF "$f" \
        || echo "  UNTRACKED EDIT: $f -- modified but not in regen.sh's list" >&2
done

# core.fileMode=false: a checkout on a Windows filesystem reports every file as
# 100755, which would put a spurious mode change at the top of each patch.
WRITTEN=()
for f in "${FILES[@]}"; do
    # A file QEMU does not have is diffed as a new file.
    git -C "$SRC" cat-file -e "HEAD:$f" 2>/dev/null || git -C "$SRC" add -N -- "$f"
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
