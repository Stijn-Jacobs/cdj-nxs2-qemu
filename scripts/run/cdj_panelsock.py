# SPDX-License-Identifier: GPL-2.0-or-later
"""The host end of the panel side channel (the peer of hw/cdj/common/cdj_panelkeys.h).

The panel key and panel state channels are UDP on 127.0.0.1, because Windows
has no datagram AF_UNIX. CDJ_PANEL_KEYSOCK and CDJ_PANEL_STATESOCK still hold a
path-like name ("/tmp/cdj-panel-keys-show1.sock"); port_for() hashes it, so two
tags get two ports. A name ending in ":<port>", or a bare number, sets the port
outright.

port_for() must agree with cdj_panelsock_port() in hw/cdj/common/cdj_panelkeys.h; the
two processes only meet on this number.
"""

import socket

HOST = "127.0.0.1"


def port_for(name):
    """FNV-1a over the name's stem, folded into 20000..44999.

    Windows reserves UDP blocks for Hyper-V from about 50000 up (`netsh int
    ipv4 show excludedportrange protocol=udp`), and a bind inside one fails
    silently, so the range stays below 45000.

    Only the stem is hashed: "/tmp/cdj-panel-keys-show1.sock",
    "C:/msys64/tmp/cdj-panel-keys-show1.sock" and "cdj-panel-keys-show1" are
    the same socket. An MSYS2 shell rewrites /tmp arguments into Windows paths,
    so the two ends would otherwise hash different strings.
    """
    base = name.replace("\\", "/").rsplit("/", 1)[-1]
    if base.endswith(".sock"):
        base = base[:-5]

    digits = base.rsplit(":", 1)[-1]
    if digits.isdigit() and 1024 <= int(digits) <= 65535:
        return int(digits)

    h = 2166136261
    for b in base.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return 20000 + h % 25000


def addr_for(name):
    return (HOST, port_for(name))


def bind(name, timeout=None):
    """The receiving end: a UDP socket bound to the name.

    Non-blocking by default (the relay selects on it); pass `timeout` in
    seconds for a reader that just wants a bounded recv().
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(addr_for(name))
    if timeout is None:
        s.setblocking(False)
    else:
        s.settimeout(timeout)
    return s


class PanelSocket:
    """A UDP socket whose sendto() takes a panel socket NAME, not an address.

    Lets callers keep passing the socket name they already have.
    """

    def __init__(self):
        self._s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def sendto(self, payload, name):
        return self._s.sendto(payload, addr_for(name))

    def __getattr__(self, attr):
        return getattr(self._s, attr)


def sender():
    """An unbound socket, for sendto(payload, "<the name you always used>")."""
    return PanelSocket()


def send(name, payload):
    """One datagram to the name. Raises OSError if nobody is bound."""
    if isinstance(payload, str):
        payload = payload.encode()
    s = sender()
    try:
        s.sendto(payload, name)
    finally:
        s.close()


def is_bound(name):
    """Is anybody listening on this name yet?

    There is no socket file to test, so try to bind the port: that fails
    exactly when the machine holds it.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.bind(addr_for(name))
    except OSError:
        return True
    finally:
        s.close()
    return False


def wait_bound(name, timeout=240.0, poll=1.0):
    """Block until the machine has bound `name`. True if it did."""
    import time
    deadline = time.time() + timeout
    while time.time() < deadline:
        if is_bound(name):
            return True
        time.sleep(poll)
    return is_bound(name)


def press(name, off, val, dur_ms=150, op="or"):
    """One key press, in the wire format cdj_pnl_drain_keys() parses."""
    send(name, f"0x{off:02x}:{val}:{dur_ms}:{op}")


def touch(name, x, y, down):
    """One touch-screen state change in LCD pixels (0..799, 0..479), in the
    wire format cdj_touch_parse() reads. Send down=True, any moves, then
    down=False. The name is the deck's CDJ_PANEL_KEYSOCK; MAIN only acts on
    it with CDJ_TOUCH=1, and holds even a short press for the firmware's
    debounce."""
    send(name, f"{int(x)}:{int(y)}:{1 if down else 0}:touch")


def tap(name, x, y, dur_ms=150):
    """A whole tap in one datagram: down now, up after dur_ms. MAIN still
    holds it for at least CDJ_TOUCH_MIN_FRAMES panel frames."""
    send(name, f"{int(x)}:{int(y)}:{max(int(dur_ms), 1)}:tap")


def _main(argv):
    """A CLI, so a shell script needs no inline socket code of its own:

        python3 scripts/run/cdj_panelsock.py send <name> <payload>
        python3 scripts/run/cdj_panelsock.py port <name>
        python3 scripts/run/cdj_panelsock.py ready <name>          # exit 0 if bound
        python3 scripts/run/cdj_panelsock.py wait <name> [secs]    # block until bound
    """
    if len(argv) >= 3 and argv[0] == "send":
        try:
            send(argv[1], argv[2])
        except OSError as e:
            print(f"cannot reach {argv[1]}: {e}  (is the machine running?)")
        return 0
    if len(argv) == 2 and argv[0] == "port":
        print(port_for(argv[1]))
        return 0
    if len(argv) == 2 and argv[0] == "ready":
        return 0 if is_bound(argv[1]) else 1
    if len(argv) >= 2 and argv[0] == "wait":
        t = float(argv[2]) if len(argv) > 2 else 240.0
        return 0 if wait_bound(argv[1], t) else 1
    print(__doc__)
    print("usage: cdj_panelsock.py send <name> <payload> | port <name>")
    print("                         | ready <name> | wait <name> [secs]")
    return 2


if __name__ == "__main__":
    import sys
    raise SystemExit(_main(sys.argv[1:]))
