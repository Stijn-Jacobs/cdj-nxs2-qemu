#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Boot one deck: the MAIN board and the GUI board joined by the SPI link, with
# no gdbstub attached (a gdbstub client halts MAIN and starves the link). The
# boards run for [seconds] and are stopped with SIGTERM so their exit counters
# print.
#
#   usage: ./scripts/run/boot_deck.sh <tag> [seconds]
#   env:   SERVICE=1 boots into the service manual's SERVICE MODE screen
#          instead of the player (SERVICE_HOLD_MS overrides how long the entry
#          keys are held, default 20000)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../cdj_paths.sh"; PROJ="$CDJ_ROOT"
# The model (CDJ_MODEL, default cdj2000nxs2) names both machines and the folder
# its images are in. This launcher needs a GUI board; a model still in
# bring-up boots its MAIN board alone with boot_main.sh.
. "$HERE/../cdj_model.sh"; cdj_model_load || exit 1
if [ -z "${MODEL_GUI_MACHINE:-}" ]; then
    echo "boot_deck.sh: $MODEL_TITLE has no GUI board yet; use scripts/run/boot_main.sh" >&2
    exit 1
fi
TAG="${1:?usage: boot_deck.sh <tag> [seconds]}"
DUR="${2:-60}"

# MAIN_QEMU / GUI_QEMU override the binaries; the defaults follow the build
# scripts' QEMU_BUILD / QEMU_EB_BUILD, with .exe on Windows.
EXESUF=""
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) EXESUF=".exe" ;;
esac
MAIN_QEMU="${MAIN_QEMU:-${QEMU_BUILD:-$HOME/qemu-build}/qemu-system-sh4$EXESUF}"
GUI_QEMU="${GUI_QEMU:-${QEMU_EB_BUILD:-$HOME/qemu-build-eb}/qemu-system-sh4eb$EXESUF}"
LOGDIR="${LOGDIR:-/tmp}"
MEDIADIR="${MEDIADIR:-$PROJ/extract/usbmedia3}"
# QEMU is a native program on Windows and cannot open MSYS2 paths (/c/...), so
# every path passed to it goes through cygpath -m where that exists.
if command -v cygpath >/dev/null 2>&1; then
    MEDIADIR="$(cygpath -m "$MEDIADIR")"
    PROJ_NATIVE="$(cygpath -m "$PROJ")"
else
    PROJ_NATIVE="$PROJ"
fi
SOCK="/tmp/cdj-$TAG-spi.sock"

# A dump path is per process, and a batch runs several QEMUs at once, so any
# CDJ_* path knob may contain %TAG%, expanded here to this run's tag. Exported
# only when non-empty: the C side treats an empty string as set.
if [ -n "${CDJ_DSP_TXDUMP-}" ]; then
    export CDJ_DSP_TXDUMP="${CDJ_DSP_TXDUMP//%TAG%/$TAG}"
fi
# The SPI wire dump. Not passive: it changes boot timing, so use it for a
# presence/absence question, never for a rate.
if [ -n "${CDJ_SPILINK_DUMP-}" ]; then
    export CDJ_SPILINK_DUMP="${CDJ_SPILINK_DUMP//%TAG%/$TAG}"
fi
# The DSP PCM capture opens with "wb"; without %TAG% a later run truncates it.
if [ -n "${CDJ_C6X_PCM-}" ]; then
    export CDJ_C6X_PCM="${CDJ_C6X_PCM//%TAG%/$TAG}"
fi
if [ -n "${CDJ_C6X_RECORD-}" ]; then
    export CDJ_C6X_RECORD="${CDJ_C6X_RECORD//%TAG%/$TAG}"
fi
# %N% is this deck's number, the last digit of the tag, so one exported value
# can give each deck its own address, e.g. CDJ_ETHER_MAC=02:00:00:00:00:0%N%.
# Only the last digit: "0%N%" must stay two hex characters.
DECK_N="${TAG: -1}"
case "$DECK_N" in [0-9]) ;; *) DECK_N=1 ;; esac
if [ -n "${CDJ_ETHER_MAC-}" ]; then
    export CDJ_ETHER_MAC="${CDJ_ETHER_MAC//%N%/$DECK_N}"
fi
# CDJ_ETHER_MAC rewrites the Ethernet header only. The firmware keeps its own
# MAC at 0x0A35F754 (six bytes, read through the getter 0x08232104 by the
# MAHR/MALR writer and the Pro DJ Link announce builder). Its backing storage is
# blank in the dump, so every instance is 00:00:00:00:00:01, and two decks with
# the same address never settle the device-number claim.
#
# OWNMAC=<byte>: hold the last byte to this value (default: the deck number);
# OWNMAC=0 leaves the firmware's address alone. With CDJ_ETHER_MAC set and
# OWNMAC unset, the whole address is held to the wire MAC. Only non-zero bytes
# and byte 5 are poked, because CDJ_MPOKE has 8 slots.
if [ -n "${CDJ_NETDEV-}" ] && [ "${OWNMAC:-$DECK_N}" != "0" ]; then
    if [ -z "${OWNMAC-}" ] && [ -n "${CDJ_ETHER_MAC-}" ]; then
        _i=0
        for _b in ${CDJ_ETHER_MAC//:/ }; do
            if [ "$_i" = 5 ] || [ "$((16#$_b))" != 0 ]; then
                export CDJ_MPOKE="${CDJ_MPOKE:+$CDJ_MPOKE,}$(printf '0x%08X' $((0x0A35F754 + _i))):0x$_b:1"
            fi
            _i=$((_i + 1))
        done
        echo "[$TAG] own MAC 0x0A35F754 held to the wire address $CDJ_ETHER_MAC (OWNMAC=0 to leave it)"
    else
        _om="${OWNMAC:-$DECK_N}"
        export CDJ_MPOKE="${CDJ_MPOKE:+$CDJ_MPOKE,}0x0A35F759:$_om:1"
        echo "[$TAG] own MAC byte 0x0A35F759 held at $_om (OWNMAC=0 to leave it)"
    fi
fi
if [ -n "${CDJ_AUDIODEV-}" ]; then
    export CDJ_AUDIODEV="${CDJ_AUDIODEV//%TAG%/$TAG}"
fi
# The QEMU monitor is not a gdbstub: screendumps go through it without
# stopping the machine. No -gdb here on purpose.
# GUI_DISPLAY: gtk (default; cocoa on macOS) shows the panel; none for a
# headless batch.
nativepath_or_self() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi
}

MON="/tmp/cdj-$TAG-gui-mon.sock"

# PERSIST=1: this deck keeps its own settings in extract/flash-<tag>.bin.
# The default is the shared image with snapshot=on, so no run can drift.
#
# PLAYERNO=<1..4|%N%>: boot with PLAYER No. already set, via
# scripts/firmware/player_flash.py. Two decks both left at 1 deadlock the Pro DJ
# Link device-number negotiation. Without PERSIST the image is minted into /tmp
# each run; with PERSIST it only seeds a new per-deck image.
PLAYERNO="${PLAYERNO:-}"
PLAYERNO="${PLAYERNO//%N%/$DECK_N}"
# SERIAL=<12 chars|auto>: the deck's serial; the firmware gives every instance
# PDJ0000001XX. "auto" derives it from the player number, empty keeps it.
SERIAL="${SERIAL:-}"
SERIAL="${SERIAL//%N%/$DECK_N}"
mint_flash() {  # <out>
    python3 "$HERE/../firmware/player_flash.py" "$PLAYERNO" "$1" "$PROJ/extract/flash.bin" \
        ${SERIAL:+--serial "$SERIAL"} | sed "s/^/[$TAG] /"
}
FLASH_FILE="$PROJ_NATIVE/extract/flash.bin"
FLASH_SNAP="snapshot=on"
if [ "${PERSIST:-0}" = "1" ]; then
    _fl="$PROJ/extract/flash-$TAG.bin"
    if [ ! -f "$_fl" ]; then
        if [ -n "$PLAYERNO" ]; then
            mint_flash "$_fl"
        else
            cp "$PROJ/extract/flash.bin" "$_fl"
        fi
        echo "[$TAG] made $_fl -- this deck now keeps its own settings"
    fi
    FLASH_FILE="$(nativepath_or_self "$_fl")"
    FLASH_SNAP="snapshot=off"
elif [ -n "$PLAYERNO" ]; then
    _fl="/tmp/flash-$TAG.bin"
    mint_flash "$_fl" || exit 1
    FLASH_FILE="$(nativepath_or_self "$_fl")"
fi

MON_ARG="$(python3 "$HERE/cdj_monsock.py" spec "$MON")"
# A leftover QEMU on the same tag still holds the monitor address.
python3 "$HERE/cdj_monsock.py" free "$MON" 30 \
    || echo "[$TAG] ⚠ the GUI monitor address is still held -- an older run of this tag is alive" >&2
MAINLOG="$LOGDIR/bridge-main-$TAG.log"
GUILOG="$LOGDIR/bridge-gui-$TAG.log"

rm -f "$SOCK" "$MEDIADIR"/tmp*.tmp 2>/dev/null

# MEDIA_MODE: how the USB medium is attached.
#   rw  (default) fat:rw:$MEDIADIR          -- vvfat, guest writes allowed
#   ro            fat:$MEDIADIR,readonly=on -- guest writes refused
#   img           a real FAT16 image (extract/usbmedia3.img, scripts/media/make_usb_image.py)
#                 copied per run; avoids vvfat committing the tree on every write
MEDIA_MODE="${MEDIA_MODE:-rw}"
case "$MEDIA_MODE" in
    ro)  MEDIA_DRIVE="format=raw,file=fat:$MEDIADIR,readonly=on" ;;
    img) MEDIA_IMG="/tmp/media-$TAG.img"
         # Cache the source on the Linux disk; /mnt/c is slow.
         MEDIA_SRC="${MEDIA_IMG_SRC:-$PROJ/extract/usbmedia3.img}"
         if [ -z "${MEDIA_IMG_SRC-}" ]; then
             [ /tmp/usbmedia3.img -nt "$MEDIA_SRC" ] || cp "$MEDIA_SRC" /tmp/usbmedia3.img || exit 1
             MEDIA_SRC=/tmp/usbmedia3.img
         fi
         # snapshot=on puts the guest's writes in a throwaway overlay, which
         # costs nothing, where copying the 256 MB image took 7-15 s a run.
         # MEDIA_COPY=1 goes back to a private copy.
         if [ "${MEDIA_COPY:-0}" = 1 ]; then
             cp "$MEDIA_SRC" "$MEDIA_IMG" || exit 1
             MEDIA_FILE="$MEDIA_IMG"
         else
             MEDIA_FILE="$(nativepath_or_self "$MEDIA_SRC"),snapshot=on"
             MEDIA_IMG=""
         fi
         # MEDIA_CACHE: unsafe ignores the guest's flushes, writeback (default)
         # honours them. The image is a throwaway copy either way.
         MEDIA_DRIVE="format=raw,file=$MEDIA_FILE,cache=${MEDIA_CACHE:-writeback}" ;;
    *)   MEDIA_DRIVE="format=raw,file=fat:rw:$MEDIADIR" ;;
esac
# NOMEDIA=1 attaches no stick. UTILITY refuses to change PLAYER No. while a
# device is mounted, so set it on a bare deck with PERSIST=1.
if [ "${NOMEDIA:-0}" = "1" ]; then
    MEDIA_ARGS=""
    echo "[$TAG] media: NONE (NOMEDIA=1) -- no track will load"
else
    MEDIA_ARGS="-drive if=none,id=usbstick,$MEDIA_DRIVE -device usb-storage,drive=usbstick,port=1"
    echo "[$TAG] media: $MEDIA_MODE ($MEDIA_DRIVE)"
fi
# The image is 256 MB per deck per run. Unlinking it while QEMU has it open is
# safe.
trap 'rm -f "${MEDIA_IMG:-}" 2>/dev/null' EXIT INT TERM

export CDJ_SPILINK_OVERFLOW=1 CDJ_SPILINK_QUEUE=0
export CDJ_SPILINK_DEDUP="${CDJ_SPILINK_DEDUP:-0}"
export SPILINK_KEEP_FRAMES="${SPILINK_KEEP_FRAMES:-64}"
export CDJ_SPILINK_KEEP_FRAMES="${CDJ_SPILINK_KEEP_FRAMES:-64}"
export CDJ_AREA4=1 CDJ_DSP_LINK=1 CDJ_DSP_READY=1 CDJ_DMA1_IEACK=1
# Subsystem 5 (E-7206 AUTH CHIP ERROR) needs IIC0 to answer address 0x10.
export CDJ_IIC_SLAVE=1 CDJ_USB_OC=1
export CDJ_IIC_ADDR="${CDJ_IIC_ADDR:-0x30,0x2c}"
export CDJ_IIC_CH="${CDJ_IIC_CH:-1}"
export CDJ_GUI_VDC_SCANOUT=1 USB_MEDIA=1
# GUI font/art archives, installed by prepare_firmware.sh --install.
export CDJ_GUI_FONTBLOB="$PROJ/extract/resblob.bin"
export CDJ_GUI_ARTBLOB="$PROJ/extract/artblob.bin"
# The front panel is a datagram socket, so keys can be pressed without halting
# MAIN.
export CDJ_PANEL_KEYSOCK="/tmp/cdj-panel-keys-$TAG.sock"
export CDJ_PANEL_RX=1 CDJ_PANEL_MAX_IRQ=4000000
# SERVICE=1: boot into the service manual's SERVICE MODE instead of the
# regular player screen, by holding TEMPO RANGE (report 0x15, mask 0x08) and
# MEMORY (0x0c, mask 0x08) from reset. Both keys release after
# SERVICE_HOLD_MS so nothing stays stuck down once the logo clears. A caller's
# own CDJ_PANEL_PRESS always wins.
if [ "${SERVICE:-0}" = "1" ]; then
    export CDJ_PANEL_PRESS="${CDJ_PANEL_PRESS-0x15:0x08:0:${SERVICE_HOLD_MS:-20000},0x0c:0x08:0:${SERVICE_HOLD_MS:-20000}}"
else
    export CDJ_PANEL_PRESS="${CDJ_PANEL_PRESS-0x13:0x04:20000:3000}"
fi
rm -f "$CDJ_PANEL_KEYSOCK" 2>/dev/null

# ICOUNT: drive MAIN's virtual clock from executed instructions instead of host
# time, which removes host jitter from the firmware's timing. Empty by default;
# the GUI board's clock is not affected.
ICOUNT_ARGS=""
[ -n "${ICOUNT:-}" ] && ICOUNT_ARGS="-icount $ICOUNT"
# MAIN's virtual clock, published for load_track.py so it waits in virtual
# seconds and an -icount run presses keys at the same machine state.
export CDJ_VCLOCK_FILE="/tmp/cdj-$TAG-vclock"
rm -f "$CDJ_VCLOCK_FILE" 2>/dev/null

# MAIN_MON=1: give MAIN a QEMU monitor socket (default off). Unlike a gdbstub
# it never halts the CPU: memsave reads RAM of a running deck.
MAIN_MON_ARGS="-monitor none"
# The JIT profile is written at exit, and on Windows kill runs no exit handlers:
# a profile run needs the monitor so it can quit cleanly below.
[ -n "${C66X_JIT_PROFILE:-}" ] && MAIN_MON=1
if [ "${MAIN_MON:-0}" = "1" ]; then
    MAINMON="/tmp/cdj-$TAG-main-mon.sock"
    rm -f "$MAINMON" 2>/dev/null
    # cdj_monsock.py picks the transport: TCP on Windows, where CPython
    # has no AF_UNIX, a unix socket elsewhere.
    MAIN_MON_ARGS="-monitor $(python3 "$HERE/cdj_monsock.py" spec "$MAINMON")"
fi

# CDJ_AUDIODEV=<spec> gives the model's DSP audio output an audio backend.
# Device selection is QEMU's own, e.g.
#   CDJ_AUDIODEV="pa,id=cdj,server=unix:/mnt/wslg/PulseServer"
#   CDJ_AUDIODEV="coreaudio,id=cdj"                                 (macOS)
#   CDJ_AUDIODEV="wav,id=cdj,path=/tmp/deck.wav"
AUDIO_ARGS=""
# -audio, not -audiodev: the model's sound card is not a qdev device and binds
# to the default audiodev, which only -audio creates.
[ -n "${CDJ_AUDIODEV:-}" ] && AUDIO_ARGS="-audio $CDJ_AUDIODEV"

# CDJ_NETDEV=<spec> adds one -netdev for Pro DJ Link; the EtherMAC's NIC finds
# its backend by id, so decks can share an L2 segment. Empty = no network.
#   CDJ_NETDEV="socket,id=djlink,mcast=230.0.0.1:1234"
NET_ARGS=""
[ -n "${CDJ_NETDEV:-}" ] && NET_ARGS="-netdev $CDJ_NETDEV"

"$MAIN_QEMU" -M "$MODEL_MAIN_MACHINE" -kernel "$PROJ_NATIVE/$MODEL_EXTRACT/main_unpacked.bin" \
    -drive if=pflash,format=raw,file="$FLASH_FILE",$FLASH_SNAP \
    $MEDIA_ARGS \
    -chardev "socket,id=spilink,path=$SOCK,server=on,wait=off" \
    $ICOUNT_ARGS \
    -nographic $AUDIO_ARGS $NET_ARGS $MAIN_MON_ARGS \
    > "$MAINLOG" 2>&1 &
MAIN_PID=$!

# -e, not -S: on Windows the unix socket is a reparse point that MSYS2's
# `test -S` does not recognise.
for _ in $(seq 1 100); do [ -e "$SOCK" ] && break; sleep 0.1; done
[ -e "$SOCK" ] || { echo "[$TAG] spilink socket never appeared" >&2
                    kill -9 "$MAIN_PID" 2>/dev/null; exit 1; }

# Fall back to headless when there is no X or Wayland socket (WSLg can lose its
# X server mid-session, and -display gtk then kills the GUI QEMU). The test is
# on the value, since boot_decks.sh always exports GUI_DISPLAY. Skipped on
# Windows, where GTK talks to Win32 and there is no such socket.
#
# macOS has no GTK build: its window is Cocoa, so the callers' default gtk
# becomes cocoa. A window needs the logged-in desktop session (Aqua); over ssh
# there is none, and the deck runs headless.
case "$(uname -s)" in
MINGW* | MSYS* | CYGWIN*) ;;
Darwin)
    [ "${GUI_DISPLAY:-gtk}" = gtk ] && GUI_DISPLAY=cocoa
    case "$GUI_DISPLAY" in
    cocoa | sdl)
        if [ "$(launchctl managername 2>/dev/null)" != Aqua ]; then
            echo "[$TAG] no desktop session (ssh?) -- falling back to GUI_DISPLAY=none" >&2
            GUI_DISPLAY=none
        fi
        ;;
    esac
    ;;
*)
    case "${GUI_DISPLAY:-gtk}" in
    gtk | sdl)
        if [ -z "$(ls -A /tmp/.X11-unix 2>/dev/null)" ] \
           && [ ! -S "${XDG_RUNTIME_DIR:-/nonexistent}/${WAYLAND_DISPLAY:-wayland-0}" ]; then
            echo "[$TAG] no X or Wayland socket -- falling back to GUI_DISPLAY=none" >&2
            GUI_DISPLAY=none
        fi
        ;;
    esac
    ;;
esac

# The touch screen makes the window an absolute pointer, and QEMU then hides
# the host cursor for a guest that never draws one; show-cursor keeps it.
GUI_DISPLAY_ARG="${GUI_DISPLAY:-gtk}"
case "$GUI_DISPLAY_ARG" in
    none | *show-cursor=*) ;;
    *) GUI_DISPLAY_ARG="$GUI_DISPLAY_ARG,show-cursor=on" ;;
esac
"$GUI_QEMU" -M "$MODEL_GUI_MACHINE" -kernel "$PROJ_NATIVE/$MODEL_EXTRACT/gui_unpacked.bin" \
    -chardev "socket,id=spilink,path=$SOCK" \
    -display "$GUI_DISPLAY_ARG" -serial null \
    -monitor "$MON_ARG" \
    > "$GUILOG" 2>&1 &
GUI_PID=$!

echo "[$TAG] main pid $MAIN_PID  gui pid $GUI_PID  running free for ${DUR}s"

# An optional driver, run while the boards are up. It may press panel keys and
# screendump through the monitor; it must NOT open either gdbstub.
if [ -n "${DRIVER:-}" ]; then
    ( cd "$PROJ" && PYTHONPATH="$HERE" python3 "$DRIVER" "$TAG" )
fi

sleep "$DUR"

# The exit notifiers print the counters and write a JIT profile. SIGTERM runs
# them on Linux; on Windows it is TerminateProcess, so quit through the monitor.
if [ -n "${MAINMON:-}" ]; then
    python3 "$HERE/cdj_monsock.py" cmd "$MAINMON" quit > /dev/null 2>&1
    for _ in $(seq 1 120); do kill -0 "$MAIN_PID" 2>/dev/null || break; sleep 0.5; done
fi
kill -TERM "$MAIN_PID" "$GUI_PID" 2>/dev/null
wait "$MAIN_PID" "$GUI_PID" 2>/dev/null
rm -f "$SOCK" 2>/dev/null
echo "[$TAG] done -- $MAINLOG $GUILOG"
