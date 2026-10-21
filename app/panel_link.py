# SPDX-License-Identifier: GPL-2.0-or-later
"""The line to an older player's panel: UDP datagrams to the MAIN emulator.

Those players have no relay. Their MAIN emulator binds CDJ_PANEL_KEYSOCK and
takes the same `<off>:<val>:<dur_ms>:<op>` datagrams the host keyboard sends
(hw/cdj/common/cdj_panelkeys.h). The interface is RelayLink's, so a deck view
sends through either. No lamp comes back over this line, so the view draws
every lamp dark.
"""

import socket


class PanelLink:
    def __init__(self, port):
        self.addr = ("127.0.0.1", port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.status = f"panel: UDP {port}"

    def start(self):
        pass

    def close(self):
        self.sock.close()

    @property
    def connected(self):
        return True

    def send(self, tag, datagram):
        try:
            self.sock.sendto(datagram.encode(), self.addr)
            return True
        except OSError:
            return False
