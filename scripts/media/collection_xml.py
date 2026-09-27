#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a rekordbox collection.xml from a plain folder of music, for baken's
`expressport --generate-analysis` to write a USB export from (baken:
github.com/M-Igashi/baken, MIT).

baken's own XML parser only needs TrackID, Name, Location and enough of Kind
plus the file extension to resolve a file type; TotalTime/BitRate/SampleRate/
AverageBpm can be left at 0 and baken fills them in from the decoded audio
when it generates analysis itself. What it does not do is detect a beat grid
from the audio -- a <TEMPO> child is copied straight onto the ANLZ beat-grid
tag, so a BPM known up front belongs here. This reads a BPM tag when the file
has one, and falls back to bpm_estimate.py's own estimator otherwise; a track
with neither loads with no beat grid rather than failing.

Location must match baken's own decoder (a rekordbox "file://localhost" URL,
POSIX slashes, percent-encoded, ':' and '/' left bare) -- reimplemented here
rather than shelling out to baken for it, since this script has no other
reason to depend on it at run time.

usage: python collection_xml.py <music_dir> <collection.xml> [--playlist NAME]
"""
import argparse
import os
import sys
import xml.sax.saxutils as sax

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpm_estimate  # noqa: E402

try:
    import mutagen
except ImportError:
    mutagen = None

KIND_BY_EXT = {
    ".mp3": "MP3 File",
    ".flac": "FLAC File",
    ".wav": "WAV File",
    ".aiff": "AIFF File",
    ".aif": "AIFF File",
}
# .m4a/.aac need the container's codec to tell AAC from ALAC; see kind_for().
CONTAINER_EXT = {".m4a", ".aac", ".mp4"}
AUDIO_EXT = set(KIND_BY_EXT) | CONTAINER_EXT


def encode_location(path):
    posix = path.replace("\\", "/")
    if not posix.startswith("/"):
        posix = "/" + posix
    unreserved = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~/:"
    out = ["file://localhost"]
    for byte in posix.encode("utf-8"):
        c = chr(byte)
        if byte < 128 and c in unreserved:
            out.append(c)
        else:
            out.append("%%%02X" % byte)
    return "".join(out)


def kind_for(path, ext, audio):
    if ext not in CONTAINER_EXT:
        return KIND_BY_EXT[ext]
    codec = getattr(getattr(audio, "info", None), "codec", "") if audio else ""
    return "ALAC File" if codec == "alac" else "M4A File"


def read_tags(path):
    """Best-effort tag read; every field defaults to empty/zero, which baken
    accepts and fills in from the audio itself where it can."""
    tags = {"title": "", "artist": "", "album": "", "genre": "", "bpm": 0.0,
            "duration": 0, "bitrate": 0, "sample_rate": 0}
    audio = None
    if mutagen is None:
        return tags, audio
    try:
        audio = mutagen.File(path, easy=True)
    except Exception:
        return tags, None
    if audio is None:
        return tags, None
    if audio.tags:
        for key in ("title", "artist", "album", "genre", "bpm"):
            values = audio.tags.get(key)
            if values:
                tags[key] = values[0]
    try:
        tags["bpm"] = float(tags["bpm"]) if tags["bpm"] else 0.0
    except ValueError:
        tags["bpm"] = 0.0
    info = getattr(audio, "info", None)
    if info is not None:
        tags["duration"] = int(getattr(info, "length", 0) or 0)
        tags["bitrate"] = int(getattr(info, "bitrate", 0) or 0) // 1000
        tags["sample_rate"] = int(getattr(info, "sample_rate", 0) or 0)
    return tags, audio


OCTAVE_TOLERANCE = 0.06   # how near 2x or 1/2 the estimate must sit to the tag


def tag_octave(path, bpm, notes):
    """The tag's BPM, doubled or halved when the audio says it is an octave
    off. Taggers often write half time for fast tracks (103 for a 206 BPM
    hardstyle edit), and the deck would then show and grid it at half speed.
    The tag stays the source of the value -- it is usually exact where the
    estimate is only good to a BPM or two -- and the estimate only picks the
    octave. Without ffmpeg/numpy, or with no clear beat, the tag is kept."""
    if not bpm_estimate.available():
        return bpm
    estimated = bpm_estimate.estimate(path)
    if not estimated:
        return bpm
    for factor in (2.0, 0.5):
        if abs(estimated[0] / (bpm * factor) - 1.0) <= OCTAVE_TOLERANCE:
            notes.append("%s: BPM tag %g is an octave off the audio (~%.0f); using %g"
                         % (os.path.basename(path), bpm, estimated[0], bpm * factor))
            return bpm * factor
    return bpm


def track_xml(track_id, path, tags, audio, notes):
    ext = os.path.splitext(path)[1].lower()
    name = tags["title"] or os.path.splitext(os.path.basename(path))[0]
    tempo = None
    if tags["bpm"] > 0:
        tempo = (tag_octave(path, tags["bpm"], notes), 0.0)
    elif bpm_estimate.available():
        estimated = bpm_estimate.estimate(path)
        if estimated:
            tempo = estimated
        else:
            notes.append("%s: no clear beat found; no beat grid" % os.path.basename(path))
    else:
        notes.append("%s: no BPM tag, and no ffmpeg/numpy to estimate one; no beat grid"
                      % os.path.basename(path))

    attrs = {
        "TrackID": str(track_id),
        "Name": name,
        "Artist": tags["artist"],
        "Album": tags["album"],
        "Genre": tags["genre"],
        "Kind": kind_for(path, ext, audio),
        "Size": str(os.path.getsize(path)),
        "TotalTime": str(tags["duration"]),
        "AverageBpm": "%.2f" % (tempo[0] if tempo else 0.0),
        "BitRate": str(tags["bitrate"]),
        "SampleRate": str(tags["sample_rate"]),
        "Location": encode_location(os.path.abspath(path)),
        "Tonality": "",
    }
    line = "    <TRACK " + " ".join(
        '%s="%s"' % (k, sax.escape(v)) for k, v in attrs.items())
    if tempo is None:
        return line + "/>\n"
    bpm, offset = tempo
    return line + ">\n      <TEMPO Inizio=\"%.3f\" Bpm=\"%.2f\" Metro=\"4/4\" Battito=\"1\"/>\n    </TRACK>\n" \
        % (offset, bpm)


def build(music_dir, playlist_name):
    """(xml_text, notes): notes lists every track that ends up with no beat
    grid and why, for the caller to show."""
    files = sorted(
        os.path.join(music_dir, n) for n in os.listdir(music_dir)
        if os.path.splitext(n)[1].lower() in AUDIO_EXT
    )
    if not files:
        raise SystemExit("no audio files (%s) found in %s" % (", ".join(sorted(AUDIO_EXT)), music_dir))

    notes = []
    tracks = []
    for i, path in enumerate(files, start=1):
        tags, audio = read_tags(path)
        tracks.append(track_xml(i, path, tags, audio, notes))

    out = ['<?xml version="1.0" encoding="UTF-8"?>\n',
           '<DJ_PLAYLISTS Version="1.0.0">\n',
           '  <PRODUCT Name="cdj-nxs2-re collection_xml.py" Version="1" Company="none"/>\n',
           '  <COLLECTION Entries="%d">\n' % len(tracks)]
    out.extend(tracks)
    out.append('  </COLLECTION>\n')
    out.append('  <PLAYLISTS>\n')
    out.append('    <NODE Type="0" Name="ROOT" Count="1">\n')
    out.append('      <NODE Name="%s" Type="1" KeyType="0" Entries="%d">\n'
                % (sax.escape(playlist_name), len(tracks)))
    for i in range(1, len(tracks) + 1):
        out.append('        <TRACK Key="%d"/>\n' % i)
    out.append('      </NODE>\n')
    out.append('    </NODE>\n')
    out.append('  </PLAYLISTS>\n')
    out.append('</DJ_PLAYLISTS>\n')
    return "".join(out), notes, len(tracks)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("music_dir")
    ap.add_argument("collection_xml")
    ap.add_argument("--playlist", default="All Tracks")
    args = ap.parse_args(argv)

    if mutagen is None:
        sys.exit("no mutagen: python -m pip install mutagen (see requirements.txt)")
    xml_text, notes, n = build(args.music_dir, args.playlist)
    with open(args.collection_xml, "w", encoding="utf-8") as f:
        f.write(xml_text)
    for note in notes:
        print("note: %s" % note)
    print("wrote %s (%d tracks, playlist %r)" % (args.collection_xml, n, args.playlist))


if __name__ == "__main__":
    main(sys.argv[1:])
