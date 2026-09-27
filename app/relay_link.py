# SPDX-License-Identifier: GPL-2.0-or-later
"""The app's line to scripts/run/midi_relay.py: panel keys out, lamps back.

The app is one more client of the relay, exactly like midi/bridge.py: it sends
`<tag> <off>:<val>:<dur_ms>:<op>` lines and reads back the `<tag> state ...`
lines each deck publishes (see midi/README.md, "LEDs"). A controller bridge
can be connected at the same time; the relay fans the lamps out to both.
"""

import os
import socket
import sys
import threading
import time
import types

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "midi"))


def _import_leds():
    """midi/leds.py, which holds the lamp table and the state parser.

    It imports mido at the top for the controller's MIDI port, which the app
    never opens; a stand-in module lets the app reuse the table without making
    every app user install a MIDI library.
    """
    try:
        import leds
    except ModuleNotFoundError as e:
        if e.name != "mido":
            raise
        sys.modules["mido"] = types.ModuleType("mido")
        import leds
    return leds


leds = _import_leds()
LAMPS = leds.LAMPS
parse_state = leds.parse_state


class DeckState:
    """What one deck last published: its lamp frame and its heartbeat fields."""

    # The jog centre display's pointer (pnl byte 9) has this many positions
    # per turn (midi/README.md, "LEDs").
    POINTER_BYTE = 9
    POINTER_STEPS = 135

    def __init__(self):
        self.pnl = None
        self.frm = {}
        self.seen = float("-inf")

    @property
    def live(self):
        return time.monotonic() - self.seen < leds.STALE_S

    def lamp(self, role):
        """True while the NXS2 would light this lamp (a LAMPS role)."""
        if not self.live or self.pnl is None or role not in LAMPS:
            return False
        byte, mask = LAMPS[role]
        return bool(self.pnl[byte] & mask)

    def pointer_turns(self):
        """The centre display's pointer as a fraction of a turn, or None."""
        if not self.live or self.pnl is None or len(self.pnl) <= self.POINTER_BYTE:
            return None
        return (self.pnl[self.POINTER_BYTE] % self.POINTER_STEPS) / self.POINTER_STEPS

    def bpm(self):
        v = self.frm.get("bpm") if self.live else None
        return v / 10 if v else None


class RelayLink:
    """A reconnecting TCP line to the relay, shared by every deck in the app."""

    RETRY_S = 1.0

    def __init__(self, host, port):
        self.addr = (host, port)
        self.sock = None
        self.lock = threading.Lock()
        self.states = {}
        self.status = f"panel: waiting for the relay on {host}:{port}"
        self.closed = False
        self.thread = threading.Thread(target=self._run, name="relay", daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.closed = True
        s = self.sock
        if s:
            try:
                s.close()
            except OSError:
                pass

    @property
    def connected(self):
        return self.sock is not None

    def state(self, tag):
        return self.states.setdefault(tag, DeckState())

    def send(self, tag, datagram):
        """One panel datagram for one deck; dropped while the relay is away."""
        s = self.sock
        if s is None:
            return False
        with self.lock:
            try:
                s.sendall(f"{tag} {datagram}\n".encode())
                return True
            except OSError:
                return False

    def _run(self):
        while not self.closed:
            try:
                s = socket.create_connection(self.addr, timeout=2)
                s.settimeout(None)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.sock = s
                self.status = f"panel: relay {self.addr[0]}:{self.addr[1]}"
                for raw in s.makefile("rb"):
                    self._feed(raw.decode("ascii", "replace").strip())
            except OSError as e:
                if not self.closed:
                    self.status = (f"panel: relay {self.addr[0]}:{self.addr[1]} "
                                   f"unreachable ({e.strerror or e})")
            finally:
                s, self.sock = self.sock, None
                if s:
                    try:
                        s.close()
                    except OSError:
                        pass
            if not self.closed:
                time.sleep(self.RETRY_S)

    def _feed(self, line):
        tag, _, rest = line.partition(" ")
        word, _, payload = rest.partition(" ")
        if word != "state":
            return
        kind, value = parse_state(payload)
        if kind is None:
            return
        st = self.state(tag)
        st.seen = time.monotonic()
        if kind == "pnl":
            st.pnl = value
        else:
            st.frm = value
