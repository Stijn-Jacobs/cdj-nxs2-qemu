# SPDX-License-Identifier: GPL-2.0-or-later
"""DJLINK=tap:<adapter>: the deck on a host TAP-Windows6 adapter. rig_env's
network knobs for it and for the multicast segment it replaces, the refusals,
and the DHCP server's tap mode on hand-built BOOTP datagrams."""

import os
import socket
import sys
import threading
import time

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

import dhcp_server as dh  # noqa: E402
from launcher import host, rig  # noqa: E402

DECK = bytes.fromhex("000000000001")


def rig_env(monkeypatch, **knobs):
    monkeypatch.setattr(host, "is_windows", lambda: True)
    monkeypatch.setattr(host, "windows_reserved_ranges", lambda proto: [])
    env = {"NOSOUND": "1", "C66X_JIT": "", "MODULE": "none"}
    env.update(knobs)
    return env, rig.rig_env(env, "t", env.get("NDECKS", "1"), "120")


def on_adapter(monkeypatch, has_ip):
    monkeypatch.setattr(host, "adapter_has_address", lambda ifname, ip: has_ip)


def test_tap_mode_attaches_the_deck_to_the_adapter(monkeypatch):
    on_adapter(monkeypatch, True)
    env, dhcp = rig_env(monkeypatch, DJLINK="tap:CDJ-Link")
    assert env["CDJ_NETDEV"] == "tap,id=djlink,ifname=CDJ-Link"
    assert env["CDJ_ETHER_PHYADS"] == "0,1,5"
    assert env["CDJ_ETHER_MAC"] == "02:00:00:00:00:0%N%"
    assert dhcp == "tap:192.168.50.1"


def test_tap_mode_can_leave_the_dhcp_to_someone_else(monkeypatch):
    on_adapter(monkeypatch, True)
    _, dhcp = rig_env(monkeypatch, DJLINK="tap:CDJ-Link", DJLINK_DHCP="0")
    assert dhcp is None


def test_segment_mode_is_unchanged(monkeypatch):
    env, dhcp = rig_env(monkeypatch, DJLINK="1")
    assert env["CDJ_NETDEV"] == "socket,id=djlink,mcast=239.77.77.1:45000"
    assert dhcp == "239.77.77.1:45000"
    env, dhcp = rig_env(monkeypatch, DJLINK="1", GROUP="239.77.77.9:44990")
    assert env["CDJ_NETDEV"] == "socket,id=djlink,mcast=239.77.77.9:44990"
    assert dhcp == "239.77.77.9:44990"


def test_no_link_sets_no_netdev(monkeypatch):
    env, dhcp = rig_env(monkeypatch, DJLINK="0")
    assert "CDJ_NETDEV" not in env
    assert dhcp is None


def test_refuses_a_missing_adapter(monkeypatch):
    monkeypatch.setattr(host, "adapter_has_address", lambda ifname, ip: None)
    with pytest.raises(SystemExit) as e:
        rig_env(monkeypatch, DJLINK="tap:CDJ-Link")
    msg = str(e.value)
    assert msg.startswith("DJLINK=tap:CDJ-Link: no adapter of that name")
    assert "install TAP-Windows6" in msg and "\n" not in msg


def test_refuses_an_adapter_without_the_host_address(monkeypatch):
    on_adapter(monkeypatch, False)
    with pytest.raises(SystemExit) as e:
        rig_env(monkeypatch, DJLINK="tap:CDJ-Link")
    assert "has no 192.168.50.1" in str(e.value)
    assert 'set address "CDJ-Link" static 192.168.50.1' in str(e.value)


def test_refuses_two_decks(monkeypatch):
    on_adapter(monkeypatch, True)
    with pytest.raises(SystemExit) as e:
        rig_env(monkeypatch, DJLINK="tap:CDJ-Link", NDECKS="2")
    assert "carries one deck" in str(e.value)


def test_refuses_off_windows(monkeypatch):
    monkeypatch.setattr(host, "is_windows", lambda: False)
    with pytest.raises(SystemExit):
        rig.tap_adapter("CDJ-Link", "1")


# -- the DHCP server's tap mode -------------------------------------------------

def bootp_request(msg_type, flags=b"\x00\x00"):
    bootp = bytearray(236)
    bootp[0:4] = b"\x01\x01\x06\x00"
    bootp[4:8] = b"\x12\x34\x56\x78"
    bootp[10:12] = flags
    bootp[28:34] = DECK
    return bytes(bootp) + dh.MAGIC + bytes([53, 1, msg_type, 255])


def test_parse_bootp_is_the_payload_half_of_parse_dhcp():
    xid, chaddr, kind, requested, flags = dh.parse_bootp(bootp_request(dh.DISCOVER))
    assert (xid, chaddr, kind, requested, flags) == (b"\x12\x34\x56\x78", DECK, dh.DISCOVER, None, b"\x00\x00")
    assert dh.parse_bootp(bootp_request(dh.DISCOVER)[:200]) is None


def test_the_tap_server_offers_the_deck_its_address(capsys):
    """Over loopback, where 127.0.0.1 stands in for the adapter's 192.168.50.1
    and 127.0.0.10 for the lease the deck gets."""
    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.settimeout(2)
    try:
        client.bind(("127.0.0.10", 68))
    except OSError:
        pytest.skip("UDP 68 is taken on this machine")
    got = []

    def ask():
        time.sleep(0.3)
        client.sendto(bootp_request(dh.DISCOVER), ("127.0.0.1", 67))
        got.append(client.recv(1500))

    asking = threading.Thread(target=ask)
    asking.start()
    try:
        assert dh.main(["dhcp_server.py", "tap:127.0.0.1", "1"]) == 0
    except OSError:
        pytest.skip("UDP 67 is taken on this machine")
    finally:
        asking.join()
        client.close()
    offer = got[0]
    assert offer[0] == 2 and offer[4:8] == b"4Vx"
    assert socket.inet_ntoa(offer[16:20]) == "127.0.0.10"
    assert dh.parse_bootp(offer) is None
    assert "DISCOVER from 00:00:00:00:00:01" in capsys.readouterr().out
