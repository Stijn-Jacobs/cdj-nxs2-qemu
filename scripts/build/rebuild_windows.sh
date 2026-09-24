#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Rebuild both Windows QEMUs and log the whole build, then grep the log for
# errors (piping a build through `tail` can cut off the error).
#
#   usage (from MSYS2/mingw64 bash):  bash scripts/build/rebuild_windows.sh
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG="${LOG:-/tmp/cdj-winbuild.log}"

echo "=== rebuilding both Windows QEMUs -> $LOG"
bash "$HERE/build_windows.sh" both > "$LOG" 2>&1
rc=$?
echo "exit=$rc"
echo "--- errors, if any:"
grep -n -i 'error:\|FAILED\|ValueError\|Postconf' "$LOG" | head -20 || echo "  (none)"
echo "--- binaries:"
ls -la /c/qemu-build-mingw/qemu-system-sh4.exe /c/qemu-build-mingw-eb/qemu-system-sh4eb.exe 2>&1
exit $rc
