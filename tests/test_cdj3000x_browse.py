# SPDX-License-Identifier: GPL-2.0-or-later
"""The local cursor on the browse list Serato shows a CDJ-3000X. x3000browse
counts the encoder steps in the HID IN task and shows Serato's last answer
with the steps not answered yet applied; x3000rxview keeps each answer, counts
it against the oldest step and shows it with the rest applied. Both run here
on the interpreter, fed the reports of a recorded Serato session scrolling a
crate up to its top in their order, with the framework a stub that records
every list it is handed. Every list must be the page a model of Serato's
paging predicts from the last answer and the steps outstanding."""
import os
import struct

import pytest

import patch_main
import sigpatch
from players import cdj2000nxs2 as nxs2
from test_cdj3000x_rx import nxs2_list, texts
from test_cdj3000x_wave import RET, STACK, Sh4 as WaveSh4, blob

RX_CODE, BROWSE_CODE = 0x08140000, 0x08148000
RX_BUFFER = 0x081FFBE0
HIDO_STATE, AUTH = 0x0BD50E90, 0x0BD4B144
FRAMEWORK, TICK = 0x084F9DB2, 0x0B0C5258
SLOTS, SLOT = 0x0BD4B124, 0x0BD5109C
WORDS = 0x081FB520
GUARD, ANSWERED, STEPS, LIST_OWED = 0x081FB544, 0x081FB548, 0x081FB54C, 0x081FB56C
SAVED = STACK - 0x100
PREEMPT_STACK = STACK - 0x4000
BASE = 0x08000000
DATA = os.path.join(os.path.dirname(__file__), 'data')
MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')
needs_main = pytest.mark.skipif(not os.path.exists(MAIN), reason='needs extract/main_unpacked.bin')


class Sh4(WaveSh4):
    """The wave test's interpreter plus tas.b, exts.w, neg, and jsr into a routine that is loaded rather than
    stubbed."""

    def step_op(self, op, pc):
        n, m = op >> 8 & 15, op >> 4 & 15
        if op & 0xF0FF == 0x401B:
            byte = self.read(self.r[n], 1)[0]
            self.t = int(byte == 0)
            self.write(self.r[n], bytes([byte | 0x80]))
        elif op & 0xF00F == 0x600F:
            self.r[n] = (self.r[m] & 0xFFFF) - (self.r[m] << 1 & 0x10000) & 0xFFFFFFFF
        elif op & 0xF00F == 0x600B:
            self.r[n] = -self.r[m] & 0xFFFFFFFF
        elif op & 0xF0FF == 0x400B and self.r[n] not in self.stubs:
            target = self.r[n]
            self.delay(pc)
            self.pr = pc + 4
            return target
        else:
            return super().step_op(op, pc)
        return pc + 2


def load(name):
    """(time, 'in' or 'out', report) for each line: a state report the deck sent, or a report Serato sent."""
    out = []
    with open(os.path.join(DATA, name)) as f:
        for line in f:
            if line.startswith('#'):
                continue
            t, way, data = line.split()
            out.append((float(t), way, bytes.fromhex(data).ljust(64 if way == 'in' else 1024, b'\0')))
    return out


SCROLL = load('cdj3000x_browse_scroll.txt')
# The deck sends state reports from the moment it is connected; the recording
# starts with Serato's lists, so the first state report goes in front again.
SCROLL.insert(0, (63.690, 'in', next(data for _, way, data in SCROLL if way == 'in')))


def records(report):
    """(8-byte header, text) of each record of a 0x2a list, as Serato sends them."""
    payload, out, i = report[6:], [], 0
    while i + 8 <= len(payload):
        if i and not any(payload[i:i + 10]):
            break
        j = i + 8
        while payload[j:j + 2] != b'\0\0':
            j += 2
        out.append((payload[i:i + 8], payload[i + 8:j].decode('utf-16-le')))
        i = j + 2
    return out


def library(rows):
    """The cursor's row of a library list (a header of style 0, eight rows or more, a cursor), else None."""
    cursor = next((h[0] for h, _ in rows[1:] if h[1] & 0x80), None)
    return cursor if len(rows) >= 9 and not rows[0][0][1] & 0x7F else None


def predict(prev, cur, step, offset):
    """Serato's next page from its last one and one step: prev {row: record or None}, cur the cursor's row,
    offset the rows above row 1 (None if not known)."""
    last = max(prev)
    if step > 0:
        if cur < 5 or cur >= last:
            return prev, min(cur + 1, last), offset
        return {r - 1: t for r, t in prev.items() if r > 1}, cur, None if offset is None else offset + 1
    if cur > 5:
        return prev, cur - 1, offset
    if offset == 0 or cur < 5:
        return prev, max(1, cur - 1), offset
    shifted = {r + 1: t for r, t in prev.items() if r + 1 <= last}
    shifted[1] = None
    return shifted, cur, None if offset is None else offset - 1


def predicted_list(rows, steps, offset):
    """The list the framework gets for the answer `rows` with `steps` still to come."""
    page, cur = {h[0]: (h, t) for h, t in rows[1:]}, library(rows)
    for step in steps:
        page, cur, offset = predict(page, cur, step, offset)
    out = [(rows[0][0].hex(), rows[0][1])]
    for row in sorted(page):
        flag = 0x80 if row == cur else 0
        if page[row] is None:
            out.append((bytes([row, flag]).hex() + '00' * 6, ''))
        else:
            header, text = page[row]
            out.append((bytes([row, header[1] & 0x7F | flag]).hex() + header[2:].hex(), text))
    last = max(page)
    return nxs2_list(out, max(1, min(cur - 4, max(1, last - 7))))


class Model:
    """What the counters should hold: the steps taken while a library list is up, one answered per library
    list, the ones still outstanding 150 ms after the last step dropped."""

    def __init__(self):
        self.rows, self.offset, self.steps, self.answered = None, None, [], 0
        self.encoder, self.last_step = None, None

    def report(self, t, data):
        encoder = data[20] | data[21] << 8
        if self.encoder is not None and encoder != self.encoder and data[5] & 0x80 and self.rows:
            delta = ((encoder - self.encoder + 0x8000) & 0xFFFF) - 0x8000
            self.steps += [1 if delta > 0 else -1] * abs(delta)
            self.last_step = t
        self.encoder = encoder
        if self.answered < len(self.steps) and t - self.last_step >= 0.150:
            del self.steps[self.answered:]

    def answer(self, data):
        rows = records(data)
        cursor = library(rows)
        if cursor is None:
            self.rows = None
            return nxs2_list([(h.hex(), t) for h, t in rows])
        if self.answered < len(self.steps):
            if self.rows:
                prev = {h[0]: (h, t) for h, t in self.rows[1:]}
                self.offset = predict(prev, library(self.rows), self.steps[self.answered], self.offset)[2]
            self.answered += 1
        if cursor < 5:
            self.offset = 0
        self.rows = rows
        return self.expected()

    def expected(self):
        return predicted_list(self.rows, self.steps[self.answered:], self.offset)


class Deck:
    """MAIN with x3000rxview on the HID OUT read and, unless `browse` is off, x3000browse on the HID IN send.
    The words in the erased run start as erased flash."""

    def __init__(self, browse=True):
        self.lists, self.task, self.preempt = [], None, None
        cpu = self.cpu = Sh4()
        cpu.write(RX_CODE, blob('x3000rxview'))
        cpu.write(BROWSE_CODE, blob('x3000browse'))
        cpu.write(WORDS, b'\xff' * 0x60)
        cpu.put32(HIDO_STATE, 2)
        cpu.put32(AUTH, 1)
        cpu.stubs[FRAMEWORK] = self.framework
        self.browse = browse

    def framework(self, cpu):
        self.lists.append((self.t, self.task, cpu.read(cpu.r[4], cpu.r[5])))
        if self.preempt and self.task == 'in':
            report, self.preempt = self.preempt, None
            self.interrupt(report)

    def interrupt(self, report):
        """The HID OUT task taking the CPU from inside the HID IN task's call."""
        cpu = self.cpu
        held = list(cpu.r), cpu.pr, cpu.t
        self.hid_out(self.t, report, stack=PREEMPT_STACK)
        cpu.r[:], cpu.pr, cpu.t = held[0], held[1], held[2]
        self.task = 'in'

    def run(self, code, stack):
        cpu = self.cpu
        cpu.r[4], cpu.r[15], cpu.pr = SAVED if stack == STACK else stack - 0x100, stack, RET
        for k in range(8, 15):
            cpu.r[k] = 0x80000000 + k
        cpu.run(code, limit=400000)
        assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [stack]

    def hid_in(self, t, report):
        if not self.browse:
            return
        cpu, self.t, self.task = self.cpu, t, 'in'
        cpu.put32(TICK, round(t * 1000))
        cpu.write(SLOT, report)
        cpu.put32(SLOTS + 4, SLOT)
        cpu.write(SAVED, struct.pack('<8I', 4, 1, 2, 3, 0x2A, 5, 64, SLOTS))
        self.run(BROWSE_CODE, STACK)
        assert cpu.read(SLOT, 64) == report

    def hid_out(self, t, report, count=1024, stack=STACK):
        cpu, self.t, self.task = self.cpu, t, 'out'
        cpu.write(RX_BUFFER, report)
        cpu.write(stack - 0x100, struct.pack('<8I', count & 0xFFFFFFFF, 1, 2, 3, 4, count & 0xFFFFFFFF, 6, 7))
        cpu.t = int(count == -5)
        self.run(RX_CODE, stack)

    def feed(self, events):
        for t, way, data in events:
            (self.hid_in if way == 'in' else self.hid_out)(t, data)
        return self


def replay(events=SCROLL):
    """Feed the deck and the model the same reports; every list the framework gets is checked against the
    model's page for the answer and the steps outstanding when it was handed over."""
    deck, model = Deck(), Model()
    for t, way, data in events:
        before = len(deck.lists)
        if way == 'in':
            model.report(t, data)
            deck.hid_in(t, data)
            want = model.expected() if model.rows else None
        else:
            deck.hid_out(t, data)
            want = model.answer(data) if data[1] == 0x2A else None
        for _, _, got in deck.lists[before:]:
            assert got == want, t
    return deck, model


def test_every_list_shown_is_the_predicted_page_of_the_last_answer():
    deck, model = replay()
    shown = [task for _, task, _ in deck.lists]
    # 16 answers, three of them already on screen as predicted
    assert shown.count('in') == 14 and shown.count('out') == 13


def test_an_answer_only_fills_in_blank_rows_save_two_snaps():
    deck, _ = replay()
    snaps = []
    for (_, _, before), (t, task, after) in zip(deck.lists, deck.lists[1:]):
        if task == 'out' and t > 65:
            if any(old and old != new for old, new in zip(texts(before), texts(after))):
                snaps.append(round(t, 3))
    # Serato moved two rows for one step at 65.966, and at 66.914 the crate's
    # top, which the deck could not know, moved the cursor instead of the rows
    assert snaps == [65.966, 66.914]


def test_the_deck_moves_on_each_step_before_serato_answers():
    deck, _ = replay()
    states = [(t, data) for t, way, data in SCROLL if way == 'in']
    steps = [t for (t, data), (_, before) in zip(states[1:], states) if data[20:22] != before[20:22]]
    shown = [t for t, task, _ in deck.lists if task == 'in']
    # the last step, at the crate's top, changes nothing on screen
    assert len(steps) == 15 and set(steps[:-1]) <= set(shown)


def test_a_step_up_at_row_5_shows_the_rows_moved_down_with_a_blank_top_row():
    deck, _ = replay()
    t, _, first = next(entry for entry in deck.lists if entry[1] == 'in')
    assert round(t, 3) == 65.671
    rows = texts(first)
    assert rows[0] == 'Uptempo selection' and rows[1] == ''
    answer = next(entry for entry in deck.lists if entry[0] > t)
    assert texts(answer[2])[2:] == rows[2:]


def test_once_the_cursor_is_above_row_5_a_step_up_moves_the_cursor():
    deck, _ = replay()
    after = [entry for entry in deck.lists if entry[1] == 'in' and 67.0 < entry[0] < 67.35]
    assert after
    for _, _, message in after:
        assert '' not in texts(message)


def test_a_step_serato_does_not_answer_is_dropped_after_150_ms():
    events, t = first_library()
    encoder = struct.unpack('<H', SCROLL[0][2][20:22])[0]
    deck = Deck().feed(events + [state(t + 1, encoder), state(t + 2, encoder - 1)])
    assert texts(deck.lists[-1][2])[1] == ''
    for later in (2.1, 2.149):
        deck.feed([state(t + later, encoder - 1)])
    assert [task for _, task, _ in deck.lists[2:]] == ['in']
    deck.feed([state(t + 2.150, encoder - 1)])
    assert deck.lists[-1] == (t + 2.150, 'in', translated(events[-1][2]))
    assert deck.cpu.u32(STEPS) == deck.cpu.u32(ANSWERED)


def test_the_unanswered_step_at_the_crates_top_is_dropped():
    deck, model = replay()
    assert deck.cpu.u32(STEPS) == deck.cpu.u32(ANSWERED)
    assert len(model.steps) == model.answered


def test_a_list_equal_to_the_last_one_shown_is_not_handed_over_again():
    deck, _ = replay()
    for (_, _, a), (_, _, b) in zip(deck.lists, deck.lists[1:]):
        assert a != b


def test_the_hid_in_task_shows_at_most_one_list_every_30_ms():
    deck, _ = replay()
    times = [t for t, task, _ in deck.lists if task == 'in']
    assert all(b - a >= 0.030 for a, b in zip(times, times[1:]))


def first_library():
    """The reports up to the first library list, and the time it came."""
    k = next(i for i, (_, way, data) in enumerate(SCROLL) if way == 'out' and data[1] == 0x2A
             and library(records(data)))
    return SCROLL[:k + 1], SCROLL[k][0]


def state(t, encoder, library_open=True):
    report = bytearray(SCROLL[0][2])
    report[5] = 0x80 if library_open else 0
    report[20:22] = struct.pack('<H', encoder & 0xFFFF)
    return t, 'in', bytes(report)


def test_steps_inside_30_ms_are_shown_together_at_the_next_report():
    events, t = first_library()
    encoder = struct.unpack('<H', SCROLL[0][2][20:22])[0]
    deck, model = Deck().feed(events + [state(t + 1, encoder)]), Model()
    for ev in events + [state(t + 1, encoder)]:
        (model.report(ev[0], ev[2]) if ev[1] == 'in' else model.answer(ev[2]) if ev[2][1] == 0x2A else None)
    before = len(deck.lists)
    for ev in (state(t + 2, encoder - 1), state(t + 2.010, encoder - 2)):
        model.report(ev[0], ev[2])
        deck.feed([ev])
    assert [entry[0] for entry in deck.lists[before:]] == [t + 2]
    deck.feed([state(t + 2.040, encoder - 2)])
    model.report(t + 2.040, state(t + 2.040, encoder - 2)[2])
    assert [entry[0] for entry in deck.lists[before:]] == [t + 2, t + 2.040]
    assert deck.lists[-1][2] == model.expected()
    assert model.steps[model.answered:] == [-1, -1]


def test_steps_with_the_library_closed_are_not_counted():
    events, t = first_library()
    encoder = struct.unpack('<H', SCROLL[0][2][20:22])[0]
    deck = Deck().feed(events + [state(t + 1, encoder), state(t + 2, encoder - 3, library_open=False)])
    assert deck.cpu.u32(STEPS) == deck.cpu.u32(ANSWERED)
    assert all(task == 'out' for _, task, _ in deck.lists)


def answer_after(t0):
    return next((t, data) for t, way, data in SCROLL if way == 'out' and data[1] == 0x2A and t > t0)


@pytest.mark.parametrize('retry', ['report', 'timeout'])
def test_hid_out_finding_the_guard_taken_owes_the_list_and_commits_it_later(retry):
    events, t = first_library()
    deck = Deck().feed(events)
    t1, answer = answer_after(t)
    deck.cpu.write(GUARD, b'\x80')
    before = len(deck.lists)
    deck.hid_out(t1, answer)
    assert deck.lists[before:] == [] and deck.cpu.u32(LIST_OWED) > 0
    deck.cpu.write(GUARD, b'\x00')
    if retry == 'report':
        deck.hid_out(t1 + 0.002, bytes([0, 0x3A]).ljust(1024, b'\0'))
    else:
        deck.hid_out(t1 + 0.100, bytes(1024), count=-5)
    assert [task for _, task, _ in deck.lists[before:]] == ['out']
    assert deck.lists[-1][2] == translated(answer)
    assert deck.cpu.u32(LIST_OWED) == 0 and deck.cpu.read(GUARD, 1) == b'\x00'


def test_an_answer_arriving_while_hid_in_hands_over_its_list_is_committed_at_the_next_report():
    events, t = first_library()
    encoder = struct.unpack('<H', SCROLL[0][2][20:22])[0]
    deck = Deck().feed(events + [state(t + 1, encoder)])
    t1, answer = answer_after(t)
    deck.preempt = answer
    before = len(deck.lists)
    deck.hid_in(t + 2, state(t + 2, encoder - 1)[2])
    assert [task for _, task, _ in deck.lists[before:]] == ['in']
    assert deck.cpu.u32(LIST_OWED) > 0 and deck.cpu.read(GUARD, 1) == b'\x00'
    deck.hid_out(t + 2.002, bytes([0, 0x3A]).ljust(1024, b'\0'))
    assert [task for _, task, _ in deck.lists[before:]] == ['in', 'out']
    assert deck.cpu.u32(LIST_OWED) == 0
    assert deck.cpu.u32(STEPS) == deck.cpu.u32(ANSWERED)


def test_hid_in_finding_the_guard_taken_keeps_the_steps_for_the_next_report():
    events, t = first_library()
    encoder = struct.unpack('<H', SCROLL[0][2][20:22])[0]
    deck = Deck().feed(events + [state(t + 1, encoder)])
    deck.cpu.write(GUARD, b'\x80')
    before = len(deck.lists)
    deck.feed([state(t + 2, encoder - 1)])
    assert deck.lists[before:] == []
    assert (deck.cpu.u32(STEPS) - deck.cpu.u32(ANSWERED)) & 0xFFFFFFFF == 1
    deck.cpu.write(GUARD, b'\x00')
    deck.feed([state(t + 2.1, encoder - 1)])
    assert [task for _, task, _ in deck.lists[before:]] == ['in']


def translated(data):
    """A list as x3000rxview hands it over without the local cursor."""
    rows = records(data)
    cursor, last = library(rows) or 0, max(h[0] for h, _ in rows)
    return nxs2_list([(h.hex(), x) for h, x in rows], max(1, min(cursor - 4, max(1, last - 7))))


def test_with_the_knob_off_every_answer_is_shown_as_serato_sent_it_and_nothing_else():
    deck = Deck(browse=False).feed(SCROLL)
    answers = [data for _, way, data in SCROLL if way == 'out' and data[1] == 0x2A]
    assert [(task, message) for _, task, message in deck.lists] == [('out', translated(data)) for data in answers]
    words = bytearray(deck.cpu.read(WORDS, 0x60))
    words[LIST_OWED - WORDS:LIST_OWED - WORDS + 4] = b'\xff' * 4
    assert words == b'\xff' * 0x60


# -- on the real image ----------------------------------------------------------------

@pytest.fixture(scope='module')
def real():
    with open(MAIN, 'rb') as f:
        return f.read()


@needs_main
def test_the_hook_runs_after_x3000in_in_the_hid_in_task(real):
    img = sigpatch.Image(nxs2.MAIN_PROFILE, real)
    mod = patch_main.MODS['x3000browse']
    assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == 0x084F70C2
    for stock, source in mod.imports:
        assert sigpatch.import_value(img, source) == stock
    assert sigpatch.hook_sites(patch_main.MODS)[sigpatch.site_key(mod)] == ['x3000in', 'x3000browse']


@needs_main
def test_it_goes_in_with_the_identity_and_is_left_as_it_is_when_patched_again(real):
    names = ['x3000ident', 'x3000bcd', 'x3000hs', 'x3000in', 'x3000out', 'x3000rxbuf', 'x3000rxview', 'x3000browse']
    report = {}
    once = patch_main.patch(real, names)
    assert patch_main.patch(once, names, report=report) == once
    assert report['x3000browse'][0] == 'present'
    assert patch_main.patch(real, names[::-1]) == once
