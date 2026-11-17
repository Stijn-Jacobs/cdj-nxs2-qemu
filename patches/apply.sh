#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Reapply the local QEMU modifications to qemu-src/.
#
# qemu-src/ is not tracked and is lost if re-fetched, so the changes are kept
# as patches here. Full reproduction:
#
#   git clone --depth 1 --branch v11.1.2 https://gitlab.com/qemu-project/qemu.git qemu-src
#   ./patches/apply.sh                         # patches + cdj-pcring.h
#   ./scripts/build/install_machine.sh qemu-src   # the board sources
#   ./build.sh main display
#
# After any edit under qemu-src/, run ./patches/regen.sh, then
# ./patches/verify.sh.
#
#   usage: ./patches/apply.sh [path-to-qemu-src]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../scripts/cdj_paths.sh"
SRC="${1:-$CDJ_ROOT/qemu-src}"

[ -d "$SRC/hw/sh4" ] || { echo "no QEMU source at $SRC" >&2; exit 1; }
# A checkout from before a QEMU rebase takes none of these patches.
if [ "$(cat "$SRC/VERSION")" != 11.1.2 ]; then
    echo "$SRC is QEMU $(cat "$SRC/VERSION"); these patches are for 11.1.2." >&2
    echo "move it aside and fetch v11.1.2 (./build.sh source)." >&2
    exit 1
fi

# The one new file the patches cannot create. The board sources belong to
# install_machine.sh. No install -D: BSD install (macOS) reads it differently.
mkdir -p "$SRC/include/exec"
install -m644 "$HERE/include_exec_cdj-pcring.h" "$SRC/include/exec/cdj-pcring.h"
echo "installed include/exec/cdj-pcring.h"

failed=0
for p in "$HERE"/*.patch; do
    [ -s "$p" ] || continue
    name="$(basename "$p")"
    if git -C "$SRC" apply --check --ignore-whitespace "$p" 2>/dev/null; then
        git -C "$SRC" apply --ignore-whitespace "$p"
        echo "applied  $name"
    elif git -C "$SRC" apply --check --reverse --ignore-whitespace "$p" 2>/dev/null; then
        echo "already  $name"
    elif git -C "$SRC" apply --numstat "$p" | cut -f3 |
             while IFS= read -r f; do
                 if git -C "$SRC" cat-file -e "HEAD:$f" 2>/dev/null; then
                     git -C "$SRC" checkout -q HEAD -- "$f" || exit 1
                 else
                     rm -f "$SRC/$f"
                 fi
             done &&
         git -C "$SRC" apply --check --ignore-whitespace "$p" 2>/dev/null; then
        # An older version of this patch was applied: its files went back to
        # QEMU's own version above (or away, if the patch creates them), and
        # the current one goes on top.
        git -C "$SRC" apply --ignore-whitespace "$p"
        echo "updated  $name"
    else
        echo "FAILED   $name -- apply by hand" >&2
        failed=1
    fi
done
[ "$failed" = 0 ] || exit 1

echo
echo "now: ./scripts/build/install_machine.sh $SRC   # the board sources"
echo "then ./build.sh main display"
echo "then ./patches/verify.sh         # confirm tree and patches agree"
