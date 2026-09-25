#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Reapply the local QEMU modifications to qemu-src/.
#
# qemu-src/ is not tracked and is lost if re-fetched, so the changes are kept
# as patches here. Full reproduction:
#
#   git clone --depth 1 --branch v9.1.0 https://gitlab.com/qemu-project/qemu.git qemu-src
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

# The one new file the patches cannot create. The board sources belong to
# install_machine.sh. No install -D: BSD install (macOS) reads it differently.
mkdir -p "$SRC/include/exec"
install -m644 "$HERE/include_exec_cdj-pcring.h" "$SRC/include/exec/cdj-pcring.h"
echo "installed include/exec/cdj-pcring.h"

for p in "$HERE"/*.patch; do
    [ -s "$p" ] || continue
    name="$(basename "$p")"
    if git -C "$SRC" apply --check --ignore-whitespace "$p" 2>/dev/null; then
        git -C "$SRC" apply --ignore-whitespace "$p"
        echo "applied  $name"
    elif git -C "$SRC" apply --check --reverse --ignore-whitespace "$p" 2>/dev/null; then
        echo "already  $name"
    else
        echo "FAILED   $name -- apply by hand" >&2
    fi
done

echo
echo "now: ./scripts/build/install_machine.sh $SRC   # the board sources"
echo "then ./build.sh main display"
echo "then ./patches/verify.sh         # confirm tree and patches agree"
