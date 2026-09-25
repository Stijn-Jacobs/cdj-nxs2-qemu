#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Configure and build qemu-system-sh4 with the CDJ-2000NXS2 board.
#
# The source is the untracked QEMU checkout in qemu-src/; the build tree lives
# elsewhere ($QEMU_BUILD) because a DrvFs mount is slow to compile onto.
#
# qemu-src/ is generated: install_machine.sh copies hw/cdj/ into it, and every
# successful build regenerates and verifies patches/ so no binary exists that
# the patches do not describe. Never hand-edit patches/.
#
#   usage: scripts/build/build_main.sh
#   env:   SKIP_PATCH_REGEN=1  to build without touching patches/
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../cdj_paths.sh"; PROJ="$CDJ_ROOT"
SRC="$PROJ/qemu-src"
BUILD="${QEMU_BUILD:-$HOME/qemu-build}"

[ -d "$SRC/hw/sh4" ] || { echo "no QEMU source at $SRC" >&2; exit 1; }

# Refresh the board file from the project copy before every build.
"$HERE/install_machine.sh" "$SRC"

missing=""
for t in ninja meson pkg-config flex bison; do
    command -v "$t" >/dev/null || missing="$missing $t"
done
if [ -n "$missing" ] && [ "$(uname -s)" = Darwin ]; then
    cat >&2 <<EOF
missing build tools:$missing

install them with Homebrew:
  brew install ninja meson pkgconf glib pixman
(flex and bison come with the Xcode Command Line Tools: xcode-select --install)
EOF
    exit 1
elif [ -n "$missing" ]; then
    cat >&2 <<EOF
missing build tools:$missing

install them (needs sudo):
  sudo apt-get update
  sudo apt-get install -y build-essential ninja-build meson pkg-config \\
      python3-venv flex bison libglib2.0-dev libpixman-1-dev zlib1g-dev
EOF
    exit 1
fi

mkdir -p "$BUILD"
cd "$BUILD"

# On Windows the target is qemu-system-sh4.exe.
EXESUF=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXESUF=".exe" ;;
esac

# Configure runs only once, so a build tree created before a UI library was
# installed keeps its old feature set forever and -display help silently stays
# at "none, dbus". Reconfigure when the requested UI backend is not in the
# existing config, or when QEMU_RECONFIGURE=1 is set.
CFG_ARGS="--target-list=sh4-softmmu --disable-werror --disable-docs --disable-tools"
# Ask explicitly for the UI and sound backends, so a missing dependency fails
# loudly instead of quietly producing a binary with no window or no sound. On
# macOS they are Cocoa and Core Audio, which need nothing beyond the SDK.
case "$(uname -s)" in
    Darwin)
        CFG_ARGS="$CFG_ARGS --enable-cocoa --enable-coreaudio"
        # A Python QEMU's configure can use (qemu_python.sh). Only here:
        # elsewhere the host's python3 has always done.
        . "$HERE/qemu_python.sh"
        CFG_ARGS="$CFG_ARGS${QEMU_PYTHON_ARG:+ $QEMU_PYTHON_ARG}" ;;
    *)
        if pkg-config --exists gtk+-3.0 2>/dev/null; then
            CFG_ARGS="$CFG_ARGS --enable-gtk"
        fi ;;
esac

need_configure=0
[ -f build.ninja ] || need_configure=1
[ "${QEMU_RECONFIGURE:-0}" = "1" ] && need_configure=1
if [ -f build.ninja ] && [ "$CFG_ARGS" != "$(cat .cdj-cfg-args 2>/dev/null)" ]; then
    echo "configure flags changed -- reconfiguring"
    need_configure=1
fi

if [ "$need_configure" = "1" ]; then
    rm -f build.ninja
    # shellcheck disable=SC2086
    "$SRC/configure" $CFG_ARGS
    printf '%s' "$CFG_ARGS" > .cdj-cfg-args
fi
ninja "qemu-system-sh4$EXESUF"

# Capture what was just built. Done after the build rather than before, so the
# patches describe a tree that actually compiles.
if [ "${SKIP_PATCH_REGEN:-0}" != "1" ]; then
    echo
    "$HERE/../../patches/regen.sh" "$SRC" | tail -2
    if ! "$HERE/../../patches/verify.sh" "$SRC" >/dev/null 2>&1; then
        echo "WARNING: patches/ does not match the tree that was just built." >&2
        echo "         run ./patches/verify.sh to see which file drifted." >&2
    fi
fi

echo
echo "built: $BUILD/qemu-system-sh4$EXESUF"
"$BUILD/qemu-system-sh4$EXESUF" -M help | grep -i cdj || true
echo "display backends: $("$BUILD/qemu-system-sh4$EXESUF" -display help 2>/dev/null \
    | tail -n +2 | tr '\n' ' ')"
