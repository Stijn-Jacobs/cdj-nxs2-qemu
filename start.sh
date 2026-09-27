#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Start the decks the way ./setup.sh configured them (cdj.conf): one or two
# CDJ-2000NXS2 windows, the controller relay and, if you chose a MIDI
# controller, the bridge. Ctrl-C stops everything.
#
#   usage: ./start.sh             start
#          ./start.sh --app       start with the virtual deck app as the window
#          ./start.sh stop        stop a running rig from another shell
#          ./start.sh --dry-run   show what would be started
#   env:   every knob of scripts/run/rig.sh still works (AUDIODEV=, NOSOUND=1,
#          GUI_DISPLAY=, PRIO=, TBFAST=0, ...).
#
# The work is done by launcher/ (python -m launcher start), the same code the
# packaged program runs.
. "$(dirname "${BASH_SOURCE[0]}")/scripts/cdj_python.sh"
cdj_launcher start "$@"
