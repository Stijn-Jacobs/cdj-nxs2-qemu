#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Configure and build qemu-system-sh4eb with the SH7269 GUI board.
#
# The GUI board is big-endian, so it is a second binary from a second build
# tree (sh4eb-softmmu); build_main.sh never produces it.
#
#   usage: scripts/build/build_display.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../cdj_paths.sh"; PROJ="$CDJ_ROOT"
SRC="$PROJ/qemu-src"
BUILD="${QEMU_EB_BUILD:-$HOME/qemu-build-eb}"

[ -d "$SRC/hw/sh4" ] || { echo "no QEMU source at $SRC" >&2; exit 1; }

"$HERE/install_machine.sh" "$SRC"

mkdir -p "$BUILD"
cd "$BUILD"

# On Windows the target is qemu-system-sh4eb.exe.
EXESUF=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXESUF=".exe" ;;
esac

CFG_ARGS="--target-list=sh4eb-softmmu --disable-werror --disable-docs --disable-tools"
# The display board's window: Cocoa on macOS, GTK elsewhere (see build_main.sh).
case "$(uname -s)" in
    Darwin)
        CFG_ARGS="$CFG_ARGS --enable-cocoa" ;;
    *)
        if pkg-config --exists gtk+-3.0 2>/dev/null; then
            CFG_ARGS="$CFG_ARGS --enable-gtk"
        fi ;;
esac
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) ;;
    *) . "$HERE/qemu_python.sh"
       CFG_ARGS="$CFG_ARGS${QEMU_PYTHON_ARG:+ $QEMU_PYTHON_ARG}" ;;
esac

need_configure=0
[ -f build.ninja ] || need_configure=1
[ "${QEMU_RECONFIGURE:-0}" = "1" ] && need_configure=1
if [ "$need_configure" = "1" ]; then
    rm -f build.ninja
    # shellcheck disable=SC2086
    "$SRC/configure" $CFG_ARGS
    printf '%s' "$CFG_ARGS" > .cdj-eb-cfg-args
fi
ninja "qemu-system-sh4eb$EXESUF"

echo
echo "built: $BUILD/qemu-system-sh4eb$EXESUF"
"$BUILD/qemu-system-sh4eb$EXESUF" -M help | grep -i sh7269 || true
