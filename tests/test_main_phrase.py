# SPDX-License-Identifier: GPL-2.0-or-later
"""The phrase MAIN mods. phrasefetch is hooked on the colour-preview reader and
reduces the track's PSSI and PQT2 tags to a 600-column table; phrasedata is
hooked on the overview publisher and writes that table into the records.
Both are run here as placed blobs, with the firmware's tag fetch, malloc, free
and memset stubbed, against the real PSSI of a rekordbox 7 export (masked)."""
import os
import struct

import pytest

import patch_main
import sigpatch

CODE = 0x08140000
SHARED, SLOT = 0x081FFFF0, 0x0B569528
FETCH, MALLOC, FREE, MEMSET = 0x082545F8, 0x08344A9C, 0x08344B42, 0x08533D50
HEAP = 0x09000000
HANDLE, HASH = 0x5150, 0x7A7A
STACK = 0x0A000000
RET = 0xFFFF0000

# The PSSI tag of a 148.8 s track: 11 phrases, mood high, masked.
PSSI = bytes.fromhex(
    '50535349000000200000012800000018000bd6edf905f0f9b8f9f53ff4f7ecf4fef2f4feecd7ecf905f0f9b8'
    'f9f4ddf4f6ecf4fef3f4ffecd6eef924f0fbb8f9f4ddf4f6ecf4fef3f4ffecd6ecf8059df9bbf985ddf6f6ecf4'
    'fef3f4ffecd6ecf905f0f9b8f8f410f4f2ec25fef1f4ffecd6ecf905f0f9b8f9f4ddf4f6edf413f3f1ff1dd6ee'
    'f905f0f9b8f9f4ddf4f6ecf4fef3f4ffecd6ecff04e1f9bdf9f5ddf4f6ecf4fef3f4ffecd6ecf905f0f9b8fef5'
    'ecf4f3ecf5fef3f4ffecd6ecf905f0f9b8f9f4ddf4f6e4f5aff3f7ffecd6ecf905f0f9b8f9f4ddf4f6ecf4fef3'
    'f4f6ed57ecfb05f0f9b8f9f4ddf4f6ecf4fef3f4ffedd771f90ff158b8fcf4dcf4f6ecf4fef3f4ffecd6ecf905'
    'f0f9b8f9ffdc35f6eaf4fff3f4ffecd6ecf905f0f9b8f9f4ddf4f6')
BEATS, FIRST_MS, LAST_MS = 496, 61, 148534
KEY = bytes.fromhex('cbe1eefae5eeadeee9d2e9ebe1e9f3e8e9f4e1')
PALETTE_HIGH = {1: 2, 2: 8, 3: 3, 5: 5, 6: 6}


def pqt2(beats=BEATS, first=FIRST_MS, last=LAST_MS):
    """The extended beat grid: the first and last beat's time and the beat
    count at +28, +36 and +40, then two bytes a beat."""
    fields = struct.pack('>11I', 0, 0x01000002, 0, 0x14E24, first, 0x44E24, last, beats, 0x2C9B94C, 0, 0)
    return b'PQT2' + struct.pack('>II', 56, 56 + 2 * beats) + fields + bytes(2 * beats)


def unmask(tag):
    n = struct.unpack_from('>H', tag, 16)[0]
    body = bytearray(tag[18:])
    for k in range(len(body)):
        body[k] ^= (KEY[k % 19] + n) & 0xFF
    return bytes(body)


def mask(tag, body):
    n = struct.unpack_from('>H', tag, 16)[0]
    return tag[:18] + bytes(b ^ (KEY[k % 19] + n) & 0xFF for k, b in enumerate(body))


def beat_time(beat, first=FIRST_MS, last=LAST_MS, beats=BEATS):
    """A beat's time in 16 ms units; the beats are evenly spaced between the
    first and the last, 0 before the grid and the track length after it."""
    span = (last - first) >> 4
    length = (last >> 4) + span // (beats - 1)
    if beat < 1:
        return 0
    if beat > beats:
        return length
    return (first >> 4) + (beat - 1) * span // (beats - 1)


def expected_table(tag, first=FIRST_MS, last=LAST_MS, beats=BEATS):
    """The table the routine is specified to leave: a phrase's columns are those
    whose centre falls inside it."""
    body = unmask(tag)
    n = struct.unpack_from('>H', tag, 16)[0]
    mood = struct.unpack_from('>H', body, 0)[0]
    end_beat = struct.unpack_from('>H', body, 8)[0]
    length = (last >> 4) + ((last - first) >> 4) // (beats - 1)

    def col(beat):
        return min(600, (beat_time(beat, first, last, beats) * 1200 + length) // (2 * length))

    colours = {1: PALETTE_HIGH, 2: {1: 2, 2: 7, 3: 7, 4: 7, 5: 7, 6: 7, 7: 7, 8: 4, 9: 5, 10: 6},
               3: {1: 1, 2: 8, 3: 8, 4: 8, 5: 8, 6: 8, 7: 8, 8: 4, 9: 5, 10: 6}}[mood if mood in (1, 3) else 2]
    table = bytearray(600)
    beat_list = [struct.unpack_from('>H', body, 14 + 24 * i + 2)[0] for i in range(n)] + [end_beat]
    for i in range(n):
        kind = struct.unpack_from('>H', body, 14 + 24 * i + 4)[0]
        for c in range(col(beat_list[i]), col(beat_list[i + 1])):
            table[c] = colours.get(kind, 0)
    return bytes(table)


class Sh4:
    """Just the instructions the two routines use, with a sparse 32-bit memory
    and the firmware calls stubbed by address."""

    def __init__(self):
        self.pages = {}
        self.r = [0] * 16
        self.pr = RET
        self.t = 0
        self.macl = 0
        self.fpscr = 0
        self.stubs = {}

    def read(self, a, n):
        out = bytearray()
        for k in range(n):
            page = self.pages.get((a + k) >> 12)
            out.append(page[(a + k) & 0xFFF] if page else 0)
        return bytes(out)

    def write(self, a, data):
        for k, b in enumerate(data):
            self.pages.setdefault((a + k) >> 12, bytearray(4096))[(a + k) & 0xFFF] = b

    def u32(self, a):
        return struct.unpack('<I', self.read(a, 4))[0]

    def put32(self, a, v):
        self.write(a, struct.pack('<I', v & 0xFFFFFFFF))

    def s8(self, a):
        return struct.unpack('<b', self.read(a, 1))[0] & 0xFFFFFFFF

    def s16(self, a):
        return struct.unpack('<h', self.read(a, 2))[0] & 0xFFFFFFFF

    def run(self, pc, limit=4000000):
        for _ in range(limit):
            if pc == RET:
                return
            pc = self.step(pc)
        raise AssertionError('no return after %d instructions' % limit)

    def fetch_op(self, pc):
        return struct.unpack('<H', self.read(pc, 2))[0]

    def delay(self, pc):
        slot = self.fetch_op(pc + 2)
        assert self.step_op(slot, pc + 2) == pc + 4, 'branch in a delay slot'

    def step(self, pc):
        return self.step_op(self.fetch_op(pc), pc)

    def step_op(self, op, pc):
        r = self.r
        n, m, d = op >> 8 & 15, op >> 4 & 15, op & 15
        imm = op & 0xFF
        simm = imm - 256 if imm & 0x80 else imm
        disp12 = (op & 0xFFF) - 0x1000 if op & 0x800 else op & 0xFFF
        hi, lo = op >> 12, op & 15
        M = 0xFFFFFFFF
        if hi == 0xE:
            r[n] = simm & M
        elif hi == 0x7:
            r[n] = (r[n] + simm) & M
        elif hi == 0xD:
            r[n] = self.u32((pc & ~3) + 4 + imm * 4)
        elif op & 0xFF00 == 0xC700:
            r[0] = (pc & ~3) + 4 + imm * 4
        elif hi == 0x1:
            self.put32(r[n] + d * 4, r[m])
        elif hi == 0x5:
            r[n] = self.u32(r[m] + d * 4)
        elif hi == 0x6:
            if lo == 0:
                r[n] = self.s8(r[m])
            elif lo == 2:
                r[n] = self.u32(r[m])
            elif lo == 3:
                r[n] = r[m]
            elif lo == 4:
                r[n] = self.s8(r[m])
                r[m] += 1
            elif lo == 6:
                r[n] = self.u32(r[m])
                r[m] += 4
            elif lo == 8:
                r[n] = r[m] & 0xFFFF0000 | (r[m] & 0xFF) << 8 | r[m] >> 8 & 0xFF
            elif lo == 9:
                r[n] = (r[m] << 16 | r[m] >> 16) & M
            elif lo == 12:
                r[n] = r[m] & 0xFF
            else:
                raise AssertionError('unmodelled %#06x at %#x' % (op, pc))
        elif hi == 0x2:
            if lo == 0:
                self.write(r[n], bytes([r[m] & 0xFF]))
            elif lo == 2:
                self.put32(r[n], r[m])
            elif lo == 6:
                r[n] -= 4
                self.put32(r[n], r[m])
            elif lo == 8:
                self.t = int(r[n] & r[m] == 0)
            elif lo == 9:
                r[n] &= r[m]
            elif lo == 10:
                r[n] ^= r[m]
            elif lo == 11:
                r[n] |= r[m]
            else:
                raise AssertionError('unmodelled %#06x at %#x' % (op, pc))
        elif hi == 0x3:
            if lo == 0:
                self.t = int(r[n] == r[m])
            elif lo == 2:
                self.t = int(r[n] >= r[m])
            elif lo == 6:
                self.t = int(r[n] > r[m])
            elif lo == 8:
                r[n] = (r[n] - r[m]) & M
            elif lo == 12:
                r[n] = (r[n] + r[m]) & M
            else:
                raise AssertionError('unmodelled %#06x at %#x' % (op, pc))
        elif op & 0xFF00 == 0x8400:
            r[0] = self.s8(r[m] + d)
        elif op & 0xFF00 == 0x8000:
            self.write(r[m] + d, bytes([r[0] & 0xFF]))
        elif op & 0xFF00 == 0x8800:
            self.t = int(r[0] == simm & M)
        elif op & 0xFF00 == 0xC900:
            r[0] &= imm
        elif op & 0xFF00 == 0xCB00:
            r[0] |= imm
        elif op & 0xF00F == 0x000C:
            r[n] = self.s8(r[0] + r[m])
        elif op & 0xF00F == 0x0007:
            self.macl = r[n] * r[m] & M
        elif op & 0xF0FF == 0x001A:
            r[n] = self.macl
        elif op & 0xF0FF == 0x002A:
            r[n] = self.pr
        elif op & 0xF0FF == 0x006A:
            r[n] = self.fpscr
        elif op & 0xF0FF == 0x406A:
            self.fpscr = r[n]
        elif op & 0xF0FF == 0x402A:
            self.pr = r[n]
        elif op & 0xF0FF == 0x4022:
            r[n] -= 4
            self.put32(r[n], self.pr)
        elif op & 0xF0FF == 0x4026:
            self.pr = self.u32(r[n])
            r[n] += 4
        elif op & 0xF0FF == 0x4000:
            self.t = r[n] >> 31
            r[n] = r[n] << 1 & M
        elif op & 0xF0FF == 0x4001:
            self.t = r[n] & 1
            r[n] >>= 1
        elif op & 0xF0FF == 0x4008:
            r[n] = r[n] << 2 & M
        elif op & 0xF0FF == 0x4009:
            r[n] >>= 2
        elif op & 0xF0FF == 0x4018:
            r[n] = r[n] << 8 & M
        elif op & 0xF0FF == 0x4024:
            t = r[n] >> 31
            r[n] = (r[n] << 1 | self.t) & M
            self.t = t
        elif op & 0xF0FF == 0x4010:
            r[n] = (r[n] - 1) & M
            self.t = int(r[n] == 0)
        elif op & 0xF0FF == 0x4011:
            self.t = int(r[n] < 0x80000000)
        elif op & 0xF0FF == 0x4015:
            self.t = int(0 < r[n] < 0x80000000)
        elif op == 0x0009:
            pass
        elif op == 0x000B:
            back = self.pr
            self.delay(pc)
            return back
        elif op & 0xFF00 == 0x8900:
            if self.t:
                return pc + 4 + simm * 2
        elif op & 0xFF00 == 0x8B00:
            if not self.t:
                return pc + 4 + simm * 2
        elif hi == 0xA:
            self.delay(pc)
            return pc + 4 + disp12 * 2
        elif hi == 0xB:
            target = pc + 4 + disp12 * 2
            self.delay(pc)
            self.pr = pc + 4
            return target
        elif op & 0xF0FF == 0x400B:
            target = r[n]
            self.delay(pc)
            if target not in self.stubs:
                raise AssertionError('call to %#x at %#x' % (target, pc))
            self.stubs[target](self)
            for k in range(1, 8):
                r[k] = 0xDEAD0000 + k
            return pc + 4
        else:
            raise AssertionError('unmodelled %#06x at %#x' % (op, pc))
        return pc + 2


class Deck:
    """A cpu with the blobs loaded and the firmware calls stubbed."""

    def __init__(self, blob, pssi=PSSI, grid=None, fetch_fails=()):
        self.cpu = cpu = Sh4()
        self.heap = HEAP
        self.fetches, self.freed, self.malloced = [], [], []
        self.blocks = {b'PSSI': pssi, b'PQT2': pqt2() if grid is None else grid}
        self.fetch_fails = fetch_fails
        cpu.write(CODE, blob)
        cpu.stubs.update({FETCH: self.fetch, MALLOC: self.malloc, FREE: self.free, MEMSET: self.memset})
        cpu.put32(SHARED, 0xFFFFFFFF)

    def cstr(self, a):
        out = b''
        while self.cpu.read(a, 1) != b'\0' and len(out) < 4:
            out += self.cpu.read(a, 1)
            a += 1
        return out

    def fetch(self, cpu):
        r = cpu.r
        tag, ext = self.cstr(r[6]), self.cstr(r[7])
        size_at, block_at, status_at = (cpu.u32(r[15] + 4 * k) for k in range(3))
        assert (cpu.u32(size_at), cpu.u32(block_at), cpu.u32(status_at)) == (0, 0, 0)
        self.fetches.append((r[4], r[5], tag, ext))
        tag_bytes = self.blocks.get(tag)
        if tag_bytes is None or tag in self.fetch_fails:
            r[0] = 0xFFFFFFFF
            return
        block = self.heap
        self.heap += len(tag_bytes) + 8
        cpu.put32(block, len(tag_bytes))
        cpu.write(block + 4, tag_bytes)
        cpu.put32(block_at, block)
        cpu.put32(size_at, len(tag_bytes) + 4)
        r[0] = 0

    def malloc(self, cpu):
        self.malloced.append(cpu.r[4])
        cpu.r[0], self.heap = self.heap, self.heap + cpu.r[4]
        cpu.write(cpu.r[0], b'\xA5' * cpu.r[4])

    def free(self, cpu):
        self.freed.append(cpu.r[4])

    def memset(self, cpu):
        cpu.write(cpu.r[4], bytes([cpu.r[5] & 0xFF]) * cpu.r[6])

    def run_fetch(self, shared=0xFFFFFFFF):
        cpu = self.cpu
        cpu.put32(SHARED, shared)
        cpu.put32(STACK - 0x1000 + 20, HASH)
        for k in range(8, 15):
            cpu.r[k] = 0xC0DE0000 + k
        cpu.r[13] = HANDLE
        cpu.r[4] = STACK - 0x1000
        cpu.r[15] = STACK
        cpu.fpscr = 0xFFFFFFFF
        cpu.run(CODE)
        return cpu.u32(SHARED)

    def table(self):
        return self.cpu.read(self.cpu.u32(SHARED), 600)


def blob(name):
    with open(patch_main.blob_path(name), 'rb') as f:
        return f.read()


def test_registered_for_main_as_hooks_on_the_reader_and_the_publisher():
    fetch, data = patch_main.MODS['phrasefetch'], patch_main.MODS['phrasedata']
    assert fetch.target == data.target == 'main' and fetch.hook and data.hook
    assert dict(sig=fetch.sig, start=fetch.start, end=fetch.end) == patch_main.PREVIEW_READ
    assert dict(sig=data.sig, start=data.start, end=data.end) == patch_main.OVERVIEW_PUBLISH


def test_the_table_is_the_phrases_laid_over_the_columns():
    deck = Deck(blob('phrasefetch'))
    deck.run_fetch()
    assert deck.table() == expected_table(PSSI)
    table = deck.table()
    assert set(table) == {0, 2, 3, 5, 6, 8}
    assert table[0] == 2 and table[599] == 0
    assert table[400] == 5 and table[580] == 6


def test_the_tags_are_asked_for_with_the_readers_handle_and_hash_and_released():
    deck = Deck(blob('phrasefetch'))
    deck.run_fetch()
    assert deck.fetches == [(HANDLE, HASH, b'PSSI', b'EXT'), (HANDLE, HASH, b'PQT2', b'EXT')]
    assert len(deck.freed) == 2 and deck.malloced == [600]
    assert [deck.cpu.r[k] for k in range(8, 15)] == [0xC0DE0000 + k for k in range(8, 13)] + [HANDLE, 0xC0DE000E]


def test_an_unmasked_body_reads_the_same():
    body = unmask(PSSI)
    deck = Deck(blob('phrasefetch'), pssi=PSSI[:18] + body)
    deck.run_fetch()
    assert deck.table() == expected_table(PSSI)


def test_mid_and_low_moods_use_their_own_colours():
    for mood, want in ((2, {2, 7, 4, 5, 6, 0}), (3, {1, 8, 4, 5, 6, 0})):
        body = bytearray(unmask(PSSI))
        struct.pack_into('>H', body, 0, mood)
        for i in range(11):
            struct.pack_into('>H', body, 14 + 24 * i + 4, [1, 2, 8, 9, 10, 2, 3, 4, 8, 9, 10][i])
        tag = mask(PSSI, bytes(body))
        deck = Deck(blob('phrasefetch'), pssi=tag)
        deck.run_fetch()
        assert deck.table() == expected_table(tag)
        assert set(deck.table()) == want


def test_the_previous_tracks_table_is_dropped_first():
    deck = Deck(blob('phrasefetch'))
    deck.run_fetch(shared=0x08123450)
    assert 0x08123450 in deck.freed


@pytest.mark.parametrize('missing', [b'PSSI', b'PQT2'])
def test_a_track_without_the_tag_leaves_no_table_and_nothing_held(missing):
    deck = Deck(blob('phrasefetch'), fetch_fails=(missing,))
    assert deck.run_fetch() == 0
    assert len(deck.freed) == (0 if missing == b'PSSI' else 1)
    assert deck.malloced == []


@pytest.mark.parametrize('damage', ['count', 'size', 'short'])
def test_a_tag_that_is_not_what_it_should_be_leaves_no_table(damage):
    tag = bytearray(PSSI)
    if damage == 'count':
        tag[16:18] = struct.pack('>H', 65)
    elif damage == 'size':
        tag[12:16] = struct.pack('>I', 20)
    else:
        tag = tag[:100]
        tag[8:12] = struct.pack('>I', 100)
    deck = Deck(blob('phrasefetch'), pssi=bytes(tag))
    assert deck.run_fetch() == 0
    assert len(deck.freed) == 2


def test_a_beat_grid_with_a_single_beat_leaves_no_table():
    deck = Deck(blob('phrasefetch'), grid=pqt2(beats=1))
    assert deck.run_fetch() == 0


def run_publisher(shared_table):
    deck = Deck(blob('phrasedata'))
    cpu = deck.cpu
    stock = bytes((k * 37 + 11) & 0xFF for k in range(3600))
    cpu.write(SLOT, stock)
    if shared_table is None:
        cpu.put32(SHARED, 0xFFFFFFFF)
    else:
        cpu.write(HEAP, shared_table)
        cpu.put32(SHARED, HEAP)
    cpu.r[15] = STACK
    cpu.run(CODE)
    return stock, cpu.read(SLOT, 3600), cpu.read(SLOT + 3600, 8)


def test_every_record_carries_its_columns_phrase_and_nothing_else_changes():
    table = bytes(range(9)) * 66 + bytes(6)
    stock, out, after = run_publisher(table)
    for col in range(600):
        was, now = stock[6 * col:6 * col + 6], out[6 * col:6 * col + 6]
        assert now[4] == was[4] & 0xF0 | table[col]
        assert now[:4] + now[5:] == was[:4] + was[5:]
    assert after == bytes(8)


@pytest.mark.parametrize('shared', [None, bytes(600)])
def test_without_a_table_the_nibbles_are_cleared(shared):
    stock, out, _ = run_publisher(shared)
    for col in range(600):
        assert out[6 * col + 4] == stock[6 * col + 4] & 0xF0
        assert out[6 * col:6 * col + 4] == stock[6 * col:6 * col + 4]


REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')


@pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
def test_real_main_takes_the_phrase_mods_with_the_3band_ones_in_any_order():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    names = ['phrasefetch', 'phrasedata', 'wave3data', 'wave3detail', 'wave3ovdata', 'wave3ovfetch']
    assert patch_main.patch(data, names) == patch_main.patch(data, names[::-1])
    _, total = sigpatch.layout(patch_main.blob_path, patch_main.MODS)
    assert patch_main.MAIN_PROFILE.ram_base + total <= 0x081FFFE0
