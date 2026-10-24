# SPDX-License-Identifier: GPL-2.0-or-later
"""x3000segments fills the beat segment fields of the control report (bytes 47 to
54) from the beat grid x3000rxview left in the grid holder and the position in
the report. Run here on the interpreter against grids built by hand and the
grid Serato sent around one track load."""
import struct

import pytest

import patch_main
import sigpatch
import validate
from players import cdj2000nxs2 as nxs2
from test_cdj3000x_rx import RET, STACK
from test_cdj3000x_wave import Sh4 as WaveSh4, of_type, message
import test_cdj3000x_rx

CODE = 0x08140000
STOCK_BUFFER = 0x0BD50FDC
HIDO_STATE, AUTH, GRID = 0x0BD50E90, 0x0BD4B144, 0x0BF5BFFC
SAVED = STACK - 0x100


class Sh4(WaveSh4):
    """The wave test's interpreter plus the multiply, the bsr and the byte and
    word forms x3000segments uses."""

    macl = 0

    def step_op(self, op, pc):
        n, m = op >> 8 & 15, op >> 4 & 15
        if op & 0xF00F == 0x200E:
            self.macl = (self.r[m] & 0xFFFF) * (self.r[n] & 0xFFFF)
        elif op & 0xF0FF == 0x001A:
            self.r[n] = self.macl & 0xFFFFFFFF
        elif op & 0xF000 == 0xB000:
            disp = (op & 0xFFF) - 0x1000 if op & 0x800 else op & 0xFFF
            self.pr = pc + 4
            self.delay(pc)
            return pc + 4 + disp * 2
        elif op & 0xF0FF == 0x4019:
            self.r[n] >>= 8
        elif op & 0xF00F == 0x600C:
            self.r[n] = self.r[m] & 0xFF
        elif op & 0xF00F == 0x3006:
            self.t = int(self.r[n] > self.r[m])
        elif op & 0xF00F == 0x3002:
            self.t = int(self.r[n] >= self.r[m])
        elif op & 0xFF00 == 0x8400:
            self.r[0] = self.s8(self.r[m] + (op & 15))
        elif op & 0xFF00 == 0x8000:
            self.write(self.r[m] + (op & 15), bytes([self.r[0] & 0xFF]))
        elif op & 0xF00F == 0x6001:
            self.r[n] = self.s16(self.r[m]) & 0xFFFFFFFF
        elif op & 0xF00F == 0x000C:
            self.r[n] = self.s8(self.r[0] + self.r[m])
        else:
            return super().step_op(op, pc)
        return pc + 2

    def s8(self, addr):
        v = self.read(addr, 1)[0]
        return v | 0xFFFFFF00 if v & 0x80 else v


def blob():
    with open(patch_main.blob_path('x3000segments'), 'rb') as f:
        return f.read()


def grid(beats):
    """The grid holder's message: a u16 count, then (beat in bar, ms) of each beat."""
    return struct.pack('<H', len(beats)) + b''.join(
        bytes([b]) + struct.pack('<I', ms)[:3] for b, ms in beats)


def regular(first, spacing, count):
    return [(1 + k % 4, first + k * spacing) for k in range(count)]


def control(ms, kind=0x21):
    report = bytearray(64)
    report[1] = kind
    report[11], report[12] = ms // 60000, ms // 1000 % 60
    report[13:15] = struct.pack('<H', ms % 1000)
    return report


def run(beats, ms, kind=0x21, state=2, auth=1, count=64):
    cpu = Sh4()
    cpu.write(CODE, blob())
    cpu.put32(HIDO_STATE, state)
    cpu.put32(AUTH, auth)
    cpu.write(GRID + 16, grid(beats))
    cpu.write(STOCK_BUFFER, bytes(control(ms, kind)))
    cpu.write(SAVED, struct.pack('<8I', 0, 1, 2, 3, 4, count, 6, 7))
    cpu.r[4], cpu.r[15], cpu.pr, cpu.t = SAVED, STACK, RET, 0
    for k in range(8, 15):
        cpu.r[k] = 0x80000000 + k
    cpu.run(CODE)
    assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [STACK]
    assert cpu.t == 0
    return struct.unpack('<4H', cpu.read(STOCK_BUFFER + 47, 8)), cpu.read(STOCK_BUFFER, 47) == bytes(control(ms, kind))[:47]


def test_the_position_in_the_bar_is_the_beats_gone_by_plus_the_time_since_the_beat():
    beats = regular(1000, 500, 16)
    fields, rest_kept = run(beats, 1000 + 5 * 500 + 120)
    assert fields == (2000, 620, 0, 0)
    assert rest_kept


def test_the_downbeat_is_the_start_of_the_bar():
    assert run(regular(1000, 500, 16), 1000 + 4 * 500)[0] == (2000, 0, 0, 0)


def test_before_the_first_beat_the_fields_stay_empty():
    assert run(regular(1000, 500, 16), 999)[0] == (0, 0, 0, 0)


def test_the_last_beat_takes_the_length_of_the_one_before_and_stops_at_its_end():
    beats = regular(1000, 500, 8)
    assert run(beats, 1000 + 7 * 500 + 100)[0] == (2000, 3 * 500 + 100, 0, 0)
    assert run(beats, 1000 + 7 * 500 + 9000)[0] == (2000, 3 * 500 + 499, 0, 0)


def test_the_length_follows_the_beat_it_is_in():
    beats = [(1, 0), (2, 600), (3, 1100), (4, 1700), (1, 2300)]
    assert run(beats, 1200)[0] == (2400, 2 * 600 + 100, 0, 0)


@pytest.mark.parametrize('beats,kind,state,auth,count', [
    ([], 0x21, 2, 1, 64),
    (regular(0, 500, 4), 0x2F, 2, 1, 64),
    (regular(0, 500, 4), 0x21, 1, 1, 64),
    (regular(0, 500, 4), 0x21, 2, 0, 64),
    (regular(0, 500, 4), 0x21, 2, 1, 54),
    ([(1, 0)], 0x21, 2, 1, 64),
    ([(1, 0), (2, 20000)], 0x21, 2, 1, 64),
])
def test_nothing_is_written_without_a_beat_to_measure_or_a_control_report(beats, kind, state, auth, count):
    assert run(beats, 700, kind, state, auth, count) == ((0, 0, 0, 0), True)


def test_the_grid_of_a_real_track_gives_the_beat_in_the_bar_all_through_it():
    stream = message(of_type(0x2F)[:2])
    count = struct.unpack_from('<H', stream)[0]
    beats = []
    for k in range(count):
        entry = stream[2 + 4 * k:6 + 4 * k]
        beats.append((entry[0] or 1, int.from_bytes(entry[1:4], 'little')))
    assert all(a[1] < b[1] for a, b in zip(beats, beats[1:]))
    for k in range(0, count - 1, 7):
        beat, start = beats[k]
        spacing = beats[k + 1][1] - start
        for ms in (start, start + spacing // 2):
            length, position, _, _ = run(beats, ms)[0]
            assert length == 4 * spacing
            assert position // spacing + 1 == beat
            assert position % spacing == ms - start


def test_the_hook_runs_after_the_report_translation_it_reads():
    assert sorted(patch_main.MODS).index('x3000segments') > sorted(patch_main.MODS).index('x3000rxview')


NAMES = ['x3000out', 'x3000rxbuf', 'x3000rxview', 'x3000segments']


@pytest.fixture(scope='module')
def real():
    with open(test_cdj3000x_rx.MAIN, 'rb') as f:
        return f.read()


@test_cdj3000x_rx.needs_main
def test_every_firmware_address_the_routine_holds_is_found(real):
    img = sigpatch.Image(nxs2.MAIN_PROFILE, real)
    for stock, source in patch_main.MODS['x3000segments'].imports:
        assert sigpatch.import_value(img, source) == stock


@test_cdj3000x_rx.needs_main
def test_the_hook_shares_the_read_site_and_the_image_passes_the_validator(real):
    spans = {}
    out = patch_main.patch(real, NAMES, spans=spans)
    rep = validate.Report()
    validate.check_main_image(real, out, nxs2.MAIN_PROFILE, spans, rep)
    assert not rep.failed, [text for state, text in rep.lines if state == 'FAIL']
    report = {}
    assert patch_main.patch(out, NAMES, report=report) == out
    assert {name: report[name][0] for name in NAMES} == dict.fromkeys(NAMES, 'present')


def test_the_cdj3000x_hooks_declare_the_order_they_need():
    for first, second in (('x3000rxview', 'x3000segments'), ('x3000hs', 'x3000key')):
        assert first in patch_main.MODS[second].after
        site = sigpatch.hook_sites(patch_main.MODS)[sigpatch.site_key(patch_main.MODS[second])]
        assert site.index(first) < site.index(second)
