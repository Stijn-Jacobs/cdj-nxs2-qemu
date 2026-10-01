# SPDX-License-Identifier: GPL-2.0-or-later
"""The usb_midi MAIN mod pair: the timing half on the 1 ms timer interrupt and
the sending half at the top of the MIDI task's loop, found by signature, and --
through the SH-4 interpreter of test_main_osc extended with the instructions
these routines add, run tick by tick against a model of the MIDI task --
turning the deck's play flag and BPM into USB-MIDI Start / Stop / Continue and
24 Timing Clocks per beat on a 1 ms grid."""
import os
import struct

import pytest

import patch_main
import sigpatch
from test_main_osc import Sh4

BASE = 0x08000000
DEVICE = 0x0AB84CFA
GATE = 0x10DE1FA4
RECORDS = 0x0A371D3C
FILL, COMMIT = 0x084F511E, 0x084F5148
INTSTS0 = 0xA4D90040
TASK_STATE = 0x0BD50E8C
CLASS_FLAGS = 0x0BD4B0C8
SET_FLG = 0x08516448
CONFIGURED = 0x20000000
WAKE = 0x2                      # the bit the timer half posts; no task state accepts it
FLAG_ID = 0x48
BLOCK = 0x2100                  # the trampoline's saved r0-r7
FILL_BUFFERS = (0x5000, 0x5100)
BUFFERS = 0x0BD4B0F0            # send index, fill index, then a committed length per buffer
PERIOD = 250000                 # phase per clock: 24 per beat, BPM x 100, 1000 ticks per s

REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')


def blob(name):
    with open(patch_main.blob_path(name), 'rb') as f:
        return f.read()


# -- registry and placement ------------------------------------------------------

def test_registered_as_hooks_on_the_midi_task_and_the_timer_interrupt():
    for name, site in (('usbmidi', patch_main.MIDI_TASK), ('usbmiditick', patch_main.TIMER_TICK)):
        mod = patch_main.MODS[name]
        assert mod.target == 'main' and mod.hook and not mod.literal
        assert dict(sig=mod.sig, start=mod.start, end=mod.end) == site
        assert mod.end - mod.start == 6


def test_the_registry_row_enables_both_halves():
    path = os.path.join(os.path.dirname(patch_main.__file__), 'mods.conf')
    rows = [line.split('|') for line in open(path, encoding='utf-8')
            if line.strip() and not line.startswith('#')]
    (row,) = [r for r in rows if r[0] == 'usb_midi']
    assert row[1].split('+') == ['CDJ_MAIN_USBMIDI', 'CDJ_MAIN_USBMIDITICK']


def test_the_timing_half_is_placed_right_after_the_sending_half():
    offsets, _ = sigpatch.layout(patch_main.blob_path, patch_main.MODS)
    assert len(blob('usbmidi')) % 16 == 0
    assert offsets['usbmiditick'] - offsets['usbmidi'] == len(blob('usbmidi'))


@pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
def test_real_main_has_each_site_once_where_the_design_puts_it():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    img = sigpatch.Image(patch_main.MAIN_PROFILE, data)
    out = patch_main.patch(data, sorted(patch_main.MODS))
    for name, at in (('usbmidi', 0x084F6250), ('usbmiditick', 0x08448F9E)):
        mod = patch_main.MODS[name]
        assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == at
        span = at - BASE
        assert out[span:span + 12] != data[span:span + 12]
        assert out[span + 12:span + 16] == data[span + 12:span + 16]


# -- the routines, run -----------------------------------------------------------

class UsbMidiCpu(Sh4):
    """The oscbeat interpreter plus the instructions these routines add, with
    the firmware's fixed addresses mapped onto a small scratch memory and its
    functions replaced by a model of the MIDI task's buffers and flag."""
    CODE_AT = 0x1000
    WINDOWS = ((DEVICE, 0x2010, 1), (GATE, 0x2020, 4), (BUFFERS, 0x2040, 0x10),
               (INTSTS0, 0x2050, 2), (TASK_STATE, 0x2060, 4), (CLASS_FLAGS, 0x2070, 8),
               (RECORDS, 0x3000, 0x600))

    def __init__(self):
        super().__init__(blob('usbmidi'), self.CODE_AT)
        offsets, _ = sigpatch.layout(patch_main.blob_path, patch_main.MODS)
        self.tick_at = self.CODE_AT + offsets['usbmiditick'] - offsets['usbmidi']
        tick = blob('usbmiditick')
        self.mem[self.tick_at:self.tick_at + len(tick)] = tick
        self.macl = self.mach = 0
        self.flag = 0
        self.posted = []
        self.sends = []
        self.poke(BUFFERS, 2, 1)            # buffer 0 is being filled, buffer 1 sent
        self.poke(BUFFERS + 2, 2, 0)

    def loc(self, a):
        for real, scratch, size in self.WINDOWS:
            if real <= a < real + size:
                return scratch + a - real
        return a

    def u32(self, a):
        return super().u32(self.loc(a))

    def put32(self, a, v):
        super().put32(self.loc(a), v)

    def s8(self, a):
        return super().s8(self.loc(a))

    def index(self, k):
        return struct.unpack_from('<H', self.mem, self.loc(BUFFERS + 2 * k))[0]

    def length(self, buffer):
        return self.u32(BUFFERS + 4 + 4 * buffer)

    def fill_length(self, value):
        self.poke(BUFFERS + 4 + 4 * self.index(1), 4, value)

    def swap_buffers(self):
        send, fill = self.index(0), self.index(1)
        self.poke(BUFFERS, 2, fill)
        self.poke(BUFFERS + 2, 2, send)

    def poke(self, real, size, value):
        at = self.loc(real)
        self.mem[at:at + size] = value.to_bytes(size, 'little')

    def branch(self, cond, pc, simm, delayed):
        if delayed:
            taken = cond
            self.delay(pc)
            return pc + 4 + simm * 2 if taken else pc + 4
        return pc + 4 + simm * 2 if cond else pc + 2

    def step(self, op, pc):
        r = self.r
        n, m = op >> 8 & 15, op >> 4 & 15
        imm = op & 0xFF
        simm = imm - 256 if imm & 0x80 else imm
        if op & 0xFF00 in (0x8900, 0x8B00, 0x8D00, 0x8F00):
            cond = self.t if op & 0xFF00 in (0x8900, 0x8D00) else not self.t
            return self.branch(cond, pc, simm, op & 0xFF00 in (0x8D00, 0x8F00))
        if op >> 12 == 0xA:                                     # bra
            disp = op & 0xFFF
            disp = disp - 0x1000 if disp & 0x800 else disp
            self.delay(pc)
            return pc + 4 + disp * 2
        if op >> 12 == 0x9:                                     # mov.w @(disp,pc),Rn
            v = struct.unpack_from('<H', self.mem, pc + 4 + imm * 2)[0]
            r[n] = v - 0x10000 if v & 0x8000 else v
        elif op & 0xFF00 == 0x8500:                             # mov.w @(disp,Rm),r0
            r[0] = struct.unpack_from('<H', self.mem, self.loc(r[m] + (op & 15) * 2))[0]
        elif op & 0xF00F == 0x6000:                             # mov.b @Rm,Rn
            r[n] = self.s8(r[m])
        elif op & 0xF00F == 0x6001:                             # mov.w @Rm,Rn
            r[n] = struct.unpack_from('<H', self.mem, self.loc(r[m]))[0]
        elif op & 0xF00F == 0x6002:                             # mov.l @Rm,Rn
            r[n] = self.u32(r[m])
        elif op & 0xF00F == 0x600C:                             # extu.b
            r[n] = r[m] & 0xFF
        elif op & 0xF00F == 0x600D:                             # extu.w
            r[n] = r[m] & 0xFFFF
        elif op & 0xF00F == 0x2000:                             # mov.b Rm,@Rn
            self.mem[self.loc(r[n])] = r[m] & 0xFF
        elif op & 0xF00F == 0x0006:                             # mov.l Rm,@(R0,Rn)
            self.put32(r[0] + r[n], r[m])
        elif op & 0xF00F == 0x2008:                             # tst Rm,Rn
            self.t = int(r[n] & r[m] == 0)
        elif op & 0xF00F == 0x200A:                             # xor Rm,Rn
            r[n] ^= r[m]
        elif op & 0xF00F == 0x3000:                             # cmp/eq
            self.t = int(r[n] == r[m])
        elif op & 0xF00F == 0x3002:                             # cmp/hs
            self.t = int(r[n] >= r[m])
        elif op & 0xF00F == 0x3006:                             # cmp/hi
            self.t = int(r[n] > r[m])
        elif op & 0xF00F == 0x3008:                             # sub
            r[n] = (r[n] - r[m]) & 0xFFFFFFFF
        elif op & 0xF00F == 0x000E:                             # mov.l @(R0,Rm),Rn
            r[n] = self.u32(r[0] + r[m])
        elif op & 0xF00F == 0x0007:                             # mul.l
            self.macl = r[n] * r[m] & 0xFFFFFFFF
        elif op & 0xF00F == 0x3005:                             # dmulu.l
            product = r[n] * r[m]
            self.macl, self.mach = product & 0xFFFFFFFF, product >> 32
        elif op & 0xF0FF == 0x001A:                             # sts macl,Rn
            r[n] = self.macl
        elif op & 0xF0FF == 0x000A:                             # sts mach,Rn
            r[n] = self.mach
        elif op & 0xF0FF in (0x4002, 0x4012):                   # sts.l mach/macl,@-Rn
            r[n] = (r[n] - 4) & 0xFFFFFFFF
            self.put32(r[n], self.mach if op & 0xF0 == 0 else self.macl)
        elif op & 0xF0FF in (0x4006, 0x4016):                   # lds.l @Rm+,mach/macl
            v = self.u32(r[n])
            r[n] += 4
            if op & 0xF0 == 0:
                self.mach = v
            else:
                self.macl = v
        elif op & 0xF00F == 0x200B:                             # or
            r[n] |= r[m]
        elif op & 0xF0FF == 0x0029:                             # movt
            r[n] = self.t
        elif op & 0xF0FF == 0x4011:                             # cmp/pz
            self.t = int(r[n] < 0x80000000)
        elif op & 0xF0FF == 0x4000:                             # shll
            r[n] = r[n] << 1 & 0xFFFFFFFF
        elif op & 0xF0FF == 0x4009:                             # shlr2
            r[n] = r[n] >> 2
        elif op & 0xF0FF == 0x4018:                             # shll8
            r[n] = r[n] << 8 & 0xFFFFFFFF
        elif op & 0xF0FF == 0x4029:                             # shlr16
            r[n] = r[n] >> 16
        elif op & 0xFF00 == 0xC800:                             # tst #imm,r0
            self.t = int(r[0] & imm == 0)
        elif op & 0xFF00 == 0xC900:                             # and #imm,r0
            r[0] &= imm
        elif op & 0xFF00 == 0x8800:                             # cmp/eq #imm,r0
            self.t = int(r[0] == simm & 0xFFFFFFFF)
        else:
            return super().step(op, pc)
        return pc + 2

    def call(self, target, resume):
        if target == FILL:
            self.r[0] = FILL_BUFFERS[self.index(1)]
        elif target == SET_FLG:
            self.posted.append((self.r[4], self.r[5]))
            if self.r[4] == FLAG_ID:
                self.flag |= self.r[5]
            self.r[0] = 0
        elif target == COMMIT:
            assert 0 < self.r[4] <= 0xA0
            self.fill_length(self.r[4])
            self.flag |= 1
            self.r[0] = 0
        else:
            raise AssertionError('unexpected call to %#x' % target)
        return resume


SAVED = dict(zip(range(8, 15), (0xC0DE0008, 0xC0DE0009, 0xC0DE000A, 0xC0DE000B,
                                0xC0DE000C, 0xC0DE000D, 0xFFE7FFFF)))


def run_hook(cpu, at):
    cpu.r[4], cpu.r[5], cpu.r[15] = BLOCK, 0xF000, 0xF000
    for k, v in SAVED.items():
        cpu.r[k] = v
    cpu.mach, cpu.macl = 0x11111111, 0x22222222
    cpu.pr = Sh4.RET
    cpu.run(at)
    assert cpu.r[15] == 0xF000
    assert all(cpu.r[k] == v for k, v in SAVED.items())


class Deck:
    """One deck, player 2, with the host attached and the MIDI interface
    configured unless told otherwise, run a millisecond at a time: the timer
    half on every tick, then the MIDI task for as long as its flag has bits
    set, the way its loop goes round from the wait (which clears the flag):
    the send when bit 0 was among the bits, then the hook at the top."""

    def __init__(self, bpm=128.0, playing=False, configured=True):
        self.cpu = UsbMidiCpu()
        self.ms = 0
        self.cpu.poke(DEVICE, 1, 2)
        self.cpu.poke(CLASS_FLAGS + 4, 4, FLAG_ID)
        if configured:
            self.cpu.poke(INTSTS0, 2, 0x30)
            self.cpu.poke(TASK_STATE, 4, 2)
            self.cpu.poke(GATE, 4, 1)
        self.bpm = bpm
        self.set(bpm=bpm, playing=playing)

    def set(self, bpm=None, playing=None, valid=True, pitch=0x100000):
        record = RECORDS + 2 * 0x124
        self.cpu.poke(record + 0x8C, 4, pitch)
        if playing is not None:
            self.cpu.poke(record + 0x89, 1, 0x40 if playing else 0)
        self.bpm = self.bpm if bpm is None else bpm
        self.cpu.poke(record + 0x90, 4, round(self.bpm * 100) | (0x80000000 if valid else 0))

    def tick(self):
        cpu = self.cpu
        run_hook(cpu, cpu.tick_at)
        assert (cpu.mach, cpu.macl) == (0x11111111, 0x22222222)
        self.ms += 1
        sent = []
        for _ in range(8):
            if not cpu.flag:
                return sent
            bits, cpu.flag = cpu.flag, 0
            if bits & 1:
                cpu.swap_buffers()
                send = cpu.index(0)
                length = cpu.length(send)
                assert 0 < length <= 0xA0
                data = bytes(cpu.mem[FILL_BUFFERS[send]:FILL_BUFFERS[send] + length])
                cpu.sends.append((self.ms, data))
                sent.append(data)
            run_hook(cpu, UsbMidiCpu.CODE_AT)
        raise AssertionError('the MIDI task never settles')

    def run(self, ms):
        """The status bytes sent in the next `ms` ticks, with the tick each
        went out on."""
        out = []
        for _ in range(ms):
            for data in self.tick():
                packets = [data[i:i + 4] for i in range(0, len(data), 4)]
                assert all(p[0] == 0x0F and p[2:] == b'\0\0' for p in packets)
                out += [(self.ms, p[1]) for p in packets]
        return out


def statuses(events):
    return [s for _, s in events]


def test_24_clocks_per_beat_at_128_bpm():
    deck = Deck()
    deck.set(playing=True)
    beat = 60000 / 128
    events = deck.run(round(8 * beat))
    assert statuses(events).count(0xF8) == 8 * 24


def test_clocks_leave_on_the_1ms_grid_one_per_transfer():
    deck = Deck()
    deck.set(playing=True)
    events = deck.run(2000)
    start = events[0][0]
    clock_ms = [ms for ms, s in events if s == 0xF8]
    per_tick = {}
    for ms in clock_ms:
        per_tick[ms] = per_tick.get(ms, 0) + 1
    assert max(per_tick.values()) == 1
    spacing = 60000 / (128 * 24)
    for k, ms in enumerate(clock_ms):
        assert abs(ms - start - k * spacing) < 1
    assert all(len(data) <= 8 for _, data in deck.cpu.sends)


@pytest.mark.parametrize('bpm, percent', [(90.0, 0), (150.0, 0), (120.0, 6), (120.0, -6),
                                          (120.0, 8), (120.0, -50)])
def test_clock_follows_the_bpm_and_the_pitch(bpm, percent):
    deck = Deck(bpm=bpm)
    deck.set(playing=True, pitch=0x100000 + 0x100000 * percent // 100)
    clocks = statuses(deck.run(5000)).count(0xF8)
    assert abs(clocks - bpm * 24 * (100 + percent) / 100 / 12) <= 1


def test_the_tempo_product_does_not_overflow_32_bits():
    deck = Deck(bpm=300.0)
    deck.set(playing=True, pitch=0x200000)
    assert abs(statuses(deck.run(2000)).count(0xF8) - 300 * 2 * 24 / 30) <= 1


def test_start_with_the_first_clock_stop_on_pause_continue_on_resume():
    deck = Deck()
    assert deck.run(20) == []                                # idle: silent
    deck.set(playing=True)
    events = deck.run(500)
    assert deck.cpu.sends[0][1][1::4] == bytes([0xFA, 0xF8])
    assert 0xFA not in statuses(events)[1:]
    deck.set(playing=False)
    assert statuses(deck.run(1)) == [0xFC]
    assert deck.run(500) == []                               # no clocks while paused
    deck.set(playing=True)
    events = deck.run(500)
    assert statuses(events)[:2] == [0xFB, 0xF8]
    assert 0xFB not in statuses(events)[1:]


def test_clocks_follow_the_generators_notes_in_the_same_send():
    deck = Deck(bpm=300.0, playing=True)
    deck.run(10)
    note = bytes([0x09, 0x90, 0x3C, 0x7F])
    cpu = deck.cpu
    fill = FILL_BUFFERS[cpu.index(1)]
    cpu.mem[fill:fill + 4] = note                           # committed, not yet sent
    cpu.fill_length(4)
    sends = []
    while not sends:
        sends = deck.tick()
    assert sends[0][:4] == note and sends[0][5] == 0xF8


def test_the_length_of_the_buffer_sent_last_is_not_kept():
    deck = Deck(bpm=300.0, playing=True)
    deck.run(1000)
    assert deck.cpu.sends
    assert all(len(data) == 4 for _, data in deck.cpu.sends[1:])


def test_clocks_that_do_not_fit_are_dropped_not_overflowed():
    deck = Deck(bpm=300.0, playing=True)
    deck.run(10)
    cpu = deck.cpu
    cpu.fill_length(152)                                     # 38 notes the generator left
    sends = []
    while not sends:
        sends = deck.tick()
    assert len(sends[0]) == 156 and sends[0][153] == 0xF8


def test_a_wake_with_nothing_new_commits_nothing():
    deck = Deck(playing=True)
    deck.run(5)
    before = len(deck.cpu.sends)
    deck.cpu.flag = WAKE
    run_hook(deck.cpu, UsbMidiCpu.CODE_AT)
    assert deck.cpu.flag == WAKE and len(deck.cpu.sends) == before


def test_silent_without_a_configured_host():
    deck = Deck(playing=True, configured=False)
    assert deck.run(200) == []
    assert deck.cpu.posted == []


def test_silent_without_a_valid_bpm_or_player():
    deck = Deck()
    deck.set(playing=True, valid=False)
    assert 0xF8 not in statuses(deck.run(500))
    deck = Deck(playing=True)
    deck.cpu.poke(DEVICE, 1, 0)
    assert deck.run(500) == []


def configure(deck, state=1, gate=0):
    deck.cpu.poke(INTSTS0, 2, 0x30)
    deck.cpu.poke(TASK_STATE, 4, state)
    deck.cpu.poke(GATE, 4, gate)


def arms(deck):
    return [p for p in deck.cpu.posted if p[1] == CONFIGURED]


def test_arms_the_midi_class_once_when_the_host_has_configured_it():
    deck = Deck(configured=False)
    configure(deck)
    deck.run(3)
    assert arms(deck) == [(FLAG_ID, CONFIGURED)]


def test_waits_for_the_attached_task_state():
    deck = Deck(configured=False)
    configure(deck, state=0)
    deck.run(1)
    assert arms(deck) == []
    deck.cpu.poke(TASK_STATE, 4, 1)
    deck.run(1)
    assert len(arms(deck)) == 1


def test_not_armed_while_the_host_is_unconfigured_or_the_gate_is_open():
    for dvsq in (0x00, 0x10, 0x20, 0x40, 0x70):
        deck = Deck(configured=False)
        configure(deck)
        deck.cpu.poke(INTSTS0, 2, dvsq | 0x8081)
        deck.run(1)
        assert arms(deck) == []
    deck = Deck(configured=False)
    configure(deck, gate=1)
    deck.run(1)
    assert arms(deck) == []


def test_arms_again_after_the_host_has_left_and_come_back():
    deck = Deck(configured=False)
    configure(deck)
    deck.run(1)
    deck.cpu.poke(INTSTS0, 2, 0x10)
    deck.run(1)
    configure(deck)
    deck.run(1)
    assert len(arms(deck)) == 2


def test_sends_after_arming_from_the_tasks_initial_lengths():
    deck = Deck(configured=False, playing=True)
    deck.cpu.poke(BUFFERS + 4, 4, 0xA0)         # the MIDI task's init: both buffers "full"
    deck.cpu.poke(BUFFERS + 8, 4, 0xA0)
    configure(deck)
    deck.run(1)
    assert len(arms(deck)) == 1
    deck.cpu.poke(TASK_STATE, 4, 2)             # the task has taken the event
    deck.cpu.poke(GATE, 4, 1)
    deck.set(playing=False)
    deck.run(1)
    deck.set(playing=True)
    assert 0xF8 in statuses(deck.run(200))
    assert all(len(data) <= 8 for _, data in deck.cpu.sends)


def test_arms_without_a_player_number():
    deck = Deck(configured=False)
    deck.cpu.poke(DEVICE, 1, 0)
    configure(deck)
    deck.run(1)
    assert len(arms(deck)) == 1


def test_the_timer_half_posts_only_the_wake_bit_and_only_with_something_new():
    deck = Deck(playing=True)
    deck.run(1000)
    wakes = [p for p in deck.cpu.posted if p != (FLAG_ID, CONFIGURED)]
    assert set(wakes) == {(FLAG_ID, WAKE)}
    assert len(wakes) == len(deck.cpu.sends)
