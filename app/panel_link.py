# SPDX-License-Identifier: GPL-2.0-or-later
"""The line to an older player's panel: UDP datagrams to and from the MAIN emulator.

Those players have no relay. Their MAIN emulator binds CDJ_PANEL_KEYSOCK and
takes the same `<off>:<val>:<dur_ms>:<op>` datagrams the host keyboard sends
(hw/cdj/common/cdj_panelkeys.h), and sends MAIN's own frame to the panel MCU
as `pnl <hex>` to CDJ_PANEL_STATESOCK (common/cdj_pnl_stateout.c). That frame
carries the panel's lamps, which the face lights from the bits measured for
each key. The interface is RelayLink's, so a deck view sends through either.
"""

import socket
import threading
import time

# MAIN repeats an unchanged frame every second; with none for this long the
# deck is gone and every lamp goes dark.
STALE_S = 3.0


class PanelLink:
    def __init__(self, port, state_port=0):
        self.addr = ("127.0.0.1", port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.status = f"panel: UDP {port}"
        self.frame = None
        self.seen = float("-inf")
        self.closed = False
        self.state = None
        if state_port:
            self.state = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.state.bind(("127.0.0.1", state_port))
            self.state.settimeout(0.5)
            self.status += f", lamps on UDP {state_port}"
        self.thread = threading.Thread(target=self._listen, name="panel-state", daemon=True)

    def start(self):
        if self.state:
            self.thread.start()

    def close(self):
        self.closed = True
        self.sock.close()
        if self.state:
            self.state.close()

    @property
    def connected(self):
        return True

    def send(self, tag, datagram):
        try:
            self.sock.sendto(datagram.encode(), self.addr)
            return True
        except OSError:
            return False

    def lit(self, off, mask):
        """True while MAIN's latest panel frame sets any bit of `mask` in byte `off`."""
        frame = self.frame
        if frame is None or time.monotonic() - self.seen > STALE_S or off >= len(frame):
            return False
        return bool(frame[off] & mask)

    def _listen(self):
        while not self.closed:
            try:
                msg = self.state.recv(256)
            except socket.timeout:
                continue
            except OSError:
                return
            kind, _, payload = msg.decode("ascii", "replace").partition(" ")
            if kind != "pnl":
                continue
            try:
                self.frame = bytes.fromhex(payload.strip())
            except ValueError:
                continue
            self.seen = time.monotonic()
