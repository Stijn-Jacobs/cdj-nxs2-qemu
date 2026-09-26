# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/media/collection_xml.py and bpm_estimate.py: the rekordbox XML
generator baken's expressport reads, and the BPM fallback for a track it has
no tag for. The synthetic audio needs ffmpeg, which is not guaranteed on
every machine running the tests, so those cases are skipped without it."""
import os
import shutil
import subprocess
import types

import pytest

import bpm_estimate
import collection_xml

mutagen = pytest.importorskip("mutagen")

FFMPEG = shutil.which("ffmpeg")
needs_ffmpeg = pytest.mark.skipif(not FFMPEG, reason="no ffmpeg on PATH")


def test_encode_location_matches_bakens_own_decoder():
    # crates/baken-core/src/cdjsafe/location.rs: file://localhost, POSIX
    # slashes, ':' and '/' left bare, everything else percent-encoded.
    assert (collection_xml.encode_location("/Users/dj/M\u00fcsic/track one.mp3")
            == "file://localhost/Users/dj/M%C3%BCsic/track%20one.mp3")
    assert collection_xml.encode_location("C:/Music/track.flac") == "file://localhost/C:/Music/track.flac"


def test_kind_for_resolves_by_extension():
    assert collection_xml.kind_for("t.mp3", ".mp3", None) == "MP3 File"
    assert collection_xml.kind_for("t.flac", ".flac", None) == "FLAC File"
    assert collection_xml.kind_for("t.wav", ".wav", None) == "WAV File"
    assert collection_xml.kind_for("t.aiff", ".aiff", None) == "AIFF File"


def test_kind_for_tells_alac_from_aac_by_codec():
    # .m4a is ambiguous by extension alone; baken's own file_type_for keys
    # off Kind == "ALAC File" specifically, so this has to get it right.
    aac = types.SimpleNamespace(info=types.SimpleNamespace(codec="mp4a.40.2"))
    alac = types.SimpleNamespace(info=types.SimpleNamespace(codec="alac"))
    assert collection_xml.kind_for("t.m4a", ".m4a", aac) == "M4A File"
    assert collection_xml.kind_for("t.m4a", ".m4a", alac) == "ALAC File"
    assert collection_xml.kind_for("t.m4a", ".m4a", None) == "M4A File"


@pytest.fixture(scope="module")
def click_track(tmp_path_factory):
    """A short, exactly-timed click track: a 1 kHz burst at the start of every
    beat, silence between -- real transients, not just a modulated tone (an
    earlier attempt using ffmpeg's apulsator filter turned out not to gate
    the signal at all on the ffmpeg build this was written against)."""
    if not FFMPEG:
        pytest.skip("no ffmpeg on PATH")
    path = tmp_path_factory.mktemp("audio") / "click_150bpm.mp3"
    period = 60.0 / 150
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-f", "lavfi",
         "-i", "aevalsrc=exprs=sin(2*PI*1000*t)*lt(mod(t\\,%.6f)\\,0.04):s=44100:d=8" % period,
         "-ac", "1", "-c:a", "libmp3lame", "-b:a", "192k", str(path)],
        check=True)
    return path, 150.0


@needs_ffmpeg
def test_bpm_estimate_recovers_a_known_bpm_and_first_beat(click_track):
    path, true_bpm = click_track
    if not bpm_estimate.available():
        pytest.skip("no numpy")
    result = bpm_estimate.estimate(str(path))
    assert result is not None
    bpm, offset = result
    assert bpm == pytest.approx(true_bpm, rel=0.02)
    assert offset < 60.0 / true_bpm  # the very first beat, not some later one


@needs_ffmpeg
def test_bpm_estimate_returns_none_for_a_steady_tone(tmp_path):
    path = tmp_path / "tone.wav"
    subprocess.run(
        ["ffmpeg", "-y", "-v", "error", "-f", "lavfi",
         "-i", "sine=frequency=220:sample_rate=44100:duration=3", "-ac", "1", str(path)],
        check=True)
    if not bpm_estimate.available():
        pytest.skip("no numpy")
    assert bpm_estimate.estimate(str(path)) is None


@needs_ffmpeg
def test_build_uses_the_bpm_tag_when_present_and_estimates_otherwise(tmp_path, click_track):
    click_path, true_bpm = click_track
    tagged = tmp_path / "tagged.mp3"
    untagged = tmp_path / "untagged.mp3"
    shutil.copyfile(click_path, tagged)
    shutil.copyfile(click_path, untagged)
    audio = mutagen.File(str(tagged), easy=True)
    audio.tags["bpm"] = "%.2f" % true_bpm
    audio.save()

    xml_path = tmp_path / "collection.xml"
    xml_text, notes, n = collection_xml.build(str(tmp_path), "All Tracks")
    assert n == 2
    assert "<TEMPO Inizio=\"0.000\" Bpm=\"%.2f\"" % true_bpm in xml_text
    if bpm_estimate.available():
        assert notes == []
        estimated = float(xml_text.split("untagged.mp3")[1].split('Bpm="')[1].split('"')[0])
        assert estimated == pytest.approx(true_bpm, rel=0.02)
    else:
        assert any("untagged.mp3" in n for n in notes)
    xml_path.write_text(xml_text, encoding="utf-8")
    assert '<DJ_PLAYLISTS Version="1.0.0">' in xml_text
    assert 'Entries="2"' in xml_text


def test_build_dies_on_an_empty_folder(tmp_path):
    with pytest.raises(SystemExit):
        collection_xml.build(str(tmp_path), "All Tracks")
