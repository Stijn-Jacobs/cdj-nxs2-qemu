# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/run/score_playhead.py, the project's motion oracle.

The pixel counters are lifted with ast (the script scores sys.argv at import).
The verdicts live inside main(), so they are checked end to end: synthetic
frames in /tmp/<tag>/, the script run as a subprocess."""
import os
import shutil
import sys
import uuid

import pytest

from helpers import lift, run_script

Image = pytest.importorskip("PIL.Image")

NS = lift("scripts/run/score_playhead.py",
          ["WAVE", "REMAIN", "ERR", "THR", "red_count", "blue_count", "full_count"])
W, H = 800, 480


def canvas():
    img = Image.new("RGB", (W, H))
    # A lit upper screen, so a frame stays healthy with or without a waveform.
    img.paste((255, 255, 255), (0, 0, W, 120))
    return img


def with_wave(img, x_marker=None):
    x0, y0, x1, y1 = NS["WAVE"]
    img.paste((20, 40, 220), (x0, y0 + 10, x1, y1 - 10))
    if x_marker is not None:
        img.paste((255, 255, 255), (x_marker, y0, x_marker + 2, y1))
    return img


# -- the counters ------------------------------------------------------------------

def test_blue_count_counts_only_blue_dominant_pixels():
    img = Image.new("RGB", (10, 10))
    img.paste((20, 40, 220), (0, 0, 10, 5))           # blue
    img.paste((200, 200, 220), (0, 5, 10, 10))        # white-ish: not blue-dominant
    assert NS["blue_count"](img, (0, 0, 10, 10)) == 50


def test_full_count_is_any_channel_over_threshold():
    img = Image.new("RGB", (10, 10))
    img.paste((NS["THR"] + 1, 0, 0), (0, 0, 3, 10))
    img.paste((NS["THR"], NS["THR"], NS["THR"]), (3, 0, 10, 10))
    assert NS["full_count"](img) == 30


def test_red_count_needs_mostly_red_rows():
    img = Image.new("RGB", (100, 4))
    img.paste((220, 10, 10), (0, 0, 100, 1))          # a solid banner row
    img.paste((220, 10, 10), (0, 1, 50, 2))           # half a row: waveform bars, ignored
    assert NS["red_count"](img, (0, 0, 100, 4)) == 100


# -- the verdicts ------------------------------------------------------------------

@pytest.fixture
def frames_dir():
    if sys.platform == "win32" or not os.path.isdir("/tmp"):
        pytest.skip("the scorer reads /tmp/<tag>/, a POSIX path")
    tag = "pytest-%s" % uuid.uuid4().hex[:12]
    d = os.path.join("/tmp", tag)
    os.makedirs(d)
    yield tag, d
    shutil.rmtree(d, ignore_errors=True)


def score(tag, d, images):
    for i, img in enumerate(images):
        img.save(os.path.join(d, "z89-%s-walk-f%03d.ppm" % (tag, i)))
    r = run_script("scripts/run/score_playhead.py", tag)
    assert r.returncode == 0, r.stderr
    return r.stdout


def test_moving_marker_over_a_waveform_is_motion(frames_dir):
    out = score(*frames_dir, [with_wave(canvas(), x) for x in (200, 300, 400)])
    assert "** MOTION **" in out and "(3 distinct)" in out


def test_identical_frames_are_static(frames_dir):
    out = score(*frames_dir, [with_wave(canvas(), 250) for _ in range(3)])
    assert "VERDICT: STATIC" in out


def test_empty_unchanging_strip_is_nowave_not_a_negative(frames_dir):
    out = score(*frames_dir, [canvas() for _ in range(3)])
    assert "VERDICT: NOWAVE" in out and "excludes NOTHING" in out


def test_lost_waveform_is_a_repaint(frames_dir):
    out = score(*frames_dir, [with_wave(canvas()), canvas()])
    assert "VERDICT: WAVELOST" in out


def test_black_frame_is_corrupt(frames_dir):
    out = score(*frames_dir, [with_wave(canvas()), Image.new("RGB", (W, H))])
    assert "VERDICT: CORRUPT" in out and "1/2 frames collapsed" in out


def test_error_banner_is_flagged(frames_dir):
    img = with_wave(canvas())
    x0, y0, x1, y1 = NS["ERR"]
    img.paste((220, 10, 10), (x0, y0, x1, y1))
    out = score(*frames_dir, [img, img])
    assert "ERROR BANNER" in out and "2 of 2 frames" in out


def test_no_frames_is_an_instrument_failure(frames_dir):
    tag, _ = frames_dir
    r = run_script("scripts/run/score_playhead.py", tag)
    assert "instrument dead" in r.stdout
