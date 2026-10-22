#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a USB stick image of synthetic test tracks whose rekordbox data is
known exactly: BPM and first downbeat, key, memory cues and hot cues.

A stick made with ./setup.sh --tracks from real music has an estimated grid
(good to a BPM or two) and no cues or key, which is fine for playing but not
for checking what a deck shows: a beat counter, a cue countdown, a phrase or
key display, a jump to a cue. Here every number is chosen up front, so a
screenshot can be held against it. No third-party music is involved.

Each track is rendered with ffmpeg's aevalsrc, to the sample:

  a 55 Hz kick on every beat,
  a 1.5 kHz click on every downbeat (beat 1 of a 4/4 bar),
  an 880 Hz tone on the first beat of every 8-bar phrase,
  silence before the first downbeat.

From there it is the same path ./setup.sh --tracks takes: collection_xml.py
writes the collection.xml -- here with the exact <TEMPO>, Tonality and
<POSITION_MARK>s instead of tags and an estimate --, baken's
`expressport --generate-analysis` writes the export (export.pdb, the ANLZ
waveforms, grid and cues, the audio) with the same arguments as setup's, and
make_usb_image.py images it as a FAT16 stick, sized the way setup sizes one.
Nothing in it is tied to one player.

  python scripts/media/make_test_stick.py [--spec tracks.json] [--out DIR] [--image FILE]
  python scripts/media/make_test_stick.py --print-spec > tracks.json

Without --spec it builds three tracks (100, 128 and 174 BPM, keys C, Am and
Fm, memory and hot cues; --print-spec shows them). A spec is JSON:

  {"device_name": "TESTSTICK", "playlist": "Test stick",
   "tracks": [{"title": "Grid 128 Am", "bpm": 128.0, "first_downbeat": 0.5,
               "bars": 96, "key": "Am",
               "memory_cues": [[5, 2.5], 17], "hot_cues": [9, 49]}]}

A cue is a bar (on its first beat) or [bar, beat] with a beat from 1 to
less than 5, fractional allowed; bars and beats count from 1. Hot cues are
A, B, ... in list order (at most 8). key is rekordbox's Tonality ("Am",
"F#m", "Db", ...), shown by the player as it is.

Result: <out>/audio/ (the tracks), <out>/collection.xml, <out>/device/ (the
export), <out>/summary.txt (every bar and cue position in seconds) and the
image, by default extract/teststick.img. The two-deck rig takes it with
  MEDIA_IMG_SRC=extract/teststick.img ./start.sh
A one-window deck always uses extract/usbmedia3.img: pass --image with that
path to replace it (./setup.sh --music or --tracks makes your own again).
An --out folder that is not empty and holds no collection.xml or summary.txt
of an earlier run is refused, since its audio/ and device/ are replaced;
--force uses it anyway.

The tracks are MP3, and LAME puts about 25 ms of padding before the first
sample: a deck that does not skip it plays the whole grid that much late.

Needs ffmpeg with libmp3lame, pyfatfs in this Python (requirements.txt), and
baken: --baken, else the copy ./setup.sh fetched into tools/baken/ or one on
PATH, else it is fetched the same way.
"""
import argparse
import copy
import importlib.util
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
for _p in (HERE, ROOT):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import collection_xml  # noqa: E402
from launcher import baken, setup  # noqa: E402
from launcher.console import Console, stdin_is_tty  # noqa: E402
from launcher.layout import Layout  # noqa: E402

RATE = 44100
BEATS_PER_BAR = 4
PHRASE_BARS = 8
HOT_CUES = "ABCDEFGH"
ARTIST = "make_test_stick"
ALBUM = "Test stick"

DEFAULT_SPEC = {
    "device_name": "TESTSTICK",
    "playlist": "Test stick",
    "tracks": [
        {"title": "Grid 128 Am", "bpm": 128.0, "first_downbeat": 0.5, "bars": 96, "key": "Am",
         "memory_cues": [[5, 2.5], 17, 33, 65], "hot_cues": [9, 49, 81]},
        {"title": "Grid 174 Fm", "bpm": 174.0, "first_downbeat": 0.0, "bars": 128, "key": "Fm",
         "memory_cues": [33, 65], "hot_cues": [1]},
        {"title": "Grid 100 C no cues", "bpm": 100.0, "first_downbeat": 1.234, "bars": 64, "key": "C"},
    ],
}


class SpecError(ValueError):
    pass


def cue_position(entry):
    """(bar, beat) of a cue entry: a bar alone is its first beat."""
    if isinstance(entry, (list, tuple)):
        if len(entry) != 2:
            raise SpecError("a cue is a bar or [bar, beat], not %r" % (entry,))
        return entry[0], entry[1]
    return entry, 1


def beat_time(track, bar, beat=1):
    """Seconds into the track of a bar and beat, both counted from 1; the
    beat may be fractional."""
    return track["first_downbeat"] + ((bar - 1) * BEATS_PER_BAR + (beat - 1)) * 60.0 / track["bpm"]


def track_length(track):
    """The whole track: up to the end of its last bar, then one second more."""
    return beat_time(track, track["bars"] + 1) + 1.0


def _number(track, key, low, high=None):
    v = track.get(key)
    if isinstance(v, bool) or not isinstance(v, (int, float)) or v < low or (high is not None and v > high):
        raise SpecError("%s: %s must be a number from %g%s, not %r"
                        % (track.get("title", "?"), key, low, " to %g" % high if high is not None else " up", v))
    return float(v)


def check_spec(spec):
    """A complete copy of spec with every value checked, defaults filled in
    and cue entries as (bar, beat); SpecError says what is wrong."""
    if not isinstance(spec, dict) or not isinstance(spec.get("tracks"), list) or not spec["tracks"]:
        raise SpecError("a spec is an object with a non-empty list of tracks")
    out = {"device_name": str(spec.get("device_name", DEFAULT_SPEC["device_name"])),
           "playlist": str(spec.get("playlist", DEFAULT_SPEC["playlist"])), "tracks": []}
    for i, track in enumerate(spec["tracks"], start=1):
        if not isinstance(track, dict) or not isinstance(track.get("title"), str) or not track["title"].strip():
            raise SpecError("track %d has no title" % i)
        t = {"title": track["title"],
             "bpm": _number(track, "bpm", 40, 300),
             "first_downbeat": _number(track, "first_downbeat", 0),
             "key": str(track.get("key", ""))}
        bars = track.get("bars")
        if isinstance(bars, bool) or not isinstance(bars, int) or bars < 1:
            raise SpecError("%s: bars must be a whole number from 1 up, not %r" % (t["title"], bars))
        t["bars"] = bars
        for kind in ("memory_cues", "hot_cues"):
            entries = track.get(kind, [])
            if not isinstance(entries, list):
                raise SpecError("%s: %s must be a list" % (t["title"], kind))
            cues = []
            for entry in entries:
                bar, beat = cue_position(entry)
                if isinstance(bar, bool) or not isinstance(bar, int) or not 1 <= bar <= bars:
                    raise SpecError("%s: cue bar %r is not a bar of this track (1 to %d)" % (t["title"], bar, bars))
                if isinstance(beat, bool) or not isinstance(beat, (int, float)) or not 1 <= beat < BEATS_PER_BAR + 1:
                    raise SpecError("%s: cue beat %r is not from 1 to less than %d"
                                    % (t["title"], beat, BEATS_PER_BAR + 1))
                cues.append((bar, beat))
            t[kind] = cues
        if len(t["hot_cues"]) > len(HOT_CUES):
            raise SpecError("%s: %d hot cues, at most %d" % (t["title"], len(t["hot_cues"]), len(HOT_CUES)))
        out["tracks"].append(t)
    return out


def marks(track):
    """(start_seconds, num) for every cue of a checked track: memory cues,
    then hot cues A, B, ... as rekordbox numbers them."""
    out = [(beat_time(track, bar, beat), collection_xml.MEMORY_CUE) for bar, beat in track["memory_cues"]]
    out += [(beat_time(track, bar, beat), num) for num, (bar, beat) in enumerate(track["hot_cues"])]
    return out


def audio_expr(track):
    """The aevalsrc expression of one channel. With n the beat index from the
    first downbeat and tau the time since that beat, every part is a decaying
    sine started at tau = 0, so each beat begins on its exact sample."""
    period = 60.0 / track["bpm"]
    t = "(t-%.6f)" % track["first_downbeat"]
    n = "floor(%s/%.9f)" % (t, period)
    tau = "(%s-%s*%.9f)" % (t, n, period)
    kick = "0.6*sin(2*PI*55*%s)*exp(-25*%s)" % (tau, tau)
    click = "0.35*eq(mod(%s,%d),0)*sin(2*PI*1500*%s)*exp(-60*%s)" % (n, BEATS_PER_BAR, tau, tau)
    phrase = "0.3*eq(mod(%s,%d),0)*sin(2*PI*880*%s)*exp(-6*%s)" % (
        n, BEATS_PER_BAR * PHRASE_BARS, tau, tau)
    return "gte(%s,0)*(%s+%s+%s)" % (t, kick, click, phrase)


def ffmpeg_argv(track, path, length=None):
    """ffmpeg's command that renders a track: MP3 at 320 kbit/s, or 16-bit
    WAV for a .wav path. The tags make it a sensible track for ./setup.sh
    --tracks too (title, and the BPM tag collection_xml.py reads)."""
    expr = audio_expr(track)
    length = track_length(track) if length is None else length
    codec = (["-c:a", "pcm_s16le"] if path.lower().endswith(".wav")
             else ["-c:a", "libmp3lame", "-b:a", "320k"])
    meta = {"title": track["title"], "artist": ARTIST, "album": ALBUM, "TBPM": "%g" % track["bpm"]}
    if track.get("key"):
        meta["TKEY"] = track["key"]
    argv = ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-f", "lavfi",
            "-i", "aevalsrc=exprs='%s|%s':s=%d:d=%.3f" % (expr, expr, RATE, length)] + codec
    for k, v in meta.items():
        argv += ["-metadata", "%s=%s" % (k, v)]
    return argv + [path]


def run(argv, what):
    """Run one step; False (with what failed on stderr) when it did not work."""
    try:
        rc = subprocess.run(argv).returncode
    except OSError as e:
        rc = e
    if rc != 0:
        sys.stderr.write("%s failed: %s\n" % (what, rc if isinstance(rc, OSError) else "exit %d" % rc))
        return False
    return True


def file_name(number, title):
    """The track's file: numbered in spec order, with what a FAT stick or
    a path cannot hold in a name replaced."""
    safe = "".join("_" if c in '\\/:*?"<>|' or ord(c) < 32 else c for c in title).strip(" .")
    return "%02d %s.mp3" % (number, safe or "track")


def have_ffmpeg():
    return shutil.which("ffmpeg") is not None


def render_audio(track, path):
    return run(ffmpeg_argv(track, path), "ffmpeg rendering %s" % os.path.basename(path))


def collection(spec, paths):
    """collection.xml for a checked spec, its tracks rendered at `paths`."""
    entries = []
    for i, (track, path) in enumerate(zip(spec["tracks"], paths), start=1):
        tags = {"title": track["title"], "artist": ARTIST, "album": ALBUM, "genre": "Test",
                "bpm": track["bpm"], "duration": int(track_length(track)),
                "bitrate": 320 if path.lower().endswith(".mp3") else 1411, "sample_rate": RATE}
        entries.append(collection_xml.track_xml(
            i, path, tags, None, [], tempo=(track["bpm"], track["first_downbeat"]),
            key=track["key"], marks=marks(track)))
    return collection_xml.document(entries, spec["playlist"], product="make_test_stick.py")


def summary(spec):
    """Every track's grid and cue positions, in seconds, as text."""
    lines = []
    for i, track in enumerate(spec["tracks"], start=1):
        lines.append("%d %s: %.2f BPM, key %s, first downbeat %.3f s, %d bars, %.1f s"
                     % (i, track["title"], track["bpm"], track["key"] or "-",
                        track["first_downbeat"], track["bars"], track_length(track)))
        phrases = range(1, track["bars"] + 1, PHRASE_BARS)
        lines.append("    phrases at bar %s" % ", ".join(str(b) for b in phrases))
        cues = [("memory cue", bar, beat) for bar, beat in track["memory_cues"]]
        cues += [("hot cue " + HOT_CUES[j], bar, beat) for j, (bar, beat) in enumerate(track["hot_cues"])]
        for label, bar, beat in cues:
            lines.append("    %-10s bar %3d beat %-4g = %8.3f s" % (label, bar, beat, beat_time(track, bar, beat)))
    return "\n".join(lines) + "\n"


def find_baken(explicit=None):
    """baken as ./setup.sh finds it (tools/baken/, then PATH), or fetched or
    cargo-installed the same way; None, with the reason printed, when that
    fails too."""
    if explicit:
        return explicit if os.path.isfile(explicit) else None
    return baken.ensure(Console(dry=False, interactive=stdin_is_tty()), Layout(), setup.Options())


def made_here(out):
    """Whether --out is missing, empty or left by an earlier run."""
    if not os.path.isdir(out) or not os.listdir(out):
        return True
    return any(os.path.isfile(os.path.join(out, n)) for n in ("collection.xml", "summary.txt"))


def default_paths():
    extract = Layout().extract
    return os.path.join(extract, "teststick"), os.path.join(extract, "teststick.img")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].replace("\n", " "))
    ap.add_argument("--spec", help="the tracks as JSON (default: the built-in three)")
    ap.add_argument("--out", help="working folder (default extract/teststick)")
    ap.add_argument("--image", help="the stick image to write (default extract/teststick.img)")
    ap.add_argument("--size-mb", type=int, help="image size (default: as ./setup.sh sizes a stick)")
    ap.add_argument("--baken", help="the baken binary (default: as ./setup.sh finds it)")
    ap.add_argument("--print-spec", action="store_true", help="print the built-in spec and stop")
    ap.add_argument("--force", action="store_true",
                    help="use an --out folder that is not one of this tool's")
    args = ap.parse_args(argv)

    if args.print_spec:
        print(json.dumps(DEFAULT_SPEC, indent=2))
        return 0
    try:
        if args.spec:
            with open(args.spec, encoding="utf-8") as f:
                raw = json.load(f)
        else:
            raw = copy.deepcopy(DEFAULT_SPEC)
        spec = check_spec(raw)
    except (OSError, ValueError) as e:
        sys.stderr.write("spec: %s\n" % e)
        return 2
    if not have_ffmpeg():
        sys.stderr.write("no ffmpeg on PATH: it renders the tracks\n")
        return 1
    if importlib.util.find_spec("pyfatfs") is None:
        sys.stderr.write("no pyfatfs in %s: python -m pip install -r requirements.txt\n" % sys.executable)
        return 1
    out, image = default_paths()
    out = os.path.abspath(args.out or out)
    image = os.path.abspath(args.image or image)
    if not args.force and not made_here(out):
        sys.stderr.write("%s is not empty and not a test stick folder; --force replaces its "
                         "audio/ and device/\n" % out)
        return 2
    baken_bin = find_baken(args.baken)
    if not baken_bin:
        sys.stderr.write("no baken at %s\n" % args.baken if args.baken else "no baken\n")
        return 1

    audio_dir, device = os.path.join(out, "audio"), os.path.join(out, "device")
    for d in (audio_dir, device):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    paths = []
    for i, track in enumerate(spec["tracks"], start=1):
        path = os.path.join(audio_dir, file_name(i, track["title"]))
        print("rendering %s (%.0f s)" % (os.path.basename(path), track_length(track)), flush=True)
        if not render_audio(track, path):
            return 1
        paths.append(path)

    xml_path = os.path.join(out, "collection.xml")
    with open(xml_path, "w", encoding="utf-8") as f:
        f.write(collection(spec, paths))
    if not run(baken.expressport_argv(baken_bin, xml_path, device, spec["device_name"]), "baken expressport"):
        return 1

    size = args.size_mb
    if size is None:
        mb, size = setup.stick_size_mb(device)
        if size > setup.STICK_MAX_MB:
            sys.stderr.write("%d MB of tracks do not fit a FAT16 stick; make fewer or shorter ones\n" % mb)
            return 1
    os.makedirs(os.path.dirname(image), exist_ok=True)
    if not run([sys.executable, os.path.join(HERE, "make_usb_image.py"), device, image, str(size)],
               "make_usb_image.py"):
        return 1
    text = summary(spec)
    with open(os.path.join(out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    sys.stdout.write(text)
    print("image: %s" % image)
    return 0


if __name__ == "__main__":
    sys.exit(main())
