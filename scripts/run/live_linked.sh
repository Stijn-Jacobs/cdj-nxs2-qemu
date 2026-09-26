#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# The two-deck live rig with the MIDI relay and Pro DJ Link: two decks with
# windows and DSP audio (cdjA -> <tag>1, cdjB -> <tag>2), the controller relay,
# and a DHCP server so each deck takes a lease and announces itself. Everything
# runs in the foreground of this script, since background processes do not
# survive a `wsl -- ...` wrapper; Ctrl-C stops it all.
#
# On Windows, once the deck windows are up:
#   python -u midi\bridge.py --relay 127.0.0.1:$RELAY_PORT
#
# Two decks need roughly twice the CPU of one. The DSP JIT cache should be warm,
# or the first minutes run slow.
#
#   usage: bash scripts/run/live_linked.sh [prefix=show] [decks=2]
#   env:   SNIFF=1        also capture the segment to /tmp/j2-<tag>.pcap
#          GROUP=<ip:port>  put this rig on its own segment
#          DJLINK=0       decks only, no network (rig.sh's own knob)
#          FRAMES=240     the rig lives FRAMES x 5 s, so ~20 min by default
#
# The work is done by launcher/live.py.
. "$(dirname "${BASH_SOURCE[0]}")/../cdj_python.sh"
cdj_launcher_script live_linked "$@"
