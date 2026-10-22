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


def _tags(**kw):
    tags = {"title": "", "artist": "", "album": "", "genre": "", "bpm": 0.0,
            "duration": 0, "bitrate": 0, "sample_rate": 0}
    tags.update(kw)
    return tags


def test_track_xml_writes_a_known_grid_key_and_cues_as_given(tmp_path, monkeypatch):
    # make_test_stick.py's path: nothing is read from the audio or estimated.
    def no(*_a, **_k):
        raise AssertionError("a known grid must not be estimated")
    monkeypatch.setattr(bpm_estimate, "estimate", no)
    path = tmp_path / "t.mp3"
    path.write_bytes(b"x" * 10)
    notes = []
    xml = collection_xml.track_xml(
        7, str(path), _tags(title="T & U"), None, notes, tempo=(128.0, 0.5), key="F#m",
        marks=[(8.703125, collection_xml.MEMORY_CUE), (15.5, 0)])
    assert notes == []
    assert xml.startswith('    <TRACK TrackID="7" Name="T &amp; U" ')
    assert 'AverageBpm="128.00"' in xml and 'Tonality="F#m"' in xml
    assert xml.endswith(
        '>\n      <TEMPO Inizio="0.500" Bpm="128.00" Metro="4/4" Battito="1"/>\n'
        '      <POSITION_MARK Name="" Type="0" Start="8.703" Num="-1"/>\n'
        '      <POSITION_MARK Name="" Type="0" Start="15.500" Num="0"/>\n'
        '    </TRACK>\n')


def test_track_xml_without_a_grid_or_cues_is_one_empty_element(tmp_path, monkeypatch):
    monkeypatch.setattr(bpm_estimate, "available", lambda: False)
    path = tmp_path / "t.wav"
    path.write_bytes(b"x")
    notes = []
    xml = collection_xml.track_xml(1, str(path), _tags(), None, notes)
    assert xml.endswith('Tonality=""/>\n')
    assert notes and "no beat grid" in notes[0]


def test_cues_without_a_grid_still_make_a_track_with_children(tmp_path, monkeypatch):
    monkeypatch.setattr(bpm_estimate, "available", lambda: False)
    path = tmp_path / "t.wav"
    path.write_bytes(b"x")
    xml = collection_xml.track_xml(1, str(path), _tags(), None, [], marks=[(1.0, 2)])
    assert "<TEMPO" not in xml
    assert '<POSITION_MARK Name="" Type="0" Start="1.000" Num="2"/>' in xml
    assert xml.endswith("    </TRACK>\n")


def test_document_wraps_the_tracks_in_one_playlist():
    doc = collection_xml.document(["    <TRACK TrackID=\"1\"/>\n", "    <TRACK TrackID=\"2\"/>\n"],
                                  "A & B", product="x.py")
    assert '<PRODUCT Name="cdj-nxs2-re x.py"' in doc
    assert '<COLLECTION Entries="2">' in doc
    assert '<NODE Name="A &amp; B" Type="1" KeyType="0" Entries="2">' in doc
    assert doc.index('<TRACK Key="1"/>') < doc.index('<TRACK Key="2"/>')
    assert collection_xml.document([], "x").count("<TRACK") == 0
