# SPDX-License-Identifier: GPL-2.0-or-later
"""patch_main.py and the sigpatch.py additions it needed: executed-in-place
placement (no scatter-load table), a byteorder-aware engine (MAIN is a
little-endian SH-4, the GUI's SH7269 is big-endian), the literal-pool safety
check, and the MAIN section codec (main_decode.py/main_encode.py).

Most mods here are test-only and built by the test itself, the same way this
file always has; patch_main.MODS's own real entry (wave3data) is exercised
through its --list/--assemble output below rather than through sigpatch.patch()
directly, matching how patch_gui.py's real mods are covered."""
import contextlib
import io
import os
import shutil
import struct
import sys

import pytest

import gui_encode
import main_decode
import main_encode
import make_flash
import patch_main
import patch_update
import sigpatch
from sigpatch import Mod
from helpers import path, run_script


# -- sigpatch.py: executed-in-place placement (loadtab_n == 0) -------------------

def build_xip_image():
    """A tiny image with no scatter-load table at all: the whole blob is one
    identity-mapped copy (MAIN's own shape), a signature in the middle, and a
    run of erased flash for the routine to land in, in place."""
    flash_base = 0x08000000
    profile = sigpatch.ImageProfile(flash_base=flash_base, loadtab_off=0, loadtab_n=0,
                                    ram_base=flash_base + 0x40, ram_end=flash_base + 0x80,
                                    byteorder='<')
    sig = [0xAAA1, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888]
    code = [0x1010, 0x2020] + sig + [0xE0E0, 0xF0F0]

    d = bytearray(0x100)
    for i, v in enumerate(code):
        struct.pack_into('<H', d, 0x10 + 2 * i, v)
    d[0x40:0x80] = b'\xff' * 0x40          # the executed-in-place code cave

    mod = Mod(what='xip test mod', target='test', fw_versions=('9.99',),
             sig=sig, start=0, end=len(sig), args=[], literal=False)
    return profile, bytes(d), {'xip': mod}


def test_xip_profile_places_the_routine_in_place_with_no_load_record(tmp_path):
    profile, data, mods = build_xip_image()
    routine = b'\x01\x02\x03\x04'
    blob = tmp_path / 'xip.bin'
    blob.write_bytes(routine)

    out = sigpatch.patch(profile, lambda name: str(blob), data, mods, ['xip'])

    # the routine landed inside the declared cave, at flash_base + its own offset
    seg_off = out.index(routine, 0x40, 0x80)
    assert profile.ram_base <= profile.flash_base + seg_off < profile.ram_end
    # nothing outside the cave changed except the call site itself
    sig_off = sigpatch.find_sig(sigpatch.Image(profile, data), mods['xip'])
    assert out[:sig_off] == data[:sig_off]
    assert out[0x40:seg_off] == data[0x40:seg_off]


def test_xip_profile_refuses_when_the_cave_is_too_small(tmp_path):
    profile, data, mods = build_xip_image()
    profile = profile._replace(ram_end=profile.ram_base + 2)   # smaller than any routine
    blob = tmp_path / 'xip.bin'
    blob.write_bytes(b'\x01\x02\x03\x04')
    with pytest.raises(SystemExit, match='no erased span'):
        sigpatch.patch(profile, lambda name: str(blob), data, mods, ['xip'])


# -- sigpatch.py: the literal-pool safety check ----------------------------------

def build_image_with_literal_pool(pool_inside_span):
    """A signature whose replaced span (start=0, end=len(sig)) either does or
    does not contain a word that a mov.l elsewhere in the image still reads,
    via PC-relative addressing, after the patch."""
    flash_base = 0x1000
    profile = sigpatch.ImageProfile(flash_base=flash_base, loadtab_off=0, loadtab_n=0,
                                    ram_base=flash_base + 0x200, ram_end=flash_base + 0x240)
    sig = [0xAAA1, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888]
    d = bytearray(0x300)
    struct.pack_into('>H', d, 0x100, 0x1010)
    for i, v in enumerate(sig):
        struct.pack_into('>H', d, 0x102 + 2 * i, v)
    struct.pack_into('>H', d, 0x102 + 2 * len(sig), 0xE0E0)
    d[0x200:0x240] = b'\xff' * 0x40

    # pool_off is just an existing word's address -- what a reader elsewhere
    # loads from it is unimportant, only whether that address falls inside
    # the span the mod is about to overwrite. mov.l's PC-relative disp is an
    # unsigned forward offset, so the reader sits before the pool it reads.
    pool_off, reader_pc = (0x104, 0xF0) if pool_inside_span else (0x180, 0x140)
    disp = (pool_off - ((reader_pc & ~3) + 4)) // 4
    struct.pack_into('>H', d, reader_pc, 0xD000 | disp)   # mov.l @(disp,PC),r0

    mod = Mod(what='pool test mod', target='test', fw_versions=('9.99',),
             sig=sig, start=0, end=len(sig), args=[], literal=False)
    return profile, bytes(d), {'m': mod}


def test_patch_refuses_to_overwrite_a_literal_pool_still_read_elsewhere(tmp_path):
    profile, data, mods = build_image_with_literal_pool(pool_inside_span=True)
    blob = tmp_path / 'm.bin'
    blob.write_bytes(b'\x00\x09' * 2)
    with pytest.raises(SystemExit, match='would overwrite a literal pool'):
        sigpatch.patch(profile, lambda name: str(blob), data, mods, ['m'])


def test_patch_allows_a_literal_pool_outside_the_replaced_span(tmp_path):
    profile, data, mods = build_image_with_literal_pool(pool_inside_span=False)
    blob = tmp_path / 'm.bin'
    blob.write_bytes(b'\x00\x09' * 2)
    out = sigpatch.patch(profile, lambda name: str(blob), data, mods, ['m'])
    assert out != data


def test_a_mov_l_inside_its_own_replaced_span_is_not_a_conflict_with_itself(tmp_path):
    # A signature that itself contains a mov.l reading a literal from later
    # in the SAME replaced span: both the reader and its literal are
    # discarded together, which must not trip the safety check.
    flash_base = 0x1000
    profile = sigpatch.ImageProfile(flash_base=flash_base, loadtab_off=0, loadtab_n=0,
                                    ram_base=flash_base + 0x200, ram_end=flash_base + 0x240)
    disp = 2   # literal at (pc&~3)+4+2*4 = sig word 6, later in the same span
    sig = [0xD000 | disp, 0x2222, 0x3333, 0x4444, 0, 0, 0xCAFE, 0xF00D, 0x8888]
    d = bytearray(0x300)
    for i, v in enumerate(sig):
        struct.pack_into('>H', d, 0x100 + 2 * i, v)
    d[0x200:0x240] = b'\xff' * 0x40

    mod = Mod(what='self-pool mod', target='test', fw_versions=('9.99',),
             sig=sig, start=0, end=len(sig), args=[], literal=False)
    blob = tmp_path / 'r.bin'
    blob.write_bytes(b'\x00\x09' * 2)

    out = sigpatch.patch(profile, lambda name: str(blob), bytes(d), {'m': mod}, ['m'])
    assert out != bytes(d)


# -- patch_main.py: CLI shape matches patch_gui.py -------------------------------

def test_patch_main_list_shows_the_real_mod():
    r = run_script('mods/patch_main.py', '--list')
    assert r.returncode == 0, r.stderr
    assert 'wave3data' in r.stdout


needs_sh4_as = pytest.mark.skipif(not shutil.which('sh4-linux-gnu-as'),
                                  reason='sh4-linux-gnu-as not on PATH')


@needs_sh4_as
def test_patch_main_assemble_rebuilds_the_committed_blob(tmp_path):
    blob = patch_main.blob_path('wave3data')
    before = open(blob, 'rb').read()
    r = run_script('mods/patch_main.py', '--assemble')
    assert r.returncode == 0, r.stderr
    after = open(blob, 'rb').read()
    assert after == before


def test_patch_main_unknown_mod_prints_usage(tmp_path):
    src = tmp_path / 'in.bin'
    src.write_bytes(b'\x00' * 16)
    r = run_script('mods/patch_main.py', src, tmp_path / 'out.bin', 'not_a_mod')
    assert r.returncode != 0
    assert 'usage' in r.stdout + r.stderr


# -- main_decode.py / main_encode.py: the S-record codec, on a made-up section ---

def build_main_section(image, version='9.99', prefix=b''):
    """A synthetic MAIN section: an arbitrary prefix (standing in for the
    bootloader/emergency-updater S-records) plus the packed image at
    MAIN_IMG_OFF, an S7 terminator, and the section's own CRC trailer."""
    label = ('CDJ-2000NXS2MAINVer%s        0' % version).encode().ljust(32, b' ')[:32]
    lines = []
    for off in range(0, len(prefix), 32):
        chunk = prefix[off:off + 32].ljust(32, b'\xff')
        lines.append(main_encode._srecord(off, chunk))
    packed = main_encode.encode_image(image)
    for off in range(0, len(packed), 32):
        chunk = packed[off:off + 32]
        if len(chunk) < 32:
            chunk += b'\xff' * (32 - len(chunk))
        lines.append(main_encode._srecord(main_decode.MAIN_IMG_OFF + off, chunk))
    lines.append(b'S705A00000005A')
    body = b'\r\n'.join(lines) + b'\r\n'
    section = label + body
    return section + struct.pack('<H', gui_encode.crc16_xmodem(section))


def test_main_encode_round_trips_through_main_decode():
    image = b'synthetic MAIN image payload ' * 500
    section = build_main_section(image, version='9.99')
    label, declared, avail, out = main_decode.decode(section)
    assert patch_update.section_version(label) == '9.99'
    assert out == image


def test_main_encode_carries_the_bootloader_prefix_forward_unchanged():
    image = b'X' * 4000
    prefix = b'\xAB' * 96          # stands in for the untouched IPL bytes
    section = build_main_section(image, prefix=prefix)
    prefix_lines = [line for addr, _, line in main_decode.srecords(section[32:])
                    if addr < main_decode.MAIN_IMG_OFF]

    patched = image[:10] + b'PATCHED' + image[17:]
    new_section = main_encode.encode(section, patched)

    _, _, _, redecoded = main_decode.decode(new_section)
    assert redecoded == patched
    new_prefix_lines = [line for addr, _, line in main_decode.srecords(new_section[32:])
                        if addr < main_decode.MAIN_IMG_OFF]
    assert new_prefix_lines == prefix_lines


def test_main_encode_recomputes_a_self_consistent_checksum():
    image = b'Y' * 9000
    section = build_main_section(image)
    reencoded = main_encode.encode(section, image)
    trailer = struct.unpack('>H', reencoded[-2:])[0]
    # main's trailer is little-endian (MAIN is little-endian, unlike the GUI's)
    assert struct.unpack('<H', reencoded[-2:])[0] == gui_encode.crc16_xmodem(reencoded[:-2])
    assert trailer != gui_encode.crc16_xmodem(reencoded[:-2])   # (not big-endian)


# -- patch_update.py: the real 'main' target -------------------------------------

def test_registry_includes_main_once_it_has_mods(monkeypatch):
    dummy = Mod(what='registry test', target='main', fw_versions=('1.87',),
               sig=[0x1234], start=0, end=1, args=[], literal=False)
    monkeypatch.setitem(patch_main.MODS, 'regtest', dummy)
    reg = patch_update.registry()
    assert reg['regtest'] == ('main', patch_main)


def test_list_cli_would_show_a_main_mod(monkeypatch):
    dummy = Mod(what='a MAIN test mod', target='main', fw_versions=('1.87',),
               sig=[0x1234], start=0, end=1, args=[], literal=False)
    monkeypatch.setitem(patch_main.MODS, 'regtest', dummy)
    monkeypatch.setattr(sys, 'argv', ['patch_update.py', '--list'])
    buf = io.StringIO()
    with contextlib.redirect_stdout(buf):
        patch_update.main()
    assert 'regtest  [main Ver1.87] a MAIN test mod' in buf.getvalue()


# -- gated on the real, user-supplied firmware -----------------------------------

REAL_SECTION3 = path('..', 'extract', 'section3.bin')
REAL_MAIN = path('..', 'extract', 'main_unpacked.bin')
needs_real_main = pytest.mark.skipif(not (os.path.exists(REAL_SECTION3) and os.path.exists(REAL_MAIN)),
                                     reason='extract/section3.bin or extract/main_unpacked.bin not present')


@needs_real_main
def test_real_section3_decodes_to_the_known_main_image():
    section = open(REAL_SECTION3, 'rb').read()
    label, declared, avail, image = main_decode.decode(section)
    assert patch_update.section_version(label) == '1.87'
    real_main = open(REAL_MAIN, 'rb').read()
    # lzss_decode.py's own driver runs 102 bytes past the declared stream
    # (see its module docstring); main_decode.decode() stops at the boundary
    # the bootloader itself checks, so it is 102 bytes shorter.
    assert image == real_main[:len(image)]


@needs_real_main
def test_real_section3_repacks_byte_identical_with_no_mods(tmp_path):
    section = open(REAL_SECTION3, 'rb').read()
    _, _, _, image = main_decode.decode(section)
    reencoded = main_encode.encode(section, image)
    _, _, _, redecoded = main_decode.decode(reencoded)
    assert redecoded == image


@needs_real_main
def test_real_main_dummy_mod_lands_in_the_confirmed_code_cave(tmp_path, monkeypatch):
    # No real MAIN mod exists yet; this proves the mechanism -- profile, cave,
    # call-site splicing, the S-record round trip and the flash checksum --
    # against the real firmware, with a signature this test picks itself and
    # a mod that is never added to patch_main.MODS.
    image = open(REAL_MAIN, 'rb').read()
    off = 0x300000
    sig = list(struct.unpack_from('<8H', image, off))
    dummy = Mod(what='test-only dummy', target='main', fw_versions=('1.87',),
               sig=sig, start=0, end=len(sig), args=[], literal=False)
    blob = tmp_path / 'dummy.bin'
    blob.write_bytes(b'\x00\x09' * 2)

    patched = sigpatch.patch(patch_main.MAIN_PROFILE, lambda n: str(blob), image, {'dummy': dummy}, ['dummy'])
    assert patched != image
    cave_off = patch_main.MAIN_PROFILE.ram_base - patch_main.MAIN_PROFILE.flash_base
    assert patched[cave_off:cave_off + 4] == b'\x00\x09\x00\x09'
    assert patched[off:off + 16] != image[off:off + 16]

    section = open(REAL_SECTION3, 'rb').read()
    new_section = main_encode.encode(section, patched)
    _, _, _, redecoded = main_decode.decode(new_section)
    assert redecoded == patched

    settings = tmp_path / 'settings.bin'
    settings.write_bytes(b'\xff' * 0x2000)
    sec_path = tmp_path / 'sec3.bin'
    sec_path.write_bytes(new_section)
    flash_path = tmp_path / 'flash.bin'
    r = run_script('scripts/firmware/make_flash.py', flash_path, settings, sec_path)
    assert r.returncode == 0, r.stderr
    flat = flash_path.read_bytes()
    length = struct.unpack_from('<I', flat, main_decode.MAIN_IMG_OFF)[0]
    stream_end = main_decode.MAIN_IMG_OFF + 4 + length
    stored = struct.unpack_from('<H', flat, stream_end)[0]
    assert stored == sum(flat[main_decode.MAIN_IMG_OFF:stream_end]) & 0xFFFF


@needs_real_main
def test_patch_flash_erases_its_window_and_leaves_everything_else_alone(tmp_path):
    section = open(REAL_SECTION3, 'rb').read()
    settings = tmp_path / 'settings.bin'
    settings.write_bytes(b'\xff' * 0x2000)
    sec_path = tmp_path / 'sec3.bin'
    sec_path.write_bytes(section)
    flash_path = tmp_path / 'flash.bin'
    r = run_script('scripts/firmware/make_flash.py', flash_path, settings, sec_path)
    assert r.returncode == 0, r.stderr
    orig_flat = flash_path.read_bytes()

    image = open(REAL_MAIN, 'rb').read()
    off = 0x300000
    sig = list(struct.unpack_from('<8H', image, off))
    dummy = Mod(what='test-only dummy', target='main', fw_versions=('1.87',),
               sig=sig, start=0, end=len(sig), args=[], literal=False)
    # patch_flash() goes through patch_main.patch(), whose blob_path() is
    # fixed to mods/ -- unlike the direct sigpatch.patch() calls elsewhere in
    # this file, this one needs the routine there, not under tmp_path.
    blob = patch_main.blob_path('dummy')
    with open(blob, 'wb') as f:
        f.write(b'\x00\x09' * 2)
    patch_main.MODS['dummy'] = dummy
    try:
        new_flat = patch_main.patch_flash(orig_flat, ['dummy'])
    finally:
        del patch_main.MODS['dummy']
        os.remove(blob)

    assert len(new_flat) == len(orig_flat)
    assert new_flat[:main_decode.MAIN_IMG_OFF] == orig_flat[:main_decode.MAIN_IMG_OFF]
    assert new_flat[0x7F6000:0x7F8000] == orig_flat[0x7F6000:0x7F8000]
    length = struct.unpack_from('<I', new_flat, main_decode.MAIN_IMG_OFF)[0]
    stream_end = main_decode.MAIN_IMG_OFF + 4 + length
    stored = struct.unpack_from('<H', new_flat, stream_end)[0]
    assert stored == sum(new_flat[main_decode.MAIN_IMG_OFF:stream_end]) & 0xFFFF
    assert all(b == 0xFF for b in new_flat[stream_end + 2:make_flash.MAIN_END])
