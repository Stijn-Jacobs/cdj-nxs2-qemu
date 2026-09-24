#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Boot N decks in parallel, each driven to a loaded track by load_track.py, and
# print each run's summary lines.
#
#   usage: ./scripts/run/boot_decks.sh <tag-prefix> [n]
#   env:   JOBS FILMN MOTION_MS WALK KEYBYTE KEYBITS plus any CDJ_* knob
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../cdj_paths.sh"
PREFIX="${1:?usage: boot_decks.sh <tag-prefix> [n]}"
N="${2:-3}"
JOBS="${JOBS:-3}"

export CDJ_ATA=1
export CDJ_IIC_CH=both
export CDJ_IIC_ADDR=0x30,0x2c,0x10
# The DSP reply knobs are defaults only, so a caller's own values win.
export CDJ_DSP_REPLY="${CDJ_DSP_REPLY:-1}"
export CDJ_DSP_REPLY_ID="${CDJ_DSP_REPLY_ID:-0}"
export CDJ_DSP_REPLY_WORDS="${CDJ_DSP_REPLY_WORDS:-4}"
export CDJ_DSP_TAG="${CDJ_DSP_TAG:-1}"
export DRIVER="$HERE/load_track.py"
export GUI_DISPLAY="${GUI_DISPLAY:-gtk}"
export FILMN="${FILMN:-5}" MOTION_MS="${MOTION_MS:-1800}"

# PRIVATE_MEDIA=1: each concurrent run gets its own copy of the USB medium,
# since separate vvfat models writing one directory corrupt each other.
# MEDIA_MODE=img already gives each run its own image, so it defaults to 0 there.
PRIVATE_MEDIA="${PRIVATE_MEDIA:-$([ "${MEDIA_MODE:-rw}" = img ] && echo 0 || echo 1)}"
SRC_MEDIA="${MEDIADIR:-$CDJ_ROOT/extract/usbmedia3}"

# A warm-up wave that is thrown away: the first $JOBS runs of a batch tend to
# fail with the panel key socket refusing connections. WARMUP=0 skips it.
if [ "${WARMUP:-1}" = "1" ]; then
    for w in $(seq 1 "$JOBS"); do
        TAG="${PREFIX}w$w"
        mkdir -p "/tmp/$TAG"
        RUN_MEDIA="$SRC_MEDIA"
        if [ "$PRIVATE_MEDIA" = "1" ]; then
            RUN_MEDIA="/tmp/media-$TAG"
            rm -rf "$RUN_MEDIA"
            cp -a "$SRC_MEDIA" "$RUN_MEDIA"
        fi
        ( MEDIADIR="$RUN_MEDIA" SHOTDIR="/tmp/$TAG"           bash "$HERE/boot_deck.sh" "$TAG" 2             > "/tmp/run-$TAG.txt" 2>&1 ) &
    done
    wait
    rm -rf "/tmp/${PREFIX}w"* "/tmp/media-${PREFIX}w"* 2>/dev/null
fi

# Read the big inputs once so the first runs do not start while the host is
# still faulting them in.
for f in "${QEMU:-$HOME/qemu-build/qemu-system-sh4}" \
         "${GUI_QEMU:-$HOME/qemu-build-eb/qemu-system-sh4eb}" \
         "$CDJ_ROOT/extract/main_unpacked.bin" \
         "$CDJ_ROOT/extract/gui_unpacked.bin"; do
    [ -f "$f" ] && cat "$f" > /dev/null 2>&1
done

for i in $(seq 1 "$N"); do
    TAG="$PREFIX$i"
    mkdir -p "/tmp/$TAG"
    RUN_MEDIA="$SRC_MEDIA"
    if [ "$PRIVATE_MEDIA" = "1" ]; then
        RUN_MEDIA="/tmp/media-$TAG"
        rm -rf "$RUN_MEDIA"
        cp -a "$SRC_MEDIA" "$RUN_MEDIA"
    fi
    ( MEDIADIR="$RUN_MEDIA" SHOTDIR="/tmp/$TAG" \
      bash "$HERE/boot_deck.sh" "$TAG" 2 \
        > "/tmp/run-$TAG.txt" 2>&1 ) &
    # LAUNCH_STAGGER (s): spread the launches so the QEMUs do not all race to
    # bind their sockets at once.
    sleep "${LAUNCH_STAGGER:-4}"
    while [ "$(jobs -rp | wc -l)" -ge "$JOBS" ]; do sleep 0.3; done
done
wait

for i in $(seq 1 "$N"); do
    echo "=== $PREFIX$i"
    grep -E '^(byte|  bit|\[)' "/tmp/run-$PREFIX$i.txt"
    grep -o 'iic0: .*rx=[0-9]*' "/tmp/bridge-main-$PREFIX$i.log" | tail -1
done
