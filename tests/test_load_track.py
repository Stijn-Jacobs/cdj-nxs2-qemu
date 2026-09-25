# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/run/load_track.py helpers, lifted out with ast: the file runs its
whole key walk at import, so it is never imported."""
import socket
import time

import pytest

from helpers import lift

W, H = 800, 480
NS = lift("scripts/run/load_track.py",
          ["_PROMPT", "_read_to_prompt", "pixels", "density", "nonzero", "bright"],
          {"socket": socket, "time": time, "W": W, "H": H})


class FakeMonitor:
    """recv() hands out the chunks in order, then times out forever (or
    returns b"" for a closed peer)."""

    def __init__(self, chunks, closes=False):
        self.chunks = list(chunks)
        self.closes = closes

    def settimeout(self, _):
        pass

    def recv(self, _):
        if self.chunks:
            return self.chunks.pop(0)
        if self.closes:
            return b""
        time.sleep(0.01)
        raise socket.timeout()


# The bytes QEMU's HMP monitor really sends after a screendump: the echo, an
# erase-to-end-of-line, CRLF, then the prompt WITH its trailing space.
REAL = b"screendump /tmp/f.ppm\r\n\x1b[K\r\n(qemu) "


def test_prompt_with_trailing_space_ends_the_read_at_once():
    t0 = time.time()
    assert NS["_read_to_prompt"](FakeMonitor([REAL]), timeout=5.0)
    assert time.time() - t0 < 1.0


def test_prompt_split_across_reads():
    chunks = [REAL[i:i + 5] for i in range(0, len(REAL), 5)]
    assert NS["_read_to_prompt"](FakeMonitor(chunks), timeout=5.0)


def test_prompt_without_trailing_space_also_counts():
    assert NS["_read_to_prompt"](FakeMonitor([b"x\r\n(qemu)"]), timeout=2.0)


def test_no_prompt_times_out():
    t0 = time.time()
    assert not NS["_read_to_prompt"](FakeMonitor([b"screendump /tmp/f.ppm\r\n"]), timeout=0.3)
    assert time.time() - t0 >= 0.3


def test_closed_monitor_is_a_failure():
    assert not NS["_read_to_prompt"](FakeMonitor([b"partial"], closes=True), timeout=5.0)


def test_prompt_inside_the_stream_is_not_the_end():
    assert not NS["_read_to_prompt"](FakeMonitor([b"(qemu) info\r\nmore"]), timeout=0.3)


# -- pixel helpers --------------------------------------------------------------

def frame(fill, span=16):
    """A black frame with fill() painted into its top-left span x span corner."""
    px = bytearray(W * H * 3)
    for y in range(span):
        for x in range(span):
            c = fill(x, y)
            if c != (0, 0, 0):
                px[(y * W + x) * 3:(y * W + x) * 3 + 3] = bytes(c)
    return bytes(px)


BLACK = bytes(W * H * 3)


def test_pixels_skips_the_ppm_header(tmp_path):
    p = tmp_path / "f.ppm"
    body = bytes(range(256)) * 3
    p.write_bytes(b"P6\n800 480\n255\n" + body)
    assert NS["pixels"](str(p)) == body


def test_pixels_of_a_missing_file_is_none(tmp_path):
    assert NS["pixels"](str(tmp_path / "none.ppm")) is None
    assert NS["pixels"](None) is None


def test_nonzero():
    assert NS["nonzero"](BLACK) == 0
    assert NS["nonzero"](b"\x00\x01\x02") == 2
    assert NS["nonzero"](None) == 0 and NS["nonzero"](b"") == 0


def test_density_samples_every_third_row():
    lit_row = frame(lambda x, y: (255, 255, 255) if y == 3 else (0, 0, 0))
    rect = (0, 0, 10, 6)                       # rows 0 and 3 are sampled
    assert NS["density"](lit_row, rect) == pytest.approx(0.5)
    assert NS["density"](BLACK, rect) == 0.0
    assert NS["density"](BLACK, (0, 0, 0, 0)) == 0.0


def test_bright_counts_near_white_pixels_on_even_rows():
    px = frame(lambda x, y: (200, 200, 200) if x < 4 and y < 4 else
               ((200, 10, 10) if x < 8 and y < 4 else (0, 0, 0)))
    assert NS["bright"](px, (0, 0, 10, 4)) == 8   # 4 px on rows 0 and 2; red is not white
    assert NS["bright"](px, (0, 0, 10, 4), thr=250) == 0
