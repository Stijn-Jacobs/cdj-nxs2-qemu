# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-850 update repacker: LZSS reference encoder, container build/check.

Synthetic vectors only, except the stock-file tests, which skip when
C850MAIN.UPD is not on this machine (it is Pioneer's and never committed).
"""
import hashlib
import os
import random
import struct

import pytest

C850 = "/Users/julius/Documents/cdj850-emu/C850MAIN.UPD"
STOCK_SHA = "12ed34539d844e4c3d9230eebccbc31f0b6ff2ac4d813455084987c53beac3b7"


def _samples():
    rnd = random.Random(850)
    text = b"CDJ-850 test vector " * 40
    return [
        b"",
        b"A",
        b"ABCDEFGHIJKLMNOPQ",                 # 17 bytes, shorter than F
        b"ABCDEFGHIJKLMNOPQR",                # exactly F
        b" " * 5000,                          # matches into the fill
        b"\x00" * 9000,
        text,
        bytes(rnd.randrange(256) for _ in range(6000)),
        bytes(rnd.choice(b"\x00\x09\x0b\x20\xff") for _ in range(12000)),
    ]


@pytest.mark.parametrize("data", _samples(), ids=lambda d: "len%d" % len(d))
def test_pack_tree_round_trips(data):
    from lzss_decode import unpack
    from lzss_encode import pack_tree
    assert unpack(pack_tree(data), 0) == data


def test_pack_tree_is_not_larger_than_greedy_on_text():
    from lzss_encode import pack, pack_tree
    data = b"".join(b"line %05d of a repetitive log\n" % (i % 97)
                    for i in range(400))
    assert len(pack_tree(data)) <= len(pack(data))


def test_sum16():
    from c850_repack import sum16
    assert sum16(b"") == 0
    assert sum16(b"\x01\x02\xff") == 0x102
    assert sum16(b"\xff" * 0x101) == (0xFF * 0x101) & 0xFFFF


def _fake_base():
    """A made-up base: recovery image at 0x20000 in the same format, label
    at 0x60000. No firmware bytes."""
    from c850_repack import sum16
    from lzss_encode import pack_tree
    base = bytearray(b"\xA5" * 0x60020)
    rec = pack_tree(b"recovery " * 50)
    packed = struct.pack(">I", len(rec)) + rec
    base[0x20000:0x20000 + len(packed) + 2] = packed + struct.pack(
        ">H", sum16(packed))
    base[0x60000:0x60020] = b"CDJ-850 MAIN".ljust(16, b"\0") + \
        b"1.13".ljust(16, b"\0")
    return bytes(base)


def test_build_and_check_synthetic():
    from c850_repack import (PAD_LEN, PACKED_OFF, build_upd, check_upd,
                             read_packed, unpack_main)
    base = _fake_base()
    image = bytes(range(256)) * 64 + b"\x00\x09" * 1000
    out = build_upd(base, image, check_boot=False)
    assert out[:PACKED_OFF] == base[:PACKED_OFF]
    stream, stored, calc = read_packed(out)
    assert stored == calc
    assert len(out) == PACKED_OFF + 4 + len(stream) + 2 + PAD_LEN
    assert not any(out[-PAD_LEN:])
    assert unpack_main(out) == image
    assert check_upd(out, image=image, check_boot=False) == []


def test_check_catches_bad_checksum_and_missing_label():
    from c850_repack import PACKED_OFF, build_upd, check_upd
    out = bytearray(build_upd(_fake_base(), b"hello" * 100, check_boot=False))
    out[PACKED_OFF + 10] ^= 0x01
    out[0x60000] = ord("X")
    problems = check_upd(bytes(out), check_boot=False)
    assert any("MAIN checksum" in p for p in problems)
    assert any("label" in p for p in problems)


def test_build_refuses_foreign_boot_region():
    from c850_repack import build_upd
    with pytest.raises(ValueError, match="boot region"):
        build_upd(_fake_base(), b"x" * 100)


def test_build_refuses_oversize_main():
    from c850_repack import build_upd
    rnd = random.Random(1)
    big = bytes(rnd.randrange(256) for _ in range(0x1A0000))
    with pytest.raises(ValueError, match="past the MAIN region"):
        build_upd(_fake_base(), big, check_boot=False)


def test_normalise_drops_only_zero_overrun():
    from c850_repack import STOCK_IMAGE_LEN, normalise_image
    img = b"\x01" * STOCK_IMAGE_LEN
    assert normalise_image(img + bytes(0x233E)) == img
    longer = img + b"\x00\x01"
    assert normalise_image(longer) == longer


stock = pytest.mark.skipif(not os.path.exists(C850),
                           reason="C850MAIN.UPD is not on this machine")


@stock
def test_stock_checks_ok_and_unpacks_to_pinned_image():
    from c850_repack import (STOCK_IMAGE_LEN, STOCK_IMAGE_SHA256, check_upd,
                             unpack_main)
    upd = open(C850, "rb").read()
    assert check_upd(upd) == []
    image = unpack_main(upd)
    assert len(image) == STOCK_IMAGE_LEN
    assert hashlib.sha256(image).hexdigest() == STOCK_IMAGE_SHA256


@stock
def test_stock_repack_is_byte_identical():
    from c850_repack import build_upd, unpack_main
    upd = open(C850, "rb").read()
    out = build_upd(upd, unpack_main(upd))
    assert hashlib.sha256(out).hexdigest() == STOCK_SHA
    assert out == upd
