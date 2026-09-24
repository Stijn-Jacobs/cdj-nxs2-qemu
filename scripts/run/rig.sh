#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The live rig: one or two decks running the real DSP, with the DSP's own
# McBSP0 PCM as audio (CDJ_C6X_AUDIO). The deck starts playing by itself at the
# end of the load; PLAY pauses and resumes. The machine is paced to real time
# (icount sleep=on,align=on); unpaced it overfills or underruns the PCM ring.
#
# Three processes, in this order, in separate sessions:
#   1  bash scripts/run/rig.sh show 120               (WSL, foreground, stays up)
#   2  python3 scripts/run/midi_relay.py                  (WSL, second session)
#   3  python -u midi\bridge.py --relay 127.0.0.1:7202     (WINDOWS)
#
#   usage: bash scripts/run/rig.sh [prefix=show] [film-frames=120]
#   env:   DJLINK=1 (Pro DJ Link on; 0 = off)   GROUP=<ip:port> (own segment)
#          NDECKS=1 (2 for both DJ-202 sides)   GUI_DISPLAY=gtk|none   AUDIODEV=<-audio spec, %TAG% ok>
#          RING=3000 PREFILL=150 MAXLAT=450 (ms)   NOSOUND=1   WARM=0 (1 = throwaway warm-up wave first)
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="${1:-show}"
FRAMES="${2:-120}"
NDECKS="${NDECKS:-1}"

for i in $(seq 1 "$NDECKS"); do
    rm -f "/tmp/bridge-main-${TAG}${i}.log" "/tmp/bridge-gui-${TAG}${i}.log" 2>/dev/null
done

if [ "${NOSOUND:-0}" != "1" ]; then
    # Ring/prefill/cap 3000/150/450 ms; a narrower band ping-pongs between
    # underruns and trims. The larger pa buffer rides over a host sink that
    # drains slower than the DSP produces under load; the model's resampler
    # (CDJ_C6X_AUDIO_ASRC) absorbs the rest. out.period-length is not a valid
    # pa parameter and stops the machine at launch.
    export CDJ_C6X_AUDIO="1:${RING:-3000}:${PREFILL:-150}:${MAXLAT:-450}"
    # Windows: QEMU 9.1 has no WASAPI audiodev, but SDL2's Windows backend is
    # WASAPI. AUDIODEV overrides the default.
    case "$(uname -s)" in
        MINGW*|MSYS*|CYGWIN*)
            DEFAULT_AUDIODEV="sdl,out.buffer-length=200000,timer-period=5000" ;;
        *)
            DEFAULT_AUDIODEV="pa,server=unix:/mnt/wslg/PulseServer,out.buffer-length=200000,timer-period=5000" ;;
    esac
    export CDJ_AUDIODEV="${AUDIODEV:-$DEFAULT_AUDIODEV}"
fi
unset CDJ_AUDIO_LIVE CDJ_AUDIO_OUT

echo "[$TAG] $NDECKS real-DSP deck(s): tags ${TAG}1 .. ${TAG}${NDECKS} (controller mapping: cdjA -> ${TAG}1, cdjB -> ${TAG}2)"
echo "[$TAG] audio: ${CDJ_AUDIODEV:-off}  ring/prefill/cap ${RING:-3000}/${PREFILL:-150}/${MAXLAT:-450} ms"
[ "${AUTOLOAD:-1}" = 1 ] && echo "[$TAG] each deck loads the first track and starts playing by itself (AUTOLOAD=0 leaves that to you)."
[ "${AUTOLOAD:-1}" = 1 ] && echo "[$TAG] the screen may stay on the BROWSE list after the auto-load: BROWSE toggles to the waveform view."

# A raw FAT16 image instead of vvfat: vvfat commits the whole tree on every
# guest write with the BQL held, which stalls the machine when the firmware
# rewrites export.pdb. The writes go to a throwaway /tmp/media-<tag>.img.
export MEDIA_MODE="${MEDIA_MODE:-img}"

# The display board's RTOS idle loop at 0x0E510588 becomes a halt, which frees
# most of a core. The hook checks the opcode first. Empty = no hook.
export CDJ_GUI_IDLE_PC="${CDJ_GUI_IDLE_PC-0x0E51058A}"

# QEMU is a native program on Windows: paths handed to it go through cygpath -m.
native_path() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi
}
_legacy_jit=/tmp/c14gen/g18u/m.so
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        export QEMU_BUILD="${QEMU_BUILD:-/c/qemu-build-mingw}"
        export QEMU_EB_BUILD="${QEMU_EB_BUILD:-/c/qemu-build-mingw-eb}"
        _legacy_jit="" ;;
esac
# The DSP JIT module: the first that exists of C66X_JIT (named by hand),
# ~/c14gen/$MODULE/m.so (MODULE=none: no module), ~/c14gen/curated/m.so (built
# by ./setup.sh --curated-jit), then the maintainers' g20u800 (g18u plus MASTER
# TEMPO's code) and g18u. Without one the run-time auto-JIT compiles the hot DSP
# code into ~/c14gen as the deck plays; with one it is off, so no gcc runs
# mid-session. AUTOJIT=1 keeps it on beside a module, AUTOJIT=0 turns it off.
if [ -z "${C66X_JIT:-}" ] && [ "${MODULE:-}" != none ]; then
    for _m in ${MODULE:+"$HOME/c14gen/$MODULE/m.so"} "$HOME/c14gen/curated/m.so" \
              "$HOME/c14gen/g20u800/m.so" "$HOME/c14gen/g18u/m.so" $_legacy_jit; do
        [ -f "$_m" ] && { C66X_JIT="$(native_path "$_m")"; break; }
    done
fi
export C66X_JIT="${C66X_JIT:-}"
export C66X_JIT_AUTO="${C66X_JIT_AUTO:-$(native_path "$HOME/c14gen")}"
case "${AUTOJIT:-}" in
    1) ;;
    0) unset C66X_JIT_AUTO ;;
    *) [ -n "$C66X_JIT" ] && unset C66X_JIT_AUTO ;;
esac
if [ -z "$C66X_JIT" ] && [ -n "${C66X_JIT_AUTO:-}" ]; then
    echo "[$TAG] no curated JIT module: the auto-JIT compiles the DSP's hot code as it plays"
fi
export CDJ_NATIVE_LIBC="${CDJ_NATIVE_LIBC:-1}"

# ---------------------------------------------------------------------------
# Pro DJ Link, on by default. DJLINK=0 turns it off.
#
#  CDJ_NETDEV        a multicast segment shared by every deck on GROUP; QEMU's
#                    socket backend carries one Ethernet frame per datagram.
#  CDJ_ETHER_PHYADS  0,1,5. The link-up gate is link_status(0) | link_status(1).
#  CDJ_ETHER_MAC     a distinct MAC per deck (%N% = deck number, see
#                    boot_deck.sh); the firmware's own is 00:00:00:00:00:01.
#  CDJ_PCALL2        wakes the ether worker, which waits in twai_flg, with
#                    iset_flg(42, 0x50000001). The pattern needs a bit in
#                    0xFC000000 or the worker goes straight back to waiting.
#
# The deck will not announce without a DHCP lease, so a DHCP server is started
# here and lives as long as the rig.
export DJLINK="${DJLINK:-1}"
DJLINK_PID=""
if [ "$DJLINK" = "1" ]; then
    # Windows reserves UDP ranges for Hyper-V/WSL, and a bind inside one fails
    # with only "Unknown error" from QEMU. Check and say so.
    case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        _gport="${GROUP:-239.77.77.1:45000}"; _gport="${_gport##*:}"
        if netsh int ipv4 show excludedportrange protocol=udp 2>/dev/null |
           awk -v p="$_gport" 'NF>=2 && $1 ~ /^[0-9]+$/ && $1<=p && $2>=p {found=1}
                               END {exit !found}'; then
            echo "[$TAG] ⚠ UDP port $_gport is in a Windows RESERVED range -- the" >&2
            echo "[$TAG]   netdev cannot bind it and MAIN will not start. Pick another:" >&2
            echo "[$TAG]   GROUP=239.77.77.1:45000 bash scripts/run/live_linked.sh $TAG" >&2
            echo "[$TAG]   (netsh int ipv4 show excludedportrange protocol=udp)" >&2
        fi
        ;;
    esac
    export CDJ_NETDEV="${CDJ_NETDEV:-socket,id=djlink,mcast=${GROUP:-239.77.77.1:45000}}"
    export CDJ_ETHER_PHYADS="${CDJ_ETHER_PHYADS:-0,1,5}"
    export CDJ_ETHER_MAC="${CDJ_ETHER_MAC-02:00:00:00:00:0%N%}"
    export CDJ_PCALL2="${CDJ_PCALL2:-0x08345574:0x08516448:0x2a:0x50000001:0x0}"
    if [ "${DJLINK_DHCP:-1}" = "1" ]; then
        python3 "$HERE/../net/dhcp_server.py" "${GROUP:-239.77.77.1:45000}" \
            > "/tmp/cdj-$TAG-dhcpd.log" 2>&1 &
        DJLINK_PID=$!
        trap 'kill $DJLINK_PID 2>/dev/null' EXIT
    fi
    echo "[$TAG] Pro DJ Link ON: ${GROUP:-239.77.77.1:45000}, MAC $CDJ_ETHER_MAC, leases -> /tmp/cdj-$TAG-dhcpd.log"
    echo "[$TAG]   watch it: python3 scripts/net/capture_link.py ${GROUP:-239.77.77.1:45000} /tmp/cdj-$TAG.pcap  (score with score_link.py)"
else
    echo "[$TAG] Pro DJ Link off (DJLINK=0)"
fi
# ---------------------------------------------------------------------------
# MAIN/DSP lockstep quantum. It sets how coarsely MAIN sees the DSP's play
# position; at 16 ms the Pro DJ Link beat sender misses a few beats. One deck
# can afford 4 ms, two decks cannot.
if [ "$NDECKS" = "1" ]; then
    export CDJ_C6X_QUANTUM_US="${CDJ_C6X_QUANTUM_US:-4000}"
else
    export CDJ_C6X_QUANTUM_US="${CDJ_C6X_QUANTUM_US:-16000}"
fi
# A new heartbeat supersedes the queued ones on the GUI side, so keys reach the
# screen quickly. 16, not 2: a queue of 2 starves the GUI link.
export CDJ_SPILINK_FRESH="${CDJ_SPILINK_FRESH:-16}"
# A receive that finds the queue empty gets the newest heartbeat again after
# 20 ms; a starved GUI link driver gives up and the display freezes.
export CDJ_GUI_LINK_IDLE_MS="${CDJ_GUI_LINK_IDLE_MS:-20}"
# Diagnostic re-read and scan of every DMA'd display frame; off.
export CDJ_GUI_FRAME_SCAN="${CDJ_GUI_FRAME_SCAN:-0}"
# TOUCH=1 (default): a click/drag in the display window, or a 'touch'/'tap' on
# the panel key socket, becomes the report's touch X/Y (bytes 0x16..0x19).
export CDJ_TOUCH="${TOUCH:-1}"
# MAXLAG=<ms>: cap how far a deck may fall behind real time. Without it a deck
# that falls behind catches up by running fast for a while. 0 = QEMU's catch-up.
export CDJ_ICOUNT_MAXLAG_MS="${MAXLAG:-0}"
# TBFAST=1 (default): seven QEMU fast paths (TB lookup, FPSCR exits, code-page
# store checks, getenv cache, MMIO splitting) that remove most of MAIN's
# emulation overhead. TBFAST=0 runs the plain QEMU paths.
if [ "${TBFAST:-1}" = "1" ]; then
    export CDJ_TB_FPSCR="${CDJ_TB_FPSCR:-1}" CDJ_TB_XPAGE="${CDJ_TB_XPAGE:-1}" \
        CDJ_TB_CALLPRED="${CDJ_TB_CALLPRED:-1}" CDJ_SMC="${CDJ_SMC:-2}" \
        CDJ_ENVCACHE="${CDJ_ENVCACHE:-1}" CDJ_JC_HASH="${CDJ_JC_HASH:-1}" \
        CDJ_IOSPLIT="${CDJ_IOSPLIT:-1}"
fi
echo "[$TAG] icount ${ICOUNT:-shift=2,sleep=on,align=on}  DSP thread ${DSPTHREAD:-2}  module ${C66X_JIT:-<none: auto-JIT into ${C66X_JIT_AUTO:-off}>}  native libc $CDJ_NATIVE_LIBC"
# PRIO=AboveNormal|High|Normal: Windows priority class for the rig's QEMUs.
# A busy desktop otherwise keeps MAIN's vCPU off the CPU. Normal leaves it.
PRIO="${PRIO:-AboveNormal}"
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*)
        if [ "$PRIO" != "Normal" ]; then
            /c/Windows/System32/WindowsPowerShell/v1.0/powershell.exe -NoProfile \
                -ExecutionPolicy Bypass -File "$(cygpath -w "$HERE/raise_priority.ps1")" \
                -Seconds 0 -Class "$PRIO" > "/tmp/$TAG-prio.txt" 2>&1 &
            echo "[$TAG] QEMU priority $PRIO (PRIO=Normal to disable; log /tmp/$TAG-prio.txt)"
        fi
        ;;
esac

env LADDER=0 W1=0 THIN="${THIN:-0}" HOLDS=0 C6X=1 KEYBITS=0x00 \
    ICOUNT="${ICOUNT:-shift=2,sleep=on,align=on}" DSPTHREAD="${DSPTHREAD:-2}" \
    GUI_DISPLAY="${GUI_DISPLAY:-gtk}" JOBS="$NDECKS" WARMUP="${WARM:-0}" \
    FILMN="$FRAMES" MOTION_MS="${MOTION_MS:-5000}" \
    bash "$HERE/play_real_dsp.sh" "$TAG" "$NDECKS"
