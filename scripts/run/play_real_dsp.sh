#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Load and play on the real DSP with every DSP stand-in switched off, so the
# decoder's buffer level (status 0x158), the play position and the audio can
# only come from IC301's own program. MAIN/GUI-side pokes are kept: the PPOKE
# loader (instrumented_batch.sh), the play-key hold 0x0B056E38, the colour
# capability byte, the edge RPOKE 0x08443182 and the GUI link thinning.
#
# C6X=0 is the control: the same arm on the stand-in peer, which does not play.
#
#   usage: bash scripts/run/play_real_dsp.sh <tag> [n]
#   env:   C6X=1 MHZ=250 LOCKSTEP=1 JOBS=3 FILMN=8 MOTION_MS=1500
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PREFIX="${1:?usage: play_real_dsp.sh <tag> [n]}"
N="${2:-4}"

unset CDJ_AUDIO_DRAIN CDJ_DSP_ENGINE CDJ_DSP_ENGINE_GATE CDJ_DSP_ENGINE_POS \
      CDJ_CUEKEY CDJ_JOGDRIVE CDJ_BEATDRIVE CDJ_BEATGRID CDJ_PITCHDRIVE \
      CDJ_PSTORE CDJ_DMA1_COPY CDJ_PCALL
if [ "${C6X:-1}" = "1" ]; then
    # LOCKSTEP=1 (default): MAIN under -icount and the DSP stepped against it,
    # so MAIN waits for the DSP rather than the DSP dropping time. The driver
    # waits in virtual time (CDJ_VCLOCK_FILE).
    if [ "${LOCKSTEP:-1}" = "1" ]; then
        # DSPTHREAD=2: the DSP on its own host thread, synchronised to MAIN one
        # quantum at a time; 0 steps it inside the vCPU thread.
        export ICOUNT="${ICOUNT:-shift=1,sleep=off}" CDJ_C6X_THREAD="${DSPTHREAD:-0}" \
               CDJ_C6X_CATCHUP_MS="${CDJ_C6X_CATCHUP_MS:-100000000}" \
               CDJ_C6X_MHZ="${MHZ:-250}"
    else
        export CDJ_C6X_MHZ="${MHZ:-30}" CDJ_C6X_THREAD="${CDJ_C6X_THREAD:-1}"
    fi
    export CDJ_C6X=1
    # W1=0 (default for the real DSP): the load poke writes 0, so the loader
    # takes bsr 0x082F118C and the file layer keeps filling. W1=1 forces the
    # file state machine and leaves the real deck stuck in prebuffer.
    export W1="${W1:-0}"
    [ -n "${PCM:-}" ] && export CDJ_C6X_PCM="$PCM"
else
    unset CDJ_C6X
fi
export CDJ_AREA6=1
# Heartbeat thinning paces a free-running MAIN; under LOCKSTEP it starves the
# GUI link, so it defaults off there.
[ "${C6X:-1}" = "1" ] && [ "${LOCKSTEP:-1}" = "1" ] && THIN="${THIN:-0}"
export CDJ_SPILINK_THIN3="${THIN:-8}" SPILINK_KEEP_FRAMES="${KEEPF:-1024}" CDJ_SPILINK_DEDUP=1
# LADDER=1: MAIN-side transport forces -- the edge RPOKEs and the start-arm hold
# anchored at the edge detector's load 0x0844323A, armed once the waveform
# publisher 0x084E294E has run. No CDJ_PCALL: instrumented_batch.sh's PPOKE is
# the loader, and two loaders give a title bar with no track.
if [ "${LADDER:-1}" = "1" ]; then
    export CDJ_RPOKE="${CDJ_RPOKE:-0x08443182:0:3,0x08443232:0:1}"
    export CDJ_PPOKE_ARM="${ARM:-0x084E294E}"
    export PPOKEX="${PPOKEX:-${EDGEPC:-0x0844323A}:0x0B058C10:0x00010000:0xFF00FFFF:0:1}"
elif [ "${C6X:-1}" = "1" ] && [ "${HOLDS:-0}" = "0" ]; then
    # With the real DSP the deck mode reaches 3 unforced, so the 0x08443182
    # mode force is not needed.
    export CDJ_RPOKE="${CDJ_RPOKE:-none}"
else
    export CDJ_RPOKE="${CDJ_RPOKE:-0x08443182:0:3}"
fi
unset CDJ_PCALL
# HOLDS=0 (default for C6X=1): no play-key holds. 0x0B058C00=1 sends the panel
# PLAY handler down its MAIN-only arm and the Player never sees the key.
if [ "${C6X:-1}" = "1" ] && [ "${HOLDS:-0}" = "0" ] && [ "${LADDER:-1}" != "1" ]; then
    export MPOKE="${MPOKE:-0x0A35FAAA:3:1${POKEX:+,$POKEX}}"
else
    export MPOKE="${MPOKE:-0x0A35FAAA:3:1,0x0B056E38:1:4,0x0B058C00:1:4${POKEX:+,$POKEX}}"
fi
# The real deck starts playing at the end of the load, so PLAY would pause it;
# the no-holds real-DSP arm presses nothing by default.
if [ "${C6X:-1}" = "1" ] && [ "${HOLDS:-0}" = "0" ] && [ "${LADDER:-1}" != "1" ]; then
    KEYBITS="${KEYBITS:-0x00}"
fi
export KEYBYTE="${KEYBYTE:-0x10}" KEYBITS="${KEYBITS:-0x01}" KEYDUR="${KEYDUR:-150}"
export TAPS="${TAPS:-0x08443252,0x08443276,0x0842A160,0x084E294E}"
export WORDS="${WORDS:-0x0B054D44 0x0B5125E8 0x09944170 0x09947478}"
export SNAP="${SNAP:-0x09944000:0x01C40000}"
export FILMN="${FILMN:-8}" MOTION_MS="${MOTION_MS:-1500}"
# What load_track.py sees as the settings modal here is first the real MY
# SETTINGS modal, which dismisses itself about 5 s after it appears, and then
# the browse list behind it, which never clears; waiting for that cost ~30 s a
# run. The cap has to outlast the real modal, which swallows keys: at 2 s the
# walk's first presses were lost and the deck never loaded its track.
export MODAL_CLEAR="${MODAL_CLEAR:-7}"
export GUI_DISPLAY="${GUI_DISPLAY:-none}" PREFLIGHT="${PREFLIGHT:-0}"
export JOBS="${JOBS:-3}"
# The 1/5/15-minute load averages; /proc/loadavg is Linux-only.
loadavg() { cut -d' ' -f1-3 /proc/loadavg 2>/dev/null || sysctl -n vm.loadavg 2>/dev/null | tr -d '{}' | xargs; }
echo "=== $PREFIX  C6X=${C6X:-1} MHZ=${CDJ_C6X_MHZ:-} LOCKSTEP=${LOCKSTEP:-1} ICOUNT=${ICOUNT:-}  qemu before: $(pgrep qemu-system | wc -l | tr -d ' ')  load: $(loadavg)"
bash "$HERE/instrumented_batch.sh" "$PREFIX" "$N"
echo "--- DSP (c6x exit lines)"
bash "$HERE/report_dsp.sh" "$PREFIX" "$N"
echo "--- minimap playhead"
for i in $(seq 1 "$N"); do
    python3 "$HERE/score_playhead.py" "$PREFIX$i" 2>&1 | tail -2
done
echo "=== host load after: $(loadavg)"
