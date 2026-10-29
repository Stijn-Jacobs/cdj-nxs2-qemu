# SPDX-License-Identifier: GPL-2.0-or-later
"""The hot cues (0x30) and the memory cues and loops (0x2d) Serato sends a
CDJ-3000X. The hot cues are in the layout the NXS2's own arm reads and reach
it as they come; a 3000X's memory cue record has a kind byte in front of what
the NXS2 reads, and x3000rxview rewrites the list in the NXS2 form. Run here on
the interpreter with the reports of recorded sessions and with lists built by
hand; where the real 1.87 MAIN is extracted, the firmware's own report
framework, arms and converters run on what x3000rxview leaves them."""
import functools
import struct

import pytest

from test_cdj3000x_rx import MAIN, RET, STACK, needs_main
from test_cdj3000x_wave import AUTH, FRAMEWORK, MEMSET, Deck, Sh4 as WaveSh4, load

BASE = 0x08000000
MEMCPY = 0x085336DC
HOT_CUE_CONVERTER = 0x084D6BFC
CLEAR8 = 0x08403CBC
MS_TO_TICKS = 0x084042F4
# saving and restoring the interrupt mask, and the time the list was built
NO_OPS = (0x08404A38, 0x08404A5E, 0x08404822)
# the short cue list (message 0x32): the track length at +12, 10 memory entries
# from +0xA0 and 8 hot cue entries from +0x140, 16 bytes each
CUE_LIST = 0x0B546758
MEMORY_ENTRIES = CUE_LIST + 0xA0
HOT_CUE_ENTRIES = CUE_LIST + 0x140
EMPTY = (0, 0, 0xFFFFFFFF, 0xFFFFFFFF)
# where the 0x2d arm keeps the list: the count at +0xE18, 10 slots of 6 bytes from +0xE22
MARKERS = 0x0B531F5C

TRACK_INFO = load('cdj3000x_track_info.txt')
LENGTHS = [r for r in TRACK_INFO if r[1] == 0x30]
# A load Serato sent the deck with the CDJ-3000X identity: length 2:30.674,
# slot A empty, slot B a cue of colour 37 at 0:19.248.
HOT_CUES = bytes.fromhex('003001000100021ea20200000000000001250013f8').ljust(1024, b'\0')


class MainSh4(WaveSh4):
    """The wave test's interpreter plus what the report framework and the key
    parser use: the multiply, the FPSCR and MACL saves, the indexed loads and
    stores, braf and jmp."""

    def step_op(self, op, pc):
        n, m = op >> 8 & 15, op >> 4 & 15
        r = self.r
        if op & 0xF0FF == 0x4012:
            r[n] = (r[n] - 4) & 0xFFFFFFFF
            self.put32(r[n], self.macl)
        elif op & 0xF0FF == 0x4016:
            self.macl = self.u32(r[n])
            r[n] = (r[n] + 4) & 0xFFFFFFFF
        elif op & 0xF0FF == 0x001A:
            r[n] = self.macl
        elif op & 0xF00F == 0x0007:
            self.macl = r[n] * r[m] & 0xFFFFFFFF
        elif op & 0xF0FF == 0x006A:
            r[n] = self.fpscr
        elif op & 0xF0FF == 0x406A:
            self.fpscr = r[n]
        elif op & 0xF0FF == 0x4062:
            r[n] = (r[n] - 4) & 0xFFFFFFFF
            self.put32(r[n], self.fpscr)
        elif op & 0xF0FF == 0x4066:
            self.fpscr = self.u32(r[n])
            r[n] = (r[n] + 4) & 0xFFFFFFFF
        elif op & 0xF00F == 0x000D:
            r[n] = self.s16(r[0] + r[m])
        elif op & 0xF00F == 0x000C:
            r[n] = self.s8(r[0] + r[m])
        elif op & 0xF00F == 0x0006:
            self.put32(r[0] + r[n] & 0xFFFFFFFF, r[m])
        elif op & 0xF00F == 0x0005:
            self.write(r[0] + r[n] & 0xFFFFFFFF, struct.pack('<H', r[m] & 0xFFFF))
        elif op & 0xF0FF == 0x0023:
            self.delay(pc)
            return pc + 4 + r[n] & 0xFFFFFFFF
        elif op & 0xF0FF == 0x402B:
            target = r[n]
            self.delay(pc)
            return target
        else:
            return super().step_op(op, pc)
        return pc + 2


@functools.cache
def main_image():
    with open(MAIN, 'rb') as f:
        return f.read()


def in_image(addr):
    """A stub that runs the firmware's own code at addr."""
    def run(cpu):
        pr = cpu.pr
        cpu.pr = RET
        cpu.run(addr)
        cpu.pr = pr
    return run


def ticks(ms):
    """The firmware's ms to 1/150 s, done in floating point there."""
    return ms * 15 // 100


def firmware_cpu():
    """The extracted MAIN in memory, with the library calls the framework and
    the cue converters make."""
    cpu = MainSh4()
    image = main_image()
    for off in range(0, len(image), 4096):
        cpu.pages[(BASE + off) >> 12] = bytearray(image[off:off + 4096].ljust(4096, b'\0'))
    cpu.stubs[MEMSET] = lambda c: c.write(c.r[4], bytes([c.r[5] & 0xFF]) * c.r[6])
    cpu.stubs[MEMCPY] = lambda c: c.write(c.r[4], c.read(c.r[5], c.r[6]))
    cpu.stubs[FRAMEWORK] = in_image(FRAMEWORK)
    cpu.stubs[HOT_CUE_CONVERTER] = in_image(HOT_CUE_CONVERTER)
    cpu.stubs[CLEAR8] = in_image(CLEAR8)
    cpu.stubs[MS_TO_TICKS] = lambda c: c.r.__setitem__(0, ticks(c.r[4]))
    for addr in NO_OPS:
        cpu.stubs[addr] = lambda c: c.r.__setitem__(0, 0)
    return cpu


def entries(view, at, count):
    """The cue list entries (colour, flags, position, end) the firmware makes of a stock view."""
    cpu = firmware_cpu()
    cpu.write(0x0A100000, view)
    cpu.r[4], cpu.r[5], cpu.r[15], cpu.pr = 0x0A100000, 64, STACK, RET
    cpu.run(FRAMEWORK)
    return [struct.unpack('<IBxxxII', cpu.read(at + 16 * k, 16)) for k in range(count)], cpu


def ms(minutes, seconds, millis):
    return (minutes * 60 + seconds) * 1000 + millis


def length_ms(report):
    return ms(*struct.unpack_from('<BBH', report, 6))


@pytest.mark.parametrize('report', LENGTHS + [HOT_CUES], ids=length_ms)
def test_the_hot_cues_reach_the_stock_path_as_they_come(report):
    deck = Deck()
    assert deck.feed(report) == report[:64]
    assert deck.injected == []


@needs_main
def test_the_firmware_shows_the_captured_hot_cue_in_slot_b():
    got, cpu = entries(HOT_CUES[:64], HOT_CUE_ENTRIES, 8)
    # the colour word is the hue wheel's RGB of colour 37 above a 9; flag 0x40 is a used entry
    assert got == [EMPTY, (0xFF750009, 0x40, ticks(ms(0, 19, 248)), 0xFFFFFFFF)] + [EMPTY] * 6
    assert cpu.u32(CUE_LIST + 12) == ticks(ms(2, 30, 674))


@needs_main
@pytest.mark.parametrize('report', LENGTHS, ids=length_ms)
def test_a_load_without_hot_cues_leaves_every_slot_empty(report):
    got, _ = entries(report[:64], HOT_CUE_ENTRIES, 8)
    assert got == [EMPTY] * 8


def marker_report(records, count=None, length=(2, 30, 674)):
    """A 0x2d as Serato sends a CDJ-3000X: the length, the count and six-byte
    records (kind, colour, minutes, seconds, ms)."""
    body = b''.join(struct.pack('<BBBBH', *r) for r in records)
    head = struct.pack('<BBHB', *length, len(records) if count is None else count)
    return (bytes([0, 0x2D, 1, 0, 1, 0]) + head + body).ljust(1024, b'\0')


def nxs2_markers(report):
    """The 64-byte view the NXS2 reads: the same length, the records a 3000X
    keeps (count 1 to 10, kind 1 to 3), each without its kind, bit 7 on any
    kind but a memory cue (1), a colour outside 1 to 8 as red (2), and zeros
    for the rest of the ten."""
    count = report[10]
    kept = []
    for k in range(count if 1 <= count <= 10 else 0):
        kind, colour = report[11 + 6 * k:13 + 6 * k]
        if 1 <= kind <= 3:
            colour = colour if 1 <= colour <= 8 else 2
            kept.append(bytes([colour | (0x80 if kind != 1 else 0)]) + report[13 + 6 * k:17 + 6 * k])
    records = b''.join(kept).ljust(50, b'\0')
    return report[:10] + bytes([len(kept)]) + records + report[61:64]


MEMORY_CUES = [(1, 1, 0, 12, 345), (1, 4, 1, 2, 0), (1, 8, 3, 59, 999)]
LOOPS = [(2, 5, 0, 30, 0), (3, 6, 0, 45, 500), (2, 2, 1, 0, 1)]
TEN = [(1 + k % 3, 1 + k % 8, k, 7 * k, 100 * k) for k in range(10)]
LISTS = {
    'empty': marker_report([]),
    'memory cues': marker_report(MEMORY_CUES),
    'loops': marker_report(LOOPS),
    'both': marker_report(MEMORY_CUES + LOOPS),
    'ten': marker_report(TEN),
    'eleven': marker_report(TEN + [(1, 1, 9, 0, 0)]),
    'count past the records': marker_report(MEMORY_CUES, count=5),
    'other kinds': marker_report([(0, 1, 0, 1, 0), (4, 2, 0, 2, 0), (1, 3, 0, 3, 0), (0xFF, 4, 0, 4, 0)]),
    'other colours': marker_report([(1, 0, 0, 1, 0), (2, 9, 0, 2, 0), (1, 0xFF, 0, 3, 0), (3, 0x85, 0, 4, 0)]),
}


@pytest.mark.parametrize('name', LISTS)
def test_the_markers_reach_the_nxs2_in_its_five_byte_records(name):
    report = LISTS[name]
    deck = Deck()
    assert deck.feed(report) == nxs2_markers(report)
    assert deck.injected == []


def test_a_list_past_ten_is_empty_as_on_the_3000x():
    view = Deck().feed(LISTS['eleven'])
    assert view[10] == 0 and view[11:61] == bytes(50)


def test_skipped_records_close_up():
    view = Deck().feed(LISTS['other kinds'])
    assert view[10] == 1 and view[11:16] == bytes([3, 0, 3, 0, 0])


def test_the_markers_are_left_alone_before_authentication():
    deck = Deck()
    deck.cpu.put32(AUTH, 0)
    report = LISTS['both']
    assert deck.feed(report) == report[:64]


@needs_main
def test_the_firmware_shows_memory_cues_plain_and_the_other_kinds_flagged():
    report = LISTS['both']
    got, cpu = entries(Deck().feed(report), MEMORY_ENTRIES, 10)
    # flag 0x40 is a used entry; the NXS2 adds 0x10 for a record with bit 7
    want = [(colour, 0x40 | (0x10 if kind != 1 else 0), ticks(ms(m, s, x)), 0xFFFFFFFF)
            for kind, colour, m, s, x in MEMORY_CUES + LOOPS]
    assert got == want + [EMPTY] * 4
    assert cpu.u32(CUE_LIST + 12) == ticks(ms(2, 30, 674))


@needs_main
@pytest.mark.parametrize('name', ['empty', 'eleven'])
def test_the_firmware_takes_an_empty_list(name):
    _, cpu = entries(Deck().feed(LISTS[name]), MEMORY_ENTRIES, 10)
    assert cpu.u32(MARKERS + 0xE18) == 0
    assert cpu.read(MARKERS + 0xE22, 60) == bytes(60)
