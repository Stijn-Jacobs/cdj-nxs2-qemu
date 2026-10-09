# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/run/score_playhead.py, the project's motion oracle.

The pixel counters are lifted with ast (the script scores sys.argv at import).
The verdicts live inside main(), so they are checked end to end: synthetic
frames in /tmp/<tag>/, the script run as a subprocess."""
import os
import shutil
import sys
import uuid
from collections import namedtuple

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


# -- the older decks ---------------------------------------------------------------
#
# Their frames come from a directory or a glob, so these run on Windows too.

OLD = lift("scripts/run/score_playhead.py", ["THR", "Layout", "NXS2", "LAYOUTS", "marker_x"],
           {"namedtuple": namedtuple, "WAVE": None, "REMAIN": None})


def player(lay, x=None, colour=(255, 255, 255), wave=True):
    """A loaded screen in this layout: a lit top half, a blue strip, a marker."""
    w, h = lay.size
    img = Image.new("RGB", lay.size)
    img.paste((255, 255, 255), (0, 0, w, h // 4))
    x0, y0, x1, y1 = lay.wave
    if wave:
        img.paste((90, 120, 230), (x0, y0 + 4, x1, y1 - 4))
    if x is not None:
        img.paste(colour, (x, y0, x + 1, y1))
    return img


def score_dir(tmp_path, images, *opts, ext="ppm"):
    for i, img in enumerate(images):
        p = tmp_path / ("t%03d.%s" % (i * 15, ext))
        if ext == "fb":
            rgb = img.tobytes()
            p.write_bytes(b"".join((r >> 3 << 11 | g >> 2 << 5 | b >> 3).to_bytes(2, "little")
                                   for r, g, b in zip(rgb[0::3], rgb[1::3], rgb[2::3])))
        else:
            img.save(p)
    r = run_script("scripts/run/score_playhead.py", *opts, tmp_path)
    assert r.returncode == 0, r.stderr
    return r.stdout


def test_marker_is_the_whitest_or_reddest_strip_column():
    lay = OLD["LAYOUTS"]["cdj2000"]
    assert OLD["marker_x"](player(lay, 120), lay) == 120
    assert OLD["marker_x"](player(lay, 77, (230, 20, 20)), lay) == 77
    assert OLD["marker_x"](player(lay), lay) == -1


def test_a_short_column_is_not_a_marker():
    lay = OLD["LAYOUTS"]["xdj"]
    img = player(lay)
    img.paste((255, 255, 255), (300, lay.wave[1], 301, lay.wave[1] + lay.marker - 1))
    assert OLD["marker_x"](img, lay) == -1


def test_480x255_frames_pick_the_cdj2000_layout_and_a_moving_marker_is_motion(tmp_path):
    lay = OLD["LAYOUTS"]["cdj2000"]
    out = score_dir(tmp_path, [player(lay, x) for x in (40, 46, 52)])
    assert "** MOTION **" in out and "(x 40 46 52)" in out


def test_a_marker_that_only_turns_red_is_static(tmp_path):
    lay = OLD["LAYOUTS"]["cdj2000"]
    out = score_dir(tmp_path, [player(lay, 40), player(lay, 40, (230, 20, 20))])
    assert "VERDICT: STATIC -- playhead at x=40" in out


def test_frames_before_the_load_are_not_scored(tmp_path):
    lay = OLD["LAYOUTS"]["cdj2000"]
    browse = Image.new("RGB", lay.size, (255, 255, 255))
    out = score_dir(tmp_path, [browse, player(lay, 40), player(lay, 44)])
    assert "(before the load)" in out and "** MOTION **" in out


def test_a_deck_that_never_loads_is_nowave(tmp_path):
    lay = OLD["LAYOUTS"]["cdj2000"]
    out = score_dir(tmp_path, [player(lay, wave=False) for _ in range(3)])
    assert "VERDICT: NOWAVE" in out


def test_xdj_frame_buffer_dumps_score_with_the_model(tmp_path):
    lay = OLD["LAYOUTS"]["xdj"]
    out = score_dir(tmp_path, [player(lay, x) for x in (120, 128)], "--model", "xdj1000", ext="fb")
    assert "** MOTION **" in out and "(x 120 128)" in out


def test_cdj900nxs_strip_lost_is_a_repaint(tmp_path):
    lay = OLD["LAYOUTS"]["cdj900nxs"]
    out = score_dir(tmp_path, [player(lay, 130), player(lay, wave=False)], "--model", "cdj900nxs")
    assert "VERDICT: WAVELOST" in out


def glass(x=None):
    """A CDJ-900 screen: a lit waveform band and, when x is given, the playhead dot under it."""
    lay = OLD["LAYOUTS"]["cdj900"]
    img = Image.new("RGB", lay.size, (16, 32, 42))
    img.paste((159, 232, 255), (0, 0, lay.size[0], 45))
    img.paste((159, 232, 255), (lay.wave[0], lay.wave[1] + 20, lay.wave[2], lay.wave[3]))
    if x is not None:
        img.paste((159, 232, 255), (x, lay.head[1], x + 5, lay.head[3]))
    return img


def test_cdj900_playhead_dot_moving_is_motion(tmp_path):
    out = score_dir(tmp_path, [glass(x) for x in (115, 120, 135)], "--model", "cdj900")
    assert "** MOTION **" in out and "(x 115 120 135)" in out


def test_cdj900_waveform_bars_are_not_the_playhead():
    lay = OLD["LAYOUTS"]["cdj900"]
    assert OLD["marker_x"](glass(), lay) == -1


def test_an_800x480_frame_without_a_model_is_still_the_nxs2(tmp_path):
    out = score_dir(tmp_path, [with_wave(canvas(), x) for x in (200, 300)])
    assert "wave-sig" in out and "(2 distinct)" in out
