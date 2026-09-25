#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Start the decks the way ./setup.sh configured them (cdj.conf): one or two
# CDJ-2000NXS2 windows, the controller relay and, if you chose a MIDI
# controller, the bridge. Ctrl-C stops everything.
#
#   usage: ./start.sh             start
#          ./start.sh stop        stop a running rig from another shell
#          ./start.sh --dry-run   show what would be started
#   env:   every knob of scripts/run/rig.sh still works (AUDIODEV=, NOSOUND=1,
#          GUI_DISPLAY=, PRIO=, TBFAST=0, ...).
. "$(dirname "${BASH_SOURCE[0]}")/scripts/cdj_bash.sh"
set -uo pipefail
E="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CONF="$E/cdj.conf"
DRY=0
case "${1:-}" in
    stop) exec bash "$E/scripts/run/stop_rig.sh" ;;
    --dry-run) DRY=1 ;;
    -h | --help) sed -n '/^#   usage:/,/^#          GUI_DISPLAY/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
    "") ;;
    *) echo "unknown argument: $1 (./start.sh --help)" >&2; exit 2 ;;
esac

if [ ! -f "$CONF" ]; then
    echo "no cdj.conf yet -- run ./setup.sh first (it builds, prepares the firmware" >&2
    echo "and the USB stick, and asks how you want the decks set up)." >&2
    exit 1
fi
CDJ_DECKS=1; CDJ_NAME=show; CDJ_DJLINK=0; CDJ_AUDIO=1; CDJ_CONTROLLER=none
CDJ_RELAY_PORT=7202; CDJ_GROUP=239.77.77.1:45000; CDJ_MIDI_PYTHON=""
QEMU_BUILD=""; QEMU_EB_BUILD=""
# shellcheck source=/dev/null
. "$CONF"

[ -n "$QEMU_BUILD" ] && export QEMU_BUILD
[ -n "$QEMU_EB_BUILD" ] && export QEMU_EB_BUILD
export RELAY_PORT="$CDJ_RELAY_PORT" DJLINK="$CDJ_DJLINK" GROUP="$CDJ_GROUP"
[ "$CDJ_AUDIO" = 1 ] || export NOSOUND=1
# One frame of 24 hours: the rig lives until Ctrl-C and writes nothing per frame.
export FRAMES="${FRAMES:-1}" MOTION_MS="${MOTION_MS:-86400000}" AUTOLOAD="${AUTOLOAD:-0}"
if [ "$CDJ_DECKS" = 2 ]; then
    LAUNCH=("$E/scripts/run/live_linked.sh" "$CDJ_NAME" 2)
else
    LAUNCH=("$E/scripts/run/live.sh" "$CDJ_NAME" 1)
fi

. "$E/scripts/cdj_paths.sh"

# The boards and patches are compiled into the QEMU binaries, so after a pull
# that changed them the decks would run the old code. build.sh stamps each build
# tree with a fingerprint of those sources; a mismatch means rebuild.
# STALE_CHECK=0 skips the check.
if [ "${STALE_CHECK:-1}" = 1 ]; then
    . "$E/scripts/build/source_stamp.sh"
    CDJ_EMU_DIR="$E"
    stale="$(cdj_stale_builds)"
    if [ -n "$stale" ]; then
        echo "the emulator's source has changed since it was last built:"
        printf '%s\n' "$stale" | sed 's/^/    /'
        if [ "$DRY" = 0 ] && [ -t 0 ]; then
            read -r -p "rebuild now (a few minutes)? [Y/n] " ans
            case "${ans:-y}" in
                [Yy]*) bash "$E/build.sh" main display || exit 1 ;;
                *) echo "starting the old build (./build.sh main display rebuilds it)" ;;
            esac
        else
            echo "run ./build.sh main display to pick the changes up"
        fi
    fi
fi
missing=""
for f in extract/main_unpacked.bin extract/gui_unpacked.bin extract/flash.bin; do
    [ -f "$CDJ_ROOT/$f" ] || missing="$missing $f"
done
if [ -n "$missing" ]; then
    echo "missing firmware images:$missing -- run ./setup.sh --firmware <C2KNXS2.UPD>" >&2
    exit 1
fi
if [ "${MEDIA_MODE:-img}" = img ] && [ -z "${MEDIA_IMG_SRC:-}" ] && [ ! -f "$CDJ_ROOT/extract/usbmedia3.img" ]; then
    echo "no USB image yet -- run ./setup.sh --music <your rekordbox USB folder>" >&2
    exit 1
fi

# The bridge runs on the Python setup.sh recorded: on Windows a native one, as
# an absolute path that may contain spaces; elsewhere possibly a command.
BRIDGE=()
if [ "$CDJ_CONTROLLER" != none ]; then
    if [ -f "$CDJ_MIDI_PYTHON" ]; then
        BRIDGE_PY=("$CDJ_MIDI_PYTHON")
    else
        read -r -a BRIDGE_PY <<< "$CDJ_MIDI_PYTHON"
    fi
    if [ "${#BRIDGE_PY[@]}" -eq 0 ] || ! command -v "${BRIDGE_PY[0]}" >/dev/null 2>&1; then
        echo "controller '$CDJ_CONTROLLER' is configured but the Python with mido +"
        echo "python-rtmidi (${CDJ_MIDI_PYTHON:-none recorded}) is not there; starting"
        echo "without it (install them, then ./setup.sh)"
    else
        BRIDGE=("${BRIDGE_PY[@]}" -u "$E/midi/bridge.py" --controller "$CDJ_CONTROLLER"
                --relay "127.0.0.1:$CDJ_RELAY_PORT" --prefix "$CDJ_NAME")
    fi
fi

echo "decks: $CDJ_DECKS ($CDJ_NAME)   Pro DJ Link: $([ "$DJLINK" = 1 ] && echo "on $GROUP" || echo off)   sound: $([ "$CDJ_AUDIO" = 1 ] && echo on || echo off)   controller: $CDJ_CONTROLLER"
if [ "$DRY" = 1 ]; then
    echo "would run:  RELAY_PORT=$RELAY_PORT DJLINK=$DJLINK GROUP=$GROUP${NOSOUND:+ NOSOUND=1}${QEMU_BUILD:+ QEMU_BUILD=$QEMU_BUILD} bash ${LAUNCH[*]}"
    [ "${#BRIDGE[@]}" -gt 0 ] && echo "and:        $(printf '%q ' "${BRIDGE[@]}") > logs/bridge.log"
    exit 0
fi

BRIDGE_PID=""
if [ "${#BRIDGE[@]}" -gt 0 ]; then
    mkdir -p "$E/logs"
    # The bridge reconnects on its own, so it can start before the relay is up.
    "${BRIDGE[@]}" > "$E/logs/bridge.log" 2>&1 &
    BRIDGE_PID=$!
    echo "controller bridge running (log: logs/bridge.log)"
fi
trap '[ -n "$BRIDGE_PID" ] && kill "$BRIDGE_PID" 2>/dev/null' EXIT
echo "the first boot takes a minute; then press USB (or LINK) to browse, load a track and play."
bash "${LAUNCH[@]}"
