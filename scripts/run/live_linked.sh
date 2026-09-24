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
set -uo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TAG="${1:-show}"
N="${2:-2}"
RELAY_PORT="${RELAY_PORT:-7202}"
GROUP="${GROUP:-239.77.77.1:45000}"
TAGS=$(seq -s, -f "${TAG}%g" 1 "$N")

echo "[$TAG] $N deck(s); DJ-202 relay on 127.0.0.1:$RELAY_PORT; Pro DJ Link on $GROUP"
echo "[$TAG] on WINDOWS, once the deck windows are up:"
echo "[$TAG]     python -u midi\\bridge.py --relay 127.0.0.1:$RELAY_PORT"

KILL=""
python3 "$HERE/midi_relay.py" --tags "$TAGS" --port "$RELAY_PORT" &
KILL="$!"

if [ "${SNIFF:-0}" = "1" ]; then
    python3 "$HERE/../net/capture_link.py" "$GROUP" "/tmp/j2-$TAG.pcap" \
        > "/tmp/j2-$TAG.sniff" 2>&1 &
    KILL="$KILL $!"
    echo "[$TAG] capturing the segment -> /tmp/j2-$TAG.pcap"
fi

# shellcheck disable=SC2064  -- KILL is wanted expanded NOW, not at trap time.
trap "kill $KILL 2>/dev/null" EXIT

GROUP="$GROUP" NDECKS="$N" bash "$HERE/rig.sh" "$TAG" "${FRAMES:-240}"

if [ "${SNIFF:-0}" = "1" ] && [ -f "/tmp/j2-$TAG.pcap" ]; then
    echo
    echo "===== what reached the wire ====="
    python3 "$HERE/../net/score_link.py" "/tmp/j2-$TAG.pcap"
fi
echo
echo "===== leases ====="
cat "/tmp/cdj-$TAG-dhcpd.log" 2>/dev/null || echo "  (no DHCP log -- DJLINK=0?)"
