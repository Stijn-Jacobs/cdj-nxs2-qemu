#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Send a front-panel key to a running machine, from a shell.

The same datagram socket the window's keyboard uses (cdj_panelkeys.h), so this
and the window are interchangeable -- which is what makes a sweep scriptable
instead of one boot per guess.

  ./scripts/run/panel_key.py browse                 a named key
  ./scripts/run/panel_key.py 0x15:0x01              a raw report bit
  ./scripts/run/panel_key.py rot:0x16:+1            turn the knob on byte 0x16
  ./scripts/run/panel_key.py lvl:0x16:0x01          set an ANALOGUE byte, held until changed
  ./scripts/run/panel_key.py sweep 0x15             every bit of byte 0x15, 1 s apart

Named keys marked (?) below are ordered guesses from the front panel's layout,
not decoder facts. The select knob is in NO identified byte -- finding it is
what `sweep` is for.
"""
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdj_panelsock


SOCK = os.environ.get("CDJ_PANEL_KEYSOCK", "/tmp/cdj-panel-keys.sock")

KEYS = {
    "browse": (0x14, 0x01, True),
    "menu":   (0x14, 0x08, True),
    "usb":    (0x13, 0x04, True),
    "rekordbox": (0x13, 0x01, False),
    "link":   (0x13, 0x02, False),
    "sd":     (0x13, 0x08, False),
    "disc":   (0x13, 0x10, False),
    "tag":    (0x14, 0x02, False),
    "info":   (0x14, 0x04, False),
    "back":   (0x14, 0x10, False),
}


def send(off, val, dur=150, op="or"):
    s = cdj_panelsock.sender()
    try:
        s.sendto(f"0x{off:02x}:{val}:{dur}:{op}".encode(), SOCK)
    except OSError as e:
        sys.exit(f"cannot reach {SOCK}: {e}  (is the machine running?)")
    finally:
        s.close()


def main(argv):
    if not argv:
        print(__doc__)
        print("  named keys:", " ".join(
            k + ("" if v[2] else "(?)") for k, v in KEYS.items()))
        return
    a = argv[0]
    if a == "sweep":
        off = int(argv[1], 0)
        for bit in range(8):
            print(f"  report[0x{off:02x}] |= 0x{1 << bit:02x}")
            send(off, 1 << bit)
            time.sleep(float(argv[2]) if len(argv) > 2 else 1.0)
    elif a in KEYS:
        off, mask, confirmed = KEYS[a]
        send(off, mask)
        print(f"{a}: report[0x{off:02x}] |= 0x{mask:02x}"
              f"{'' if confirmed else '   [UNVERIFIED map]'}")
    elif a.startswith("rot:"):
        _, off, delta = a.split(":")
        send(int(off, 0), int(delta, 0), 0, "rot")
    elif a.startswith("lvl:"):
        # An analogue byte, held until changed, e.g. the touch axes at
        # report[0x16..0x19], which a bit mask cannot express.
        _, off, val = a.split(":")
        send(int(off, 0), int(val, 0), 0, "lvl")
        print(f"lvl: report[{int(off, 0):#04x}] = {int(val, 0):#04x} (held)")
    else:
        parts = a.split(":")
        send(int(parts[0], 0), int(parts[1], 0),
             int(parts[2]) if len(parts) > 2 else 150)


if __name__ == "__main__":
    main(sys.argv[1:])
