#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Relay between the MIDI bridge and the machines: TCP line in, panel-key
datagram out, deck state back.

    python3 scripts/run/midi_relay.py [--tags show1,show2] [--port 7202]

live.sh and live_linked.sh start it for you. It knows nothing about MIDI or
the controller; midi/bridge.py holds the mapping. One line in, one datagram out:

    <tag> <off>:<val>:<dur_ms>:<op>        show1 0x14:1:150:or

`tag` selects the machine: each binds its own key socket named after its tag
(/tmp/cdj-panel-keys-show1.sock). The rest of the line is passed through to
the panel key parser in hw/cdj/cdj_panelkeys.h. It can be driven by hand:

    printf 'show1 0x12:16:150:or
' | nc 127.0.0.1 7202

It listens on 127.0.0.1 only, so the key injector is not exposed to the network.

Deck state for the controller's LEDs comes back on the same connection. Each
machine sends its state (beat in the bar, play state, MAIN's frames to the
panel micro) as datagrams to /tmp/cdj-panel-state-<tag>.sock, which this relay
binds, and every datagram goes to every connected bridge prefixed with its tag:

    show1 state frm beat=3 bars=13 ...

The relay binds, not the machine, so a machine running without a relay just
finds nobody listening and carries on.
"""
import argparse
import os
import re
import select
import socket
import socketserver
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdj_panelsock
import threading

LINE = re.compile(r"^(\S+)\s+(0x[0-9a-fA-F]+):(-?\d+):(\d+)(?::(\w+))?$")


class Fanout:
    """Tag -> panel socket. Machines come and go; this does not care."""

    def __init__(self, pattern, verbose):
        self.pattern = pattern
        self.verbose = verbose
        self.sock = cdj_panelsock.sender()
        self.missing = set()

    def path_for(self, tag):
        return self.pattern.replace("<tag>", tag)

    def send(self, tag, payload):
        path = self.path_for(tag)
        try:
            # Pass the panel-socket name, not an address: sendto() resolves it.
            self.sock.sendto(payload.encode(), path)
        except OSError as e:
            # A machine that is still booting has not bound yet. Say it once
            # per tag, or a held key would fill the terminal.
            if tag not in self.missing:
                self.missing.add(tag)
                print(f"  {tag}: {path} unreachable ({e.strerror}); "
                      "is that machine running?", flush=True)
            return
        if tag in self.missing:
            self.missing.discard(tag)
            print(f"  {tag}: socket back", flush=True)
        if self.verbose:
            print(f"{tag:<8} {payload}", flush=True)


class StateIn:
    """Tag -> bound state socket; every datagram goes to every bridge.

    Tags are bound up front from --tags and lazily the first time a bridge
    addresses a new one, so a relay started without --tags still lights LEDs
    once the first key has been pressed.
    """

    def __init__(self, pattern):
        self.pattern = pattern
        self.socks = {}
        self.clients = set()
        self.lock = threading.Lock()
        # A loopback UDP pair, not os.pipe(): select() on Windows accepts only
        # sockets.
        self.wake_r = cdj_panelsock.socket.socket(
            cdj_panelsock.socket.AF_INET, cdj_panelsock.socket.SOCK_DGRAM)
        self.wake_r.bind(("127.0.0.1", 0))
        self.wake_w = cdj_panelsock.socket.socket(
            cdj_panelsock.socket.AF_INET, cdj_panelsock.socket.SOCK_DGRAM)
        self.wake_w.connect(self.wake_r.getsockname())

    def path_for(self, tag):
        return self.pattern.replace("<tag>", tag)

    def ensure(self, tag):
        if tag in self.socks:
            return
        path = self.path_for(tag)
        s = cdj_panelsock.bind(path)
        with self.lock:
            self.socks[tag] = s
        self.wake_w.send(b"x")
        print(f"  {tag}: state in on {path}", flush=True)

    def add_client(self, wfile):
        with self.lock:
            self.clients.add(wfile)

    def drop_client(self, wfile):
        with self.lock:
            self.clients.discard(wfile)

    def run(self):
        while True:
            with self.lock:
                by_fd = {s.fileno(): (t, s) for t, s in self.socks.items()}
            ready, _, _ = select.select(list(by_fd) + [self.wake_r], [], [])
            for fd in ready:
                if fd is self.wake_r:
                    self.wake_r.recv(64)
                    continue
                tag, s = by_fd[fd]
                while True:
                    try:
                        data = s.recv(512)
                    except BlockingIOError:
                        break
                    line = f"{tag} state {data.decode('ascii', 'replace').strip()}\n"
                    self._broadcast(line.encode())

    def _broadcast(self, line):
        with self.lock:
            clients = list(self.clients)
        for w in clients:
            try:
                w.write(line)
                w.flush()
            except OSError:
                self.drop_client(w)


class Handler(socketserver.StreamRequestHandler):
    def handle(self):
        print(f"bridge connected from {self.client_address[0]}", flush=True)
        state = self.server.state
        state.add_client(self.wfile)
        try:
            for raw in self.rfile:
                line = raw.decode("utf-8", "replace").strip()
                if not line:
                    continue
                m = LINE.match(line)
                if not m:
                    print(f"  malformed: {line!r}", flush=True)
                    continue
                tag, off, val, dur, op = m.groups()
                state.ensure(tag)
                self.server.fanout.send(tag, f"{off}:{val}:{dur}:{op or 'or'}")
        finally:
            state.drop_client(self.wfile)
        print("bridge disconnected", flush=True)


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=int(os.environ.get("RELAY_PORT", "7202")),
                    help="TCP port for bridge.py (env RELAY_PORT)")
    ap.add_argument("--socket-pattern", default="/tmp/cdj-panel-keys-<tag>.sock",
                    help="where each machine binds; <tag> is substituted")
    ap.add_argument("--state-pattern", default="/tmp/cdj-panel-state-<tag>.sock",
                    help="where this relay binds each machine's state socket")
    ap.add_argument("--tags", default="show1,show2",
                    help="comma-separated tags to check for and bind state for")
    ap.add_argument("-q", "--quiet", action="store_true")
    args = ap.parse_args()

    fanout = Fanout(args.socket_pattern, not args.quiet)
    state_in = StateIn(args.state_pattern)
    for tag in filter(None, args.tags.split(",")):
        path = fanout.path_for(tag)
        # Ask the port; there is no socket file.
        state = "ready" if cdj_panelsock.is_bound(path) else "NOT BOUND YET"
        print(f"  {tag:<8} {path}  {state}")
        state_in.ensure(tag)
    threading.Thread(target=state_in.run, daemon=True).start()

    try:
        server = Server((args.host, args.port), Handler)
    except OSError as e:
        # Hyper-V reserves TCP blocks at boot and they move between reboots;
        # a port inside one fails with WinError 10013, not "in use".
        sys.exit(f"relay cannot bind {args.host}:{args.port} ({e}).\n"
                 f"  On Windows check: netsh int ipv4 show excludedportrange protocol=tcp\n"
                 f"  and pick a free port with RELAY_PORT=<port> (and bridge.py --relay).")
    server.fanout = fanout
    server.state = state_in
    print(f"relay listening on {args.host}:{args.port}, "
          f"sockets {args.socket_pattern}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopped")
        server.shutdown()


if __name__ == "__main__":
    sys.exit(main())
