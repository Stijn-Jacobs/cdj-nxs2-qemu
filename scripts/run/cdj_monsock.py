# SPDX-License-Identifier: GPL-2.0-or-later
"""The host end of the QEMU monitor, one transport on both platforms.

CPython has no AF_UNIX on Windows, so there the monitor is a TCP socket on
127.0.0.1; on Linux it stays a unix socket. QEMU needs only a different
-monitor string (see spec()), and every client connects through connect().

Callers keep naming the monitor by path ("/tmp/cdj-show1-gui-mon.sock").
port_for() hashes the path's stem, as cdj_panelsock.port_for() does, so an
MSYS2 shell rewriting /tmp/... into C:/msys64/tmp/... still lands both ends on
the same port. TCP and UDP are separate port spaces, so sharing the hash range
with cdj_panelsock is harmless.
"""

import socket
import sys
import time

from cdj_panelsock import port_for

HOST = "127.0.0.1"

_excluded = None


def excluded_tcp_ranges():
    """The TCP port blocks Windows has reserved for Hyper-V/WSL, as (lo, hi).

    A monitor hashed into one of these fails to bind ("Failed to bind socket:
    Input/output error"). The blocks are chosen at boot, so ask the host.
    Empty off Windows, or if netsh cannot be run.
    """
    global _excluded
    if _excluded is not None:
        return _excluded
    _excluded = []
    if not is_windows():
        return _excluded
    import subprocess

    try:
        out = subprocess.run(
            ["netsh", "int", "ipv4", "show", "excludedportrange", "protocol=tcp"],
            capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return _excluded
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 2 and f[0].isdigit() and f[1].isdigit():
            _excluded.append((int(f[0]), int(f[1])))
    return _excluded


def tcp_port(name):
    """port_for(name), stepped past any reserved block.

    Deterministic for a given host: the launcher (spec) and every client
    (connect) run this same function against the same reservation list, so they
    still agree without passing the port around.
    """
    p = port_for(name)
    ranges = excluded_tcp_ranges()
    for _ in range(len(ranges) + 1):
        hit = next(((lo, hi) for lo, hi in ranges if lo <= p <= hi), None)
        if hit is None:
            return p
        p = hit[1] + 1
        if p > 44999:
            p = 20000
    return p


def is_windows():
    return sys.platform.startswith(("win", "cygwin")) or sys.platform == "msys"


def use_tcp():
    """Windows has no AF_UNIX in Python; elsewhere the unix socket is fine.

    CDJ_MONSOCK_TCP=1 forces TCP on Linux too, which is how this module is
    tested on the host that does not need it.
    """
    import os

    forced = os.environ.get("CDJ_MONSOCK_TCP")
    if forced is not None:
        return forced not in ("", "0")
    return is_windows() or not hasattr(socket, "AF_UNIX")


def spec(name):
    """The value for QEMU's -monitor argument, for this name on this host.

    Shell scripts ask for this rather than building the string themselves, so
    the launcher and the Python client can never disagree about the transport.
    """
    if use_tcp():
        return "tcp:%s:%d,server=on,wait=off" % (HOST, tcp_port(name))
    return "unix:%s,server=on,wait=off" % name


def connect(name, timeout=5.0):
    """A connected monitor socket, whatever the transport is here."""
    if use_tcp():
        s = socket.create_connection((HOST, tcp_port(name)), timeout)
    else:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(name)
    return s


def is_up(name):
    """Is the machine serving its monitor yet?

    Always a connect attempt, never a stat: on Windows an AF_UNIX endpoint is
    a reparse point that `test -S` does not recognise as a socket.
    """
    try:
        connect(name, timeout=0.5).close()
    except OSError:
        return False
    return True


def wait_up(name, timeout=120.0, poll=0.5):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if is_up(name):
            return True
        time.sleep(poll)
    return is_up(name)


def command(name, line, settle=0.25, timeout=5.0):
    """Send one monitor command and drain the greeting. Returns what came back.

    The monitor greets on connect, so a client that writes immediately gets its
    reply interleaved with the banner.
    """
    s = connect(name, timeout)
    try:
        s.settimeout(settle)
        try:
            s.recv(65536)
        except OSError:
            pass
        if isinstance(line, str):
            line = line.encode()
        s.sendall(line.rstrip(b"\n") + b"\n")
        out = b""
        try:
            while True:
                b = s.recv(65536)
                if not b:
                    break
                out += b
        except OSError:
            pass
        return out
    finally:
        s.close()


def wait_free(name, timeout=30.0, poll=0.5):
    """Block until nothing holds this monitor's address. True if it came free.

    On Windows the monitor port is derived from the tag, so a leftover QEMU of
    the same tag makes the next launch fail to bind.
    """
    deadline = time.time() + timeout
    while time.time() < deadline:
        if not is_up(name):
            return True
        time.sleep(poll)
    return not is_up(name)


def _main(argv):
    """A CLI so shell scripts need no inline socket code:

        python3 scripts/run/cdj_monsock.py spec <name>     # the -monitor argument
        python3 scripts/run/cdj_monsock.py port <name>
        python3 scripts/run/cdj_monsock.py ready <name>          # exit 0 if serving
        python3 scripts/run/cdj_monsock.py wait <name> [secs]
        python3 scripts/run/cdj_monsock.py free <name> [secs]   # until nobody holds it
        python3 scripts/run/cdj_monsock.py cmd <name> <command...>
    """
    if len(argv) == 2 and argv[0] == "spec":
        print(spec(argv[1]))
        return 0
    if len(argv) == 2 and argv[0] == "port":
        print(tcp_port(argv[1]) if use_tcp() else port_for(argv[1]))
        return 0
    if len(argv) == 2 and argv[0] == "ready":
        return 0 if is_up(argv[1]) else 1
    if len(argv) >= 2 and argv[0] == "wait":
        t = float(argv[2]) if len(argv) > 2 else 120.0
        return 0 if wait_up(argv[1], t) else 1
    if len(argv) >= 2 and argv[0] == "free":
        t = float(argv[2]) if len(argv) > 2 else 30.0
        return 0 if wait_free(argv[1], t) else 1
    if len(argv) >= 3 and argv[0] == "cmd":
        sys.stdout.write(command(argv[1], " ".join(argv[2:])).decode(
            "utf-8", "replace"))
        return 0
    print(__doc__)
    print("usage: cdj_monsock.py spec|port|ready <name> | wait <name> [secs]")
    print("                       | cmd <name> <command...>")
    return 2


if __name__ == "__main__":
    raise SystemExit(_main(sys.argv[1:]))
