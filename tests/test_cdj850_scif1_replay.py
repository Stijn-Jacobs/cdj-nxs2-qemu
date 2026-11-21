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


def status_remain(enabled, *, progress=0, address_flags=0):
    frame = bytearray([0x02] + [0] * 15)
    frame[4] = progress
    frame[9] = address_flags
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


def test_replay_auto_cue_status_and_reset(scif1_replay_exe, tmp_path):
    def flags(auto_cue, remain):
        frame = bytearray(status_remain(remain))
        frame[12] = 0x50 | auto_cue  # Adjacent pitch-format and status flags.
        frame[15] = sum8ec(frame[:15])
        return bytes(frame)

    other = bytes([0x71] + [0] * 14 + [0x71])
    off, on = flags(False, True), flags(True, True)
    bad_off = off[:-1] + bytes([off[-1] ^ 1])
    page1 = bytearray(status_track(1))
    page1[12] = 1
    page1[15] = sum8ec(page1[:15])
    text = make_display_burst(
        1, 8, [(0, 0, 0, ""), (0, 0, 0, "NO DISC"), (0, 0, 0, "")])
    record = tmp_path / "auto-cue.scif1"
    write_capture(record, [
        (off, other), (page1, other),
        (on, other),
        (bad_off, other), (off,),
        (other, *text[:2]), (other, *text[2:]),
        (flags(True, False), other),      # REMAIN clears while A.CUE stays on.
        (on, other),                      # In flight at reset.
        (on, other), (off, other),
    ])
    lines = record.read_bytes().splitlines(keepends=True)
    lines.insert(9, b"R 9500000\n")
    record.write_bytes(b"".join(lines))
    result = subprocess.run([str(scif1_replay_exe), str(record)],
                            capture_output=True, text=True, check=True)
    assert [line.split("display ", 1)[1] for line in result.stdout.splitlines()
            if "display auto_cue=" in line] == [
        "auto_cue=1 (3 ms)",
        "auto_cue=0 (9 ms)",
        "auto_cue=1 (10 ms)",
        "auto_cue=0 (11 ms)",
    ]


def test_replay_navigation_status_and_reset(scif1_replay_exe, tmp_path):
    def status_lamps(value):
        frame = bytearray(status_remain(False))
        frame[7] = value
        frame[15] = sum8ec(frame[:15])
        return bytes(frame)

    browse, tag, info, menu = (status_lamps(value) for value in (0x1F, 0x2F, 0x4F, 0x8F))
    bad_off = browse[:-1] + bytes([browse[-1] ^ 1])
    other = bytes([0x71] + [0] * 14 + [0x71])
    page1 = status_track(1, (0, 1, 0x5F))
    dialog = make_display_burst(
        1, 7, [(0, 0, 0, "USB connected"), (0, 0, 0, ""), (0, 0, 0, "")])
    record = tmp_path / "navigation.scif1"
    write_capture(record, [
        (browse, other), (page1, other),
        (info, other),
        (bad_off, other), (browse,),
        (other, *dialog[:2]), (other, *dialog[2:]),  # INFO survives dialog publication.
        (info, other), (tag, other), (info, other),
        (info, other),                              # In flight at reset.
        (info, other), (browse, other),
        (browse, other),                            # In flight at second reset.
        (browse, other),
        (other, *dialog[:2]), (other, *dialog[2:]),   # BROWSE survives dialog publication.
        (browse, other),
        (tag, other), (menu, other), (bad_off, other),
        (menu, other),                              # In flight at third reset.
        (browse, other),
    ])
    lines = record.read_bytes().splitlines(keepends=True)
    lines.insert(22, b"R 22500000\n")
    lines.insert(14, b"R 14500000\n")
    lines.insert(11, b"R 11500000\n")
    record.write_bytes(b"".join(lines))
    result = subprocess.run([str(scif1_replay_exe), str(record)],
                            capture_output=True, text=True, check=True)
    assert [line.split("display ", 1)[1] for line in result.stdout.splitlines()
            if "display info=" in line] == [
        "info=1 (3 ms)",
        "info=0 (9 ms)",
        "info=1 (10 ms)",
        "info=0 (11 ms)",
        "info=1 (12 ms)",
        "info=0 (13 ms)",
    ]
    assert [line.split("display ", 1)[1] for line in result.stdout.splitlines()
            if "display browse=" in line] == [
        "browse=1 (1 ms)",
        "browse=0 (3 ms)",
        "browse=1 (13 ms)",
        "browse=0 (14 ms)",
        "browse=1 (15 ms)",
        "browse=0 (19 ms)",
        "browse=1 (23 ms)",
    ]
    assert [line.split("display ", 1)[1] for line in result.stdout.splitlines()
            if "display tag_list=" in line] == [
        "tag_list=1 (9 ms)", "tag_list=0 (10 ms)",
        "tag_list=1 (19 ms)", "tag_list=0 (20 ms)",
    ]
    assert [line.split("display ", 1)[1] for line in result.stdout.splitlines()
            if "display menu=" in line] == ["menu=1 (20 ms)", "menu=0 (22 ms)"]


def test_replay_playing_address_modes_and_flash_phases(scif1_replay_exe, tmp_path):
    other = bytes([0x69] + [0] * 14 + [0x69])
    loaded = make_display_burst(1, 0x0A, [(4, 0, 0, "TRACK"), (0, 0, 0, ""), (0, 0, 0, "")])
    transactions = [(status_track(1, (0, 30, 0)), *loaded)]
    steady_remain = status_remain(True, progress=25, address_flags=0x04)
    bad_phase = status_remain(True, progress=25, address_flags=0x02)
    bad_phase = bad_phase[:-1] + bytes([bad_phase[-1] ^ 1])

    # The phase comes from MAIN even when the capture spans only milliseconds.
    stages = [
        ("unknown", (), range(0)),
        ("beginning", (status_remain(True, address_flags=0x04), other), range(100)),
        ("remaining", (steady_remain, other), range(25, 100)),
        ("bad-checksum", (bad_phase, other), range(25, 100)),
        ("elapsed", (status_remain(False, progress=25), other), range(25)),
        ("slow-lit", (status_remain(True, progress=26, address_flags=0x06), other), range(26, 100)),
        ("slow-dark", (status_remain(True, progress=26, address_flags=0x02), other), range(0)),
        ("fast-lit", (status_remain(True, progress=60, address_flags=0x05), other), range(60, 100)),
        ("fast-dark", (status_remain(True, progress=60, address_flags=0x01), other), range(0)),
        ("elapsed-fast-lit", (status_remain(False, progress=61, address_flags=0x05), other), range(61)),
        ("elapsed-fast-dark", (status_remain(False, progress=61, address_flags=0x01), other), range(0)),
        ("invalid-count", (status_remain(True, progress=255, address_flags=0x04), other), range(0)),
        ("track-end", (status_remain(True, progress=100, address_flags=0x04), other), range(0)),
        ("inactive", (status_remain(True), other), range(0)),
    ]
    for name, frames, columns in stages:
        if frames:
            transactions.append(frames)
        record = tmp_path / "playing-address.scif1"
        ppm = tmp_path / f"{name}.ppm"
        write_capture(record, transactions)
        subprocess.run([str(scif1_replay_exe), str(record), "--ppm", str(ppm)],
                       capture_output=True, check=True, timeout=30)
        pixels = ppm.read_bytes().split(b"\n", 3)[-1]
        start = (407 * 1280 + 176) * 3
        lit = [i for i in range(100)
               if pixels[start + i * 27:start + i * 27 + 3] == b"\x68\xe4\xa5"]
        assert lit == list(columns), name


def test_replay_cue_strips_and_legends_follow_status_over_matrix_messages(scif1_replay_exe, tmp_path):
    def cue_status(cue, memory_lamp):
        frame = bytearray([0x02] + [0] * 15)
        frame[10] = cue
        frame[11] = 0x10 if memory_lamp else 0
        frame[15] = sum8ec(frame[:15])
        return bytes(frame)

    def memory_frame(*columns):
        frame = bytearray([0x60] + [0] * 15)
        for column in columns:
            frame[2 + column // 8] |= 0x80 >> (column % 8)
        frame[15] = sum8ec(frame[:15])
        return bytes(frame)

    empty = (0, 0, 0, "")
    playing = make_display_burst(1, 0x0A, [(4, 0, 0, "TRACK"), empty, empty])
    saving = make_display_burst(2, 0x07, [empty, (0, 0, 0, "MEMORY"), empty])
    deleted = make_display_burst(3, 0x07, [empty, (0, 0, 0, "DELETED"), empty])
    old_marks = memory_frame(0, 12, 99)
    saved_marks = memory_frame(0, 6, 12, 99)
    remaining_marks = memory_frame(0, 6, 99)
    transactions = [(status_track(1), *playing)]
    other = bytes([0x69] + [0] * 14 + [0x69])
    stages = [
        ("loaded", (cue_status(7, True), old_marks), {0, 12, 99}, {6}, True),
        ("memory-dark", (cue_status(7, False), saved_marks, *saving), {0, 6, 12, 99}, {6}, False),
        ("memory-lit", (cue_status(7, True), saved_marks), {0, 6, 12, 99}, {6}, True),
        ("deleted", (cue_status(13, False), remaining_marks, *deleted), {0, 6, 99}, {12}, False),
        ("empty-memory", (cue_status(13, True), memory_frame()), set(), {12}, True),
        ("track-change", (status_track(2), other), set(), set(), False),
    ]
    for name, frames, memory, cue, memory_lamp in stages:
        transactions.append(frames)
        record = tmp_path / "cue-strips.scif1"
        ppm = tmp_path / f"{name}.ppm"
        write_capture(record, transactions)
        subprocess.run([str(scif1_replay_exe), str(record), "--ppm", str(ppm)],
                       capture_output=True, check=True, timeout=30)
        pixels = ppm.read_bytes().split(b"\n", 3)[-1]

        def rgb(x, y):
            offset = (y * 1280 + x) * 3
            return pixels[offset:offset + 3]

        red, blue = b"\xff\x58\x4c", b"\x9f\xe8\xff"
        assert {i for i in range(100) if rgb(176 + 9 * i, 391) == red} == memory, name
        assert {i for i in range(100) if rgb(176 + 9 * i, 439) == blue} == cue, name
        assert (rgb(52, 390) == red) == memory_lamp, name
        assert (rgb(52, 424) == blue) == bool(cue), name


def test_replay_overview_ignores_partial_and_pre_reset_packets(scif1_replay_exe, tmp_path):
    def wave_frames(value):
        frames = []
        for part in range(8):
            payload = [1, 0] + [value] * 12 if part == 0 else [value] * 14
            if part == 7:
                payload[4:] = [0] * 10
            frame = bytes([0x70 + part, *payload])
            frames.append(frame + bytes([sum8ec(frame)]))
        return frames

    other = bytes([0x69] + [0] * 14 + [0x69])
    initial = [(status_track(1), other)]
    wave = wave_frames(15)
    damaged = wave_frames(6)
    damaged[5] = damaged[5][:-1] + bytes([damaged[5][-1] ^ 1])
    fresh = wave_frames(3)
    record = tmp_path / "overview.scif1"
    write_capture(record, initial + [(other, frame) for frame in wave + damaged + fresh])
    lines = record.read_bytes().splitlines(keepends=True)
    lines.insert(18, b"R 18500000\n")   # New wave's first frame straddled reset.
    record.write_bytes(b"".join(lines))
    result = subprocess.run([str(scif1_replay_exe), str(record)],
                            capture_output=True, text=True, check=True)
    assert [line.split("display ", 1)[1] for line in result.stdout.splitlines()
            if "display playing " in line] == [
        "playing wave=1 memory=0 position=0 cue=0 (9 ms)",
        "playing wave=0 memory=0 position=0 cue=0 (18 ms)",
    ]
