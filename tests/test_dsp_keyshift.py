# SPDX-License-Identifier: GPL-2.0-or-later
"""The DSP half of key_shift (mods/key_shift/cdj2000nxs2/dsp_keyshiftdsp.s),
run on the emulator's own C66x core (tests/c6x_run.c, built here when a C
compiler and glib are at hand) against a model of the words each hook reads.
Where the real 1.87 MAIN is extracted, the edits and the cave are checked in
its program."""
import math
import os
import shutil
import struct
import subprocess

import pytest

import dsppatch
import patch_main
from players import cdj2000nxs2 as nxs2

EMU = os.path.join(os.path.dirname(__file__), '..')
KEYSHIFT = patch_main.DSP_SETS['cdj2000nxs2'].mods['keyshiftdsp']
REAL_MAIN = os.path.join(EMU, '..', 'extract', 'main_unpacked.bin')
needs_main = pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
CC = shutil.which('gcc') or shutil.which('cc')

CODE = nxs2.DSP_PROFILE.cave[0]
KS_MT, KS_HOP_A, KS_HOP_B, KS_PULL, KS_APPEND, KS_MT_PLAY = (CODE + 8 * k for k in range(6))
STATE = CODE + 64                   # past the entries, in a fetch packet of its own
SHIFT = 0x008A73CA
DP = 0x008BC670
HOP, SPEED = DP + 1076, DP + 892
RING_L, RING_R = 0x008A8A70, 0x008AAA70
STAGE_L, STAGE_R = 0x008ACA70, 0x008B2A70
SCRATCH_L, SCRATCH_R = 0x80080000, 0x80082100
BACK = 0x80000100
EDMA_COPY, PULL_BACK = 0x8007D0B0, 0x8003E4E8
Q20 = 1 << 20


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def fbits(x):
    return struct.unpack('<I', struct.pack('<f', x))[0]


def bits_f(w):
    return struct.unpack('<f', struct.pack('<I', w))[0]


@pytest.fixture(scope='module')
def runner(tmp_path_factory):
    if not CC or not shutil.which('pkg-config'):
        pytest.skip('no C compiler or pkg-config')
    glib = subprocess.run(['pkg-config', '--cflags', '--libs', 'gmodule-2.0'], capture_output=True, text=True)
    if glib.returncode:
        pytest.skip('no glib')
    src = os.path.join(EMU, 'hw', 'cdj', 'c6x')
    core = [os.path.join(src, f) for f in sorted(os.listdir(src)) if f.startswith('c66x_') and f.endswith('.c')]
    out = str(tmp_path_factory.mktemp('c6x') / 'c6x_run')
    subprocess.run([CC, '-O1', '-std=gnu11', '-ffp-contract=off', '-frounding-math', '-I', src,
                    os.path.join(EMU, 'tests', 'c6x_run.c')] + core + ['-o', out, '-lm', '-pthread']
                   + glib.stdout.split(), check=True)
    return out


class Dsp:
    """Memory to load, registers to set, and one run of the core from an entry."""

    def __init__(self, runner, tmp_path):
        self.runner, self.tmp = runner, tmp_path
        self.mem = {}
        with open(patch_main.dsp_blob_path('keyshiftdsp'), 'rb') as f:
            self.put(CODE, f.read())

    def put(self, addr, data):
        self.mem[addr] = bytes(data)

    def words(self, addr, values):
        self.put(addr, struct.pack('<%dI' % len(values), *values))

    def run(self, entry, stop=BACK, regs=None, dumps=(), calls=()):
        args = [self.runner, hex(entry), hex(stop)]
        for k, (addr, data) in enumerate(sorted(self.mem.items())):
            path = self.tmp / ('m%d.bin' % k)
            path.write_bytes(data)
            args.append('%s@%#x' % (path, addr))
        regs = dict(regs or {}, b3=BACK)
        args += ['%s=%#x' % (r, v & 0xFFFFFFFF) for r, v in regs.items()]
        args += ['call=%#x' % a for a in calls]
        args += ['dump=%#x:%d' % d for d in dumps]
        out = subprocess.run(args, capture_output=True, text=True)
        assert out.returncode == 0, out.stdout + out.stderr
        lines = out.stdout.splitlines()
        reg = {l.split()[0]: int(l.split()[1], 16) for l in lines if l[:1] in 'ab' and len(l.split()) == 2}
        mem = {int(l.split()[0], 16): [int(w, 16) for w in l.split()[1:]] for l in lines
               if not l.startswith(('stop', 'call')) and len(l.split()) > 2}
        called = [[int(w, 16) for w in l.split()[1:]] for l in lines if l.startswith('call')]
        return reg, mem, called


def state(active=0, p=0, carry=0, on=0):
    return [active, p, carry, on]


@pytest.fixture
def dsp(runner, tmp_path):
    d = Dsp(runner, tmp_path)
    d.words(STATE, state())
    d.put(SHIFT & ~3, struct.pack('<HH', 0, 0))
    return d


def shift(dsp, n):
    dsp.put(SHIFT & ~3, struct.pack('<Hh', 0, n))


# -- the edits ---------------------------------------------------------------------

def test_every_edit_changes_its_word_and_aims_at_its_entry():
    targets = {0x80005EB0: KS_MT, 0x8000D424: KS_MT_PLAY, 0x80046CF0: KS_HOP_A, 0x8004F3B8: KS_HOP_B, 0x8003E4E0: KS_PULL,
               0x80046C2C: KS_APPEND, 0x8004F2EC: KS_APPEND}
    for pc, old, new in KEYSHIFT.edits:
        assert old != new
        if pc in targets:
            disp = dsppatch.sign_extend(new >> 7 & 0x1FFFFF, 21)
            assert (pc & ~31) + 4 * disp == targets[pc]
            assert new & 1 == old & 1                       # the execute packet keeps its shape


def test_the_code_fits_the_cave():
    with open(patch_main.dsp_blob_path('keyshiftdsp'), 'rb') as f:
        size = len(f.read())
    assert KEYSHIFT.code == ((CODE, 'keyshiftdsp'),)
    assert CODE + size <= nxs2.DSP_PROFILE.cave[1]


@needs_main
def test_the_real_program_holds_each_stock_word_and_takes_the_mod_once():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    start = dsppatch.find_blob(data, nxs2.DSP_PROFILE)
    blob = data[start:start + nxs2.DSP_PROFILE.length]
    for pc, old, _ in KEYSHIFT.edits:
        at = dsppatch.program_offset(nxs2.DSP_PROFILE, blob, pc)
        assert struct.unpack_from('<I', blob, at)[0] == old, hex(pc)
    assert dsppatch.cave_is_stock(nxs2.DSP_PROFILE, blob)
    report, spans = {}, {}
    out = patch_main.patch(data, ['keyshiftdsp'], report=report, spans=spans)
    assert report == {'keyshiftdsp': ('applied', '')} and len(out) == len(data)
    assert sorted(k for _, _, k in spans['keyshiftdsp']) == ['dsp'] * 8 + ['dspcave']
    again = {}
    assert patch_main.patch(out, ['keyshiftdsp'], report=again) == out
    assert again == {'keyshiftdsp': ('present', '')}


# -- ks_mt -------------------------------------------------------------------------

@pytest.mark.parametrize('mt, n, want', [(0, 0, 0), (1, 0, 1), (0, 3, 1), (1, -12, 1), (0, -1, 1)])
def test_the_setter_sees_master_tempo_on_whenever_there_is_a_shift(dsp, mt, n, want):
    shift(dsp, n)
    dsp.words(0x008A7384, [mt])
    reg, _, _ = dsp.run(KS_MT, regs={'a12': 0x008A7384})
    assert reg['b4'] == want


def test_the_play_setter_edit_replaces_its_mt_test():
    assert (0x8000D424, 0x02340A5A, 0x10AFE512) in KEYSHIFT.edits


# Every register the setter keeps across 0x8000D424, set to a value of its own.
KEPT = {r: 0x10000 + i for i, r in enumerate(['a%d' % k for k in range(32) if k not in (0, 3)]
                                              + ['b%d' % k for k in range(32) if k not in (3, 4, 14)])}


@pytest.mark.parametrize('mt', [0, 1])
@pytest.mark.parametrize('n', [0, 1, -3])
@pytest.mark.parametrize('resample_window', [0, 1])
def test_the_play_setter_takes_the_vocoder_with_a_shift(dsp, mt, n, resample_window):
    shift(dsp, n)
    dsp.put((DP + 502) & ~3, struct.pack('<HH', 0, resample_window))
    regs = dict(KEPT, b13=mt, b14=DP, a0=0xDEAD, a3=0xBEEF, b4=0xF00D)
    reg, _, _ = dsp.run(KS_MT_PLAY, regs=regs)
    assert reg['b4'] == int(mt == 0 and (n == 0 or resample_window == 1))
    kept = {r: v for r, v in regs.items() if r not in ('a0', 'a3', 'b4')}
    assert {r: reg[r] for r in kept} == kept


# -- ks_hop ------------------------------------------------------------------------

def hop(dsp, n, r, engine='a', active_before=0, speed=None):
    shift(dsp, n)
    dsp.words(STATE, state(active=active_before, p=0x12345, carry=0x777, on=0))
    dsp.words(SPEED & ~3, [speed if speed is not None else int(r * Q20)])
    regs = {'b4': fbits(r), 'b5': 0xAAAA} if engine == 'a' else {'b5': fbits(r), 'b4': 0xBBBB}
    regs['b14'] = DP
    reg, mem, _ = dsp.run(KS_HOP_A if engine == 'a' else KS_HOP_B, regs=regs, dumps=[(STATE, 4)])
    return reg, mem[STATE]


@pytest.mark.parametrize('engine', ['a', 'b'])
def test_no_shift_leaves_r_and_turns_the_hooks_off(dsp, engine):
    reg, st = hop(dsp, 0, 1.06, engine, active_before=1)
    assert reg['b4' if engine == 'a' else 'b5'] == fbits(1.06)
    assert st[0] == 0


@pytest.mark.parametrize('engine', ['a', 'b'])
@pytest.mark.parametrize('n', [1, 2, -3, 7, 12, -6])
def test_a_shift_divides_r_by_p_and_keeps_p(dsp, engine, n):
    r = 0.97
    reg, st = hop(dsp, n, r, engine)
    p = f32(2 ** (n / 12))
    other = 'b5' if engine == 'a' else 'b4'
    assert reg[other] == (0xAAAA if engine == 'a' else 0xBBBB)
    assert bits_f(reg['b4' if engine == 'a' else 'b5']) == f32(f32(r) * f32(2 ** (-n / 12)))
    assert st[1] == round(p * Q20)
    assert st[0] == 1 and st[2] == 0 and st[3] == 1          # just switched on


def test_an_active_shift_keeps_its_carry(dsp):
    _, st = hop(dsp, 2, 1.0, active_before=1)
    assert st[0] == 1 and st[2] == 0x777 and st[3] == 0


def test_past_twice_r_the_shift_gives_way_to_the_tempo(dsp):
    r = 1.2
    reg, st = hop(dsp, -12, r)
    assert bits_f(reg['b4']) == 2.0
    assert st[1] == int(r * Q20) >> 1


# -- ks_pull -----------------------------------------------------------------------

@pytest.mark.parametrize('active, p, carry, h', [(0, 0x150000, 0, 512), (1, Q20, 0, 512),
                                                 (1, int(2 ** (2 / 12) * Q20), 0x80000, 456),
                                                 (1, int(2 ** (-5 / 12) * Q20), 0xFFFFF, 1000)])
def test_the_puller_is_told_the_source_frames_the_hop_needs(dsp, active, p, carry, h):
    dsp.words(STATE, state(active, p, carry))
    dsp.words(HOP, [h])
    reg, _, _ = dsp.run(KS_PULL, stop=PULL_BACK, regs={'b14': DP, 'a0': 3, 'b8': 0x0089A138})
    assert reg['b4'] == ((carry + h * p) >> 20 if active else h)
    assert (reg['a0'], reg['b8']) == (3, 0x0089A138)


# -- ks_append ---------------------------------------------------------------------

APPEND_ARGS = {'a4': 5, 'b4': 0, 'a6': 4, 'a8': RING_L, 'a10': RING_R}


def test_with_no_shift_the_append_is_the_stock_copy_with_its_arguments(dsp):
    h = 512
    regs = dict(APPEND_ARGS, b6=h, b8=STAGE_L + 0x3000 - 4 * h, b10=STAGE_R + 0x3000 - 4 * h,
                a11=0x1111, b11=0x2222, a12=0x3333, b12=0x4444)
    reg, _, called = dsp.run(KS_APPEND, regs=regs, calls=[EDMA_COPY])
    assert called == [[EDMA_COPY, 5, 0, 4, h, RING_L, STAGE_L + 0x3000 - 4 * h, RING_R, STAGE_R + 0x3000 - 4 * h]]


def hermite(x, k, f):
    xm1, x0, x1, x2 = x[k - 1], x[k], x[k + 1], x[k + 2]
    wm1 = f * f - f ** 3 / 2 - f / 2
    w0 = 1 + f * f * (1.5 * f - 2.5)
    w2 = f ** 3 / 2 - f * f / 2
    w1 = 1 - wm1 - w0 - w2
    return wm1 * xm1 + w0 * x0 + w1 * x1 + w2 * x2


class Stream:
    """A sine on each channel and the hook state carried across frames, as the
    vocoder's frames hand them on: the frames pulled for a hop, then the append."""

    def __init__(self, dsp, n, r=1.0):
        self.dsp = dsp
        self.p_q = round(2 ** (n / 12) * Q20)
        self.r = r
        self.pos = 0                    # source frames handed out
        self.st = state(1, self.p_q, 0, 1)
        self.out_l, self.out_r = [], []

    def src(self, j):
        return f32(math.sin(0.013 * j)), f32(0.5 * math.cos(0.021 * j))

    def frame(self, h):
        need = (self.st[2] + h * self.p_q) >> 20
        frames = [self.src(self.pos + j) for j in range(need)]
        self.pos += need
        d = self.dsp
        d.words(STATE, self.st)
        d.put(RING_L, struct.pack('<%df' % need, *[l for l, _ in frames]) if need else b'')
        d.put(RING_R, struct.pack('<%df' % need, *[r for _, r in frames]) if need else b'')
        regs = dict(APPEND_ARGS, b6=h, b8=STAGE_L, b10=STAGE_R)
        _, mem, called = d.run(KS_APPEND, regs=regs, calls=[EDMA_COPY],
                               dumps=[(STATE, 4), (STAGE_L, h), (STAGE_R, h), (SCRATCH_L, 5), (SCRATCH_R, 5)])
        assert called == []
        self.st = mem[STATE]
        d.words(SCRATCH_L, mem[SCRATCH_L])
        d.words(SCRATCH_R, mem[SCRATCH_R])
        self.out_l += [bits_f(w) for w in mem[STAGE_L]]
        self.out_r += [bits_f(w) for w in mem[STAGE_R]]


@pytest.mark.parametrize('n', [2, -3, 12, -12, 0])
def test_the_append_resamples_at_step_p_across_frames(dsp, n):
    stream = Stream(dsp, n)
    for h in (300, 301, 299, 300):
        stream.frame(h)
    p = stream.p_q / Q20
    # the reference: the same Hermite on the same source, three frames behind
    src_l = [0.0] * 5 + [stream.src(j)[0] for j in range(stream.pos + 4)]
    src_r = [0.0] * 5 + [stream.src(j)[1] for j in range(stream.pos + 4)]
    for i in range(len(stream.out_l)):
        u = 2 + i * p
        k = int(u)
        assert stream.out_l[i] == pytest.approx(hermite(src_l, k, u - k), abs=2e-5), i
        assert stream.out_r[i] == pytest.approx(hermite(src_r, k, u - k), abs=2e-5), i
    assert stream.st[3] == 0


def test_the_carry_is_what_the_frames_did_not_take(dsp):
    stream = Stream(dsp, 5)
    stream.frame(400)
    assert stream.st[2] == (400 * stream.p_q) & (Q20 - 1)
