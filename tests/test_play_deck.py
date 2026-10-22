# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/run/play_deck.py and snapshot_deck.py: waiting for a deck to settle."""
import socket

import cdj_monsock
import play_deck
import snapshot_deck as sd


class Clock:
    def __init__(self):
        self.now = 1000.0

    def time(self):
        return self.now

    def sleep(self, s):
        self.now += s


def watch(monkeypatch, screens):
    """The deck shows screens(seconds since the call) on a fake clock; returns the clock."""
    clock = Clock()
    t0 = clock.now
    monkeypatch.setattr(sd, "time", clock)
    monkeypatch.setattr(sd, "screendump", lambda mon, path: screens(clock.now - t0))
    return clock


def test_settles_at_the_minimum_when_the_screen_never_changes(monkeypatch):
    clock = watch(monkeypatch, lambda t: "idle")
    t0 = clock.now
    assert sd.wait_settled("mon", 70, "screen.ppm")
    assert 70 <= clock.now - t0 < 70 + sd.SETTLE_POLL_S


def test_a_late_redraw_restarts_the_stable_window(monkeypatch):
    clock = watch(monkeypatch, lambda t: int(t // 10) if t < 100 else "idle")
    t0 = clock.now
    assert sd.wait_settled("mon", 70, "screen.ppm")
    assert clock.now - t0 >= 100 + sd.SETTLE_S


def test_a_screen_that_keeps_changing_gives_up_at_the_timeout(monkeypatch):
    clock = watch(monkeypatch, lambda t: int(t))
    t0 = clock.now
    assert not sd.wait_settled("mon", 20, "screen.ppm")
    assert sd.SETTLE_TIMEOUT_S <= clock.now - t0 < sd.SETTLE_TIMEOUT_S + sd.SETTLE_POLL_S


class FakeMonitor:
    """A monitor that greets, and answers a screendump by writing the file and
    then the prompt. Records whether it was closed with the reply unread."""

    def __init__(self, path=None):
        self.path = path
        self.sent = []
        self.unread = [b"QEMU monitor\r\n(qemu) "]

    def settimeout(self, _):
        pass

    def recv(self, _):
        if not self.unread:
            raise socket.timeout()
        return self.unread.pop(0)

    def sendall(self, data):
        self.sent.append(data.decode())
        if self.path:
            self.path.write_bytes(b"P6 pixels")
        self.unread.append(b"screendump\r\n(qemu) ")

    def close(self):
        self.closed_unread = bool(self.unread)


def test_screendump_digests_the_file_the_monitor_wrote(monkeypatch, tmp_path):
    path = tmp_path / "screen.ppm"
    monitor = FakeMonitor(path)
    monkeypatch.setattr(cdj_monsock, "connect", lambda mon: monitor)
    first = sd.screendump("mon", str(path))
    assert monitor.sent == ["screendump %s\n" % str(path).replace("\\", "/")]
    assert not monitor.closed_unread
    monkeypatch.setattr(cdj_monsock, "connect", lambda mon: FakeMonitor(path))
    assert first == sd.screendump("mon", str(path))


def test_screendump_is_none_when_the_monitor_wrote_no_picture(monkeypatch, tmp_path):
    monkeypatch.setattr(cdj_monsock, "connect", lambda mon: FakeMonitor())
    assert sd.screendump("mon", str(tmp_path / "screen.ppm")) is None


def test_play_deck_names_the_screen_when_the_deck_never_settles(monkeypatch, capsys):
    monkeypatch.setattr(cdj_monsock, "wait_up", lambda mon: True)
    monkeypatch.setattr(play_deck, "wait_settled", lambda mon, idle_s, screen: False)
    monkeypatch.setenv("MODEL_IDLE_S", "70")
    assert play_deck.main(["mon", "30", "last.ppm"]) == 1
    assert "still changing 300 s" in capsys.readouterr().out


def test_play_deck_says_when_the_monitor_never_comes_up(monkeypatch, capsys):
    monkeypatch.setattr(cdj_monsock, "wait_up", lambda mon: False)
    assert play_deck.main(["mon", "30", "last.ppm"]) == 1
    assert "monitor never came up" in capsys.readouterr().out
