# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/run/midi_relay.py, cdj_panelsock.py and cdj_monsock.py: line
parsing, fan-out and the name -> port helpers. Only loopback sockets."""
import io
import os
import socket
import uuid

import pytest

import cdj_monsock
import cdj_panelsock
import midi_relay


# -- the relay's line format ---------------------------------------------------

@pytest.mark.parametrize("line, groups", [
    ("show1 0x14:1:150:or", ("show1", "0x14", "1", "150", "or")),
    ("show2 0x0e:-3:0:rot", ("show2", "0x0e", "-3", "0", "rot")),
    ("show1 0x12:16:150", ("show1", "0x12", "16", "150", None)),
    ("show1   0x190:240:150:tap", ("show1", "0x190", "240", "150", "tap")),
])
def test_line_accepts_the_bridge_format(line, groups):
    assert midi_relay.LINE.match(line).groups() == groups


@pytest.mark.parametrize("line", ["show1", "show1 14:1:150:or", "show1 0x14:1:-5:or",
                                  "0x14:1:150:or", "show1 0x14:1:150:or:x", "show1 0x14:a:1"])
def test_line_rejects_malformed(line):
    assert midi_relay.LINE.match(line) is None


class FakeSock:
    def __init__(self, fail=False):
        self.fail = fail
        self.sent = []

    def sendto(self, payload, name):
        if self.fail:
            raise OSError(111, "Connection refused")
        self.sent.append((payload, name))


def test_fanout_substitutes_the_tag_and_sends(capsys):
    f = midi_relay.Fanout("/tmp/keys-<tag>.sock", verbose=False)
    f.sock = FakeSock()
    f.send("show2", "0x10:2:150:or")
    assert f.sock.sent == [(b"0x10:2:150:or", "/tmp/keys-show2.sock")]
    assert capsys.readouterr().out == ""


def test_fanout_reports_a_missing_machine_once_and_its_return(capsys):
    f = midi_relay.Fanout("<tag>", verbose=False)
    f.sock = FakeSock(fail=True)
    for _ in range(3):
        f.send("show1", "x")
    assert capsys.readouterr().out.count("unreachable") == 1
    f.sock.fail = False
    f.send("show1", "x")
    assert "socket back" in capsys.readouterr().out
    assert f.missing == set()


class FakeState:
    def __init__(self):
        self.ensured, self.clients = [], set()

    def ensure(self, tag):
        self.ensured.append(tag)

    def add_client(self, w):
        self.clients.add(w)

    def drop_client(self, w):
        self.clients.discard(w)


def test_handler_forwards_good_lines_and_defaults_the_op(capsys):
    fanout = midi_relay.Fanout("<tag>", verbose=False)
    fanout.sock = FakeSock()
    server = type("S", (), {"fanout": fanout, "state": FakeState()})()
    h = midi_relay.Handler.__new__(midi_relay.Handler)
    h.server, h.client_address = server, ("127.0.0.1", 1)
    h.rfile = io.BytesIO(b"show1 0x14:1:150:or\r\n\n garbage \nshow2 0x12:16:150\n")
    h.wfile = io.BytesIO()
    h.handle()
    assert fanout.sock.sent == [(b"0x14:1:150:or", "show1"), (b"0x12:16:150:or", "show2")]
    assert server.state.ensured == ["show1", "show2"]
    assert server.state.clients == set()
    assert "malformed: 'garbage'" in capsys.readouterr().out


def test_state_broadcast_drops_a_dead_client():
    class Dead:
        def write(self, _):
            raise OSError("gone")

    s = midi_relay.StateIn("<tag>")
    try:
        good, dead = io.BytesIO(), Dead()
        s.add_client(good)
        s.add_client(dead)
        s._broadcast(b"show1 state frm beat=1\n")
        assert good.getvalue() == b"show1 state frm beat=1\n"
        assert s.clients == {good}
    finally:
        s.wake_r.close()
        s.wake_w.close()


# -- cdj_panelsock --------------------------------------------------------------

def fnv_port(stem):
    h = 2166136261
    for b in stem.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return 20000 + h % 25000


def test_port_is_fnv1a_of_the_stem():
    assert cdj_panelsock.port_for("cdj-panel-keys-show1") == fnv_port("cdj-panel-keys-show1")


@pytest.mark.parametrize("name", ["/tmp/cdj-panel-keys-show1.sock",
                                  "C:/msys64/tmp/cdj-panel-keys-show1.sock",
                                  "C:\\msys64\\tmp\\cdj-panel-keys-show1.sock",
                                  "cdj-panel-keys-show1"])
def test_every_spelling_of_one_socket_lands_on_one_port(name):
    assert cdj_panelsock.port_for(name) == cdj_panelsock.port_for("cdj-panel-keys-show1")


def test_tags_get_different_ports_below_the_hyperv_ranges():
    ports = {cdj_panelsock.port_for("/tmp/cdj-panel-keys-show%d.sock" % i) for i in range(1, 9)}
    assert len(ports) == 8
    assert all(20000 <= p <= 44999 for p in ports)


@pytest.mark.parametrize("name, port", [("/tmp/x:7202", 7202), ("7202", 7202), ("x:65535", 65535)])
def test_explicit_port(name, port):
    assert cdj_panelsock.port_for(name) == port


@pytest.mark.parametrize("name", ["x:1023", "x:70000", "x:"])
def test_out_of_range_explicit_port_is_hashed(name):
    assert 20000 <= cdj_panelsock.port_for(name) <= 44999


def test_addr_for_is_loopback():
    assert cdj_panelsock.addr_for("7202") == ("127.0.0.1", 7202)


def test_send_reaches_a_bound_name_and_is_bound_sees_it():
    name = "pytest-%s.sock" % uuid.uuid4().hex
    rx = cdj_panelsock.bind(name, timeout=2.0)
    try:
        assert cdj_panelsock.is_bound(name)
        cdj_panelsock.press(name, 0x14, 1)
        assert rx.recv(64) == b"0x14:1:150:or"
        cdj_panelsock.tap(name, 400, 240, 0)
        assert rx.recv(64) == b"400:240:1:tap"
        cdj_panelsock.touch(name, 1.9, 2, True)
        assert rx.recv(64) == b"1:2:1:touch"
    finally:
        rx.close()
    assert not cdj_panelsock.is_bound(name)


def test_cli_port(capsys):
    assert cdj_panelsock._main(["port", "7202"]) == 0
    assert capsys.readouterr().out.strip() == "7202"
    assert cdj_panelsock._main(["nonsense"]) == 2


# -- cdj_monsock ----------------------------------------------------------------

@pytest.fixture
def no_reservations(monkeypatch):
    monkeypatch.setattr(cdj_monsock, "_excluded", [])


def test_tcp_port_is_the_panel_hash_without_reservations(no_reservations):
    assert cdj_monsock.tcp_port("/tmp/cdj-show1-gui-mon.sock") == \
        cdj_panelsock.port_for("/tmp/cdj-show1-gui-mon.sock")


def test_tcp_port_steps_past_reserved_blocks(monkeypatch):
    p = cdj_panelsock.port_for("m")
    monkeypatch.setattr(cdj_monsock, "_excluded", [(p - 5, p + 5), (p + 6, p + 10)])
    assert cdj_monsock.tcp_port("m") == p + 11


def test_tcp_port_wraps_to_the_bottom_of_the_range(monkeypatch):
    p = cdj_panelsock.port_for("m")
    monkeypatch.setattr(cdj_monsock, "_excluded", [(p, 44999)])
    assert cdj_monsock.tcp_port("m") == 20000


def test_spec_forced_tcp(monkeypatch, no_reservations):
    monkeypatch.setenv("CDJ_MONSOCK_TCP", "1")
    port = cdj_panelsock.port_for("/tmp/a.sock")
    assert cdj_monsock.spec("/tmp/a.sock") == "tcp:127.0.0.1:%d,server=on,wait=off" % port


@pytest.mark.skipif(not hasattr(socket, "AF_UNIX") or os.name == "nt",
                    reason="unix monitor sockets only exist off Windows")
def test_spec_forced_unix(monkeypatch):
    monkeypatch.setenv("CDJ_MONSOCK_TCP", "0")
    assert cdj_monsock.spec("/tmp/a.sock") == "unix:/tmp/a.sock,server=on,wait=off"


def test_command_drains_the_greeting_and_returns_the_answer(monkeypatch, no_reservations):
    """A one-shot fake monitor on the port cdj_monsock derives for the name."""
    import threading
    monkeypatch.setenv("CDJ_MONSOCK_TCP", "1")
    lsock = socket.socket()
    lsock.bind(("127.0.0.1", 0))
    lsock.listen(1)
    port = lsock.getsockname()[1]
    got = []

    def serve():
        c, _ = lsock.accept()
        c.sendall(b"QEMU monitor\r\n(qemu) ")
        got.append(c.recv(100))
        c.sendall(b"answer\r\n(qemu) ")
        c.close()

    t = threading.Thread(target=serve, daemon=True)
    t.start()
    try:
        out = cdj_monsock.command(str(port), "info status\n", settle=0.5)
    finally:
        t.join(5)
        lsock.close()
    assert got == [b"info status\n"]
    assert out == b"answer\r\n(qemu) "
