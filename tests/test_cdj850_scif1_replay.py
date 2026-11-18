# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise standalone replay with synthetic SCIF1 transactions."""
import os
from pathlib import Path
import shutil
import subprocess

import pytest

from helpers import path
from test_cdj850_panel_peer import make_display_burst, sum8ec


@pytest.fixture(scope="session")
def scif1_replay_exe(tmp_path_factory):
    cc = os.environ.get("CC") or shutil.which("cc")
    if not cc:
        pytest.skip("no C compiler")
    xc = Path(path("hw", "cdj", "boards", "cdj850", "xc"))
    name = "scif1-replay.exe" if os.name == "nt" else "scif1-replay"
    exe = tmp_path_factory.mktemp("scif1-replay") / name
    result = subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2", "-I", str(xc),
         path("scripts", "debug", "cdj850_scif1_replay.c"),
         str(xc / "panel_display.c"), "-o", str(exe)],
        capture_output=True, text=True, timeout=60,
    )
    assert result.returncode == 0, result.stderr
    return exe


def write_capture(path, transactions, suffix=""):
    lines = ["CDJ850-SCIF1 1\n"]
    for seq, frames in enumerate(transactions, 1):
        start_ns = seq * 1000000
        fields = [f"T {seq} {start_ns} {start_ns + 999999} {len(frames)}"]
        fields.extend(f"{start_ns + 1000 * i}:{frame.hex()}"
                      for i, frame in enumerate(frames, 1))
        lines.append(" ".join(fields) + "\n")
    path.write_bytes(("".join(lines) + suffix).encode("ascii"))


def test_replay_complete_display_states(scif1_replay_exe, tmp_path):
    unknown = bytes([0x71] + [0] * 14 + [0x71])
    idle = make_display_burst(1, 8, [(0, 0, 0, ""), (0, 0, 0, "NO DISC"), (0, 0, 0, "")])
    damaged = make_display_burst(2, 10, [(1, 2, 3, "PARTIAL"), (0, 0, 0, ""), (0, 0, 0, "")])
    bad_continuation = damaged[1][:-1] + bytes([damaged[1][-1] ^ 1])
    bad_header = damaged[0][:-1] + bytes([damaged[0][-1] ^ 1])
    header_only = bytearray(idle[0])
    header_only[3] = 0x42
    header_only[15] = sum8ec(header_only[:15])
    browse = make_display_burst(
        3, 1, [(1, 2, 3, "ALPHA"), (4, 5, 6, "BETA"), (7, 8, 9, "GAMMA")],
        tags=(1, 0x26, 3),
    )

    record = tmp_path / "synthetic.scif1"
    write_capture(
        record,
        [
            [], (damaged[0],),  # aborted windows do not feed the decoder
            (unknown, *idle[:2]), (unknown, *idle[2:]),
            (damaged[0], bad_continuation), (unknown, *damaged[2:]),
            (unknown, unknown),
            (damaged[0], damaged[1]), (unknown, unknown),  # truncated text
            (bad_header, *damaged[1:]), (unknown, unknown),
            (header_only, unknown), (unknown, unknown),
            (unknown, *browse[:2]), (unknown, *browse[2:]),
        ],
        suffix="LIMIT\n",
    )
    result = subprocess.run([str(scif1_replay_exe), str(record)],
                            capture_output=True, text=True, check=True)
    assert result.stdout.splitlines() == [
        "cdj850-panel: display mode=0x08 tag=11/12/13 "
        "attr=00:00:00/00:00:00/00:00:00 [] | [NO DISC] | [] (4 ms)",
        "cdj850-panel: display hdr mode=0x08 "
        "bytes=42 00 00 00 00 00 00 00 00 00 00 00 (13 ms)",
        "cdj850-panel: display mode=0x01 tag=01/26/03 "
        "attr=01:02:03/04:05:06/07:08:09 [ALPHA] | [BETA] | [GAMMA] (15 ms)",
    ]
    assert "byte limit" in result.stderr


@pytest.mark.parametrize("line", [
    "T 1 0 100 65\n",
    "T 1 0 100 2 5:" + "00" * 16 + "\n",
    "T 1 10 100 1 5:" + "00" * 16 + "\n",
    "T 1 0 100 1 5:" + "0z" * 16 + "\n",
    "T 1 0 100 0",
])
def test_replay_rejects_malformed_capture_record(scif1_replay_exe, tmp_path, line):
    record = tmp_path / "malformed.scif1"
    write_capture(record, [], suffix=line)
    result = subprocess.run([str(scif1_replay_exe), str(record)],
                            capture_output=True, text=True)
    assert result.returncode == 1
    assert "Malformed capture on line 2" in result.stderr
    assert not result.stdout


def status_track(number, msf=(0xFF, 0xFF, 0xFF), bpm=0xFFFF):
    frame = bytearray([0x01, 0, 0, 0, number, *msf, bpm >> 8, bpm & 0xFF] + [0] * 6)
    frame[15] = sum8ec(frame[:15])
    return bytes(frame)


def test_replay_track_number_updates_and_reset(scif1_replay_exe, tmp_path):
    other = bytes([0x02] + [0] * 15)
    bad = status_track(2)[:-1] + bytes([0])
    record = tmp_path / "track.scif1"
    write_capture(record, [
        (other, other),           # absent at boot
        (status_track(10), other),
        (bad, other),             # CRC failure preserves 10
        (status_track(2),),       # aborted window preserves 10
        (other, other),           # alternating page preserves 10
        (status_track(0xBB), other),  # overflow sentinel
        (status_track(11), other),
        (status_track(2), other),  # transaction straddled the reset
        (status_track(0), other),  # explicit 00 is known
        (status_track(0xFF), other),  # blank sentinel
    ])
    lines = record.read_bytes().splitlines(keepends=True)
    lines.insert(8, b"R 8500000\n")
    record.write_bytes(b"".join(lines))
    result = subprocess.run([str(scif1_replay_exe), str(record)],
                            capture_output=True, text=True, check=True)
    assert result.stdout.splitlines() == [
        "cdj850-panel: display track=10 (2 ms)",
        "cdj850-panel: display track=-- (6 ms)",
        "cdj850-panel: display track=11 (7 ms)",
        "cdj850-panel: display track=-- (8 ms)",
        "cdj850-panel: display track=00 (9 ms)",
        "cdj850-panel: display track=-- (10 ms)",
    ]


def status_remain(enabled):
    frame = bytearray([0x02] + [0] * 15)
    frame[13] = 0x2E | enabled
    frame[15] = sum8ec(frame[:15])
    return bytes(frame)


def test_replay_time_status_pages_and_reset(scif1_replay_exe, tmp_path):
    other = bytes([0x71] + [0] * 14 + [0x71])
    sample = status_track(1, (1, 5, 74))
    bad_digits = status_track(1, (0, 0, 21))[:-1] + b"\x00"
    bad_mode = status_remain(False)[:-1] + b"\x00"
    record = tmp_path / "time.scif1"
    write_capture(record, [
        (other, other),                           # no status yet
        (status_remain(True), other),             # lamp without digits
        (sample, other),
        (bad_digits, other),
        (status_remain(False),),                  # aborted window
        (bad_mode, other),
        (status_remain(False), other),
        (status_track(1, (0, 0, 21)), other),
        (status_track(1, (0, 0, 75)), other),      # impossible frame count
        (status_track(1), other),                 # firmware unavailable tuple
        (status_track(1, (99, 59, 74)), other),
        (sample, other),                          # straddles the reset
        (status_track(0, (0, 0, 0)), other),
        (status_remain(True), other),
    ])
    lines = record.read_bytes().splitlines(keepends=True)
    lines.insert(12, b"R 12500000\n")
    record.write_bytes(b"".join(lines))
    result = subprocess.run([str(scif1_replay_exe), str(record)],
                            capture_output=True, text=True, check=True)
    assert [line.split("display ", 1)[1] for line in result.stdout.splitlines()
            if "display time=" in line] == [
        "time=--:--:-- remain=1 (2 ms)",
        "time=01:05:74 remain=1 (3 ms)",
        "time=01:05:74 remain=0 (7 ms)",
        "time=00:00:21 remain=0 (8 ms)",
        "time=--:--:-- remain=0 (9 ms)",
        "time=99:59:74 remain=0 (11 ms)",
        "time=--:--:-- remain=? (12 ms)",
        "time=00:00:00 remain=? (13 ms)",
        "time=00:00:00 remain=1 (14 ms)",
    ]


def test_replay_bpm_status_validation_and_reset(scif1_replay_exe, tmp_path):
    other = status_remain(False)
    unavailable = status_track(1)
    bad = status_track(1, bpm=120)[:-1] + b"\x00"
    record = tmp_path / "bpm.scif1"
    write_capture(record, [
        (unavailable, other),                 # no reading yet
        (status_track(1, bpm=128), other),
        (bad, other),                         # bad checksum preserves 128
        (status_track(2, bpm=96),),            # aborted window preserves 128
        (other, other),                        # other status page preserves 128
        (status_track(2, bpm=96), other),
        (unavailable, other),                  # firmware unavailable word
        (status_track(3, bpm=256), other),      # exercise both bytes
        (status_track(3, bpm=999), other),
        (status_track(3, bpm=1000), other),     # outside three-digit display
        (status_track(3, bpm=0), other),
        (status_track(4, bpm=123), other),
        (status_track(4, bpm=123), other),      # straddles the reset
        (status_track(5, bpm=174), other),
    ])
    lines = record.read_bytes().splitlines(keepends=True)
    lines.insert(13, b"R 13500000\n")
    record.write_bytes(b"".join(lines))
    result = subprocess.run([str(scif1_replay_exe), str(record)],
                            capture_output=True, text=True, check=True)
    assert [line.split("display ", 1)[1] for line in result.stdout.splitlines()
            if "display bpm=" in line] == [
        "bpm=128 (2 ms)",
        "bpm=96 (6 ms)",
        "bpm=--- (7 ms)",
        "bpm=256 (8 ms)",
        "bpm=999 (9 ms)",
        "bpm=--- (10 ms)",
        "bpm=123 (12 ms)",
        "bpm=--- (13 ms)",
        "bpm=174 (14 ms)",
    ]


def test_replay_pitch_status_pairing_and_reset(scif1_replay_exe, tmp_path):
    def magnitude(value):
        frame = bytearray(status_track(1))
        frame[10:12] = value.to_bytes(2, "big")
        frame[15] = sum8ec(frame[:15])
        return bytes(frame)

    def flags(sign, decimals, range_flags):
        frame = bytearray(status_remain(False))
        frame[11] = 0x40 | sign
        frame[12] = 0xC0 | (decimals << 4)
        frame[14] = range_flags
        frame[15] = sum8ec(frame[:15])
        return bytes(frame)

    other = bytes([0x71] + [0] * 14 + [0x71])
    narrow = flags(3, 1, 0x22)
    negative = flags(2, 1, 0x82)
    wide = flags(3, 2, 0x06)
    corrupt = narrow[:-1] + bytes([narrow[-1] ^ 1])
    record = tmp_path / "pitch.scif1"
    write_capture(record, [
        (magnitude(1600), other),                # missing page 2
        (narrow, other), (magnitude(1600), other),
        (negative, other), (magnitude(600),),   # incomplete magnitude
        (magnitude(600), other),                # cannot reuse old flags
        (negative, other), (corrupt, other),    # damaged flags
        (magnitude(600), other),
        (flags(0, 0, 0x42), other), (magnitude(0), other),  # range only
        (flags(1, 1, 0x42), other), (magnitude(25), other), # malformed sign
        (flags(3, 1, 0x62), other), (magnitude(25), other), # ambiguous range
        (wide, other), (magnitude(1000), other),
        (negative, other), (magnitude(600), other),  # magnitude crossed reset
        (magnitude(600), other),                     # flags cleared by reset
        (negative, other), (magnitude(600), other),
    ])
    lines = record.read_bytes().splitlines(keepends=True)
    lines.insert(19, b"R 19500000\n")
    record.write_bytes(b"".join(lines))
    result = subprocess.run([str(scif1_replay_exe), str(record)],
                            capture_output=True, text=True, check=True)
    assert [line.split("display ", 1)[1] for line in result.stdout.splitlines()
            if "display pitch=" in line] == [
        "pitch=+16.00 range=16 (3 ms)",
        "pitch=--- range=10 (11 ms)",
        "pitch=--- range=-- (15 ms)",
        "pitch=+100.0 range=WIDE (17 ms)",
        "pitch=--- range=-- (19 ms)",
        "pitch=-6.00 range=6 (22 ms)",
    ]


def test_replay_master_tempo_status_and_reset(scif1_replay_exe, tmp_path):
    def flags(enabled):
        frame = bytearray(status_remain(False))
        frame[11] = 0x03 | (0x80 if enabled else 0)  # Adjacent pitch sign bits.
        frame[15] = sum8ec(frame[:15])
        return bytes(frame)

    off, on = flags(False), flags(True)
    bad_off = off[:-1] + bytes([off[-1] ^ 1])
    other = bytes([0x71] + [0] * 14 + [0x71])
    page1 = bytearray(status_track(1))
    page1[11] = 0x80
    page1[15] = sum8ec(page1[:15])
    text = make_display_burst(
        1, 8, [(0, 0, 0, ""), (0, 0, 0, "NO DISC"), (0, 0, 0, "")])
    record = tmp_path / "master-tempo.scif1"
    write_capture(record, [
        (off, other), (page1, other),
        (on, other),
        (bad_off, other), (off,),                   # Ignore damaged and aborted windows.
        (other, *text[:2]), (other, *text[2:]),      # Text publication preserves MT.
        (on, other), (off, other), (on, other),
        (on, other),                                # In flight at reset.
        (on, other), (off, other),
    ])
    lines = record.read_bytes().splitlines(keepends=True)
    lines.insert(11, b"R 11500000\n")
    record.write_bytes(b"".join(lines))
    result = subprocess.run([str(scif1_replay_exe), str(record)],
                            capture_output=True, text=True, check=True)
    assert [line.split("display ", 1)[1] for line in result.stdout.splitlines()
            if "display master_tempo=" in line] == [
        "master_tempo=1 (3 ms)",
        "master_tempo=0 (9 ms)",
        "master_tempo=1 (10 ms)",
        "master_tempo=0 (11 ms)",
        "master_tempo=1 (12 ms)",
        "master_tempo=0 (13 ms)",
    ]
