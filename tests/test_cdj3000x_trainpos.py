# SPDX-License-Identifier: GPL-2.0-or-later
"""x3000trainpos, executed: gui_x3000trainpos.s runs on the SH-2A interpreter
with the display's system time call stubbed and the tick enable in memory, and
is compared with a model written from the rules in its header, on a played
stretch with the detail waveform trains of an incremental Serato fill-in.
Against the real display image the hook is placed in the centre-waveform
renderer, the image passes the update validator, an image that already carries
it is left as it is, and the renderer sets up the same column range as stock
from what the hook hands it."""
import os
import struct

import pytest

import patch_gui
import sigpatch
import validate
from players import cdj2000nxs2 as nxs2
from helpers import path
from sh2a import MASK, Memory, Sh2a, signed

REAL_GUI = path('..', 'extract', 'gui_unpacked.bin')
needs_real_gui = pytest.mark.skipif(not os.path.exists(REAL_GUI),
                                    reason='extract/gui_unpacked.bin not present')

CODE = 0x1C300000
STACK = 0x1C3F0000
SAVED = STACK - 0x100
SYSTEM_TIME = 0x0E52E9FC
TICK_ENABLE = 0x0E5BC274
STATE_WORDS = 14
BLOB = open(patch_gui.blob_path('x3000trainpos'), 'rb').read()
STATE = len(BLOB) - 4 * STATE_WORDS


class Display(Sh2a):
    """The interpreter with the time call answered from `now`, and the stack
    pushes and pops, the divide and the compare the routine uses."""

    now = 0

    def step(self, pc):
        if pc == SYSTEM_TIME:
            self.store(self.r[4] + 4, self.now, 4)
            return self.pr
        op = self.word(pc)
        n, m = op >> 8 & 15, op >> 4 & 15
        r = self.r
        if op & 0xF0FF == 0x4022:                       # sts.l pr,@-Rn
            r[n] = (r[n] - 4) & MASK
            self.store(r[n], self.pr, 4)
        elif op & 0xF0FF == 0x4026:                     # lds.l @Rn+,pr
            self.pr = self.load(r[n], 4)
            r[n] = (r[n] + 4) & MASK
        elif op & 0xF00F == 0x2006:                     # mov.l Rm,@-Rn
            r[n] = (r[n] - 4) & MASK
            self.store(r[n], r[m], 4)
        elif op & 0xF00F == 0x6006:                     # mov.l @Rm+,Rn
            r[n] = self.load(r[m], 4)
            r[m] = (r[m] + 4) & MASK
        elif op & 0xF0FF == 0x4084:                     # divu R0,Rn
            r[n] = r[n] // r[0]
        elif op & 0xF0FF == 0x4011:                     # cmp/pz
            self.t = int(r[n] & 0x80000000 == 0)
        elif op & 0xF0FF == 0x402B:                     # jmp @Rn
            return self.branch(pc, r[n])
        elif op & 0xF00F == 0x3001 and self.word(pc + 2) >> 12 == 6:   # mov.l @(disp12,Rm),Rn
            r[n] = self.load(r[m] + (self.word(pc + 2) & 0xFFF) * 4, 4)
            return pc + 4
        else:
            return super().step(pc)
        return pc + 2


class Hook:
    """The routine with its state kept between calls, as on the display."""

    def __init__(self):
        self.mem = Memory()
        self.mem.add(CODE, BLOB)
        self.mem.add(STACK - 0x1000, bytes(0x1000 + 0x100))
        self.mem.add(SYSTEM_TIME, bytes(4))
        self.mem.add(TICK_ENABLE, bytes(4))

    def call(self, position, now, enable):
        self.mem.put32(TICK_ENABLE, enable)
        cpu = Display(self.mem)
        cpu.now = now
        cpu.r[15] = STACK
        cpu.r[4] = SAVED
        self.mem.write(SAVED, struct.pack('>8I', *range(8)))
        self.mem.put32(SAVED + 16, position)
        for k in range(8, 15):
            cpu.r[k] = 0x80000000 + k
        cpu.run(CODE, Sh2a.RET)
        assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [STACK]
        saved = struct.unpack('>8I', self.mem.read(SAVED, 32))
        assert [saved[k] for k in (0, 1, 2, 3, 5, 6, 7)] == [0, 1, 2, 3, 5, 6, 7]
        assert self.mem.u32(CODE + STATE) == saved[4]
        return saved[4]


class Model:
    """The rules of the routine's header."""

    def __init__(self):
        self.out = self.seen = self.seen_t = self.rate = 0
        self.base_w = self.base_t = self.next_w = self.next_t = 0
        self.enable = self.phase = self.anchor_w = self.anchor_t = self.edge_t = self.last_t = 0

    def call(self, w, now, enable):
        prev, self.enable = self.enable, enable
        moved = signed(w - self.seen)
        jump, at = False, now
        if moved:
            at = self.last_t + ((now - self.last_t) & MASK) // 2
            self.seen, self.seen_t = w, at
            jump = not -12 <= moved <= 12
        if self.phase == 0:
            self.learn(w, now, at, moved, jump, not enable and prev)
            if enable or not prev or not self.rate:
                return self.store(w, now)
            self.anchor_w, self.anchor_t, self.phase = self.seen, self.seen_t, 1
            return self.extrapolate(w, now)
        if self.phase == 1 and enable:
            self.phase, self.edge_t = 2, now
        elif self.phase == 2 and not enable and prev:
            self.anchor_w, self.anchor_t, self.phase = self.out, self.last_t, 1
        if now - self.anchor_t > 300 or self.phase == 2 and now - self.edge_t > 100:
            return self.resolve(w, now)
        if jump and not -12 <= signed(w - self.out) <= 12:
            return self.take(w, now)
        return self.extrapolate(w, now)

    def learn(self, w, now, at, moved, jump, starts):
        if not moved:
            if not starts and now - self.seen_t > 40:
                self.rate = 0
                self.rebase(w, now)
        elif jump:
            self.rebase(w, now)
        elif moved < 0:
            self.rate = 0
            self.rebase(w, now)
        else:
            span = at - self.base_t
            if span < 100:
                return
            self.rate = ((w - self.base_w) << 8 & MASK) // span
            if not self.next_t:
                if span >= 500:
                    self.next_w, self.next_t = w, at
            elif span >= 1000:
                self.base_w, self.base_t, self.next_t = self.next_w, self.next_t, 0

    def rebase(self, w, now):
        self.base_w, self.base_t, self.next_t = w, now, 0

    def extrapolate(self, w, now):
        pred = self.anchor_w + ((self.rate * (now - self.anchor_t)) & MASK) // 256
        last = self.out
        if w <= pred:
            return self.store(max(pred, last), now)
        if last >= w:
            return self.store(last, now)
        step = (self.rate * (now - self.last_t) * 5 & MASK) // 1024 or 1
        out = max(last + step, pred)
        if out >= w:
            return self.take(w, now)
        return self.store(out, now)

    def resolve(self, w, now):
        if w >= self.out or self.out - w > 12:
            return self.take(w, now)
        self.phase = 0
        return self.store(self.out, now)

    def take(self, w, now):
        self.phase = 0
        return self.store(w, now)

    def store(self, out, now):
        self.out, self.last_t = out, now
        return out


RATE = 0.15             # counts per ms at 1x
TRAIN_EVERY = 1250      # ms between trains
TRAIN_MS = 110          # how long the word stands still in one
HEARTBEAT_MS = 19       # MAIN's position updates between trains


def session(ms, rate=RATE, renders=(17, 23, 15, 31, 19, 25), events=()):
    """(word, time, tick enable, true position) at each render of a played
    stretch: the word is the position MAIN sent last, every HEARTBEAT_MS
    except while a train is on the link; the enable is 0 from a train's first
    chunk to its ticks, and the first word after a train comes one heartbeat
    later. `events` are (time, new position) jumps of the true position (a
    cue) or (time, None) for a pause."""
    out, position, step, word = [], 20000.0, rate, 20000
    events = dict(events)
    render = 1000
    for now in range(1000, 1000 + ms):
        if now in events:
            position = position if events[now] is None else float(events[now])
            step = 0 if events[now] is None else step
        phase = (now - 1000) % TRAIN_EVERY
        stalled = 400 <= phase < 400 + TRAIN_MS + HEARTBEAT_MS
        if not stalled and (now - 1000) % HEARTBEAT_MS == 0:
            word = int(position)
        if now == render:
            out.append((word, now, 0 if 400 <= phase < 400 + TRAIN_MS else 1, position))
            render += renders[len(out) % len(renders)]
        position += step
    return out


def run(stretch, hook=None):
    hook = hook or Hook()
    return [hook.call(w, t, e) for w, t, e, _ in stretch]


def windows(stretch):
    """(first render of a train, first render with a word sent after it)."""
    spans, start = [], None
    for i, (w, t, e, _) in enumerate(stretch):
        if not e and start is None:
            start = i
        elif e and start is not None and w != stretch[start][0]:
            spans.append((start, i))
            start = None
    return spans


def test_it_matches_the_rules_of_its_header():
    stretches = [session(6000), session(6000, events=((3460, None),)), session(6000, events=((2200, 900),)),
                 session(4000, rate=RATE * 1.16), session(4000, rate=-RATE)]
    for stretch in stretches:
        hook, model = Hook(), Model()
        for w, t, e, _ in stretch:
            assert hook.call(w, t, e) == model.call(w, t, e), (w, t, e)


def test_the_waveform_keeps_moving_through_every_train():
    """No render in a train leaves the waveform where it was, none moves it
    more than 1.5 times as far as steady play does in the same time (plus the
    count the rounding can add), and the output is at most 2 counts from the
    first word sent after the train."""
    stretch = session(8000)
    outs = run(stretch)
    trains = windows(stretch)
    assert len(trains) == 6
    speeds = sorted((outs[i] - outs[i - 1]) / (stretch[i][1] - stretch[i - 1][1]) for i in range(20, len(outs)))
    steady = speeds[len(speeds) // 2]
    for start, end in trains:
        for i in range(start, end + 3):
            step, ms = outs[i] - outs[i - 1], stretch[i][1] - stretch[i - 1][1]
            assert 0 < step <= 1.5 * steady * ms + 1, (i, step, ms)
        assert abs(outs[end] - stretch[end][0]) <= 2
    raw = [stretch[i][0] - stretch[i - 1][0] for start, end in trains for i in range(start, end)]
    assert raw.count(0) > 3 * len(trains)


def test_it_stays_near_the_true_position_in_a_train():
    stretch = session(8000)
    outs = run(stretch)
    for start, end in windows(stretch):
        for i in range(start, end):
            assert abs(outs[i] - stretch[i][3]) < 6


def test_outside_a_train_the_word_goes_through():
    stretch = session(8000)
    outs = run(stretch)
    inside = {i for start, end in windows(stretch) for i in range(start, end + 6)}
    assert all(outs[i] == stretch[i][0] for i in range(len(stretch)) if i not in inside and stretch[i][2])


def test_a_track_with_no_beat_grid_is_left_alone():
    stretch = [(w, t, 0, x) for w, t, _, x in session(5000)]
    assert run(stretch) == [w for w, _, _, _ in stretch]


def test_playing_backward_is_left_alone():
    stretch = session(5000, rate=-RATE)
    assert run(stretch) == [w for w, _, _, _ in stretch]


def test_a_pause_inside_a_train_leaves_no_offset():
    stretch = session(4000, events=((1440, None),))
    outs = run(stretch)
    (start, end), = [s for s in windows(stretch) if stretch[s[0]][1] < 2000]
    after = [i for i, (_, t, _, _) in enumerate(stretch) if t > stretch[end][1] + 150]
    assert all(outs[i] == stretch[i][0] for i in after)
    assert max(outs[start:end + 10]) - stretch[end][0] <= 0.15 * 300 + 1


def test_a_cue_inside_a_train_is_taken_at_once():
    stretch = session(4000, events=((1450, 900),))
    outs = run(stretch)
    first = next(i for i, (w, _, _, _) in enumerate(stretch) if w < 2000)
    assert outs[first] == stretch[first][0]
    assert all(outs[i] == stretch[i][0] for i in range(first, first + 20))


def test_a_cue_before_a_train_keeps_the_speed():
    stretch = session(3000, events=((1300, 900),))
    outs = run(stretch)
    start, end = windows(stretch)[0]
    assert stretch[start][0] < 2000
    assert min(outs[i] - outs[i - 1] for i in range(start + 1, end)) > 0


def test_the_first_call_takes_the_word_as_it_is():
    assert Hook().call(123456, 1, 1) == 123456


@needs_real_gui
def test_the_hook_sits_in_the_renderer_and_the_image_passes_the_validator():
    with open(REAL_GUI, 'rb') as f:
        stock = f.read()
    spans = {}
    out = patch_gui.patch(stock, ['x3000trainpos', 'x3000keepwave'], spans=spans)
    rep = validate.Report()
    validate.check_gui_image(stock, out, nxs2.GUI_PROFILE, spans, rep)
    assert not rep.failed, [text for state, text in rep.lines if state == 'FAIL']
    report = {}
    assert patch_gui.patch(out, ['x3000trainpos'], report=report) == out
    assert report['x3000trainpos'][0] == 'present'


RENDERER = (0x1C009AB4, 0x1C00A309)


@needs_real_gui
def test_the_site_is_in_the_centre_waveform_renderer_that_wave3_is_already_in():
    """Why the display validator may take the site: it is in a function the
    list already holds a site of. The renderer begins by asking the draw
    mailbox for a request and does nothing unless it gets one, and this hook
    sits inside the `if` that holds the request, after that call, so it runs
    only when a waveform is being drawn, like wave3's. Nothing else calls the
    hook's routine."""
    with open(REAL_GUI, 'rb') as f:
        image = sigpatch.Image(nxs2.GUI_PROFILE, f.read())
    for name in ('wave3', 'x3000trainpos'):
        mod = patch_gui.MODS[name]
        site = sigpatch.find_sig(image, mod)
        first, last = (image.addr_of(site + 2 * k) for k in (mod.start, mod.end - 1))
        assert RENDERER[0] <= first and last < RENDERER[1], name


SET_UP = (0x1C009AE4, 0x1C009B52)     # from the hooked span to the column loop


def renderer_set_up(image, position, count, zoom, enable):
    """The locals the renderer's column loop reads, after it has run from the
    hook's site with this position word, render count and zoom."""
    img = sigpatch.Image(nxs2.GUI_PROFILE, image)
    mem = Memory()
    for _, dest, src, size in img.copies():
        if nxs2.GUI_PROFILE.ram_base <= dest < nxs2.GUI_PROFILE.ram_end:
            mem.add(dest, image[src:src + size])
    mem.add(SYSTEM_TIME, bytes(4))
    mem.add(TICK_ENABLE, struct.pack('>I', enable))
    mem.add(0x0E5BC26C, struct.pack('>I', count))
    regs = bytearray(0x100)
    struct.pack_into('>II', regs, 0x70, position, 60000)
    struct.pack_into('>I', regs, 0xE0, zoom)
    mem.add(0xFFF88000, bytes(regs))
    mem.add(STACK - 0x1000, bytes(0x1100))
    cpu = Display(mem)
    cpu.now = 5000
    cpu.r[15] = SAVED
    cpu.run(*SET_UP)
    return struct.unpack('>11I', mem.read(cpu.r[15] + 4, 44)), cpu.r[8:]


@needs_real_gui
@pytest.mark.parametrize('zoom', [0, 2, 4])
@pytest.mark.parametrize('position, count', [(5000, 60000), (5000, 5003), (100, 60000)])
def test_the_renderer_sets_up_the_same_columns_as_stock(zoom, position, count):
    with open(REAL_GUI, 'rb') as f:
        stock = f.read()
    hooked = patch_gui.patch(stock, ['x3000trainpos'])
    assert (renderer_set_up(hooked, position, count, zoom, 1)
            == renderer_set_up(stock, position, count, zoom, 1))
