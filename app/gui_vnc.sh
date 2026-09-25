#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The display board's QEMU with its screen on VNC, for the virtual deck app.
# `./start.sh --app` sets GUI_QEMU to this script; boot_deck.sh then runs it
# with the arguments it would give the real binary, and this runs that binary
# with `-display` replaced by a VNC server on 127.0.0.1, port
# CDJ_APP_VNC_BASE + <deck number> (5921 for show1, 5922 for show2).
#
# Why a wrapper and not GUI_DISPLAY=vnc=...: boot_deck.sh appends
# ",show-cursor=on" to every display but "none", which a VNC display rejects,
# and one GUI_DISPLAY value cannot give two decks two ports.
#
# The server has no password and listens on loopback only, like the rig's
# other sockets. CDJ_APP_FRAME_DIR (a native path; start.sh sets it) adds the
# frame file.
set -uo pipefail
EXESUF=""
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) EXESUF=".exe" ;;
esac
REAL="${CDJ_APP_GUI_QEMU:-${QEMU_EB_BUILD:-$HOME/qemu-build-eb}/qemu-system-sh4eb$EXESUF}"
BASE="${CDJ_APP_VNC_BASE:-5920}"

# The deck's tag is in the SPI link socket's name: .../cdj-<tag>-spi.sock.
TAG=""
for a in "$@"; do
    case "$a" in
        *id=spilink,path=*)
            s="${a##*/}"; s="${s#cdj-}"; TAG="${s%-spi.sock}" ;;
    esac
done
N="${TAG: -1}"
case "$N" in [0-9]) ;; *) N=1 ;; esac
PORT=$((BASE + N))

ARGS=()
while [ $# -gt 0 ]; do
    if [ "$1" = -display ] && [ $# -ge 2 ]; then
        ARGS+=(-display "vnc=127.0.0.1:$((PORT - 5900))")
        shift 2
        continue
    fi
    ARGS+=("$1")
    shift
done
# The screen itself goes to a frame file the app reads at the firmware's own
# frame rate (CDJ_GUI_FRAME_FILE, sh7269gui.c); VNC then carries the touch
# screen and the keyboard, and is the screen's fallback on an older build.
if [ -n "${CDJ_APP_FRAME_DIR:-}" ]; then
    export CDJ_GUI_FRAME_FILE="$CDJ_APP_FRAME_DIR/cdj-lcd-${TAG:-deck$N}.bin"
fi
echo "[${TAG:-deck}] screen on VNC 127.0.0.1:$PORT${CDJ_GUI_FRAME_FILE:+ and $CDJ_GUI_FRAME_FILE} for the virtual deck app" >&2
exec "$REAL" "${ARGS[@]}"
