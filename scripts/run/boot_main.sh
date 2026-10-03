#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Boot a model's MAIN board alone, for bring-up: no GUI board, no link, no
# media. Every access to a region the board does not model yet is logged
# (-d unimp), which is the list of what to model next. The board runs for
# [seconds] and quits through its monitor so the exit counters print.
#
#   usage: CDJ_MODEL=<id> ./scripts/run/boot_main.sh <tag> [seconds]
#
#   CDJ_MODEL   a profile in models/ (default cdj2000nxs2)
#   MAIN_QEMU   the binary; default follows the build's QEMU_BUILD
#   LOGDIR      where <tag>.main.log goes (default /tmp)
#   QEMU_D      the -d list (default unimp,guest_errors)
#   ICOUNT      -icount value, for a run that repeats instruction for instruction
#   MAIN_ARGS   extra qemu options, split on spaces (a USB stick, for one)
#   FLASH       a NOR flash image (default none: the flash reads erased)
#   MAIN_BOOT   flash: reset into the bootloader in FLASH instead of loading
#               main_unpacked.bin (the default, kernel)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../cdj_paths.sh"; PROJ="$CDJ_ROOT"
. "$HERE/../cdj_model.sh"; cdj_model_load || exit 1
TAG="${1:?usage: CDJ_MODEL=<id> boot_main.sh <tag> [seconds]}"
DUR="${2:-30}"

EXESUF=""
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) EXESUF=".exe" ;;
esac
MAIN_QEMU="${MAIN_QEMU:-${QEMU_BUILD:-$HOME/qemu-build}/qemu-system-sh4$EXESUF}"
LOGDIR="${LOGDIR:-/tmp}"
MAINLOG="$LOGDIR/$TAG.main.log"
IMAGE="$PROJ/$MODEL_EXTRACT/main_unpacked.bin"
[ -f "$IMAGE" ] || { echo "no $IMAGE -- run scripts/firmware/prepare_firmware.sh --model $MODEL_ID --install <update>" >&2; exit 1; }
# QEMU is a native program on Windows and cannot open MSYS2 paths (/c/...).
if command -v cygpath >/dev/null 2>&1; then
    IMAGE="$(cygpath -m "$IMAGE")"
    [ -n "${FLASH:-}" ] && FLASH="$(cygpath -m "$FLASH")"
fi
BOOT_ARGS=(-kernel "$IMAGE")
[ "${MAIN_BOOT:-kernel}" = "flash" ] && BOOT_ARGS=()
[ -n "${FLASH:-}" ] && BOOT_ARGS+=(-drive "if=pflash,format=raw,file=$FLASH,snapshot=on")

# On Windows kill is TerminateProcess, which runs no exit handlers, so the
# board quits through a monitor socket (cdj_monsock.py picks the transport).
MAINMON="/tmp/cdj-$TAG-main-mon.sock"
rm -f "$MAINMON" 2>/dev/null
"$MAIN_QEMU" -M "$MODEL_MAIN_MACHINE" "${BOOT_ARGS[@]}" \
    -nographic -d "${QEMU_D:-unimp,guest_errors}" ${ICOUNT:+-icount "$ICOUNT"} ${MAIN_ARGS:-} \
    -monitor "$(python3 "$HERE/cdj_monsock.py" spec "$MAINMON")" \
    > "$MAINLOG" 2>&1 &
MAIN_PID=$!
echo "[$TAG] $MODEL_TITLE MAIN ($MODEL_MAIN_MACHINE) pid $MAIN_PID, ${DUR}s, log $MAINLOG"

sleep "$DUR"
python3 "$HERE/cdj_monsock.py" cmd "$MAINMON" quit > /dev/null 2>&1
for _ in $(seq 1 60); do kill -0 "$MAIN_PID" 2>/dev/null || break; sleep 0.5; done
kill -TERM "$MAIN_PID" 2>/dev/null
wait "$MAIN_PID" 2>/dev/null
echo "[$TAG] done; unmodelled regions touched:"
grep -a -oE '^[a-z0-9_.-]+: unimplemented device (read|write)' "$MAINLOG" \
    | sort | uniq -c | sort -rn | head -20
