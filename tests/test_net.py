# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/net/: the DHCP packet parser and builder, and the Pro DJ Link
capture scorer, on hand-built frames and pcaps."""
import socket
import struct

import pytest

import dhcp_server as dh
import score_link as sl

DECK = bytes.fromhex("020000000001")
SERVER_IP = socket.inet_aton("192.168.50.1")
CLIENT_IP = socket.inet_aton("192.168.50.10")
MASK = socket.inet_aton("255.255.255.0")


def ip_udp(src_ip, dst_ip, sport, dport, payload, proto=17):
    udp = struct.pack(">HHHH", sport, dport, 8 + len(payload), 0) + payload
    ip = bytearray(struct.pack(">BBHHHBBH", 0x45, 0, 20 + len(udp), 0, 0, 64, proto, 0))
    ip += src_ip + dst_ip
    ip[10:12] = struct.pack(">H", dh.checksum(bytes(ip)))
    return bytes(ip) + udp


def eth(dst, src, body, ethertype=b"\x08\x00"):
    return dst + src + ethertype + body


def dhcp_request(msg_type, xid=b"\x12\x34\x56\x78", flags=b"\x00\x00", chaddr=DECK,
                 requested=None, pad_opts=False):
    bootp = bytearray(236)
    bootp[0:4] = b"\x01\x01\x06\x00"
    bootp[4:8] = xid
    bootp[10:12] = flags
    bootp[28:34] = chaddr
    opts = bytearray(dh.MAGIC)
    if pad_opts:
        opts += b"\x00\x00"
    opts += bytes([53, 1, msg_type])
    if requested:
        opts += bytes([50, 4]) + requested
    opts += b"\xff"
    body = ip_udp(b"\0\0\0\0", b"\xff\xff\xff\xff", 68, 67, bytes(bootp) + bytes(opts))
    return eth(b"\xff" * 6, DECK, body)


def options(frame):
    """{option: value} of a BOOTREPLY frame built by build_reply."""
    i, out = 14 + 20 + 8 + 240, {}
    while frame[i] != 255:
        if frame[i] == 0:
            i += 1
            continue
        out[frame[i]] = frame[i + 2:i + 2 + frame[i + 1]]
        i += 2 + frame[i + 1]
    return out


# -- checksum -------------------------------------------------------------------

def test_checksum_of_a_known_ip_header():
    hdr = bytes.fromhex("450000730000400040110000c0a80001c0a800c7")
    assert dh.checksum(hdr) == 0xB861


def test_checksum_pads_odd_lengths_and_verifies_to_zero():
    assert dh.checksum(b"\x01") == dh.checksum(b"\x01\x00")
    data = b"\x45\x00\x00\x1c"
    whole = data + struct.pack(">H", dh.checksum(data))
    assert dh.checksum(whole) == 0


# -- parse_dhcp -----------------------------------------------------------------

def test_parse_discover():
    xid, chaddr, kind, requested, flags, ethsrc = dh.parse_dhcp(dhcp_request(dh.DISCOVER))
    assert (xid, chaddr, kind, requested, flags, ethsrc) == \
        (b"\x12\x34\x56\x78", DECK, dh.DISCOVER, None, b"\x00\x00", DECK)


def test_parse_request_with_requested_ip_and_pad_options():
    got = dh.parse_dhcp(dhcp_request(dh.REQUEST, requested=CLIENT_IP, pad_opts=True))
    assert got[2] == dh.REQUEST and got[3] == CLIENT_IP


def test_parse_is_strict():
    good = bytearray(dhcp_request(dh.DISCOVER))
    assert dh.parse_dhcp(bytes(good[:200])) is None                  # too short
    arp = bytearray(good)
    arp[12:14] = b"\x08\x06"
    assert dh.parse_dhcp(bytes(arp)) is None
    tcp = bytearray(good)
    tcp[14 + 9] = 6
    assert dh.parse_dhcp(bytes(tcp)) is None
    djlink = bytearray(good)
    djlink[34:38] = struct.pack(">HH", 50000, 50000)
    assert dh.parse_dhcp(bytes(djlink)) is None
    reply = bytearray(good)
    reply[42] = 2                                                     # BOOTREPLY
    assert dh.parse_dhcp(bytes(reply)) is None
    nocookie = bytearray(good)
    nocookie[42 + 236] ^= 0xFF
    assert dh.parse_dhcp(bytes(nocookie)) is None


def test_parse_without_message_type_is_none():
    f = bytearray(dhcp_request(dh.DISCOVER))
    f[42 + 240] = 12                                                  # option 53 -> 12
    assert dh.parse_dhcp(bytes(f)) is None


# -- build_reply ----------------------------------------------------------------

@pytest.fixture
def offer():
    return dh.build_reply(dh.OFFER, b"\x12\x34\x56\x78", b"\0\0\0\0\0\x01", CLIENT_IP,
                          SERVER_IP, MASK, SERVER_IP, b"\x00\x00", DECK)


def test_reply_goes_to_the_asking_mac_from_the_server_mac(offer):
    assert offer[0:6] == DECK
    assert offer[6:12] == dh.SERVER_MAC
    assert offer[12:14] == b"\x08\x00"


def test_reply_broadcasts_when_the_client_asks(offer):
    f = dh.build_reply(dh.ACK, b"xxxx", DECK, CLIENT_IP, SERVER_IP, MASK, SERVER_IP,
                       b"\x80\x00", DECK)
    assert f[0:6] == b"\xff" * 6
    assert f[14 + 20 + 8 + 10:14 + 20 + 8 + 12] == b"\x80\x00"


def test_reply_eth_falls_back_to_chaddr():
    f = dh.build_reply(dh.OFFER, b"xxxx", DECK, CLIENT_IP, SERVER_IP, MASK, SERVER_IP)
    assert f[0:6] == DECK


def test_reply_ip_and_udp_checksums_verify(offer):
    ip = offer[14:34]
    assert dh.checksum(ip) == 0
    assert ip[9] == 17 and ip[12:16] == SERVER_IP and ip[16:20] == b"\xff" * 4
    udp = offer[34:]
    assert struct.unpack_from(">HHH", udp) == (67, 68, len(udp))
    pseudo = SERVER_IP + b"\xff" * 4 + bytes([0, 17]) + struct.pack(">H", len(udp))
    assert dh.checksum(pseudo + udp) == 0
    assert struct.unpack(">H", ip[2:4])[0] == len(offer) - 14


def test_reply_bootp_fields_and_options(offer):
    bootp = offer[42:]
    assert bootp[0:4] == b"\x02\x01\x06\x00"
    assert bootp[4:8] == b"\x12\x34\x56\x78"
    assert bootp[16:20] == CLIENT_IP and bootp[20:24] == SERVER_IP
    assert bootp[28:34] == b"\0\0\0\0\0\x01"          # the firmware's chaddr, echoed
    opts = options(offer)
    assert opts[53] == bytes([dh.OFFER])
    assert opts[54] == SERVER_IP and opts[1] == MASK and opts[3] == SERVER_IP
    assert struct.unpack(">I", opts[51])[0] == 86400
    assert len(offer) - 42 - 236 >= 64


def test_the_server_never_parses_its_own_reply(offer):
    assert dh.parse_dhcp(offer) is None


# -- score_link -----------------------------------------------------------------

def pcap(frames, endian="<"):
    magic = 0xA1B2C3D4
    out = struct.pack(endian + "IHHiIII", magic, 2, 4, 0, 0, 65535, 1)
    for f in frames:
        out += struct.pack(endian + "IIII", 0, 0, len(f), len(f)) + f
    return out


def announce(mac, player, name=b"CDJ-2000NXS2", typ=0x06, ip_=b"\xc0\xa8\x32\x0a"):
    pay = bytearray(sl.MAGIC + bytes([typ, 0]) + name.ljust(20, b"\0") + b"\x01\x02\x00\x36")
    pay.append(player)
    pay += bytes(20)
    return eth(b"\xff" * 6, mac, ip_udp(ip_, b"\xc0\xa8\x32\xff", 50000, 50000, bytes(pay)))


@pytest.mark.parametrize("endian", ["<", ">"])
def test_read_pcap_both_byte_orders(tmp_path, endian):
    p = tmp_path / "c.pcap"
    p.write_bytes(pcap([b"a" * 20, b"b" * 60], endian))
    assert list(sl.read_pcap(str(p))) == [b"a" * 20, b"b" * 60]


def test_read_pcap_stops_at_a_truncated_record(tmp_path):
    p = tmp_path / "c.pcap"
    p.write_bytes(pcap([b"a" * 20, b"b" * 60])[:-10])
    assert list(sl.read_pcap(str(p))) == [b"a" * 20]


@pytest.mark.parametrize("data, msg", [(b"short", "too short"), (b"\0" * 24, "not a pcap")])
def test_read_pcap_rejects_non_pcaps(tmp_path, data, msg):
    p = tmp_path / "c.pcap"
    p.write_bytes(data)
    with pytest.raises(ValueError, match=msg):
        list(sl.read_pcap(str(p)))


def test_parse_frame_kinds():
    assert sl.parse(b"x" * 10) is None
    assert sl.parse(eth(b"\xff" * 6, DECK, b"\0" * 28, b"\x08\x06"))["kind"] == "eth-0x0806"
    icmp = sl.parse(eth(b"\xff" * 6, DECK, ip_udp(SERVER_IP, CLIENT_IP, 0, 0, b"", proto=1)))
    assert icmp["kind"] == "ip-proto-1"
    u = sl.parse(announce(DECK, 1))
    assert (u["kind"], u["sport"], u["dport"]) == ("udp", 50000, 50000)
    assert u["payload"].startswith(sl.MAGIC)


def test_mac_and_ip_formatting():
    assert sl.mac(DECK) == "02:00:00:00:00:01"
    assert sl.ip(SERVER_IP) == "192.168.50.1"


def test_report_two_decks_claiming_one_player_number(tmp_path, capsys):
    other = bytes.fromhex("020000000002")
    p = tmp_path / "c.pcap"
    p.write_bytes(pcap([announce(DECK, 1), announce(DECK, 1), announce(other, 1, name=b"DECK2")]))
    sl.report(str(p))
    out = capsys.readouterr().out
    assert "3 frame(s)" in out
    assert "PLAYER NUMBER 1 is claimed by 2 decks" in out
    assert "VERDICT     : 2 deck(s)" in out
    assert "name 'DECK2'" in out and "x2" in out


def test_report_one_deck_and_no_link(tmp_path, capsys):
    p = tmp_path / "one.pcap"
    p.write_bytes(pcap([announce(DECK, 2)]))
    sl.report(str(p))
    out = capsys.readouterr().out
    assert "1 deck(s)" in out and "CONFLICT" not in out and "player 2" in out

    q = tmp_path / "dhcp.pcap"
    q.write_bytes(pcap([dhcp_request(dh.DISCOVER)]))
    sl.report(str(q))
    assert "NO PRO DJ LINK PACKET. UDP seen: 68->67=1" in capsys.readouterr().out

    e = tmp_path / "empty.pcap"
    e.write_bytes(pcap([]))
    sl.report(str(e))
    assert "NOTHING ON THE WIRE" in capsys.readouterr().out


def test_main_reports_a_bad_file_and_carries_on(tmp_path, capsys):
    bad = tmp_path / "bad.pcap"
    bad.write_bytes(b"nope")
    good = tmp_path / "good.pcap"
    good.write_bytes(pcap([]))
    assert sl.main(["score_link.py", str(bad), str(good)]) == 0
    out = capsys.readouterr().out
    assert "too short" in out and "NOTHING ON THE WIRE" in out
