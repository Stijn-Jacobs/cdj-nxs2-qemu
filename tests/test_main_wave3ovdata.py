# SPDX-License-Identifier: GPL-2.0-or-later
"""The wave3ovdata MAIN mod: hooked on the colour-preview publisher's copy of
the 600 overview records, found by signature in the real image, and, run from
the patched site through the shared trampoline, replacing those records with
the packed PWV6 bands that the wave3ovfetch mod left behind -- or leaving the
stock copy alone when there are none."""
import os
import random
import struct

import pytest

import patch_main
import sigpatch
from test_main_wave3data import FirmwareSh4

SIG_AT = 0x200                  # where the real signature starts (4-aligned)
SPAN = SIG_AT + 6               # the hooked span, 2 mod 4 like the firmware's
RESUME = SPAN + 10
CAVE = (0x400, 0x3E00)
FRAME = 0xF000
MEMCPY = 0x085336DC
SLOT, STOCK, PACKED, SHARED_LOW, HEADER = 0x4000, 0x6000, 0x8000, 0x3F00, 0x3F10
SLOT_FIXED, SHARED = 0x0B569528, 0x081FFFFC
RECORDS = 600
NEVER_WRITTEN = 0xFFFFFFFF


def build_image():
    d = bytearray(0x4000)
    for k, op in enumerate(patch_main.OVERVIEW_PUBLISH['sig']):
        struct.pack_into('<H', d, SIG_AT + 2 * k, op)
    struct.pack_into('<I', d, (SPAN & ~3) + 4 + 0x4E * 4, MEMCPY)
    d[CAVE[0]:CAVE[1]] = b'\xff' * (CAVE[1] - CAVE[0])
    profile = sigpatch.ImageProfile(flash_base=0, loadtab_off=0, loadtab_n=0,
                                    ram_base=CAVE[0], ram_end=CAVE[1], byteorder='<')
    return profile, bytes(d)


def patched_image():
    profile, data = build_image()
    out = bytearray(sigpatch.patch(profile, patch_main.blob_path, data, patch_main.MODS,
                                   ['wave3ovdata']))
    for real, low in ((SLOT_FIXED, SLOT), (SHARED, SHARED_LOW)):
        at = out.find(struct.pack('<I', real), CAVE[0])
        out[at:at + 4] = struct.pack('<I', low)
    return bytes(out)


class PublisherSh4(FirmwareSh4):
    def call(self, target, resume):
        if target != MEMCPY:
            return super().call(target, resume)
        r = self.r
        self.mem[r[4]:r[4] + r[6]] = self.mem[r[5]:r[5] + r[6]]
        self.copies += 1
        for k in range(8):
            r[k] = 0xDEAD0000 + k
        return resume


def packed_overview(seed=11):
    rng = random.Random(seed)
    return bytes(rng.randrange(1, 41) for _ in range(3 * RECORDS))


def run_publisher(shared):
    """Run from the span's first instruction to the site's resume point, with
    the stock records copied into the slot by the span's own call. Returns
    the cpu and the stock records."""
    image = patched_image()
    cpu = PublisherSh4(image)
    cpu.copies = 0
    stock = bytes(random.Random(2).randrange(256) for _ in range(6 * RECORDS))
    cpu.mem[STOCK:STOCK + len(stock)] = stock
    cpu.mem[SLOT + len(stock):SLOT + len(stock) + 8] = b'\x77' * 8
    cpu.put32(SHARED_LOW, shared)
    cpu.mem[PACKED:PACKED + 3 * RECORDS] = packed_overview()
    cpu.r[2], cpu.r[4], cpu.r[5], cpu.r[6] = 0x1111, SLOT, STOCK, 6 * RECORDS
    cpu.r[14], cpu.r[15] = HEADER, FRAME
    for k, reg in enumerate(range(8, 14)):
        cpu.r[reg] = 0xC0DE0000 + k
    cpu.run(SPAN, stop=RESUME)
    return cpu, stock


def test_registered_for_main_as_a_hook_on_the_overview_copy():
    mod = patch_main.MODS['wave3ovdata']
    assert mod.target == 'main' and mod.hook and not mod.literal
    assert dict(sig=mod.sig, start=mod.start, end=mod.end) == patch_main.OVERVIEW_PUBLISH


def test_the_three_band_row_enables_every_main_mod_with_the_display_ones():
    path = os.path.join(os.path.dirname(patch_main.__file__), 'mods.conf')
    (row,) = [line.split('|') for line in open(path, encoding='utf-8')
              if line.startswith('three_band|')]
    assert set(row[1].split('+')) == {
        'CDJ_GUI_WAVE3', 'CDJ_GUI_WAVE3OV', 'CDJ_MAIN_WAVE3DATA', 'CDJ_MAIN_WAVE3DETAIL',
        'CDJ_MAIN_WAVE3OVFETCH', 'CDJ_MAIN_WAVE3OVDATA'}


REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')


@pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
def test_real_main_has_the_signature_once():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    img = sigpatch.Image(patch_main.MAIN_PROFILE, data)
    mod = patch_main.MODS['wave3ovdata']
    assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == 0x084E7CAA


@pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
def test_the_four_main_mods_patch_together_in_any_order_and_clear_of_the_shared_words():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    names = ['wave3data', 'wave3detail', 'wave3ovdata', 'wave3ovfetch']
    assert patch_main.patch(data, names) == patch_main.patch(data, names[::-1])


def test_the_shared_words_lie_outside_what_the_mods_occupy():
    _, total = sigpatch.layout(patch_main.blob_path, patch_main.MODS)
    assert patch_main.MAIN_PROFILE.ram_base + total <= 0x081FFFF4 < SHARED < patch_main.MAIN_PROFILE.ram_end


@pytest.mark.parametrize('shared', [0, NEVER_WRITTEN])
def test_without_an_overview_the_stock_records_stay(shared):
    cpu, stock = run_publisher(shared)
    assert cpu.copies == 1
    assert bytes(cpu.mem[SLOT:SLOT + len(stock)]) == stock


def test_with_an_overview_every_record_carries_the_bands_and_the_mark():
    cpu, stock = run_publisher(PACKED)
    packed = packed_overview()
    # Native halfwords ABC<<8|AB, A<<8|0x3B, 0xFFF0|nibble, laid out as the stock
    # records are: the display reads them back as the bytes ABC AB A 3B FF FN.
    # The nibble stays what the stock record had (the phrase mod's colour).
    want = b''.join(struct.pack('<3H', packed[3 * i] << 8 | packed[3 * i + 1],
                                packed[3 * i + 2] << 8 | 0x3B,
                                0xFFF0 | stock[6 * i + 4] & 15)
                    for i in range(RECORDS))
    assert cpu.copies == 1
    assert bytes(cpu.mem[SLOT:SLOT + 6 * RECORDS]) == want
    assert bytes(cpu.mem[SLOT + 6 * RECORDS:SLOT + 6 * RECORDS + 8]) == b'\x77' * 8


def test_the_site_resumes_with_the_spans_own_registers_and_the_callee_saved_ones():
    cpu, _ = run_publisher(PACKED)
    assert [cpu.r[reg] for reg in range(8, 14)] == [0xC0DE0000 + k for k in range(6)]
    assert cpu.r[14] == HEADER and cpu.r[15] == FRAME
