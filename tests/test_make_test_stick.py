# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/media/make_test_stick.py: the spec, the grid and cue arithmetic, the
collection.xml it hands baken, and a whole run with a stand-in baken (the
real one writes the export; what matters here is what it is given and that
the result is imaged). Rendering needs ffmpeg, so that case is skipped
without it."""
import json
import os
import shutil
import stat
import subprocess
import sys
import wave
import xml.etree.ElementTree as ET

import pytest

import collection_xml
import make_test_stick as mts

FFMPEG = shutil.which("ffmpeg")


def _track(**kw):
    t = {"title": "T", "bpm": 120.0, "first_downbeat": 0.0, "bars": 16}
    t.update(kw)
    return t


def test_beat_time_counts_bars_and_beats_from_one():
    t = _track(bpm=128.0, first_downbeat=0.5)
    assert mts.beat_time(t, 1) == 0.5
    assert mts.beat_time(t, 2) == pytest.approx(0.5 + 4 * 60 / 128)
    # A cue between two beats: bar 5, beat 2.5 = 17.5 beats after the first downbeat.
    assert mts.beat_time(t, 5, 2.5) == pytest.approx(0.5 + 17.5 * 60 / 128)
    assert mts.beat_time(t, 5, 2.5) == pytest.approx(8.703125)


def test_track_length_is_every_bar_and_one_second_more():
    t = _track(bpm=120.0, first_downbeat=1.0, bars=8)
    assert mts.track_length(t) == pytest.approx(1.0 + 8 * 2.0 + 1.0)


def test_cue_position_takes_a_bar_or_a_bar_and_beat():
    assert mts.cue_position(9) == (9, 1)
    assert mts.cue_position([5, 2.5]) == (5, 2.5)
    with pytest.raises(mts.SpecError):
        mts.cue_position([5, 2, 1])


def test_the_built_in_spec_checks_and_is_left_alone():
    before = json.dumps(mts.DEFAULT_SPEC, sort_keys=True)
    spec = mts.check_spec(mts.DEFAULT_SPEC)
    assert json.dumps(mts.DEFAULT_SPEC, sort_keys=True) == before
    assert [t["title"] for t in spec["tracks"]] == ["Grid 128 Am", "Grid 174 Fm", "Grid 100 C no cues"]
    first = spec["tracks"][0]
    assert first["memory_cues"] == [(5, 2.5), (17, 1), (33, 1), (65, 1)]
    assert first["hot_cues"] == [(9, 1), (49, 1), (81, 1)]
    assert spec["tracks"][2]["memory_cues"] == [] and spec["tracks"][2]["hot_cues"] == []


def test_check_spec_fills_in_the_defaults():
    spec = mts.check_spec({"tracks": [_track()]})
    assert spec["device_name"] == "TESTSTICK"
    assert spec["playlist"] == "Test stick"
    assert spec["tracks"][0]["key"] == ""
    assert spec["bitrate"] == spec["tracks"][0]["bitrate"] == 320


@pytest.mark.parametrize("spec", [
    None,
    {},
    {"tracks": []},
    {"tracks": [_track(title="")]},
    {"tracks": [_track(bpm=0)]},
    {"tracks": [_track(bpm=True)]},
    {"tracks": [_track(bpm="128")]},
    {"tracks": [_track(first_downbeat=-0.1)]},
    {"tracks": [_track(bars=0)]},
    {"tracks": [_track(bars=8.5)]},
    {"tracks": [_track(memory_cues=[17])]},          # bar 17 of a 16-bar track
    {"tracks": [_track(memory_cues=[0])]},
    {"tracks": [_track(memory_cues=[[2, 5]])]},       # a 4/4 bar has no beat 5
    {"tracks": [_track(memory_cues=[[2, 0.5]])]},
    {"tracks": [_track(hot_cues=list(range(1, 10)))]},  # 9 hot cues, A-H is 8
    {"tracks": [_track(hot_cues=3)]},
    {"bitrate": 100, "tracks": [_track()]},          # not one of LAME's rates
    {"bitrate": "128", "tracks": [_track()]},
    {"bitrate": True, "tracks": [_track()]},
    {"tracks": [_track(tone=10)]},
    {"tracks": [_track(tone=30000)]},
], ids=lambda s: json.dumps(s)[:60])
def test_check_spec_refuses_what_cannot_be_built(spec):
    with pytest.raises(mts.SpecError):
        mts.check_spec(spec)


def test_marks_are_memory_cues_then_hot_cues_a_to_h():
    t = mts.check_spec({"tracks": [_track(bpm=120.0, first_downbeat=0.25,
                                          memory_cues=[[3, 1.5]], hot_cues=[2, 5])]})["tracks"][0]
    assert mts.marks(t) == [
        (pytest.approx(0.25 + 8.5 * 0.5), collection_xml.MEMORY_CUE),
        (pytest.approx(0.25 + 4 * 0.5), 0),
        (pytest.approx(0.25 + 16 * 0.5), 1),
    ]


@pytest.fixture
def rendered(tmp_path):
    """The built-in spec with stand-in audio files (collection() reads their
    size, nothing else)."""
    spec = mts.check_spec(mts.DEFAULT_SPEC)
    paths = []
    for i, t in enumerate(spec["tracks"], start=1):
        p = tmp_path / ("%02d %s.mp3" % (i, t["title"]))
        p.write_bytes(b"\xff" * (100 * i))
        paths.append(str(p))
    return spec, paths


def test_collection_carries_the_exact_grid_key_and_cues(rendered):
    spec, paths = rendered
    root = ET.fromstring(mts.collection(spec, paths))
    tracks = root.find("COLLECTION").findall("TRACK")
    assert root.find("COLLECTION").get("Entries") == "3"
    assert [t.get("Name") for t in tracks] == [t["title"] for t in spec["tracks"]]

    am = tracks[0]
    assert am.get("Tonality") == "Am"
    assert am.get("AverageBpm") == "128.00"
    assert am.get("Kind") == "MP3 File"
    assert am.get("Size") == "100"
    assert am.get("TotalTime") == str(int(mts.track_length(spec["tracks"][0])))
    assert am.get("Location") == collection_xml.encode_location(paths[0])
    (tempo,) = am.findall("TEMPO")
    assert (tempo.get("Inizio"), tempo.get("Bpm"), tempo.get("Metro")) == ("0.500", "128.00", "4/4")
    marks = [(m.get("Start"), m.get("Num"), m.get("Type")) for m in am.findall("POSITION_MARK")]
    assert marks == [("8.703", "-1", "0"), ("30.500", "-1", "0"), ("60.500", "-1", "0"),
                     ("120.500", "-1", "0"), ("15.500", "0", "0"), ("90.500", "1", "0"),
                     ("150.500", "2", "0")]

    c = tracks[2]
    assert c.find("TEMPO").get("Inizio") == "1.234"
    assert c.findall("POSITION_MARK") == []

    playlist = root.find("PLAYLISTS").find("NODE").find("NODE")
    assert playlist.get("Name") == "Test stick"
    assert [k.get("Key") for k in playlist.findall("TRACK")] == ["1", "2", "3"]
    assert "make_test_stick.py" in root.find("PRODUCT").get("Name")


def test_collection_does_not_estimate_what_it_already_knows(rendered, monkeypatch):
    def no(*_a, **_k):
        raise AssertionError("the estimate must not run for a track with a known grid")
    monkeypatch.setattr(collection_xml.bpm_estimate, "estimate", no)
    spec, paths = rendered
    mts.collection(spec, paths)


def test_summary_lists_every_phrase_and_cue_in_seconds():
    text = mts.summary(mts.check_spec(mts.DEFAULT_SPEC))
    assert "1 Grid 128 Am: 128.00 BPM, key Am, first downbeat 0.500 s, 96 bars" in text
    assert "phrases at bar 1, 9, 17, 25, 33, 41, 49, 57, 65, 73, 81, 89\n" in text
    assert "memory cue bar   5 beat 2.5  =    8.703 s" in text
    assert "hot cue C  bar  81 beat 1    =  150.500 s" in text
    assert "3 Grid 100 C no cues: 100.00 BPM, key C" in text


def test_file_names_are_numbered_and_safe_on_a_fat_stick():
    assert mts.file_name(1, "Grid 128 Am") == "01 Grid 128 Am.mp3"
    assert mts.file_name(12, 'a/b\\c:d*e?f"g<h>i|j') == "12 a_b_c_d_e_f_g_h_i_j.mp3"
    assert mts.file_name(3, " . ") == "03 track.mp3"


def test_ffmpeg_argv_tags_the_track_and_picks_the_codec_by_extension():
    t = mts.check_spec({"tracks": [_track(key="F#m")]})["tracks"][0]
    mp3 = mts.ffmpeg_argv(t, "/x/a.mp3")
    assert mp3[-1] == "/x/a.mp3"
    assert "libmp3lame" in mp3 and "320k" in mp3
    assert "TBPM=120" in mp3 and "TKEY=F#m" in mp3 and "title=T" in mp3
    assert ":d=%.3f" % mts.track_length(t) in mp3[mp3.index("-i") + 1]
    wav = mts.ffmpeg_argv(t, "/x/a.wav", length=2.0)
    assert "pcm_s16le" in wav and "libmp3lame" not in wav
    assert wav[wav.index("-i") + 1].endswith(":d=2.000")
    assert "TKEY=" not in " ".join(mts.ffmpeg_argv(_track(key=""), "/x/b.mp3"))


def test_ffmpeg_argv_encodes_at_the_specs_bitrate_and_320_by_default():
    def rate(spec):
        argv = mts.ffmpeg_argv(mts.check_spec(spec)["tracks"][0], "/x/a.mp3")
        return argv[argv.index("-b:a") + 1]
    assert rate({"tracks": [_track()]}) == "320k"
    assert rate({"bitrate": 128, "tracks": [_track()]}) == "128k"


def test_the_bitrate_option_overrides_the_spec_and_a_bad_one_is_refused(tmp_path, monkeypatch, capsys):
    fake, record = _fake_baken(tmp_path)
    rendered = []
    monkeypatch.setattr(mts, "render_audio", lambda track, path: rendered.append(track["bitrate"]) or _fake_render(track, path))
    monkeypatch.setattr(mts, "have_ffmpeg", lambda: True)
    pytest.importorskip("pyfatfs")
    spec = tmp_path / "spec.json"
    spec.write_text(json.dumps({"bitrate": 192, "tracks": [_track()]}))
    args = ["--spec", str(spec), "--out", str(tmp_path / "w"), "--image", str(tmp_path / "s.img"),
            "--baken", str(fake), "--size-mb", "32"]
    assert mts.main(args) == 0
    assert mts.main(args + ["--bitrate", "128"]) == 0
    assert rendered == [192, 128]
    assert mts.main(args + ["--bitrate", "100"]) == 2
    assert "bitrate" in capsys.readouterr().err


def _goertzel(x, freq):
    import numpy as np
    n = np.arange(len(x))
    w = np.hanning(len(x))
    return abs(np.sum(x * w * np.exp(-2j * np.pi * freq * n / mts.RATE))) / len(x)


@pytest.mark.skipif(not FFMPEG, reason="no ffmpeg on PATH")
def test_rendered_audio_puts_every_beat_on_its_sample(tmp_path):
    """Each beat starts within a sample or two of first_downbeat + k * period;
    the downbeat click is on beat 1 of each bar only, the phrase tone on the
    first beat of each 8-bar phrase only, and nothing sounds before the
    first downbeat."""
    np = pytest.importorskip("numpy")
    t = mts.check_spec({"tracks": [_track(bpm=150.0, first_downbeat=0.3, bars=9)]})["tracks"][0]
    path = str(tmp_path / "grid.wav")
    assert mts.run(mts.ffmpeg_argv(t, path), "ffmpeg")
    with wave.open(path) as w:
        assert (w.getnchannels(), w.getframerate(), w.getsampwidth()) == (2, mts.RATE, 2)
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, 2)
    assert (x[:, 0] == x[:, 1]).all()
    x = x[:, 0].astype(float) / 32768.0

    first = 0.3 * mts.RATE
    assert np.abs(x[:int(first)]).max() == 0.0
    period = 60.0 / t["bpm"] * mts.RATE
    win = int(0.02 * mts.RATE)
    for k in range(9 * 4):
        onset = first + k * period
        # Each beat cuts off the one before. Only the phrase tone decays
        # slowly enough to still ring loud at the next beat, so the onset is
        # found by threshold on every beat but the one after a phrase start.
        if k % 32 != 1:
            lo = int(onset) - 200
            found = lo + int(np.argmax(np.abs(x[lo:lo + 400]) > 0.003))
            assert abs(found - onset) <= 2, "beat %d starts at sample %d, not %.1f" % (k, found, onset)
        after = x[int(onset) + 1:int(onset) + 1 + win]
        click, tone = _goertzel(after, 1500), _goertzel(after, 880)
        if k % 4 == 0:
            assert click > 0.02, "beat %d is a downbeat but has no click" % k
        else:
            assert click < 0.002, "beat %d is not a downbeat but has a click" % k
        if k % 32 == 0:
            assert tone > 0.02, "beat %d starts a phrase but has no tone" % k
        else:
            assert tone < 0.01, "beat %d does not start a phrase but has a tone" % k


@pytest.mark.skipif(not FFMPEG, reason="no ffmpeg on PATH")
def test_a_tone_sounds_steadily_from_the_first_downbeat(tmp_path):
    np = pytest.importorskip("numpy")
    t = mts.check_spec({"tracks": [_track(bpm=120.0, first_downbeat=0.5, bars=2, tone=440)]})["tracks"][0]
    path = str(tmp_path / "tone.wav")
    assert mts.run(mts.ffmpeg_argv(t, path), "ffmpeg")
    with wave.open(path) as w:
        x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").reshape(-1, 2)[:, 0] / 32768.0
    assert np.abs(x[:int(0.5 * mts.RATE)]).max() == 0.0
    # Between beats (the kick has died away), the tone alone is left.
    for at in (0.9, 1.9, 2.9):
        part = x[int(at * mts.RATE):int((at + 0.05) * mts.RATE)]
        assert _goertzel(part, 440) > 0.03, "no 440 Hz tone at %.1f s" % at


def _fake_baken(tmp_path):
    """A baken that records how it was called and writes a minimal export."""
    record = tmp_path / "baken-argv.json"
    body = ("import json, os, sys\n"
            "argv = sys.argv[1:]\n"
            "json.dump(argv, open(%r, 'w'))\n"
            "device = argv[argv.index('--device') + 1]\n"
            "os.makedirs(os.path.join(device, 'PIONEER', 'rekordbox'))\n"
            "open(os.path.join(device, 'PIONEER', 'rekordbox', 'export.pdb'), 'wb').write(b'pdb')\n"
            % str(record))
    if os.name == "nt":
        # Windows cannot start a shebang script, so a .cmd runs it instead.
        script = tmp_path / "baken.py"
        script.write_text(body)
        fake = tmp_path / "baken.cmd"
        fake.write_text('@"%s" "%s" %%*\n' % (sys.executable, script))
    else:
        fake = tmp_path / "baken"
        fake.write_text("#!%s\n%s" % (sys.executable, body))
        fake.chmod(fake.stat().st_mode | stat.S_IEXEC)
    return fake, record


def _fake_render(track, path):
    with open(path, "wb") as f:
        f.write(b"\xff\xfb" * 64)
    return True


def test_a_whole_run_hands_baken_the_xml_and_images_its_export(tmp_path, monkeypatch, capsys):
    pytest.importorskip("pyfatfs")
    from test_make_usb_image import Fat16

    fake, record = _fake_baken(tmp_path)
    monkeypatch.setattr(mts, "render_audio", _fake_render)
    monkeypatch.setattr(mts, "have_ffmpeg", lambda: True)
    out, image = tmp_path / "work", tmp_path / "stick.img"
    (out / "audio").mkdir(parents=True)
    (out / "audio" / "stale.mp3").write_bytes(b"old")
    (out / "summary.txt").write_text("an earlier run")
    rc = mts.main(["--out", str(out), "--image", str(image), "--baken", str(fake), "--size-mb", "32"])
    assert rc == 0, capsys.readouterr().err

    argv = json.loads(record.read_text())
    assert argv[:2] == ["expressport", "--device"]
    assert "--generate-analysis" in argv and "--no-settings" in argv
    assert argv[argv.index("--device-name") + 1] == "TESTSTICK"
    assert argv[-1] == str(out / "collection.xml")
    xml = (out / "collection.xml").read_text(encoding="utf-8")
    assert xml.count("<TRACK TrackID=") == 3 and 'Tonality="Fm"' in xml

    assert sorted(os.listdir(out / "audio")) == [
        "01 Grid 128 Am.mp3", "02 Grid 174 Fm.mp3", "03 Grid 100 C no cues.mp3"]
    assert "hot cue A" in (out / "summary.txt").read_text(encoding="utf-8")
    assert "image: %s" % image in capsys.readouterr().out

    data = image.read_bytes()
    assert len(data) == 32 * 1024 * 1024
    assert Fat16(data[63 * 512:]).read("PIONEER/rekordbox/export.pdb") == b"pdb"


def test_a_spec_file_is_used_and_a_bad_one_refused(tmp_path, monkeypatch, capsys):
    fake, record = _fake_baken(tmp_path)
    monkeypatch.setattr(mts, "render_audio", _fake_render)
    monkeypatch.setattr(mts, "have_ffmpeg", lambda: True)
    bad = tmp_path / "bad.json"
    bad.write_text(json.dumps({"tracks": [_track(bars=0)]}))
    assert mts.main(["--spec", str(bad), "--out", str(tmp_path / "w"), "--baken", str(fake)]) == 2
    assert "bars" in capsys.readouterr().err
    assert not record.exists()

    broken = tmp_path / "broken.json"
    broken.write_text("{")
    assert mts.main(["--spec", str(broken), "--baken", str(fake)]) == 2


def test_a_folder_it_did_not_make_is_left_alone_without_force(tmp_path, monkeypatch, capsys):
    fake, record = _fake_baken(tmp_path)
    monkeypatch.setattr(mts, "render_audio", _fake_render)
    monkeypatch.setattr(mts, "have_ffmpeg", lambda: True)
    out = tmp_path / "mine"
    (out / "audio").mkdir(parents=True)
    (out / "audio" / "song.mp3").write_bytes(b"keep")
    assert mts.main(["--out", str(out), "--baken", str(fake)]) == 2
    assert "--force" in capsys.readouterr().err
    assert (out / "audio" / "song.mp3").read_bytes() == b"keep"
    assert not record.exists()

    monkeypatch.setattr(mts, "run", lambda argv, what: False)
    assert mts.main(["--out", str(out), "--baken", str(fake), "--force"]) == 1
    assert not (out / "audio" / "song.mp3").exists()


def test_print_spec_is_a_spec_that_checks(capsys):
    assert mts.main(["--print-spec"]) == 0
    printed = json.loads(capsys.readouterr().out)
    assert printed == mts.DEFAULT_SPEC
    mts.check_spec(printed)


def test_no_baken_is_a_clear_failure(tmp_path, monkeypatch, capsys):
    monkeypatch.setattr(mts, "have_ffmpeg", lambda: True)
    rc = mts.main(["--out", str(tmp_path), "--baken", str(tmp_path / "missing")])
    assert rc == 1
    assert "no baken" in capsys.readouterr().err


def test_it_runs_as_a_script():
    r = subprocess.run([sys.executable, mts.__file__, "--print-spec"], capture_output=True, text=True)
    assert r.returncode == 0, r.stderr
    assert json.loads(r.stdout)["device_name"] == "TESTSTICK"
