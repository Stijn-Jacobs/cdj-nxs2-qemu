#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Estimate a whole-track BPM and a first-beat offset, good enough for a
straight 4/4 grid, for a track collection_xml.py has no BPM tag for.

Decodes the file to mono PCM with ffmpeg (its decoders cover every format
collection_xml.py handles, so no separate per-format decode path is needed
here) and finds the dominant beat period by autocorrelating an onset-strength
envelope -- the standard cheap tempo estimate. This assumes one tempo for the
whole track and reports one phase, which is exactly what a straight grid
needs; it does not track tempo changes or find every downbeat.

Needs ffmpeg and numpy, both soft dependencies: available() says whether
they're there, and estimate() returns None rather than raising when they are
not, so a track missing both just loads without a beat grid.

Deliberately not librosa for this: librosa's own dependency closure (scipy,
numba, llvmlite, soundfile, audioread) is tens of megabytes of wheels beyond
numpy alone, and several of those formats still reach ffmpeg or gstreamer
through audioread -- so it would not even remove this module's one real
dependency, only add to it, for a task that only needs a whole-track BPM and
one phase.

usage: python bpm_estimate.py <audio file>   (prints "BPM OFFSET_SECONDS")
"""
import shutil
import subprocess
import sys

SR = 11025           # downsampled decode rate: ample for a 60-200 BPM range
MAX_SECONDS = 90      # a whole-track estimate does not need the whole track
MIN_BPM, MAX_BPM = 60.0, 200.0
HOP = 256
CLARITY = 1.0        # min onset.max()/energy.mean(); see _onset_envelope


def available():
    if shutil.which("ffmpeg") is None:
        return False
    try:
        import numpy  # noqa: F401
    except ImportError:
        return False
    return True


def _decode_mono(path):
    argv = ["ffmpeg", "-v", "error", "-i", path, "-t", str(MAX_SECONDS),
            "-f", "f32le", "-ar", str(SR), "-ac", "1", "-"]
    try:
        out = subprocess.run(argv, capture_output=True, timeout=120)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if out.returncode != 0 or not out.stdout:
        return None
    import numpy as np

    return np.frombuffer(out.stdout, dtype="<f4")


def _onset_envelope(samples):
    """(onset, energy): energy is each frame's mean absolute level, onset its
    half-wave rectified frame-to-frame rise. A steady tone's frame energy
    barely moves (256 samples is rarely a whole number of its own cycles, so
    it still wobbles a little -- just not by much), so onset.max() relative
    to energy.mean() tells a real transient apart from that wobble; see the
    CLARITY gate in estimate()."""
    import numpy as np

    n = len(samples) // HOP
    if n < 8:
        return None, None
    frames = samples[:n * HOP].reshape(n, HOP)
    energy = np.abs(frames).mean(axis=1)
    onset = np.diff(energy, prepend=energy[0])
    onset[onset < 0] = 0.0
    return onset, energy


def estimate(path):
    """(bpm, offset_seconds), or None: no ffmpeg/numpy, nothing decoded, or
    nothing periodic in the 60-200 BPM range."""
    if not available():
        return None
    samples = _decode_mono(path)
    if samples is None or len(samples) < SR:
        return None
    import numpy as np

    frame_hz = SR / HOP
    onset, energy = _onset_envelope(samples)
    if onset is None or not onset.any():
        return None
    if onset.max() < CLARITY * energy.mean():
        return None  # no transient stands out from a steady tone's own wobble
    lag_lo = int(frame_hz * 60.0 / MAX_BPM)
    lag_hi = int(frame_hz * 60.0 / MIN_BPM)
    if lag_hi >= len(onset) - 1 or lag_lo < 1:
        return None
    autocorr = np.correlate(onset, onset, mode="full")[len(onset) - 1:]
    # A period that is not a whole number of frames splits its correlation
    # peak over two adjacent lags (the frame grid falls sometimes short,
    # sometimes long of it), which a raw argmax reads as a weaker fundamental
    # than its clean, unsplit 2x/3x harmonic -- the classic octave error.
    # Smoothing over 3 lags recombines the split; picking the smallest lag
    # within 15% of the smoothed peak, rather than the biggest, then prefers
    # the fundamental over a harmonic that only ties it.
    smoothed = np.convolve(autocorr, np.ones(3), mode="same")
    window = smoothed[lag_lo:lag_hi + 1]
    if not window.any():
        return None
    threshold = 0.85 * window.max()
    period_frames = next(lag_lo + i for i, v in enumerate(window) if v >= threshold)
    # Parabolic interpolation across the peak's immediate neighbours for
    # sub-frame precision -- a whole-frame lag alone is only good to a few BPM.
    y0, y1, y2 = smoothed[period_frames - 1:period_frames + 2]
    denom = y0 - 2 * y1 + y2
    frac = max(-0.5, min(0.5, 0.5 * (y0 - y2) / denom)) if denom else 0.0
    bpm = round(60.0 * frame_hz / (period_frames + frac), 2)

    # The offset rekordbox itself would write is simply the time of an actual
    # beat, not a phase folded into [0, period) -- so the earliest frame with
    # a real onset in it, not a best-fit comb search. A comb search over a
    # period this coarse (whole frames, ~23 ms each) drifts out of phase with
    # the true, fractional period within a few dozen beats and ends up fitting
    # some compromise position in the middle of the track instead of the
    # first beat; the very first onset does not have that problem, and a
    # small residual error against the true first beat (less than one frame,
    # or a whole period if the track starts on a beat with no silence before
    # it to rise from) is well within "good enough for a straight grid".
    first = int(np.argmax(onset > 0.5 * onset.max()))
    offset = round(first / frame_hz, 3)
    return bpm, offset


def main(argv):
    if len(argv) != 1:
        sys.exit(__doc__.splitlines()[-2])
    result = estimate(argv[0])
    if result is None:
        if not available():
            sys.exit("no ffmpeg and/or numpy: cannot estimate a BPM for %s" % argv[0])
        sys.exit("no clear beat found in %s" % argv[0])
    print("%.2f %.3f" % result)


if __name__ == "__main__":
    main(sys.argv[1:])
