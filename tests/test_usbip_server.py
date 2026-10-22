# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/net/usbip_server.py: the USB/IP wire format, and a scripted client
enumerating a fake deck that speaks the built-in host's datagram protocol."""
import socket
import struct
import threading
import time

import pytest

import usbip_fakehost as fakehost
import usbip_server as us

DEVICE = bytes.fromhex("12 01 00 02 00 00 00 40 73 2b 05 00 00 01 01 02 05 01")
CONFIG = (bytes.fromhex("09 02 28 00 03 01 00 c0 00")
          + bytes.fromhex("09 04 00 00 00 01 01 00 02")
          + bytes.fromhex("09 04 01 00 00 01 02 00 00")
          + bytes.fromhex("09 04 01 01 01 01 02 00 00")
          + bytes.fromhex("09 04 02 00 02 03 00 00 04"))
CONFIG = CONFIG[:2] + len(CONFIG).to_bytes(2, "little") + CONFIG[4:]
REPORT = bytes.fromhex("06 a0 ff 09 01 a1 01 c0")
STATUS = (b"\0configured vid=2b73 pid=0005 midi_in=85 midi_out=00 "
          b"hid_in=87 hid_out=06")


class FakeDeck:
    """The datagram side of usbf_host.c: answers control requests, queues OUT."""

    def __init__(self, server_port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.settimeout(0.05)
        self.server = ("127.0.0.1", server_port)
        self.out_packets = []
        self.control_log = []
        self.stall_requests = set()
        self.stop = False
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def answer(self, setup):
        kind, request = setup[0], setup[1]
        value = int.from_bytes(setup[2:4], "little")
        length = int.from_bytes(setup[6:8], "little")
        if (kind, request) in self.stall_requests:
            return False, b""
        if kind == 0x80 and request == 6:
            if value >> 8 == 1:
                return True, DEVICE[:length]
            if value >> 8 == 2:
                return True, CONFIG[:length]
        if kind == 0x81 and request == 6 and value >> 8 == 0x22:
            return True, REPORT[:length]
        return True, b""

    def run(self):
        last = 0
        while not self.stop:
            if time.time() - last > 0.1:
                self.sock.sendto(STATUS, self.server)
                last = time.time()
            try:
                data, _ = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            if data[0] == 0x80:
                setup = data[1:9]
                self.control_log.append(bytes(setup))
                ok, payload = self.answer(setup)
                reply = b"\x80" + bytes(8) + bytes([0 if ok else 1]) + payload
                self.sock.sendto(reply, self.server)
            else:
                self.out_packets.append(data)

    def send_in(self, ep, payload):
        self.sock.sendto(bytes([ep]) + bytes(8) + payload, self.server)

    def close(self):
        self.stop = True
        self.thread.join()
        self.sock.close()


@pytest.fixture
def rig():
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.bind(("127.0.0.1", 0))
    deck_link = us.UdpDeck(udp)
    stop = threading.Event()
    receiver = threading.Thread(target=us.receive_loop,
                                args=(udp, deck_link, stop.is_set), daemon=True)
    receiver.start()
    server = us.Server(deck_link)
    probe = socket.socket()
    probe.bind(("127.0.0.1", 0))
    address = probe.getsockname()
    probe.close()
    threading.Thread(target=server.listen, args=(address, stop.is_set),
                     daemon=True).start()
    fake = FakeDeck(udp.getsockname()[1])
    deadline = time.time() + 5
    while not deck_link.configured and time.time() < deadline:
        time.sleep(0.01)
    time.sleep(0.1)
    yield address, fake, deck_link
    stop.set()
    fake.close()
    receiver.join()
    udp.close()


def connect(address):
    for _ in range(50):
        try:
            return socket.create_connection(address)
        except OSError:
            time.sleep(0.02)
    raise AssertionError("no server")


def import_device(address):
    conn = connect(address)
    conn.sendall(us.op_header(us.OP_REQ_IMPORT) + us.BUSID.ljust(32, b"\0"))
    version, code, status = us.OP_HEADER.unpack(us.read_exact(conn, 8))
    assert (version, code, status) == (us.USBIP_VERSION, us.OP_REP_IMPORT, 0)
    us.read_exact(conn, us.DEVICE_FIELDS.size)
    return conn


def cmd_submit(seq, ep, direction, length, setup=bytes(8), data=b"", packets=0xFFFFFFFF,
               iso=b""):
    return (us.URB_HEADER.pack(us.CMD_SUBMIT, seq, 0x00010002, direction, ep)
            + us.CMD_SUBMIT_BODY.pack(0, length, 0, packets, 0, setup) + data + iso)


def read_ret(conn):
    command, seq, *_ = us.URB_HEADER.unpack(us.read_exact(conn, 20))
    body = us.read_exact(conn, 28)
    return command, seq, body


def control_in(conn, seq, setup_hex, length):
    setup = bytes.fromhex(setup_hex)
    conn.sendall(cmd_submit(seq, 0, 1, length, setup))
    command, rseq, body = read_ret(conn)
    assert (command, rseq) == (us.RET_SUBMIT, seq)
    status, actual = struct.unpack(">ii", body[:8])
    return status, us.read_exact(conn, actual) if actual else b""


# -- encode and decode ------------------------------------------------------------

def test_parse_descriptors():
    info = us.parse_descriptors(DEVICE, CONFIG)
    assert (info["vid"], info["pid"], info["bcd"]) == (0x2B73, 5, 0x0100)
    assert info["configs"] == 1 and info["config_value"] == 1
    assert info["interfaces"] == [(1, 1, 0), (1, 2, 0), (3, 0, 0)]


def test_device_record_layout():
    info = us.parse_descriptors(DEVICE, CONFIG)
    rec = us.device_record(info, False)
    assert len(rec) == 312
    assert struct.unpack(">3I", rec[288:300]) == (1, 2, us.SPEED_FULL)
    assert struct.unpack(">3H", rec[300:306]) == (0x2B73, 5, 0x0100)
    assert len(us.device_record(info, True)) == 312 + 4 * 3


def test_devlist_reply_empty_and_one():
    assert us.rep_devlist(None) == struct.pack(">HHII", 0x0111, 5, 0, 0)
    info = us.parse_descriptors(DEVICE, CONFIG)
    assert struct.unpack(">I", us.rep_devlist(info)[8:12]) == (1,)


def test_import_unknown_busid_fails():
    info = us.parse_descriptors(DEVICE, CONFIG)
    assert us.rep_import(info, b"9-9".ljust(32, b"\0")) == struct.pack(">HHI", 0x0111, 3, 1)
    assert us.rep_import(None, us.BUSID.ljust(32, b"\0")) == struct.pack(">HHI", 0x0111, 3, 1)


def test_ret_submit_layout():
    out = us.ret_submit(7, 0, b"abc")
    assert len(out) == 48 + 3
    assert struct.unpack(">5I", out[:20]) == (3, 7, 0, 0, 0)
    assert struct.unpack(">iiiIi", out[20:40]) == (0, 3, 0, 0xFFFFFFFF, 0)
    assert out[48:] == b"abc"


def test_ret_unlink_layout():
    out = us.ret_unlink(9, -104)
    assert len(out) == 48
    assert struct.unpack(">5I", out[:20]) == (4, 9, 0, 0, 0)
    assert struct.unpack(">i", out[20:24]) == (-104,)


def test_parse_urb_out_with_data():
    class Conn:
        def __init__(self, data):
            self.data = data

        def recv(self, n):
            chunk, self.data = self.data[:n], self.data[n:]
            return chunk

    req = us.parse_urb(Conn(cmd_submit(5, 6, 0, 4, data=b"\1\2\3\4")))
    assert (req.seqnum, req.ep, req.is_in, req.data) == (5, 6, False, b"\1\2\3\4")
    unlink = struct.pack(">5I", 2, 6, 0, 0, 0) + struct.pack(">I24x", 5)
    assert us.parse_urb(Conn(unlink)) == ("unlink", 6, 5)


# -- a scripted enumeration -------------------------------------------------------

def test_devlist_over_tcp(rig):
    address, _, _ = rig
    conn = connect(address)
    conn.sendall(us.op_header(us.OP_REQ_DEVLIST))
    head = us.read_exact(conn, 12)
    assert struct.unpack(">HHII", head) == (0x0111, 5, 0, 1)
    rec = us.read_exact(conn, 312 + 12)
    assert struct.unpack(">3H", rec[300:306]) == (0x2B73, 5, 0x0100)


def test_enumeration_like_windows(rig):
    address, fake, _ = rig
    conn = import_device(address)
    assert control_in(conn, 1, "80 06 00 01 00 00 12 00", 18) == (0, DEVICE)
    status, config = control_in(conn, 2, "80 06 00 02 00 00 ff 00", 255)
    assert (status, config) == (0, CONFIG)
    # SET_CONFIGURATION and SET_INTERFACE never reach the deck
    conn.sendall(cmd_submit(3, 0, 0, 0, bytes.fromhex("00 09 01 00 00 00 00 00")))
    assert read_ret(conn)[:2] == (us.RET_SUBMIT, 3)
    conn.sendall(cmd_submit(4, 0, 0, 0, bytes.fromhex("01 0b 01 00 01 00 00 00")))
    assert read_ret(conn)[:2] == (us.RET_SUBMIT, 4)
    assert control_in(conn, 5, "81 0a 00 00 01 00 01 00", 1) == (0, b"\x01")
    # the HID report descriptor comes from the deck
    assert control_in(conn, 6, "81 06 00 22 02 00 08 00", 8) == (0, REPORT)
    assert all(s[1] not in (9, 11) for s in fake.control_log)
    assert bytes.fromhex("81 06 00 22 02 00 08 00") in fake.control_log


def test_stalled_request_fails_with_epipe(rig):
    address, fake, _ = rig
    fake.stall_requests.add((0x21, 0x0A))
    conn = import_device(address)
    conn.sendall(cmd_submit(1, 0, 0, 0, bytes.fromhex("21 0a 00 00 02 00 00 00")))
    _, _, body = read_ret(conn)
    assert struct.unpack(">i", body[:4]) == (-us.EPIPE,)


def test_control_out_reaches_deck_with_data(rig):
    address, fake, _ = rig
    conn = import_device(address)
    setup = bytes.fromhex("21 09 00 02 02 00 04 00")
    conn.sendall(cmd_submit(1, 0, 0, 4, setup, b"\x09\x08\x07\x06"))
    command, seq, body = read_ret(conn)
    assert (command, seq, struct.unpack(">ii", body[:8])) == (us.RET_SUBMIT, 1, (0, 4))
    assert setup in fake.control_log


def test_hid_out_is_forwarded_and_logged(rig):
    address, fake, deck = rig
    seen = []
    deck.log = lambda ep, payload: ep != us.CONTROL_ENDPOINT and seen.append((ep, payload[1]))
    conn = import_device(address)
    report = bytes([1, 0x2C, 1, 0, 73, 0]) + bytes(58)
    conn.sendall(cmd_submit(1, 6, 0, 64, data=report))
    assert read_ret(conn)[:2] == (us.RET_SUBMIT, 1)
    time.sleep(0.2)
    assert fake.out_packets == [b"\x06" + report]
    assert seen == [(6, 0x2C)]


def test_interrupt_in_waits_for_data_and_unlinks(rig):
    address, fake, _ = rig
    conn = import_device(address)
    conn.sendall(cmd_submit(1, 7, 1, 64))
    time.sleep(0.2)
    fake.send_in(0x87, b"\xf0" + bytes(63))
    command, seq, body = read_ret(conn)
    assert (command, seq, struct.unpack(">ii", body[:8])) == (us.RET_SUBMIT, 1, (0, 64))
    assert us.read_exact(conn, 64)[0] == 0xF0
    conn.sendall(cmd_submit(2, 7, 1, 64))
    time.sleep(0.1)
    conn.sendall(struct.pack(">5I", 2, 3, 0, 0, 0) + struct.pack(">I24x", 2))
    command, seq, body = read_ret(conn)
    assert (command, seq, struct.unpack(">i", body[:4])) == (us.RET_UNLINK, 3, (-us.ECONNRESET,))


def test_hid_reports_are_logged_with_the_authentication_stage(tmp_path):
    path = tmp_path / "bridge.log"
    log = us.make_logger(str(path))
    log(7, bytes([1, 0xF0, 1, 0, 1, 0, 0x11]) + bytes(57))
    log(7, bytes(64))
    log(6, bytes([1, 0x2C]) + bytes(62))
    lines = [line.split(None, 1)[1] for line in path.read_text().splitlines()]
    assert lines == ["ep07 type f0 len 64 stage 11", "ep07 type 00 len 64",
                     "ep06 type 2c len 64"]


def test_control_transfers_are_logged(tmp_path):
    path = tmp_path / "bridge.log"
    log = us.make_logger(str(path))
    log(us.CONTROL_ENDPOINT, bytes.fromhex("21 09 00 02 01 00 40 00") + bytes(64))
    line = path.read_text().split(None, 1)[1].strip()
    assert line == "control 21 request 09 value 0200 index 0001 length 64 data 64"


def test_queued_in_packet_completes_a_later_urb(rig):
    address, fake, _ = rig
    conn = import_device(address)
    fake.send_in(0x87, b"\x21" * 64)
    time.sleep(0.2)
    conn.sendall(cmd_submit(1, 7, 1, 64))
    assert read_ret(conn)[:2] == (us.RET_SUBMIT, 1)
    assert us.read_exact(conn, 64) == b"\x21" * 64


def test_isochronous_in_completes_with_silence(rig):
    address, _, _ = rig
    conn = import_device(address)
    iso = b"".join(us.ISO_DESCRIPTOR.pack(i * 192, 192, 0, 0) for i in range(2))
    conn.sendall(cmd_submit(1, 4, 1, 384, packets=2, iso=iso))
    command, seq, body = read_ret(conn)
    assert (command, seq) == (us.RET_SUBMIT, 1)
    assert struct.unpack(">iiiIi", body[:20])[:2] == (0, 384)
    assert us.read_exact(conn, 384) == bytes(384)
    descs = us.read_exact(conn, 32)
    assert us.ISO_DESCRIPTOR.unpack_from(descs, 16) == (192, 192, 192, 0)


def test_fake_host_enumerates_and_sends_one_report(rig, capsys, monkeypatch):
    address, fake, _ = rig
    monkeypatch.setattr(fakehost, "submit", _short_in_timeout(fakehost.submit))
    fakehost.run(address, 0x21)
    out = capsys.readouterr().out
    assert "set configuration status 0" in out
    assert "hid out report 0x21 status 0" in out
    time.sleep(0.2)
    assert fake.out_packets == [b"" + bytes([1, 0x21]) + bytes(62)]


def _short_in_timeout(submit):
    def wrapped(conn, seq, ep, direction, *args, **kwargs):
        if ep == fakehost.HID_IN_EP:
            conn.settimeout(0.3)
        return submit(conn, seq, ep, direction, *args, **kwargs)
    return wrapped
