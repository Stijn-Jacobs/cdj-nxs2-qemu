# SPDX-License-Identifier: GPL-2.0-or-later
"""deck_smoke.py --keep-frames: which frame a loaded deck keeps, and the PNG
and verdict it leaves for the README showcase. The monitor is stubbed, so no
QEMU runs."""

import configparser
import json
import os
import sys

import pytest

from helpers import path

sys.path.insert(0, path("scripts", "test"))
import deck_smoke  # noqa: E402

Image = pytest.importorskip("PIL.Image")

W, H = 800, 480


def pixels(level):
    return bytes([level, 255 - level, 7]) * (W * H)


def deck(tmp_path, fb):
    ini = configparser.ConfigParser()
    ini.read_string("[xdj1000]\nmachine = xdj1000\nqemu = %s\nkernel = k\ngolden = g.ppm\nport = 1\n%s"
                    % (sys.executable, "fb = 0x14600000 800 480\n" if fb else ""))
    keep = tmp_path / "keep"
    keep.mkdir()
    return deck_smoke.Deck("xdj1000", ini["xdj1000"], str(tmp_path), str(tmp_path), str(tmp_path), str(keep))


def stub_screendump(monkeypatch, frame):
    """A console that writes `frame` (or nothing, for None) on screendump."""
    def monitor(port, line, timeout=5.0):
        if line.startswith("screendump ") and frame is not None:
            deck_smoke.write_ppm(line.split(" ", 1)[1], *frame)
        return b""
    monkeypatch.setattr(deck_smoke, "monitor", monitor)


def test_png_round_trips(tmp_path):
    out = tmp_path / "f.png"
    deck_smoke.write_png(str(out), W, H, pixels(40))
    with Image.open(out) as img:
        assert img.size == (W, H) and img.mode == "RGB"
        assert img.tobytes() == pixels(40)


def test_frame_buffer_deck_keeps_the_console_screendump(tmp_path, monkeypatch):
    d = deck(tmp_path, fb=True)
    fb_frame = tmp_path / "motion-2.ppm"
    deck_smoke.write_ppm(str(fb_frame), W, H, pixels(10))
    stub_screendump(monkeypatch, (W, H, pixels(200)))
    deck_smoke.keep_frame(d, str(fb_frame))
    assert d.kept[1] == "screendump"
    assert deck_smoke.read_ppm(d.kept[0])[2] == pixels(200)


@pytest.mark.parametrize("console", [None, (640, 480, bytes(640 * 480 * 3))])
def test_frame_buffer_deck_without_its_console_keeps_the_read(tmp_path, monkeypatch, console):
    d = deck(tmp_path, fb=True)
    fb_frame = tmp_path / "motion-2.ppm"
    deck_smoke.write_ppm(str(fb_frame), W, H, pixels(10))
    stub_screendump(monkeypatch, console)
    deck_smoke.keep_frame(d, str(fb_frame))
    assert d.kept == (str(fb_frame), "framebuffer")


def test_nothing_is_kept_without_the_option(tmp_path):
    d = deck(tmp_path, fb=False)
    d.keep_dir = ""
    deck_smoke.keep_frame(d, "unused.ppm")
    assert d.kept is None


def test_saved_frame_carries_its_verdict(tmp_path, monkeypatch):
    d = deck(tmp_path, fb=False)
    frame = tmp_path / "xdj1000.ppm"
    deck_smoke.write_ppm(str(frame), W, H, pixels(90))
    deck_smoke.keep_frame(d, str(frame))
    d.result, d.detail = "PASS", "screen matched at 12 s; playhead MOTION"
    deck_smoke.save_kept(d)
    with Image.open(os.path.join(d.keep_dir, "xdj1000.png")) as img:
        assert img.tobytes() == pixels(90)
    with open(os.path.join(d.keep_dir, "xdj1000.json")) as f:
        side = json.load(f)
    assert side["verdict"] == "PASS" and side["firmware"] == "1.13"
    assert side["source"] == "screendump" and side["size"] == [W, H]
    assert side["qemu"] == d.qemu
