# SPDX-License-Identifier: GPL-2.0-or-later
"""The wave3data MAIN mod, executed at the address it is placed at: the
blob is copied into its cave verbatim, so the routine has to find its own
data there, and it has to hand control back to the site's resume point.
With the PWV7 tag fetch replaced by a stub that returns what the firmware's
own fetch returns (a 20-byte record whose +16 points at the entries), it
packs the 3-band words into a buffer of its own and publishes it through
the two words the wave3detail mod reads."""
import math
import random
import struct

import pytest

import patch_main
import sigpatch
from test_main_osc import Sh4

SITE = 0x102                    # span start; not 4-aligned, like the firmware's
RESUME = SITE + 16
CAVE = (0x1000, 0x3000)
FRAME = 0x7F00
PWV5_READER, FETCH, MALLOC, FREE = 0x08289E98, 0x082545F8, 0x08344A9C, 0x08344B42
CLOSE = 0x0827A34E
DETAIL, DETAIL_LOW = 0x081FFFF4, 0x7E00         # the buffer, then its entry count
PWV5_REC, PWV7_REC, HEAP = 0x8300, 0x8800, 0xB000
NEVER_WRITTEN = 0xFFFFFFFF
N = 48


def build_image():
    d = bytearray(0x8000)
    for k, op in enumerate(patch_main.MODS['wave3data'].sig):
        struct.pack_into('<H', d, SITE + 2 * k, op)
    d[0x40:0x100] = b'\xA5' * 0xC0      # what an unrelocated pointer would read
    d[CAVE[0]:CAVE[1]] = b'\xff' * (CAVE[1] - CAVE[0])
    profile = sigpatch.ImageProfile(flash_base=0, loadtab_off=0, loadtab_n=0,
                                    ram_base=CAVE[0], ram_end=CAVE[1], byteorder='<')
    return profile, bytes(d)


def patched_image():
    profile, data = build_image()
    mods = {'wave3data': patch_main.MODS['wave3data']}
    image = bytearray(sigpatch.patch(profile, patch_main.blob_path, data, mods, ['wave3data']))
    at = image.find(struct.pack('<I', DETAIL), CAVE[0])
    image[at:at + 4] = struct.pack('<I', DETAIL_LOW)
    return bytes(image)


class FirmwareSh4(Sh4):
    """Sh4 plus the branches, compares and loads the routine and its two
    transcribed fetches use, with the firmware calls they make stubbed."""

    def __init__(self, code):
        super().__init__(code, 0)
        self.freed, self.fetched = [], []
        self.stubs = {}
        self.heap = HEAP
        self.pwv5_rec = 0
        self.macl = 0

    def run(self, pc, stop=Sh4.RET):
        for _ in range(3000000):
            if pc == stop:
                return
            pc = self.step(struct.unpack_from('<H', self.mem, pc)[0], pc)
        raise AssertionError('no return to %#x after 3000000 instructions' % stop)

    def s16(self, a):
        return struct.unpack_from('<h', self.mem, a)[0] & 0xFFFFFFFF

    def step(self, op, pc):
        r = self.r
        n, m = op >> 8 & 15, op >> 4 & 15
        imm = op & 0xFF
        simm = imm - 256 if imm & 0x80 else imm
        disp12 = (op & 0xFFF) - 0x1000 if op & 0x800 else op & 0xFFF
        if op & 0xF00F == 0x6005:                               # mov.w @Rm+,Rn
            r[n] = self.s16(r[m])
            r[m] += 2
        elif op & 0xFF00 == 0x8500:                             # mov.w @(disp,Rm),R0
            r[0] = self.s16(r[m] + (op & 15) * 2)
        elif op & 0xFF00 == 0x8400:                             # mov.b @(disp,Rm),R0
            r[0] = self.s8(r[m] + (op & 15))
        elif op & 0xF00F == 0x200B:                             # or Rm,Rn
            r[n] |= r[m]
        elif op & 0xF00F == 0x2001:                             # mov.w Rm,@Rn
            struct.pack_into('<H', self.mem, r[n], r[m] & 0xFFFF)
        elif op & 0xF00F == 0x6000:                             # mov.b @Rm,Rn
            r[n] = self.s8(r[m])
        elif op & 0xF00F == 0x2000:                             # mov.b Rm,@Rn
            self.mem[r[n]] = r[m] & 0xFF
        elif op & 0xF00F == 0x600C:                             # extu.b Rm,Rn
            r[n] = r[m] & 0xFF
        elif op & 0xF00F == 0x6008:                             # swap.b Rm,Rn
            r[n] = r[m] & 0xFFFF0000 | (r[m] & 0xFF) << 8 | r[m] >> 8 & 0xFF
        elif op & 0xF00F == 0x2008:                             # tst Rm,Rn
            self.t = int(r[n] & r[m] == 0)
        elif op & 0xF00F == 0x3000:                             # cmp/eq Rm,Rn
            self.t = int(r[n] == r[m])
        elif op & 0xF00F == 0x3002:                             # cmp/hs Rm,Rn
            self.t = int(r[n] >= r[m])
        elif op & 0xFF00 == 0x8800:                             # cmp/eq #imm,R0
            self.t = int(r[0] == simm & 0xFFFFFFFF)
        elif op & 0xFF00 == 0xCB00:                             # or #imm,R0
            r[0] |= imm
        elif op & 0xF0FF == 0x4001:                             # shlr Rn
            self.t = r[n] & 1
            r[n] >>= 1
        elif op & 0xF0FF == 0x4000:                             # shll Rn
            self.t = r[n] >> 31
            r[n] = r[n] << 1 & 0xFFFFFFFF
        elif op & 0xF0FF == 0x4024:                             # rotcl Rn
            t = r[n] >> 31
            r[n] = (r[n] << 1 | self.t) & 0xFFFFFFFF
            self.t = t
        elif op & 0xF0FF == 0x4018:                             # shll8 Rn
            r[n] = r[n] << 8 & 0xFFFFFFFF
        elif op & 0xF0FF == 0x4028:                             # shll16 Rn
            r[n] = r[n] << 16 & 0xFFFFFFFF
        elif op & 0xF00F == 0x400D:                             # shld Rm,Rn
            sh = r[m] - (1 << 32 if r[m] & 0x80000000 else 0)
            r[n] = r[n] << sh & 0xFFFFFFFF if sh >= 0 else r[n] >> -sh
        elif op & 0xF00F == 0x0007:                             # mul.l Rm,Rn
            self.macl = r[n] * r[m] & 0xFFFFFFFF
        elif op & 0xF0FF == 0x001A:                             # sts macl,Rn
            r[n] = self.macl
        elif op & 0xF0FF == 0x4011:                             # cmp/pz Rn
            self.t = int(r[n] < 0x80000000)
        elif op & 0xF0FF == 0x4015:                             # cmp/pl Rn
            self.t = int(0 < r[n] < 0x80000000)
        elif op & 0xF00F == 0x3008:                             # sub Rm,Rn
            r[n] = (r[n] - r[m]) & 0xFFFFFFFF
        elif op & 0xF0FF == 0x002A:                             # sts pr,Rn
            r[n] = self.pr
        elif op & 0xF0FF == 0x402A:                             # lds Rm,pr
            self.pr = r[n]
        elif op == 0x4F12:                                      # sts.l macl,@-r15
            r[15] -= 4
            self.put32(r[15], 0)
        elif op == 0x4F16:                                      # lds.l @r15+,macl
            r[15] += 4
        elif op & 0xFF00 == 0x8900:                             # bt
            if self.t:
                return pc + 4 + simm * 2
        elif op & 0xFF00 == 0x8D00:                             # bt/s
            self.delay(pc)
            return pc + 4 + simm * 2 if self.t else pc + 4
        elif op & 0xFF00 == 0x8F00:                             # bf/s
            self.delay(pc)
            return pc + 4 + simm * 2 if not self.t else pc + 4
        elif op & 0xF000 == 0xA000:                             # bra
            self.delay(pc)
            return pc + 4 + disp12 * 2
        elif op & 0xF000 == 0xB000:                             # bsr
            target = pc + 4 + disp12 * 2
            self.delay(pc)
            if target in self.stubs:
                self.stubs[target](self)
                return pc + 4
            self.pr = pc + 4
            return target
        else:
            return super().step(op, pc)
        return pc + 2

    def call(self, target, resume):
        r = self.r
        if target == PWV5_READER:
            self.put32(r[7], self.pwv5_rec)
            self.put32(r[6], self.u32(self.pwv5_rec + 4) + 20 if self.pwv5_rec else 0)
        elif target == FETCH:
            self.fetched.append((self.cstr(r[6]), self.cstr(r[7])))
            r[0] = 0xFFFFFFFF
        elif target == MALLOC:
            r[0], self.heap = self.heap, self.heap + r[4]
        elif target == FREE:
            self.freed.append(r[4])
        else:
            return target
        for k in range(1, 8):
            r[k] = 0xDEAD0000 + k
        return resume

    def cstr(self, a):
        return bytes(self.mem[a:self.mem.index(0, a)])


def put_record(cpu, at, entries, entry_bytes, count=None):
    """A tag fetch's output at `at`: +0 count, +4 payload bytes, +8 bytes per
    entry, +16 the payload, which follows the record."""
    struct.pack_into('<IIHHII', cpu.mem, at, len(entries) // entry_bytes if count is None else count,
                     len(entries), entry_bytes, 0, 0, at + 20)
    cpu.mem[at + 20:at + 20 + len(entries)] = entries


def stub_fetch(cpu, image, record):
    """Stand in for the transcribed PWV7 fetch routine (found by its opening
    pair of pushes) when the track has the tag; without it the routine runs
    for real against the stubbed generic fetch, which finds none. out_ptr
    must come in clear, since the real routine reads it on some failures."""
    (start,) = [a for a in range(CAVE[0], CAVE[1], 2)
                if struct.unpack_from('<HH', image, a) == (0x2FD6, 0x2FE6)]

    def run(cpu):
        assert cpu.u32(cpu.r[7]) == 0 and cpu.u32(cpu.r[6]) == 0
        cpu.put32(cpu.r[7], record)
        cpu.put32(cpu.r[6], cpu.u32(record + 4) + 20)

    if record:
        cpu.stubs[start] = run


def run_site(detail=NEVER_WRITTEN, count=0, pwv5=None, pwv7=None, pwv5_entry_bytes=2, heap=HEAP):
    """Patch the site and run from the start of the span to its resume point
    with the registers the builder has there. pwv5 is the RGB entry bytes the
    stock fetch left (None = it fetched nothing); pwv7 is (entry bytes, bytes
    per entry, entry count or None for all) of the tag's record, None when
    the track has no such tag. Returns the cpu."""
    image = patched_image()
    cpu = FirmwareSh4(image)
    cpu.heap = heap
    cpu.put32(DETAIL_LOW, detail)
    cpu.put32(DETAIL_LOW + 4, count)
    cpu.put32(FRAME + 16, 0x5150)                               # the ANLZ handle
    cpu.r[4], cpu.r[10], cpu.r[14], cpu.r[15] = 0x5150, 0x1234, 0xFFE7FFFF, FRAME
    if pwv5 is not None:
        cpu.pwv5_rec = PWV5_REC
        put_record(cpu, PWV5_REC, pwv5, pwv5_entry_bytes)
    if pwv7:
        put_record(cpu, PWV7_REC, *pwv7)
    stub_fetch(cpu, image, PWV7_REC if pwv7 else None)
    cpu.run(SITE, stop=RESUME)
    return cpu


# -- the encoding, written out from the description rather than from the routine ----

def curve5(h):
    return max(1, math.ceil(31 * (0.5 - 0.5 * math.cos(math.pi * h / 128)) - 1e-9))


def band5(v):
    return max(1, -(-31 * min(v, 128) // 128))


def detail_word(low, mid, high):
    return curve5(min(high, 128)) << 11 | band5(mid) << 6 | band5(low) << 1 | 1


def rgb_entries(n=N, seed=5):
    rng = random.Random(seed)
    return b''.join(struct.pack('>H', rng.randrange(0x10000) & 0xFFFC) for _ in range(n))


def band_entries(n=N, seed=7):
    rng = random.Random(seed)
    edge = [0, 1, 127, 128, 129, 255]
    return bytes(rng.choice(edge) if rng.random() < 0.3 else rng.randrange(0, 131)
                 for _ in range(3 * n))


def expected_detail(raw):
    return b''.join(struct.pack('<H', detail_word(*raw[i:i + 3])) for i in range(0, len(raw), 3))


def published(cpu):
    """(buffer address, entry count) behind the two shared words."""
    return cpu.u32(DETAIL_LOW), cpu.u32(DETAIL_LOW + 4)


def test_runs_to_the_resume_point_with_the_close_call_loaded():
    cpu = run_site()
    assert cpu.r[10] == CLOSE
    assert cpu.r[15] == FRAME


def test_asks_for_the_pwv7_tag_from_its_own_image():
    cpu = run_site(pwv5=rgb_entries())
    assert cpu.fetched == [(b'PWV7', b'2EX')]


def test_frees_the_previous_tracks_buffer_and_clears_the_shared_word():
    cpu = run_site(detail=0x9100, count=N)
    assert cpu.freed == [0x9100]
    assert published(cpu)[0] == 0


@pytest.mark.parametrize('detail', [0, NEVER_WRITTEN])
def test_no_buffer_to_free_frees_nothing(detail):
    cpu = run_site(detail=detail)
    assert cpu.freed == []
    assert published(cpu)[0] == 0


def test_second_call_leaves_the_relocated_words_as_they_are():
    image = patched_image()
    cpu = FirmwareSh4(image)
    cpu.pwv5_rec = PWV5_REC
    for _ in range(2):
        cpu.put32(FRAME + 16, 0x5150)
        put_record(cpu, PWV5_REC, rgb_entries(), 2)
        cpu.r[4], cpu.r[10], cpu.r[14], cpu.r[15] = 0x5150, 0x1234, 0xFFE7FFFF, FRAME
        cpu.run(SITE, stop=RESUME)
    assert cpu.fetched == [(b'PWV7', b'2EX')] * 2


def test_the_packed_words_are_published_once_complete_and_the_pwv5_record_is_left_alone():
    raw = band_entries()
    stock = rgb_entries()
    cpu = run_site(pwv5=stock, pwv7=(raw, 3, None))
    at, count = published(cpu)
    assert (at, count) == (HEAP, N)
    assert bytes(cpu.mem[at:at + 2 * N]) == expected_detail(raw)
    assert bytes(cpu.mem[PWV5_REC + 20:PWV5_REC + 20 + 2 * N]) == stock
    assert cpu.freed == [PWV7_REC]


def test_every_band_value_encodes_to_its_own_field():
    raw = bytes(v for low in (0, 1, 64, 128, 130) for v in (low, 130 - low, low // 2))
    n = len(raw) // 3
    cpu = run_site(pwv5=rgb_entries(n), pwv7=(raw, 3, None))
    at, count = published(cpu)
    words = struct.unpack('<%dH' % n, bytes(cpu.mem[at:at + 2 * n]))
    assert count == n and all(w & 1 for w in words)
    assert [(w >> 1 & 31, w >> 6 & 31, w >> 11 & 31) for w in words] == \
        [(band5(raw[i]), band5(raw[i + 1]), curve5(min(raw[i + 2], 128))) for i in range(0, len(raw), 3)]


@pytest.mark.parametrize('what, pwv7', [
    ('no PWV7 tag', None),
    ('a shorter PWV7', (band_entries(N - 1), 3, None)),
    ('a longer PWV7', (band_entries(N + 1), 3, None)),
    ('PWV7 entries of another size', (band_entries(), 2, N)),
])
def test_nothing_is_published_unless_a_pwv7_of_the_same_length_arrives(what, pwv7):
    cpu = run_site(pwv5=rgb_entries(), pwv7=pwv7)
    assert published(cpu)[0] == 0, what
    assert cpu.freed == ([PWV7_REC] if pwv7 else [])


def test_pwv5_with_other_entry_sizes_publishes_nothing():
    cpu = run_site(pwv5=rgb_entries(), pwv5_entry_bytes=1, pwv7=(band_entries(), 3, None))
    assert published(cpu)[0] == 0
    assert cpu.freed == []


def test_a_pwv5_fetch_that_returned_nothing_publishes_nothing():
    cpu = run_site(pwv7=(band_entries(), 3, None))
    assert published(cpu)[0] == 0
    assert cpu.fetched == []


def test_a_failed_allocation_publishes_nothing_and_frees_the_raw_record():
    cpu = run_site(pwv5=rgb_entries(), pwv7=(band_entries(), 3, None), heap=0)
    assert published(cpu)[0] == 0
    assert cpu.freed == [PWV7_REC]
