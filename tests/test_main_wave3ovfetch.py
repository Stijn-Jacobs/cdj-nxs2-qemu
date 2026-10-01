# SPDX-License-Identifier: GPL-2.0-or-later
"""The wave3ovfetch MAIN mod: hooked on the colour-preview reader's call to
the PWV4 tag reader, found by signature in the real image and, run from the
patched site through the shared trampoline, packing the track's PWV6 into
the 1800-byte overview the wave3ovdata mod publishes. The tag fetch is
replaced by a stub that returns what the firmware's own fetch returns (a
20-byte record whose +16 points at the entries)."""
import math
import os
import random
import struct

import pytest

import patch_main
import sigpatch
from test_main_wave3data import FirmwareSh4, put_record

SIG_AT = 0x200
SPAN = SIG_AT + 8               # 4-aligned, like the firmware's
RESUME = SPAN + 18
CAVE = (0x400, 0x4400)
FRAME, HANDLE, HASH = 0xF000, 0x5150, 0x7A7A
FETCH, MALLOC, FREE = 0x082545F8, 0x08344A9C, 0x08344B42
SHARED, SHARED_LOW = 0x081FFFFC, 0xE000
PWV6_REC, HEAP = 0xA000, 0xB000
NEVER_WRITTEN = 0xFFFFFFFF
SENTINELS = {8: 1200, 9: 0xC0DE0009, 10: 0xFFE7FFFF, 11: 0xC0DE000B, 12: 0xC0DE000C,
             13: HANDLE, 14: 0xC0DE000E}


def build_image():
    d = bytearray(0x10000)
    for k, op in enumerate(patch_main.PREVIEW_READ['sig']):
        struct.pack_into('<H', d, SIG_AT + 2 * k, op)
    d[CAVE[0]:CAVE[1]] = b'\xff' * (CAVE[1] - CAVE[0])
    profile = sigpatch.ImageProfile(flash_base=0, loadtab_off=0, loadtab_n=0,
                                    ram_base=CAVE[0], ram_end=CAVE[1], byteorder='<')
    return profile, bytes(d)


def patched_image():
    profile, data = build_image()
    image = bytearray(sigpatch.patch(profile, patch_main.blob_path, data, patch_main.MODS,
                                     ['wave3ovfetch']))
    at = image.find(struct.pack('<I', SHARED), CAVE[0])
    image[at:at + 4] = struct.pack('<I', SHARED_LOW)
    return bytes(image)


def stub_fetch(cpu, image, record, fetches):
    """Stand in for the transcribed PWV6 fetch routine (found by its opening
    pair of pushes) when the track has the tag; without it the routine runs
    for real against the stubbed generic fetch, which finds none."""
    (start,) = [a for a in range(CAVE[0], CAVE[1], 2)
                if struct.unpack_from('<HH', image, a) == (0x2FD6, 0x2FE6)]

    def run(cpu):
        assert cpu.u32(cpu.r[7]) == 0 and cpu.u32(cpu.r[6]) == 0
        fetches.append((cpu.r[4], cpu.r[5]))
        cpu.put32(cpu.r[7], record)
        cpu.put32(cpu.r[6], cpu.u32(record + 4) + 20)

    if record:
        cpu.stubs[start] = run


def run_reader(shared=NEVER_WRITTEN, pwv6=None, heap=HEAP, cpu=None):
    """Patch the site and run from the start of the span to its resume point
    with the registers the reader has there. pwv6 is (entry bytes, bytes per
    entry, entry count or None for all) of the tag's record, None when the
    track has no such tag. Returns the cpu."""
    image = patched_image()
    cpu = cpu or FirmwareSh4(image)
    cpu.heap = heap
    cpu.fetches = []
    cpu.put32(SHARED_LOW, shared)
    cpu.put32(FRAME + 4, HASH)
    if pwv6:
        put_record(cpu, PWV6_REC, *pwv6)
    stub_fetch(cpu, image, PWV6_REC if pwv6 else None, cpu.fetches)
    for reg, value in SENTINELS.items():
        cpu.r[reg] = value
    cpu.r[15] = FRAME
    cpu.run(SPAN, stop=RESUME)
    return cpu


def raw_overview(seed=3):
    rng = random.Random(seed)
    return bytes(rng.choice([0, 1, 128, 200]) if rng.random() < 0.2 else rng.randrange(0, 129)
                 for _ in range(3 * 1200))


def curve8(h):
    return round(128 * (0.5 - 0.5 * math.cos(math.pi * min(h, 128) / 128)))


def expected_overview(raw):
    entries = [tuple(min(b, 128) for b in raw[i:i + 3]) for i in range(0, len(raw), 3)]

    def stack5(e):
        return 6 * e[0] + 3 * e[1] + 6 * curve8(e[2])

    d = min(128, max(1, max(e[0] + e[1] + curve8(e[2]) for e in entries)))
    out = bytearray()
    for col in range(600):
        a, b = entries[2 * col], entries[2 * col + 1]
        low, mid, high = b if stack5(b) >= stack5(a) else a
        l5 = 6 * low
        m5 = l5 + 3 * mid
        f5 = m5 + 6 * curve8(high)
        out += bytes(min(40, max(1, -(-8 * s5 // d))) for s5 in (f5, m5, l5))
    return bytes(out)


def test_registered_for_main_as_a_hook_on_the_preview_reader():
    mod = patch_main.MODS['wave3ovfetch']
    assert mod.target == 'main' and mod.hook and not mod.literal
    assert dict(sig=mod.sig, start=mod.start, end=mod.end) == patch_main.PREVIEW_READ


REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')


@pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
def test_real_main_has_the_signature_once_and_takes_every_3band_mod_in_any_order():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    img = sigpatch.Image(patch_main.MAIN_PROFILE, data)
    mod = patch_main.MODS['wave3ovfetch']
    assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == 0x08289C54
    names = ['wave3data', 'wave3detail', 'wave3ovdata', 'wave3ovfetch']
    assert patch_main.patch(data, names) == patch_main.patch(data, names[::-1])


def test_the_tag_is_asked_for_with_the_readers_handle_and_hash():
    cpu = run_reader(pwv6=(raw_overview(), 3, None))
    assert cpu.fetches == [(HANDLE, HASH)]


def test_asks_for_the_pwv6_tag_from_its_own_image():
    assert run_reader().fetched == [(b'PWV6', b'2EX')]


def test_the_overview_is_packed_behind_the_shared_word_once_complete():
    raw = raw_overview()
    cpu = run_reader(pwv6=(raw, 3, None))
    at = cpu.u32(SHARED_LOW)
    assert at == HEAP
    assert bytes(cpu.mem[at:at + 1800]) == expected_overview(raw)
    assert cpu.freed == [PWV6_REC]


def test_frees_the_previous_tracks_overview_and_clears_the_shared_word():
    cpu = run_reader(shared=0x9100)
    assert cpu.freed == [0x9100]
    assert cpu.u32(SHARED_LOW) == 0


@pytest.mark.parametrize('shared', [0, NEVER_WRITTEN])
def test_no_overview_to_free_frees_nothing(shared):
    cpu = run_reader(shared=shared)
    assert cpu.freed == []
    assert cpu.u32(SHARED_LOW) == 0


@pytest.mark.parametrize('what, pwv6', [
    ('no PWV6 tag', None),
    ('fewer than 1200 entries', (raw_overview()[:3 * 1199], 3, None)),
    ('more than 1200 entries', (raw_overview() + b'\0\0\0', 3, None)),
    ('entries of another size', (raw_overview(), 2, 1200)),
])
def test_no_overview_is_shared_unless_pwv6_has_exactly_1200_entries(what, pwv6):
    cpu = run_reader(pwv6=pwv6, shared=0x9100)
    assert cpu.u32(SHARED_LOW) == 0, what
    assert cpu.freed == [0x9100] + ([PWV6_REC] if pwv6 else [])


def test_a_failed_allocation_shares_nothing_and_frees_the_raw_record():
    cpu = run_reader(pwv6=(raw_overview(), 3, None), heap=0)
    assert cpu.u32(SHARED_LOW) == 0
    assert cpu.freed == [PWV6_REC]


def test_a_loud_track_clips_at_the_strips_40_rows():
    cpu = run_reader(pwv6=(bytes([128, 128, 128]) * 1200, 3, None))
    assert set(cpu.mem[HEAP:HEAP + 1800]) == {40}


def test_the_reader_resumes_with_its_own_registers():
    cpu = run_reader(pwv6=(raw_overview(), 3, None))
    assert {reg: cpu.r[reg] for reg in SENTINELS} == SENTINELS
    assert cpu.r[15] == FRAME - 4                       # the span's own push of the entry count
    assert (cpu.r[5], cpu.r[6], cpu.r[7]) == (HASH, FRAME - 4 + 24, FRAME - 4 + 20)


def test_second_call_leaves_the_relocated_words_as_they_are():
    cpu = run_reader(pwv6=(raw_overview(), 3, None))
    cpu = run_reader(pwv6=(raw_overview(5), 3, None), shared=cpu.u32(SHARED_LOW), cpu=cpu)
    assert cpu.u32(SHARED_LOW) != 0 and cpu.fetches == [(HANDLE, HASH)]
