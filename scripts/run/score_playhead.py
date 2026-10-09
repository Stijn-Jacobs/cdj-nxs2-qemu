#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Score the overview strip of a run's screen captures for motion, and tell
motion apart from a frame that broke.

A whole-frame pixel count cannot tell "the value was drawn" from "the frame
stopped rendering": in a packed MAIN->GUI frame most pokes (e.g. CDJ_LINKRAMP)
corrupt rather than display. So each frame is scored on:

  full     lit pixels over the whole frame (health)
  wave     blue-dominant pixels in the overview strip (100,375)-(700,432);
           the strip has a white baseline that lights up with or without a
           waveform, so only blue counts
  wsig     a hash of the strip's bytes (any change at all)
  rsig     a hash of the REMAIN rectangle (285,315)-(495,370)
  err      red pixels of the E-8302 error banner

Verdicts:

  CORRUPT   a frame went black or its lit count collapsed; not a hit
  NOWAVE    no waveform and nothing moved; the run excludes nothing
  WAVELOST  the strip had a waveform and lost it; something repainted it
  MOTION    healthy frames, strip hash changes across them
  STATIC    healthy frames, strip hash identical throughout

The other decks draw their overview elsewhere, so --model picks their layout
(and without it a 480x255 frame is taken as a CDJ-2000/CDJ-2000NXS one). On
those decks the strip is judged by the playhead marker's column instead of the
strip hash: a paused deck's marker turns red in place, which changes the hash
without moving anything. Their runs are filmed from boot, so the frames before
the first waveform are listed but not scored. The error banner is only known
on the NXS2.

usage: score_playhead.py [--model ID] <tag|dir|glob> [...]
       a tag reads /tmp/<tag>/z89-<tag>-*-f*.ppm; FRAMES_ROOT=<dir> overrides
       /tmp (a native Python reads /tmp as C:/tmp). A directory or a glob reads
       those frames (.ppm, .png, or raw RGB565 .fb dumps) in the order of the
       numbers in their names.
"""
import argparse
import glob
import hashlib
import os
import re
import struct
from collections import namedtuple

from PIL import Image

WAVE = (100, 375, 700, 432)
REMAIN = (285, 315, 495, 370)
# The red error banner across the readout, "E-8302: CANNOT PLAY TRACK(000B)".
# The firmware's ErrEnd log line does not always accompany it.
# The band avoids the red centre line (y 120-270) and the red +/-10 box (x>700).
ERR = (200, 378, 640, 408)
THR = 150
# A frame whose lit count falls below this fraction of the run's best frame is
# treated as corrupt rather than as a change.
COLLAPSE = 0.5

# wave is the overview strip; on the decks with a marker it is also where the
# marker is looked for (head, when the marker has a row of its own), and a
# column needs `marker` marker pixels to count. A monochrome glass has no white
# or red marker: its marker is a lit dot.
Layout = namedtuple("Layout", "size wave remain marker head lit", defaults=(None, False))
NXS2 = Layout((800, 480), WAVE, REMAIN, 0)
LAYOUTS = {
    # 480x255 screendumps: the overview strip under the readout, the marker
    # starting at x=40, REMAIN as minutes, seconds and frames.
    "cdj2000": Layout((480, 255), (30, 198, 446, 222), (135, 158, 300, 185), 15),
    # The XDJ touch screen: overview at the bottom left, REMAIN beside TRACK.
    "xdj": Layout((800, 480), (105, 408, 640, 442), (290, 360, 470, 400), 20),
    # The CDJ-900NXS: overview above the bottom edge, REMAIN centre right.
    "cdj900nxs": Layout((800, 480), (128, 350, 632, 400), (380, 295, 540, 340), 20),
    # The CDJ-900 glass, 180x45 dots drawn 5x5 with the readout dots below:
    # the overview fills dot columns 23-122 of the last text band (rows 36-43)
    # and its playhead is the dot under it, row 44.
    "cdj900": Layout((900, 295), (115, 180, 615, 220), (180, 225, 575, 295), 5,
                     (115, 220, 615, 225), True),
}
MODELS = {
    "cdj2000nxs2": NXS2,
    "cdj2000": LAYOUTS["cdj2000"],
    "cdj2000nxs": LAYOUTS["cdj2000"],
    "xdj1000": LAYOUTS["xdj"],
    "xdj700": LAYOUTS["xdj"],
    "cdj900nxs": LAYOUTS["cdj900nxs"],
    "cdj900": LAYOUTS["cdj900"],
}


def load(path, size=(800, 480)):
    if not path.endswith(".fb"):
        return Image.open(path).convert("RGB")
    w, h = size
    img = Image.new("RGB", size)
    img.putdata([((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)
                 for (v,) in struct.iter_unpack("<H", open(path, "rb").read()[:w * h * 2])])
    return img


def crop_bytes(img, rect):
    return img.crop(rect).tobytes()


def red_count(img, rect):
    """Red-dominant pixels in rows that are mostly red -- the banner is a solid
    fill. The colour overview strip's red bars also fall in this band, but no
    row of them is ever mostly red."""
    crop = img.crop(rect)
    w, h = crop.size
    px = list(crop.getdata())
    n = 0
    for y in range(h):
        row = sum(1 for r, g, b in px[y * w:(y + 1) * w]
                  if r > 120 and r > g * 2 and r > b * 2)
        if row > w * 0.6:
            n += row
    return n


def blue_count(img, rect):
    """Blue-dominant pixels only -- the overview waveform's own colour."""
    n = 0
    for r, g, b in img.crop(rect).getdata():
        if b > THR and b > r + 20 and b > g + 20:
            n += 1
    return n


def full_count(img):
    n = 0
    for r, g, b in img.getdata():
        if r > THR or g > THR or b > THR:
            n += 1
    return n


def marker_x(img, lay):
    """The strip column with the most white or red pixels (the playhead is
    white playing and red paused; lit on a monochrome glass), or -1 when none
    has lay.marker of them."""
    x0, y0, x1, y1 = lay.head or lay.wave
    px = img.load()
    best, at = 0, -1
    for x in range(x0, x1):
        n = sum(1 for y in range(y0, y1)
                if (px[x, y][2] > THR if lay.lit else
                    px[x, y][0] > 200 and (min(px[x, y]) > 200 or max(px[x, y][1:]) < 80)))
        if n > best:
            best, at = n, x
    return at if best >= lay.marker else -1


def frames_of(arg):
    """The frames an argument names: a run tag's first, else a directory's or
    a glob's."""
    root = os.environ.get("FRAMES_ROOT", "/tmp")
    tagged = sorted(glob.glob("%s/%s/z89-%s-*-f*.ppm" % (root, arg, arg)))
    if tagged or not (os.path.isdir(arg) or glob.has_magic(arg)):
        return tagged
    files = glob.glob(os.path.join(arg, "*")) if os.path.isdir(arg) else glob.glob(arg)
    files = [f for f in files if f.endswith((".ppm", ".png", ".fb"))]
    return sorted(files, key=lambda f: [int(n) for n in re.findall(r"\d+", os.path.basename(f))])


def layout_of(model, first):
    if model:
        return MODELS[model]
    if not first.endswith(".fb") and Image.open(first).size == LAYOUTS["cdj2000"].size:
        return LAYOUTS["cdj2000"]
    return NXS2


def score_marker(lay, frames):
    """The verdict for a deck whose playhead marker is what moves."""
    rows = []
    for f in frames:
        img = load(f, lay.size)
        rows.append((full_count(img), blue_count(img, lay.wave), marker_x(img, lay),
                     hashlib.sha1(crop_bytes(img, lay.remain)).hexdigest()[:8]))
    # The browse and source screens before the load are much brighter or
    # darker than the player's, and would read as collapses.
    start = next((i for i, r in enumerate(rows) if r[1]), len(rows))
    best = max([r[0] for r in rows[start:]] or [1])
    print("  %-4s %-9s %-7s %-9s %s" % ("#", "full", "wave", "playhead", "remain-sig"))
    for i, (fu, wv, x, rs) in enumerate(rows):
        flag = "  (before the load)" if i < start else "  <-- CORRUPT" if fu < best * COLLAPSE else ""
        print("  %-4d %-9d %-7d %-9d %s%s  %s" % (i, fu, wv, x, rs, flag, os.path.basename(frames[i])))

    loaded = rows[start:]
    healthy = [r for r in loaded if r[0] >= best * COLLAPSE]
    seen = [r[2] for r in healthy if r[2] >= 0]
    if not loaded:
        print("  VERDICT: NOWAVE -- no waveform in the strip in any frame, so "
              "this run excludes NOTHING. Not a negative.")
    elif len(healthy) < len(loaded):
        print("  VERDICT: CORRUPT -- %d/%d frames collapsed. This is "
              "'we broke it', not a hit." % (len(loaded) - len(healthy), len(loaded)))
    elif min(r[1] for r in healthy) == 0:
        print("  VERDICT: WAVELOST -- the strip had a waveform (%d px) and "
              "lost it (%d px). Something REPAINTED the strip; it is not a "
              "playhead." % (max(r[1] for r in healthy),
                             min(r[1] for r in healthy)))
    elif len(set(seen)) > 1:
        print("  VERDICT: ** MOTION ** -- the playhead moves across healthy "
              "frames (x %s)" % " ".join(str(x) for x in seen))
    else:
        print("  VERDICT: STATIC -- playhead at x=%s in every healthy frame "
              "(remain-sig distinct: %d)" % (seen[0] if seen else "none",
                                             len({r[3] for r in healthy})))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", choices=sorted(MODELS))
    ap.add_argument("runs", nargs="+")
    args = ap.parse_args()
    for tag in args.runs:
        frames = frames_of(tag)
        print("=" * 70)
        print("%s   %d frames" % (tag, len(frames)))
        if not frames:
            print("  no frames -- instrument dead, not a measurement")
            continue
        lay = layout_of(args.model, frames[0])
        if lay is not NXS2:
            score_marker(lay, frames)
            continue
        rows = []
        for f in frames:
            img = load(f)
            rows.append((full_count(img), blue_count(img, WAVE),
                         hashlib.sha1(crop_bytes(img, WAVE)).hexdigest()[:8],
                         hashlib.sha1(crop_bytes(img, REMAIN)).hexdigest()[:8],
                         red_count(img, ERR)))
        best = max(r[0] for r in rows) or 1
        print("  %-4s %-9s %-7s %-9s %-11s %s" % ("#", "full", "wave",
                                                  "wave-sig", "remain-sig",
                                                  "err"))
        for i, (fu, wv, ws, rs, er) in enumerate(rows):
            flag = "  <-- CORRUPT" if fu < best * COLLAPSE else ""
            if er > 100:
                flag += "  <-- ERROR BANNER"
            print("  %-4d %-9d %-7d %-9s %-11s %d%s"
                  % (i, fu, wv, ws, rs, er, flag))
        if max(r[4] for r in rows) > 100:
            print("  ** the red error banner is on the glass in %d of %d frames"
                  % (sum(1 for r in rows if r[4] > 100), len(rows)))

        healthy = [r for r in rows if r[0] >= best * COLLAPSE]
        if len(healthy) < len(rows):
            print("  VERDICT: CORRUPT -- %d/%d frames collapsed. This is "
                  "'we broke it', not a hit." % (len(rows) - len(healthy),
                                                 len(rows)))
        elif max(r[1] for r in rows) == 0 and len(set(r[2] for r in healthy)) == 1:
            # No waveform and an unchanging strip: nothing could have moved, so
            # the run excludes nothing. A loaded deck reads ~9,700 blue pixels
            # here. Both conditions are needed, since the playhead marker is not
            # blue and can move over an empty strip.
            print("  VERDICT: NOWAVE -- no waveform in the strip and nothing in "
                  "it moved, so this run excludes NOTHING. Not a negative.")
        elif min(r[1] for r in healthy) == 0 < max(r[1] for r in healthy):
            # The strip lost its waveform: something repainted it. A moving
            # playhead keeps ~9,700 blue pixels and moves a white column; an
            # erase takes the blue to zero.
            print("  VERDICT: WAVELOST -- the strip had a waveform (%d px) and "
                  "lost it (%d px). Something REPAINTED the strip; it is not a "
                  "playhead." % (max(r[1] for r in healthy),
                                 min(r[1] for r in healthy)))
        elif len({r[2] for r in healthy}) > 1:
            print("  VERDICT: ** MOTION ** -- the overview strip changes across "
                  "healthy frames (%d distinct)" % len({r[2] for r in healthy}))
        else:
            print("  VERDICT: STATIC -- strip identical in all healthy frames "
                  "(remain-sig distinct: %d)" % len({r[3] for r in healthy}))


main()
