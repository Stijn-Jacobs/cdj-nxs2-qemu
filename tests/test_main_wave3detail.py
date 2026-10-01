# SPDX-License-Identifier: GPL-2.0-or-later
"""The wave3detail MAIN mod: hooked on the waveform requester's success path,
found by signature in the real image and, run from the patched site through
the shared trampoline, copying the words the wave3data mod packed over the
entries of the record the firmware has just re-encoded, or leaving them alone
when there are none or the record is another length."""
import os
import random
import struct

import pytest

import patch_main
import sigpatch
from test_main_wave3data import FirmwareSh4

SIG_AT = 0x202                  # puts the span at 2 mod 4, like the firmware's
SPAN = SIG_AT + 2 * patch_main.DETAIL_CONVERTED['start']
RESUME = SPAN + 24
CAVE = (0x1000, 0x5000)
FRAME = 0xF000
FREE = 0x08344B42
CTX, CTX_FIXED = 0xC000, 0x0B531E9C
WORDS, WORDS_FIXED = 0xE000, 0x081FFFF4
OLD_REC, NEW_REC, NEW_SIZE, PACKED = 0x9000, 0xA000, 0xAB62, 0xB000
FPMASK = 0xFFE7FFFF
NEVER_WRITTEN = 0xFFFFFFFF
N = 64


def build_image():
    d = bytearray(0x10000)
    for k, op in enumerate(patch_main.DETAIL_CONVERTED['sig']):
        struct.pack_into('<H', d, SIG_AT + 2 * k, op)
    # the span's three literal loads: the shared context, the free call, the context again
    for k, value in zip((0, 1, 4), (CTX, FREE, CTX)):
        op = struct.unpack_from('<H', d, SPAN + 2 * k)[0]
        struct.pack_into('<I', d, ((SPAN + 2 * k) & ~3) + 4 + (op & 0xFF) * 4, value)
    d[CAVE[0]:CAVE[1]] = b'\xff' * (CAVE[1] - CAVE[0])
    profile = sigpatch.ImageProfile(flash_base=0, loadtab_off=0, loadtab_n=0,
                                    ram_base=CAVE[0], ram_end=CAVE[1], byteorder='<')
    return profile, bytes(d)


def patched_image():
    profile, data = build_image()
    out = bytearray(sigpatch.patch(profile, patch_main.blob_path, data, patch_main.MODS,
                                   ['wave3detail']))
    for real, low in ((0x0B531EB8, CTX + 28), (WORDS_FIXED, WORDS)):
        at = out.find(struct.pack('<I', real), CAVE[0])
        out[at:at + 4] = struct.pack('<I', low)
    return bytes(out)


def stock_words(n=N, seed=4):
    rng = random.Random(seed)
    return b''.join(struct.pack('<H', rng.randrange(0x10000) & 0xFFFC) for _ in range(n))


def packed_words(n=N, seed=9):
    rng = random.Random(seed)
    return b''.join(struct.pack('<H', rng.randrange(0x10000) | 1) for _ in range(n))


def run_requester(words=PACKED, count=N, record_count=N, entry_bytes=2, record=NEW_REC):
    """Run from the span's first instruction to the site's resume point with
    the re-encoded record in the frame, as the firmware leaves it there.
    Returns the cpu; the record's entries start as stock_words()."""
    cpu = FirmwareSh4(patched_image())
    cpu.put32(WORDS, words)
    cpu.put32(WORDS + 4, count)
    cpu.put32(CTX + 28, OLD_REC)
    struct.pack_into('<IIHHII', cpu.mem, NEW_REC, record_count, 2 * record_count, entry_bytes,
                     0, 0, NEW_REC + 20)
    cpu.mem[NEW_REC + 20:NEW_REC + 20 + 2 * N] = stock_words()
    cpu.mem[PACKED:PACKED + 2 * N] = packed_words()
    cpu.put32(FRAME + 12, NEW_SIZE)
    cpu.put32(FRAME + 16, record)
    for reg in (8, 9):
        cpu.r[reg] = 0xC0DE0000 + reg
    cpu.r[14], cpu.r[15] = FPMASK, FRAME
    cpu.run(SPAN, stop=RESUME)
    return cpu


def entries(cpu):
    return bytes(cpu.mem[NEW_REC + 20:NEW_REC + 20 + 2 * N])


def test_registered_for_main_as_a_hook_on_the_converted_record():
    mod = patch_main.MODS['wave3detail']
    assert mod.target == 'main' and mod.hook and not mod.literal
    assert dict(sig=mod.sig, start=mod.start, end=mod.end) == patch_main.DETAIL_CONVERTED


REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')


@pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
def test_real_main_has_the_signature_once():
    with open(REAL_MAIN, 'rb') as f:
        img = sigpatch.Image(patch_main.MAIN_PROFILE, f.read())
    mod = patch_main.MODS['wave3detail']
    assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == 0x0849F83A


def test_the_span_still_stores_the_new_record_and_frees_the_old_one():
    cpu = run_requester()
    assert cpu.u32(CTX + 28) == NEW_REC and cpu.u32(CTX + 32) == NEW_SIZE
    assert cpu.freed == [OLD_REC]
    assert cpu.r[10] == FREE and cpu.r[15] == FRAME


def test_the_packed_words_replace_the_records_entries():
    cpu = run_requester()
    assert entries(cpu) == packed_words()
    assert struct.unpack_from('<IIHHII', cpu.mem, NEW_REC) == (N, 2 * N, 2, 0, 0, NEW_REC + 20)


@pytest.mark.parametrize('words', [0, NEVER_WRITTEN])
def test_without_a_buffer_the_stock_words_stay(words):
    assert entries(run_requester(words=words)) == stock_words()


@pytest.mark.parametrize('what, kwargs', [
    ('another entry count', dict(count=N - 1)),
    ('another record length', dict(record_count=N - 1)),
    ('entries of another size', dict(entry_bytes=3)),
    ('no record', dict(record=0)),
])
def test_a_record_that_does_not_match_the_buffer_is_left_alone(what, kwargs):
    assert entries(run_requester(**kwargs)) == stock_words(), what


def test_the_site_resumes_with_the_registers_the_span_left():
    cpu = run_requester()
    assert (cpu.r[12], cpu.r[13]) == (NEW_REC, NEW_SIZE)
    assert cpu.r[8] == 0xC0DE0008 and cpu.r[9] == 0xC0DE0009
