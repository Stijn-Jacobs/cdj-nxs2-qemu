#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the rig's QEMUs natively on Windows under MSYS2/mingw64. This only sets
# what differs on this host and then runs the same build scripts as Linux.
#
#   PATH        mingw64 first, so gcc/pkg-config/meson/ninja build a native
#               binary rather than one needing msys-2.0.dll.
#   QEMU_BUILD  must be on the same drive as the source (QEMU's tracetool uses
#               os.path.relpath, which raises across drives on Windows) and on
#               NTFS (meson's postconf step symlinks the install tree, which
#               exFAT cannot hold; the error text suggests Developer Mode, which
#               is not the cause). The two trees take about 2 GB.
#   SKIP_PATCH_REGEN
#               on by default, so a Windows build does not rewrite patches/.
#   usage: scripts/build/build_windows.sh [main|eb|both]        (default: both)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WHICH="${1:-both}"

export PATH="/mingw64/bin:$PATH"
export QEMU_BUILD="${QEMU_BUILD:-/c/qemu-build-mingw}"
export QEMU_EB_BUILD="${QEMU_EB_BUILD:-/c/qemu-build-mingw-eb}"
export SKIP_PATCH_REGEN="${SKIP_PATCH_REGEN:-1}"

missing=""
for t in gcc ninja meson pkg-config flex bison python; do
    command -v "$t" >/dev/null || missing="$missing $t"
done
if [ -n "$missing" ]; then
    cat >&2 <<EOF
missing build tools:$missing

install them from an MSYS2 shell:
  pacman -S --needed mingw-w64-x86_64-{gcc,glib2,pkgconf,ninja,meson,SDL2,gtk3,python,pixman,zlib} \\
      bison flex make diffutils patch
EOF
    exit 1
fi

echo "gcc:      $(gcc -dumpversion)  ($(command -v gcc))"
echo "glib:     $(pkg-config --modversion glib-2.0)"
echo "gtk3:     $(pkg-config --modversion gtk+-3.0 2>/dev/null || echo 'ABSENT -- no window')"
echo "build:    $QEMU_BUILD  (eb: $QEMU_EB_BUILD)"
df -h "$(dirname "$QEMU_BUILD")" | tail -1

# A cross-drive build otherwise dies deep in the build with a ValueError from
# tracetool.
SRC_DRIVE="$(cd "$HERE/../.." && pwd | cut -c1-3)"
for d in "$QEMU_BUILD" "$QEMU_EB_BUILD"; do
    case "$d" in
        "$SRC_DRIVE"*) ;;
        *) echo "build dir $d is not on the source drive $SRC_DRIVE --" >&2
           echo "QEMU's tracetool cannot relpath across drives on Windows." >&2
           exit 1 ;;
    esac
done
echo

case "$WHICH" in
    main) bash "$HERE/build_main.sh" ;;
    eb)   bash "$HERE/build_display.sh" ;;
    both) bash "$HERE/build_main.sh" && bash "$HERE/build_display.sh" ;;
    *)    echo "usage: $0 [main|eb|both]" >&2; exit 2 ;;
esac
