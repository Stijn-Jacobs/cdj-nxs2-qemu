#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One headless deck, no sound, no Pro DJ Link, that loads a track, plays it and
# has MASTER TEMPO and the tempo fader swept (warm_jit.py), then stops by itself.
# Run with the auto-JIT on, it fills the DSP code cache (~/c14gen) so real
# sessions start fast; scripts/build/build_dsp_module.sh runs it to record the
# DSP instead. Ends with the auto-JIT's own count of what it built.
#
#   usage: bash scripts/run/warm_jit.sh [tag=warm] [--dry-run]
#   env:   PLAY_S=270 (virtual seconds of play after the load), plus any rig.sh
#          knob: MODULE=none, AUTOJIT=0|1, CDJ_C6X_RECORD, C66X_JIT_PROFILE, ...
#
# The work is done by launcher/warm_jit.py.
. "$(dirname "${BASH_SOURCE[0]}")/../cdj_python.sh"
cdj_launcher_script warm_jit "$@"
