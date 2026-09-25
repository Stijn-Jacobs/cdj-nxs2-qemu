# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced by build.sh and start.sh: a fingerprint of the sources compiled into
# the two QEMUs (the boards in hw/cdj/ and the QEMU patches), stamped into each
# build tree, so start.sh can tell a binary that predates a `git pull`.
#
#   cdj_build_dirs          sets CDJ_MAIN_BUILD / CDJ_EB_BUILD / CDJ_EXE
#   cdj_source_stamp        prints the fingerprint of <emulator dir>
#   cdj_stamp_write <dir>   records it in a build tree
#   cdj_stale_builds        prints the build trees whose stamp differs

cdj_build_dirs() {
    case "$(uname -s)" in
        MINGW* | MSYS* | CYGWIN*)
            CDJ_MAIN_BUILD="${QEMU_BUILD:-/c/qemu-build-mingw}"
            CDJ_EB_BUILD="${QEMU_EB_BUILD:-/c/qemu-build-mingw-eb}"
            CDJ_EXE=".exe" ;;
        *)
            CDJ_MAIN_BUILD="${QEMU_BUILD:-$HOME/qemu-build}"
            CDJ_EB_BUILD="${QEMU_EB_BUILD:-$HOME/qemu-build-eb}"
            CDJ_EXE="" ;;
    esac
}

# One cksum process over every file: a process per file takes a minute on
# Windows. Paths are relative, so the stamp does not depend on the checkout.
cdj_source_stamp() {
    ( cd "$1" && find hw/cdj patches -type f ! -path '*/__pycache__/*' ! -name '*.pyc' -print0 |
        LC_ALL=C sort -z | xargs -0 cksum | cksum | cut -d' ' -f1 )
}

CDJ_STAMP_NAME="cdj-source.stamp"

cdj_stamp_write() {
    cdj_source_stamp "$CDJ_EMU_DIR" > "$1/$CDJ_STAMP_NAME"
}

# A tree with a binary but no stamp was built before stamping existed; its
# sources are judged by date instead.
cdj_stale_builds() {
    local now dir bin
    cdj_build_dirs
    now="$(cdj_source_stamp "$CDJ_EMU_DIR")"
    for dir in "$CDJ_MAIN_BUILD:qemu-system-sh4" "$CDJ_EB_BUILD:qemu-system-sh4eb"; do
        bin="${dir#*:}$CDJ_EXE"; dir="${dir%:*}"
        [ -f "$dir/$bin" ] || continue
        if [ -f "$dir/$CDJ_STAMP_NAME" ]; then
            [ "$(cat "$dir/$CDJ_STAMP_NAME")" = "$now" ] || echo "$dir"
        elif [ -n "$(find "$CDJ_EMU_DIR/hw/cdj" "$CDJ_EMU_DIR/patches" -type f -newer "$dir/$bin" -print 2>/dev/null | head -1)" ]; then
            echo "$dir"
        fi
    done
}
