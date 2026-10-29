# SPDX-License-Identifier: GPL-2.0-or-later
"""The key_shift MAIN mod: keyshiftpad on the pad pass's mode switch steps the
shift while MASTER TEMPO is held and takes pads A..C from their stock action,
keyshift on DSPoutCTL's block compare puts the shift into id-block word 2, and
keyshiftload clears it on a load, keyshiftkey shows it in the title bar's key.
All four run here on the interpreter of
test_main_phrase against a model of the words they read and write; where the
real 1.87 MAIN is extracted, every site and address they import is found in
it."""
import os
import struct

import pytest

import patch_main
import sigpatch
from players import cdj2000nxs2 as nxs2
from test_main_phrase import RET, STACK, Sh4
from helpers import mod_row

CODE = 0x08140000
SAVED = STACK - 0x100
FRAME = 0x0C000000              # DSPoutCTL's r15 at the compare
PADS, PAD_COPIES = 0x0B056EDC, 0x0B057CD8
MT_KEY, PAD_MODE, PAD_QUEUE, PAD_LAST = 0x0B54CB0C, 0x0B05651C, 0x0B056BB4, 0x10DE00F8
SHIFT, PADS_DOWN = 0x081FFFEC, 0x081FFFE8
SPEED_SNAP = 0x082DFCC6
KEY_FIELD, KEY_TEXT = 0x0B540ED0, 0x081FB500
MODEL_LOCK, MODEL_UNLOCK = 0x08404A38, 0x08404A5E
MT_FLAG = 0x40000000
NONE = 0xFFFFFFFF

REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')
needs_main = pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')


class Cpu(Sh4):
    """test_main_phrase's interpreter with the few instructions more these
    routines use."""

    def step_op(self, op, pc):
        r = self.r
        n, m = op >> 8 & 15, op >> 4 & 15
        M = 0xFFFFFFFF
        if op >> 12 == 0x9:
            r[n] = self.s16(pc + 4 + (op & 0xFF) * 2)
        elif op & 0xF00F == 0x600D:
            r[n] = r[m] & 0xFFFF
        elif op & 0xF00F == 0x600E:
            r[n] = self.s8_of(r[m])
        elif op & 0xF00F == 0x6007:
            r[n] = ~r[m] & M
        elif op & 0xF00F == 0x000E:
            r[n] = self.u32(r[0] + r[m])
        elif op & 0xF00F == 0x0006:
            self.put32(r[0] + r[n], r[m])
        elif op & 0xF0FF == 0x0029:
            r[n] = self.t
        elif op & 0xF0FF == 0x4019:
            r[n] >>= 8
        elif op & 0xF00F == 0x3003:
            self.t = int(self.signed(r[n]) >= self.signed(r[m]))
        elif op & 0xF00F == 0x3007:
            self.t = int(self.signed(r[n]) > self.signed(r[m]))
        elif op & 0xFF00 == 0xC800:
            self.t = int(r[0] & op & 0xFF == 0)
        elif op & 0xF00F == 0x000D:
            r[n] = self.s16(r[0] + r[m])
        elif op & 0xF00F == 0x6001:
            r[n] = self.s16(r[m])
        elif op & 0xF00F == 0x6005:
            r[n] = self.s16(r[m])
            if n != m:
                r[m] = (r[m] + 2) & M
        elif op & 0xF00F == 0x2001:
            self.write(r[n], struct.pack('<H', r[m] & 0xFFFF))
        elif op & 0xF00F == 0x600B:
            r[n] = -r[m] & M
        elif op == 0x0008:
            self.t = 0
        elif op & 0xFF00 == 0x8F00:
            taken = not self.t
            self.delay(pc)
            disp = (op & 0xFF) - (0x100 if op & 0x80 else 0)
            return pc + 4 + disp * 2 if taken else pc + 4
        else:
            return super().step_op(op, pc)
        return pc + 2

    @staticmethod
    def s8_of(v):
        v &= 0xFF
        return (v - 0x100 if v & 0x80 else v) & 0xFFFFFFFF

    @staticmethod
    def signed(v):
        return v - (1 << 32) if v >> 31 else v


def blob(name):
    with open(patch_main.blob_path(name), 'rb') as f:
        return f.read()


def hook(cpu, at, frame=FRAME):
    """Run a hook the way the shared trampoline calls it: r4 = the saved r0-r7,
    r5 = r15 at the site; r8-r14 and the saved registers come back as they went."""
    saved = list(range(8))
    cpu.write(SAVED, struct.pack('<8I', *saved))
    cpu.r[4], cpu.r[5], cpu.r[15], cpu.pr = SAVED, frame, STACK, RET
    for k in range(8, 15):
        cpu.r[k] = 0x80000000 + k
    cpu.run(at)
    assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [STACK]
    assert list(struct.unpack('<8I', cpu.read(SAVED, 32))) == saved


def shift_word(n):
    return 0x100 | n & 0xFF


# -- the pads ----------------------------------------------------------------------

class Panel:
    def __init__(self, shift=NONE, mode=2, queue=0xFFFFFFFF):
        self.cpu = cpu = Cpu()
        cpu.write(CODE, blob('keyshiftpad'))
        cpu.put32(SHIFT, shift)
        cpu.put32(PADS_DOWN, NONE)
        cpu.put32(PAD_MODE, mode)
        cpu.put32(PAD_QUEUE, queue)
        cpu.put32(PAD_LAST, 0xFFFFFFFF)
        for slot in range(9):
            self.pad(slot, False)
            cpu.put32(PAD_COPIES + 4 * slot, 0)
        self.mt(False)
        self.pass_()

    def pad(self, slot, down):
        self.cpu.put32(PADS + 4 * slot, int(down))

    def mt(self, down):
        self.cpu.put32(MT_KEY, int(down))

    def pass_(self):
        hook(self.cpu, CODE)

    def press(self, slot):
        self.pad(slot, True)
        self.pass_()
        self.pad(slot, False)
        self.pass_()

    @property
    def shift(self):
        word = self.cpu.u32(SHIFT)
        assert word >> 8 == 1
        return Cpu.signed(Cpu.s8_of(word))


def test_pad_b_raises_and_pad_a_lowers_the_shift_while_master_tempo_is_held():
    panel = Panel()
    panel.mt(True)
    panel.press(2)
    panel.press(2)
    assert panel.shift == 2
    panel.press(1)
    panel.press(1)
    panel.press(1)
    assert panel.shift == -1


def test_pad_c_puts_the_shift_back_to_zero():
    panel = Panel(shift=shift_word(5))
    panel.mt(True)
    panel.press(3)
    assert panel.shift == 0


def test_the_shift_stops_at_an_octave_either_way():
    panel = Panel(shift=shift_word(11))
    panel.mt(True)
    for _ in range(3):
        panel.press(2)
    assert panel.shift == 12
    panel = Panel(shift=shift_word(-11))
    panel.mt(True)
    for _ in range(3):
        panel.press(1)
    assert panel.shift == -12


def test_a_held_pad_steps_once():
    panel = Panel()
    panel.mt(True)
    panel.pad(2, True)
    for _ in range(5):
        panel.pass_()
    assert panel.shift == 1


def test_without_master_tempo_held_the_pads_leave_the_shift_alone():
    panel = Panel(shift=shift_word(3))
    panel.press(1)
    panel.press(2)
    panel.press(3)
    assert panel.cpu.u32(SHIFT) == shift_word(3)


def test_a_pad_held_before_master_tempo_is_not_a_press():
    panel = Panel()
    panel.pad(2, True)
    panel.pass_()
    panel.mt(True)
    panel.pass_()
    assert panel.shift == 0


@pytest.mark.parametrize('mode', [1, 2])
def test_in_the_pad_modes_that_compare_copies_pads_a_to_c_are_taken(mode):
    panel = Panel(mode=mode)
    panel.mt(True)
    panel.pad(2, True)
    panel.pad(5, True)
    panel.pass_()
    assert [panel.cpu.u32(PAD_COPIES + 4 * s) for s in (1, 2, 3, 5)] == [0, 1, 0, 0]


def test_without_master_tempo_the_copies_are_left_to_the_firmware():
    panel = Panel(mode=2)
    panel.pad(2, True)
    panel.pass_()
    assert panel.cpu.u32(PAD_COPIES + 8) == 0


@pytest.mark.parametrize('head, last', [(1, 1), (3, 3), (4, 0xFFFFFFFF), (0xFFFFFFFF, 0xFFFFFFFF)])
def test_in_hot_cue_mode_a_queued_pad_a_to_c_is_marked_handled(head, last):
    panel = Panel(mode=0)
    panel.mt(True)
    panel.cpu.put32(PAD_QUEUE, head)
    if head in (1, 2, 3):
        panel.pad(head, True)
    panel.pass_()
    assert panel.cpu.u32(PAD_LAST) == last


# -- the wire ----------------------------------------------------------------------

class Block:
    def __init__(self, shift, speed=0x100000, flags=0, mode=0):
        self.cpu = cpu = Cpu()
        cpu.write(CODE, blob('keyshift'))
        cpu.put32(SHIFT, shift)
        cpu.put32(FRAME + 0x1D4, speed)
        cpu.put32(FRAME + 0x1D8, flags)
        cpu.put32(FRAME + 0x1E8, mode << 16)
        self.snaps = []
        cpu.stubs[SPEED_SNAP] = self.snap

    def snap(self, cpu):
        r = cpu.r
        self.snaps.append((r[4], r[5], r[6]))
        assert r[6] == FRAME + 0x1D4 and r[7] not in (FRAME + 0x1D4, FRAME + 0x1D8)
        cpu.put32(r[6], 0x100000)
        cpu.fpscr = 0xFFFFFFFF

    def run(self):
        hook(self.cpu, CODE)
        return self.cpu.u32(FRAME + 0x1E8)


@pytest.mark.parametrize('n', [1, 2, -3, 12, -12])
def test_the_shift_goes_into_the_low_half_of_word_2(n):
    assert Block(shift_word(n), mode=0x8000).run() == 0x80000000 | n & 0xFFFF


@pytest.mark.parametrize('word', [NONE, shift_word(0), 0x200 | 3, 0])
def test_no_shift_leaves_the_block_as_built(word):
    block = Block(word, speed=0x100010, mode=0xC000)
    assert block.run() == 0xC0000000
    assert block.snaps == [] and block.cpu.u32(FRAME + 0x1D4) == 0x100010


def test_with_master_tempo_off_the_speed_is_snapped_as_master_tempo_would():
    block = Block(shift_word(2), speed=0x100010)
    block.run()
    assert block.snaps == [(0x100010, 1, FRAME + 0x1D4)]
    assert block.cpu.fpscr == 0xFFFFFFFF & ~0x00180000


def test_with_master_tempo_on_the_firmware_has_snapped_already():
    block = Block(shift_word(2), speed=0x100010, flags=MT_FLAG)
    block.run()
    assert block.snaps == []


# -- the load ----------------------------------------------------------------------

@pytest.mark.parametrize('word', [NONE, shift_word(7), shift_word(-2)])
def test_a_load_clears_the_shift(word):
    cpu = Cpu()
    cpu.write(CODE, blob('keyshiftload'))
    cpu.put32(SHIFT, word)
    hook(cpu, CODE)
    assert cpu.u32(SHIFT) == shift_word(0)


# -- the readout -------------------------------------------------------------------

MINOR = ['Cm', 'C#m', 'Dm', 'Ebm', 'Em', 'Fm', 'F#m', 'Gm', 'G#m', 'Am', 'Bbm', 'Bm']
MAJOR = ['C', 'Db', 'D', 'Eb', 'E', 'F', 'F#', 'G', 'Ab', 'A', 'Bb', 'B']
CAMELOT = ['%d%s' % (k, letter) for letter in 'AB' for k in range(1, 13)]


def shifted(key, n):
    """What the title bar should read for the track's key with a shift of n."""
    if not n:
        return key
    if key[-1] in 'AB' and key[:-1].isdigit():
        return fits('%d%s' % ((int(key[:-1]) - 1 + 7 * n) % 12 + 1, key[-1]), n)
    minor = key.endswith('m')
    note = key[:-1] if minor else key
    pc = ('C D EF G A B'.index(note[0]) + {'#': 1, 'b': -1}.get(note[1:], 0)) % 12
    return fits((MINOR if minor else MAJOR)[(pc + n) % 12], n)


def fits(key, n):
    """The shift goes after the key while the two fit the title bar's five units."""
    text = '%s%+d' % (key, n)
    return text if len(text) <= 5 else key


class TitleBar:
    """The display model's key field, the routine's state as the erased run
    leaves it, the dispatch lock, and one pad pass followed by the display
    task's tick, which sends the field when it asks and clears the request."""

    def __init__(self, key, shift=NONE, kind=0x0F):
        self.cpu = cpu = Cpu()
        cpu.write(CODE, blob('keyshiftkey'))
        cpu.write(KEY_TEXT, b'\xff' * 32)
        cpu.put32(SHIFT, shift)
        cpu.write(KEY_FIELD + 0x14, struct.pack('<H', kind))
        self.locks = []
        cpu.stubs[MODEL_LOCK] = self.lock
        cpu.stubs[MODEL_UNLOCK] = self.unlock
        self.firmware_writes(key)
        self.tick()

    def lock(self, cpu):
        self.locks.append('lock')
        cpu.r[0] = 0x5A

    def unlock(self, cpu):
        assert cpu.r[4] == 0x5A and self.locks[-1] == 'lock'
        self.locks.append('unlock')

    def firmware_writes(self, text):
        units = text.encode('utf-16-le')
        self.cpu.write(KEY_FIELD + 0x28, struct.pack('<H', len(units) // 2) + units + b'\0\0')
        self.cpu.write(KEY_FIELD + 12, bytes([self.flags | 4]))

    @property
    def flags(self):
        return self.cpu.read(KEY_FIELD + 12, 1)[0]

    def tick(self):
        hook(self.cpu, CODE)
        assert len(self.locks) % 2 == 0
        asked = bool(self.flags & 4)
        self.cpu.write(KEY_FIELD + 12, bytes([self.flags & ~4]))
        return asked

    def shift(self, n):
        self.cpu.put32(SHIFT, shift_word(n))

    @property
    def text(self):
        length = struct.unpack('<H', self.cpu.read(KEY_FIELD + 0x28, 2))[0]
        return self.cpu.read(KEY_FIELD + 0x2A, 2 * length).decode('utf-16-le')


@pytest.mark.parametrize('key', MINOR + MAJOR + CAMELOT + ['Abm', 'Dbm', 'D#m', 'A#', 'Gb', 'Cb'])
def test_the_title_bar_shows_the_shifted_key_and_the_shift(key):
    bar = TitleBar(key)
    for n in range(-12, 13):
        bar.shift(n)
        bar.tick()
        assert bar.text == shifted(key, n), (key, n)


def test_a_change_is_sent_once_and_no_shift_puts_the_tracks_key_back():
    bar = TitleBar('Em')
    assert bar.text == 'Em' and not bar.tick() and bar.locks == []
    bar.shift(2)
    assert bar.tick() and bar.text == 'F#m+2' and bar.locks == ['lock', 'unlock']
    assert not bar.tick() and len(bar.locks) == 2
    bar.shift(0)
    assert bar.tick() and bar.text == 'Em'
    assert not bar.tick() and len(bar.locks) == 4


def test_a_pass_before_the_send_keeps_the_tracks_key():
    bar = TitleBar('Em')
    bar.shift(2)
    hook(bar.cpu, CODE)
    hook(bar.cpu, CODE)
    bar.shift(3)
    bar.tick()
    assert bar.text == 'Gm+3'


def test_a_new_track_is_shown_and_then_shifted_from_its_own_key():
    bar = TitleBar('Em', shift_word(2))
    assert bar.text == 'F#m+2'
    bar.shift(0)
    bar.firmware_writes('C#m')
    bar.tick()
    assert bar.text == 'C#m'
    bar.shift(-1)
    bar.tick()
    assert bar.text == 'Cm-1'


@pytest.mark.parametrize('text', ['', '\ufffaALL\ufffb', 'H', 'Emm', 'E#x', '13A', '0A', '8C', '8', 'Stay The Night'])
def test_text_that_is_not_a_key_is_left_alone(text):
    bar = TitleBar(text, shift_word(3))
    bar.tick()
    assert bar.text == text


@pytest.mark.parametrize('word', [NONE, shift_word(0), 0x200 | 3])
def test_without_a_shift_the_key_is_the_tracks(word):
    bar = TitleBar('Bbm', word)
    assert bar.text == 'Bbm' and not bar.tick()


def test_a_field_that_is_not_the_key_is_not_touched():
    bar = TitleBar('Em', shift_word(2), kind=4)
    assert bar.text == 'Em'


# -- registry ----------------------------------------------------------------------

def test_registered_as_hooks_on_their_sites():
    for name, at in (('keyshift', 'BLOCK_COMPARE'), ('keyshiftpad', None), ('keyshiftload', 'AUDIO_REQUEST'),
                     ('keyshiftkey', None)):
        mod = patch_main.MODS[name]
        assert mod.target == 'main' and mod.hook and not mod.literal
    assert patch_main.MODS['keyshiftpad'].sig == nxs2.PAD_MODES['sig']
    readout = patch_main.MODS['keyshiftkey']
    assert readout.sig == nxs2.PAD_MODES['sig'] and readout.after == ('keyshiftpad',)


def test_the_registry_row_switches_every_part_and_is_off_by_default():
    row = mod_row('key_shift')
    assert row.env.split('+')[0] == 'CDJ_MAIN_KEYSHIFT'
    assert set(row.env.split('+')) >= {'CDJ_MAIN_KEYSHIFT', 'CDJ_MAIN_KEYSHIFTPAD', 'CDJ_MAIN_KEYSHIFTLOAD'}
    assert (row.on, row.off, row.default, row.models) == ('1', '0', 'off', 'cdj2000nxs2')


# -- the real image ----------------------------------------------------------------

@needs_main
def test_real_main_has_each_site_once_where_the_design_puts_it():
    with open(REAL_MAIN, 'rb') as f:
        img = sigpatch.Image(nxs2.MAIN_PROFILE, f.read())
    for name, at in (('keyshift', 0x082DF60C), ('keyshiftpad', 0x08442D96), ('keyshiftload', 0x0830BBF4),
                     ('keyshiftkey', 0x08442D96)):
        mod = patch_main.MODS[name]
        assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == at


@needs_main
def test_every_address_the_routines_hold_is_found_in_real_main():
    with open(REAL_MAIN, 'rb') as f:
        img = sigpatch.Image(nxs2.MAIN_PROFILE, f.read())
    for name in ('keyshift', 'keyshiftpad', 'keyshiftload', 'keyshiftkey'):
        for stock, source in patch_main.MODS[name].imports:
            assert sigpatch.import_value(img, source) == stock, (name, hex(stock))


@needs_main
def test_real_main_takes_the_mod_with_every_other_main_mod():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    names = sorted(patch_main.MODS)
    report = {}
    patch_main.patch(data, names, report=report)
    assert {state for state, _ in report.values()} == {'applied'}
