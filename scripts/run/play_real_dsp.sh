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
#
# The work is done by launcher/play_real_dsp.py.
. "$(dirname "${BASH_SOURCE[0]}")/../cdj_python.sh"
cdj_launcher_script play_real_dsp "$@"
