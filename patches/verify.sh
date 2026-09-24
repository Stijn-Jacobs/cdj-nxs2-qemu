#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Check that these patches describe the qemu-src/ tree.
#
# Each patch must apply in reverse to the live tree, which succeeds only if the
# tree already contains exactly what the patch adds. The whole-file sources are
# compared as well, since they are copied rather than patched.
#
#   usage: ./patches/verify.sh [path-to-qemu-src]
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../scripts/cdj_paths.sh"; ROOT="$CDJ_ROOT"
SRC="${1:-$ROOT/qemu-src}"
FAIL=0

[ -d "$SRC/hw/sh4" ] || { echo "no QEMU source at $SRC" >&2; exit 1; }

for p in "$HERE"/*.patch; do
    [ -s "$p" ] || { echo "EMPTY    $(basename "$p")" >&2; FAIL=1; continue; }
    if git -C "$SRC" apply --check --reverse --ignore-whitespace "$p" 2>/dev/null; then
        echo "ok       $(basename "$p")"
    else
        echo "DRIFTED  $(basename "$p") -- regenerate with ./patches/regen.sh" >&2
        FAIL=1
    fi
done

# The board sources in hw/cdj/ are copied into hw/sh4/cdj/ by
# install_machine.sh; cdj-pcring.h lives here and is copied by apply.sh.
check_copy() {
    if diff -q --strip-trailing-cr "$1" "$2" >/dev/null 2>&1; then
        echo "ok       $(basename "$2") (whole file)"
    else
        echo "DRIFTED  $(basename "$2") -- $1 differs from the built tree" >&2
        FAIL=1
    fi
}
CDJ="$SRC/hw/sh4/cdj"
# Every file of the board tree, including the GUI board and shared headers.
while IFS= read -r f; do
    check_copy "$f" "$CDJ/${f#$HERE/../hw/cdj/}"
done < <(find "$HERE/../hw/cdj" -path "$HERE/../hw/cdj/c6x" -prune -o -type f \( -name '*.[ch]' -o -name meson.build \) -print | sort)
check_copy "$HERE/include_exec_cdj-pcring.h"   "$SRC/include/exec/cdj-pcring.h"
# The C6655 DSP core, installed as whole files like the board.
for f in "$HERE"/../hw/cdj/c6x/c66x_core.c "$HERE"/../hw/cdj/c6x/c66x_decode.[ch]          "$HERE"/../hw/cdj/c6x/c66x.h "$HERE"/../hw/cdj/c6x/c66x_ext_table.h          "$HERE"/../hw/cdj/c6x/soc_*.[ch] "$HERE"/../hw/cdj/c6x/binutils/*.h; do
    check_copy "$f" "$CDJ/c6x/${f#$HERE/../hw/cdj/c6x/}"
done
# A copy left by the old flat layout would not be built but would confuse
# anyone grepping qemu-src/hw/sh4.
for f in cdj2000nxs2.c sh7269gui.c cdj_panelkeys.h minimp3.h c6x; do
    [ -e "$SRC/hw/sh4/$f" ] && { echo "STALE    hw/sh4/$f -- rerun install_machine.sh" >&2; FAIL=1; }
done

echo
[ "$FAIL" = 0 ] && echo "tree and patches agree." || echo "MISMATCH -- see above." >&2
exit "$FAIL"
