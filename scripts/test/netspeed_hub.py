#!/usr/bin/env python3
"""Private synchronous Link hub with normal DHCP infrastructure.

Uses the existing segment protocol and this repo's DHCP packet builder.
No beat/status packets are generated. Replies use the same conservative
timestamp rule as the hub's ARP replies, and reach clients after the quantum.
"""
import argparse
from pathlib import Path
import socket
import sys

ROOT = Path(__file__).resolve().parents[2]


def main():
    p = argparse.ArgumentParser(add_help=False)
    p.add_argument('--hub-root', type=Path, required=True)
    a, rest = p.parse_known_args()
    sys.path.insert(0, str(a.hub_root.resolve()))
    sys.path.insert(0, str(ROOT / 'scripts/net'))
    from tools.cdj_main import link_hub
    import dhcp_server

    class DhcpHub(link_hub.SyncHub):
        def __init__(self, *args, **kwargs):
            super().__init__(*args, **kwargs)
            self.leases = {}

        def message(self, conn, client, kind, stamp, payload):
            super().message(conn, client, kind, stamp, payload)
            request = dhcp_server.parse_dhcp(payload) if kind == link_hub.SYNC_FRAME else None
            if not request: return
            xid, chaddr, msg, requested, flags, eth_mac = request
            if msg not in (dhcp_server.DISCOVER, dhcp_server.REQUEST): return
            if eth_mac not in self.leases:
                self.leases[eth_mac] = socket.inet_aton(f'192.168.50.{10+len(self.leases)}')
            server = socket.inet_aton('192.168.50.1')
            response = dhcp_server.build_reply(dhcp_server.OFFER if msg == dhcp_server.DISCOVER else dhcp_server.ACK,
                                              xid, chaddr, self.leases[eth_mac], server,
                                              socket.inet_aton('255.255.255.0'), server, flags, eth_mac)
            due = max(stamp, client['granted'])
            self.record(response, 'dhcp', guest_ns=due)
            self.send_frame(conn, due, response)

    link_hub.SyncHub = DhcpHub
    return link_hub.main(rest)


if __name__ == '__main__':
    raise SystemExit(main())
