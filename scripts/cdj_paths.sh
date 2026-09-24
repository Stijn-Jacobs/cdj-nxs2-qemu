# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced, not run. Sets CDJ_ROOT: the directory that holds the user-supplied
# data (extract/: firmware images, USB image) and the QEMU source tree
# qemu-src/. That is the directory above scripts/, or one level further up
# when emulator/ sits inside a larger working tree. CDJ_ROOT in the environment
# wins.
if [ -z "${CDJ_ROOT:-}" ]; then
    CDJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    if [ -d "$CDJ_ROOT/../lab" ] && [ -d "$CDJ_ROOT/../emulator" ]; then
        CDJ_ROOT="$(cd "$CDJ_ROOT/.." && pwd)"
    fi
fi
export CDJ_ROOT
