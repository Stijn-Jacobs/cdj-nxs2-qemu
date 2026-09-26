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
#          NDECKS=1 (2 for both DJ-202 sides)   GUI_DISPLAY=gtk|cocoa|none   AUDIODEV=<-audio spec, %TAG% ok>
#          RING=3000 PREFILL=150 MAXLAT=450 (ms)   NOSOUND=1   WARM=0 (1 = throwaway warm-up wave first)
#
# The work is done by launcher/rig.py.
. "$(dirname "${BASH_SOURCE[0]}")/../cdj_python.sh"
cdj_launcher_script rig "$@"
