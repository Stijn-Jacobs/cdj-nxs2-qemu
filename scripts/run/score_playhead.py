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

usage: score_playhead.py <tag> [tag ...]    reads /tmp/<tag>/z89-<tag>-*-f*.ppm
"""
import glob
import hashlib
import sys

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


def load(path):
    return Image.open(path).convert("RGB")


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


def main():
    for tag in sys.argv[1:]:
        frames = sorted(glob.glob("/tmp/%s/z89-%s-*-f*.ppm" % (tag, tag)))
        print("=" * 70)
        print("%s   %d frames" % (tag, len(frames)))
        if not frames:
            print("  no frames -- instrument dead, not a measurement")
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
