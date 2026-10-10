#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# One headless deck, no sound, no Pro DJ Link, that loads a track, plays it and
# stops by itself: the profile and training runs of
# scripts/build/build_dsp_module.sh. The CDJ-2000NXS2 also has
# MASTER TEMPO and the tempo fader swept (warm_jit.py).
#
#   usage: bash scripts/run/warm_jit.sh [tag=warm] [--dry-run]
#   env:   PLAY_S=45 (virtual seconds of play after the load; 30 for a
#          one-window model, which has no sweep), plus any rig.sh
#          knob: MODULE=none, AUTOJIT=0|1, C66X_JIT_PROFILE, ...
#          DSP_TRAIN_MODULE=<m.so>: run with that module instead of none
#
# The work is done by launcher/warm_jit.py.
. "$(dirname "${BASH_SOURCE[0]}")/../cdj_python.sh"
cdj_launcher_script warm_jit "$@"
