# SPDX-License-Identifier: GPL-2.0-or-later
"""setup.py's USB step: it now offers two sources -- a rekordbox export
(--music, unchanged) or a plain folder of music that baken analyses
(--tracks) -- and both must show up in --dry-run, non-interactively, exactly
like the rest of setup's steps do."""
import os
import sys

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher.console import Console  # noqa: E402
from launcher.layout import Layout  # noqa: E402
from launcher.setup import Options, Setup  # noqa: E402


def _setup(tmp_path, dry=True, **opts):
    o = Options()
    o.dry = dry
    o.yes = True
    for k, v in opts.items():
        setattr(o, k, v)
    s = Setup(o)
    s.lay = Layout()
    s.lay.extract = str(tmp_path)  # usb_image is computed from this, freshly
    s.con = Console(dry=dry, interactive=False)
    s.py_tools = sys.executable
    return s


def test_dry_run_previews_both_sources_when_neither_is_given(tmp_path, capsys):
    s = _setup(tmp_path)
    s.step_usb()
    out = capsys.readouterr().out
    assert "make_usb_image.py <your rekordbox export>" in out
    assert "collection_xml.py" in out and "baken expressport" in out


def test_dry_run_music_takes_the_rekordbox_path(tmp_path, capsys):
    s = _setup(tmp_path, music=str(tmp_path))
    s.step_usb()
    out = capsys.readouterr().out
    assert "make_usb_image.py" in out
    assert "baken" not in out


def test_dry_run_tracks_takes_the_baken_path(tmp_path, capsys):
    music = tmp_path / "music"
    music.mkdir()
    s = _setup(tmp_path, tracks=str(music))
    s.step_usb()
    out = capsys.readouterr().out
    assert "download the matching baken release asset" in out
    assert "collection_xml.py" in out
    assert "expressport" in out
    assert "make_usb_image.py" in out


def test_music_and_tracks_together_is_refused(tmp_path):
    s = _setup(tmp_path, music=str(tmp_path), tracks=str(tmp_path))
    with pytest.raises(SystemExit):
        s.step_usb()


def test_an_existing_image_is_kept_non_interactively(tmp_path, capsys):
    os.makedirs(tmp_path, exist_ok=True)
    with open(os.path.join(str(tmp_path), "usbmedia3.img"), "wb") as f:
        f.write(b"\0" * 1024)
    s = _setup(tmp_path)
    s.step_usb()
    out = capsys.readouterr().out
    assert "USB image:" in out
    assert "collection_xml.py" not in out
