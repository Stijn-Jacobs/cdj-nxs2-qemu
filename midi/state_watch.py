#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Listen to the relay beside the bridge and print every lamp or beat change.

    python midi/state_watch.py [--seconds N] [--relay 127.0.0.1:7202]

The relay sends deck state to every connected client, so this can run next to
a live bridge and show what the firmware answered to a press: the decoded
lamps from the panel frame (midi/leds.py LAMPS) and the firmware's beat.
"""
import argparse
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import leds  # noqa: E402


def lamp_word(frame):
    return " ".join(f"{name}={'#' if frame[b] & m else '.'}"
                    for name, (b, m) in leds.LAMPS.items())


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--relay", default="127.0.0.1:7202")
    ap.add_argument("--seconds", type=float, default=60)
    args = ap.parse_args()
    host, _, port = args.relay.partition(":")
    sock = socket.create_connection((host, int(port)), timeout=1)
    t0 = time.time()
    last = {}
    buf = b""
    while time.time() - t0 < args.seconds:
        try:
            chunk = sock.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        buf += chunk
        *lines, buf = buf.split(b"\n")
        for raw in lines:
            tag, _, state = raw.decode("ascii", "replace").partition(" state ")
            kind, value = leds.parse_state(state)
            if kind == "pnl":
                word = lamp_word(value)
            elif kind == "frm":
                word = f"beat={value.get('beat')} bars={value.get('bars')}"
            else:
                continue
            if last.get((tag, kind)) != word:
                last[(tag, kind)] = word
                print(f"{time.time() - t0:7.2f} {tag} {word}", flush=True)


if __name__ == "__main__":
    main()
