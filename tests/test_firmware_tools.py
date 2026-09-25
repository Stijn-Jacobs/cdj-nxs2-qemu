# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/firmware/: the LZSS codec and the image builders, on synthetic
inputs (the real firmware is never available here)."""
import os
import random
import struct

import pytest

import gui_address
import gui_decode
import gui_resources
import lzss_decode
import make_settings
import player_flash
from helpers import run_script

RING, MASK, RING_POS = 4096, 4095, 4078


def lzss_encode(data, ring_init=0x20, ring_pos=RING_POS, search=64):
    """A small greedy encoder for the decoder's exact format: one flag byte
    (LSB first, 1 = literal) per eight items; a match is two bytes naming an
    ABSOLUTE ring position and a length of 3..18. Not optimal, only correct."""
    ring = bytearray([ring_init]) * RING
    r = ring_pos
    out = bytearray()
    items = []
    i = 0
    while i < len(data):
        best_len, best_off, tried = 0, 0, 0
        for off in range(RING):
            if ring[off] != data[i]:
                continue
            tried += 1
            n, written = 0, {}
            while n < 18 and i + n < len(data):
                b = written.get((off + n) & MASK, ring[(off + n) & MASK])
                if b != data[i + n]:
                    break
                written[(r + n) & MASK] = b
                n += 1
            if n > best_len:
                best_len, best_off = n, off
            if tried >= search:
                break
        if best_len >= 3:
            items.append((False, bytes([best_off & 0xFF, ((best_off >> 4) & 0xF0) | (best_len - 3)])))
            step = best_len
        else:
            items.append((True, data[i:i + 1]))
            step = 1
        for k in range(step):
            ring[r] = data[i + k]
            r = (r + 1) & MASK
        i += step
    for g in range(0, len(items), 8):
        group = items[g:g + 8]
        out.append(sum(1 << j for j, (lit, _) in enumerate(group) if lit))
        for _, payload in group:
            out += payload
    return bytes(out)


# -- LZSS ------------------------------------------------------------------------

def test_all_literals():
    stream = b"\xff" + b"ABCDEFGH" + b"\x01" + b"I"
    assert lzss_decode.unpack(stream, 0) == b"ABCDEFGHI"


def test_match_names_an_absolute_ring_position():
    # "abc" lands at 4078..4080; the match copies 6 bytes from 4078 (0xFEE),
    # overlapping what it writes: abcabcabc.
    stream = bytes([0b0111]) + b"abc" + bytes([0xEE, 0xF0 | (6 - 3)])
    assert lzss_decode.unpack(stream, 0) == b"abcabcabc"


def test_ring_starts_filled_with_the_init_byte():
    stream = bytes([0]) + bytes([0x00, 0x00 | (5 - 3)])
    assert lzss_decode.unpack(stream, 0) == b"     "
    assert lzss_decode.unpack(stream, 0, ring_init=0) == bytes(5)


def test_stream_offset_skips_the_length_word():
    stream = struct.pack("<I", 99) + b"\x07xyz"
    assert lzss_decode.unpack(stream, 4) == b"xyz"


def test_limit_stops_early():
    stream = b"\xff" + b"ABCDEFGH"
    assert lzss_decode.unpack(stream, 0, limit=3) == b"ABC"


def test_truncated_match_is_dropped_not_misread():
    assert lzss_decode.unpack(b"\x01A\xee", 0) == b"A"


@pytest.mark.parametrize("seed", [1, 2, 3])
def test_round_trip(seed):
    rnd = random.Random(seed)
    words = [b"PUSH MEMORY", b"rekordbox", b"\x00\x00\x00\x00", b"LOOP", b"\xff\xff"]
    data = b"".join(rnd.choice(words) + bytes([rnd.randrange(256)]) for _ in range(80))
    enc = lzss_encode(data)
    assert len(enc) < len(data)
    assert lzss_decode.unpack(enc, 0) == data


def test_round_trip_incompressible():
    data = bytes(random.Random(7).randrange(256) for _ in range(300))
    assert lzss_decode.unpack(lzss_encode(data), 0) == data


def test_report_prints_entropy_and_strings(capsys):
    ent, strs = lzss_decode.report(b"HELLO WORLD!" + bytes(20), "x")
    assert strs == [b"HELLO WORLD!"]
    assert 0 < ent < 8
    assert "strings>=8 1" in capsys.readouterr().out


# -- the GUI section ----------------------------------------------------------------

def test_gui_section_is_section_one_of_the_manifest():
    upd = b"5\r\n3\r\n" + b"HELLO" + b"abc"
    assert gui_decode.gui_section_from_upd(upd) == b"HELLO"
    assert gui_decode.gui_section_from_upd(b"no manifest") is None


def test_gui_decode_reads_header_and_big_endian_length():
    payload = b"GUI image payload " * 20
    stream = lzss_encode(payload)
    section = b"CDJ-2000NXS2GUI Ver9.99".ljust(32, b" ") + struct.pack(">I", len(stream)) + stream
    hdr, declared, avail, out = gui_decode.decode(section)
    assert hdr == "CDJ-2000NXS2GUI Ver9.99"
    assert declared == avail == len(stream)
    assert out == payload


def test_gui_address_segments_round_trip():
    for base, ln, off in gui_address.SEG:
        for addr in (base, base + ln - 1):
            o = gui_address.a2o(addr)
            assert o == off + (addr - base)
            assert addr in gui_address.o2a(o)
    assert gui_address.a2o(0) is None
    assert gui_address.o2a(0) == []


def test_gui_address_twice_loaded_range_has_two_addresses():
    assert len(gui_address.o2a(0x010000)) == 2


def test_gui_glyph_render_2bpp_msb_first():
    cell = bytearray(gui_resources.CELL_BYTES)
    cell[0] = 0b11100100                   # shades 3, 2, 1, 0
    rows = gui_resources.render(bytes(cell) * 2, chr(0x21))
    assert len(rows) == gui_resources.CELL_H
    assert all(len(r) == gui_resources.CELL_W for r in rows)
    assert rows[0].startswith("#:. ")
    assert set("".join(rows)[4:]) == {" "}
    assert set("".join(gui_resources.render(b"", "~"))) == {" "}


# -- the .UPD container -------------------------------------------------------------

def test_split_update_carves_every_section(tmp_path):
    upd = tmp_path / "x.upd"
    upd.write_bytes(b"5\r\n3\r\n" + b"HELLO" + b"abc")
    out = tmp_path / "out"
    r = run_script("scripts/firmware/split_update.py", upd, out)
    assert r.returncode == 0, r.stderr
    assert "MATCH" in r.stdout and "MISMATCH" not in r.stdout
    assert (out / "section1.bin").read_bytes() == b"HELLO"
    assert (out / "section2.bin").read_bytes() == b"abc"


def test_split_update_flags_a_size_mismatch(tmp_path):
    upd = tmp_path / "x.upd"
    upd.write_bytes(b"5\r\n" + b"HELLO" + b"trailing")
    r = run_script("scripts/firmware/split_update.py", upd, tmp_path / "o")
    assert "MISMATCH" in r.stdout


# -- settings sector and flash image --------------------------------------------------

def test_settings_sector_records_then_erased():
    blob = make_settings.build([(0x7101, 0x0001), (0x1234, 0xBEEF)])
    assert len(blob) == make_settings.REGION_SIZE
    assert blob[:8] == struct.pack("<HHHH", 0x7101, 1, 0x1234, 0xBEEF)
    assert blob[8:] == b"\xff" * (len(blob) - 8)


def test_make_settings_cli_adds_to_the_default(tmp_path):
    out = tmp_path / "s.bin"
    r = run_script("scripts/firmware/make_settings.py", out, "0x10=0x20", "7=8")
    assert r.returncode == 0, r.stderr
    blob = out.read_bytes()
    assert struct.unpack_from("<6H", blob) == (0x7101, 1, 0x10, 0x20, 7, 8)
    assert blob[12:14] == b"\xff\xff"


def test_make_flash_places_the_sector_in_an_erased_8mb_image(tmp_path):
    sector = make_settings.build([(1, 2)])
    src = tmp_path / "s.bin"
    src.write_bytes(sector)
    out = tmp_path / "flash.bin"
    r = run_script("scripts/firmware/make_flash.py", out, src)
    assert r.returncode == 0, r.stderr
    img = out.read_bytes()
    assert len(img) == 0x800000
    assert img[0x7F6000:0x7F8000] == sector
    assert img[:0x7F6000].count(0xFF) == 0x7F6000
    assert img[0x7F8000:].count(0xFF) == 0x800000 - 0x7F8000


def test_make_flash_refuses_a_wrong_sized_sector(tmp_path):
    src = tmp_path / "s.bin"
    src.write_bytes(b"\xff" * 100)
    r = run_script("scripts/firmware/make_flash.py", tmp_path / "f.bin", src)
    assert r.returncode == 1 and "expected 8192" in r.stderr
    assert not os.path.exists(tmp_path / "f.bin")


# -- player number ---------------------------------------------------------------

ERASED = b"\xff" * player_flash.FLASH_SIZE


@pytest.mark.parametrize("n, halfword", [(0, 0x0179), (1, 0x0579), (2, 0x0979), (4, 0x1179)])
def test_player_halfword(n, halfword):
    assert player_flash.player_halfword(n) == halfword


def test_mint_auto_writes_only_the_format():
    img = player_flash.mint(ERASED, 0)
    assert img[0x7F0000:0x7F0008] == bytes.fromhex("79013f4095842203")
    assert img[0x7F0008:0x7F0010] == b"\xff" * 8
    for off, data in player_flash.FORMAT:
        assert img[off:off + len(data)] == data
    assert img[:0x7EFFFC] == ERASED[:0x7EFFFC]


def test_mint_appends_the_player_record_after_auto():
    img = player_flash.mint(ERASED, 3)
    assert img[0x7F0008:0x7F0010] == struct.pack("<H", 0x0179 | 3 << 10) + player_flash.RECORD_TAIL
    assert img[0x7FE000:0x7FE00C] == b"PDJ0000001XX"


def test_mint_serial():
    img = player_flash.mint(ERASED, 2, "PDJ0000002XX")
    assert img[0x7FE000:0x7FE00C] == b"PDJ0000002XX"
    with pytest.raises(SystemExit):
        player_flash.mint(ERASED, 2, "SHORT")


def test_mint_refuses_a_wrong_sized_base():
    with pytest.raises(SystemExit):
        player_flash.mint(b"\xff" * 1024, 1)


def test_player_flash_cli_auto_serial(tmp_path):
    base = tmp_path / "base.bin"
    base.write_bytes(ERASED)
    out = tmp_path / "p2.bin"
    r = run_script("scripts/firmware/player_flash.py", "2", out, base, "--serial", "auto")
    assert r.returncode == 0, r.stderr
    img = out.read_bytes()
    assert img[0x7FE000:0x7FE00C] == b"PDJ0000002XX"
    assert struct.unpack_from("<H", img, 0x7F0008)[0] == 0x0979


def test_player_flash_cli_rejects_player_5(tmp_path):
    base = tmp_path / "base.bin"
    base.write_bytes(ERASED)
    r = run_script("scripts/firmware/player_flash.py", "5", tmp_path / "o.bin", base)
    assert r.returncode != 0 and "auto or 1..4" in r.stderr
