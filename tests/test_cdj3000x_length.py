# SPDX-License-Identifier: GPL-2.0-or-later
"""The track length (0x30) Serato sends a CDJ-3000X. The NXS2's arm for that
type reads the same bytes as 8 hot cue slots; x3000rxview keeps it from seeing
them. Run here on the interpreter with the reports of two recorded sessions;
where the real 1.87 MAIN is extracted, the firmware's own report framework runs
on a length to show what it would do with it."""
import functools
import struct

import pytest

from test_cdj3000x_rx import MAIN, RET, STACK, needs_main
from test_cdj3000x_wave import FRAMEWORK, MEMSET, Deck, Sh4 as WaveSh4, load

BASE = 0x08000000
MEMCPY = 0x085336DC
HOT_CUE_CONSUMER = 0x084D6BFC
HOT_CUE_STRUCT = 0x0B531F5C
SLOT = 0x0E60
SLOT_BYTES = 16
SLOTS = 8

TRACK_INFO = load('cdj3000x_track_info.txt')
LENGTHS = [r for r in TRACK_INFO if r[1] == 0x30]


class MainSh4(WaveSh4):
    """The wave test's interpreter plus what the report framework and the key
    parser use: the multiply, the FPSCR and MACL saves, the indexed loads and
    stores and braf."""

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
        else:
            return super().step_op(op, pc)
        return pc + 2


@functools.cache
def main_image():
    with open(MAIN, 'rb') as f:
        return f.read()


def firmware_cpu():
    """The extracted MAIN in memory, with the two library calls the framework makes."""
    cpu = MainSh4()
    image = main_image()
    for off in range(0, len(image), 4096):
        cpu.pages[(BASE + off) >> 12] = bytearray(image[off:off + 4096].ljust(4096, b'\0'))
    cpu.stubs[MEMSET] = lambda c: c.write(c.r[4], bytes([c.r[5] & 0xFF]) * c.r[6])
    cpu.stubs[MEMCPY] = lambda c: c.write(c.r[4], c.read(c.r[5], c.r[6]))
    cpu.stubs[FRAMEWORK] = run_framework
    return cpu


def run_framework(cpu):
    """The framework's own code, run when the hook calls it."""
    pr = cpu.pr
    cpu.pr = RET
    cpu.run(FRAMEWORK)
    cpu.pr = pr


def length_ms(report):
    minutes, seconds, ms = struct.unpack_from('<BBH', report, 6)
    return (minutes * 60 + seconds) * 1000 + ms


@pytest.mark.parametrize('report', LENGTHS, ids=length_ms)
def test_the_track_length_is_not_handed_on(report):
    deck = Deck()
    view = deck.feed(report)
    assert view[1] == 0x3F
    assert deck.injected == []


@needs_main
@pytest.mark.parametrize('report', LENGTHS, ids=length_ms)
def test_the_stock_arm_would_take_the_length_as_a_hot_cue_position(report):
    cpu = firmware_cpu()
    slots = []
    cpu.stubs[HOT_CUE_CONSUMER] = lambda c: slots.append(c.read(HOT_CUE_STRUCT + SLOT, SLOTS * SLOT_BYTES))
    cpu.write(0x0A100000, report[:64])
    cpu.r[4], cpu.r[5], cpu.r[15], cpu.pr = 0x0A100000, 64, STACK, RET
    cpu.run(FRAMEWORK)
    assert len(slots) == 1
    assert struct.unpack_from('<I', slots[0])[0] == length_ms(report)
    assert [slots[0][SLOT_BYTES * k + 12] for k in range(SLOTS)] == [0] * SLOTS
