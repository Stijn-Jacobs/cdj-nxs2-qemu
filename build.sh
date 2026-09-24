#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build everything the decks need on THIS host: the two patched QEMUs (MAIN,
# sh4; display board, sh4eb) and the C66x DSP core library that the run-time
# JIT compiles against. Works from an MSYS2 MINGW64 shell on Windows and on Linux.
# ./setup.sh runs these same phases one by one, with progress and logs.
#
#   usage: ./build.sh [phase ...]          (default: all of them, in order)
#   phases: source   fetch QEMU v9.1.0 into qemu-src/ (skipped if it is there)
#           patches  apply patches/ to it (idempotent: applied ones are skipped)
#           main     the MAIN QEMU, qemu-system-sh4 with the CDJ-2000NXS2 board
#           display  the display-board QEMU, qemu-system-sh4eb with the SH7269
#           dsp      the DSP core library, libc66x.so, for the run-time JIT
#   env:    QEMU_BUILD / QEMU_EB_BUILD   build trees. Windows default
#                                        /c/qemu-build-mingw[-eb]: they must be on
#                                        the same drive as this source, on NTFS.
#                                        Linux default ~/qemu-build[-eb].
#           C66X_JIT_LIBDIR              where libc66x.so goes (~/build/c6x)
#           JOBS                         parallel make jobs for the DSP library
#
# The C66x DSP is interpreted except where native code has been compiled for
# its hot spots. Without a prepared module the auto-JIT does that during the
# first runs (it needs gcc at run time and libc66x.so from the dsp phase), so
# early sessions run slower than real time while ~/c14gen fills. A curated
# module is built from a recording of your own firmware and is not included.
set -euo pipefail
E="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HERE="$E/scripts"
. "$HERE/cdj_paths.sh"
SRC="$CDJ_ROOT/qemu-src"
# Regenerating patches/ from the tree is a maintainer step, not a build step.
export SKIP_PATCH_REGEN=1

on_windows() {
    case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) return 0 ;; esac
    return 1
}

phase_source() {
    if [ -d "$SRC/hw/sh4" ]; then
        echo "== QEMU source already at $SRC"
        return
    fi
    echo "== fetching QEMU v9.1.0 into $SRC"
    git clone --depth 1 --branch v9.1.0 https://gitlab.com/qemu-project/qemu.git "$SRC"
}

phase_patches() {
    echo "== applying patches/"
    bash "$E/patches/apply.sh" "$SRC"
}

phase_main() {
    if on_windows; then
        echo "== building the MAIN QEMU (Windows, MSYS2)"
        bash "$E/scripts/build/build_windows.sh" main
    else
        echo "== building the MAIN QEMU (Linux)"
        bash "$E/scripts/build/build_main.sh"
    fi
}

phase_display() {
    if on_windows; then
        echo "== building the display-board QEMU (Windows, MSYS2)"
        bash "$E/scripts/build/build_windows.sh" eb
    else
        echo "== building the display-board QEMU (Linux)"
        bash "$E/scripts/build/build_display.sh"
    fi
}

phase_dsp() {
    echo "== building the DSP core library for the auto-JIT"
    make -C "$E/hw/cdj/c6x" -j"${JOBS:-4}" "O=${C66X_JIT_LIBDIR:-$HOME/build/c6x}" \
        "${C66X_JIT_LIBDIR:-$HOME/build/c6x}/libc66x.so"
}

[ "$#" -gt 0 ] || set -- all
for phase in "$@"; do
    case "$phase" in
        all)     phase_source; phase_patches; phase_main; phase_display; phase_dsp ;;
        source)  phase_source ;;
        patches) phase_patches ;;
        main)    phase_main ;;
        display) phase_display ;;
        dsp)     phase_dsp ;;
        -h | --help) sed -n '/^#   usage:/,/^#           JOBS/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown phase '$phase' (source, patches, main, display, dsp or all)" >&2; exit 2 ;;
    esac
done

if [ "$*" = all ]; then
    echo
    echo "done. Next: ./setup.sh --skip-build walks you through the firmware, the"
    echo "USB image and your setup; or by hand: scripts/firmware/prepare_firmware.sh"
    echo "--install <C2KNXS2.UPD>, a USB image (README.md), then ./start.sh (or just ./setup.sh)"
fi
