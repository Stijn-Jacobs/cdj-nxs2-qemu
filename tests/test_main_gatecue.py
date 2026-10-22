# SPDX-License-Identifier: GPL-2.0-or-later
"""The gate_cue MAIN mod pair: gatecuepress on the hot cue recall's entry arms
the gate when a pad recalls its cue while the deck is paused, and gatecue on
the pad pass's mode switch, which runs every Player pass, sends the deck back
to that cue and pauses it once the pad is let go. Both run here on the
interpreter of test_main_phrase against a model of the Player's words and the
firmware calls they make; where the real 1.87 MAIN is extracted, both sites
and every address they import are found in it."""
import os
import struct

import pytest

import patch_main
import sigpatch
import modelfw
from players import cdj2000nxs2 as nxs2
from test_main_phrase import RET, STACK, Sh4
from helpers import mod_row

GATE_CUE = modelfw.mod_patch('gate_cue', 'cdj2000nxs2')

CODE = 0x08140000
SAVED = STACK - 0x100
PADS = 0x0B056EDC               # the panel's pad word of slot n is at +4n
PAUSED = 0x0B058C10             # 1 while PLAY has paused the deck
STATE = 0x0B058BFC              # the Player state, 7 once stopped at a cue
ARMED = 0x081FFFF8
CUE_ARG = 0x0B058C04
CLEAR_FLAG, PLAYER_STOP, CUE_COMMAND, MODE_UPDATE, LOOP_TIDY = (
    0x0844794C, 0x084330E4, 0x0842ADD2, 0x0843278A, 0x0843D712)
FP_BITS = 0x00180000            # fpscr size and precision, cleared after every call
NONE = 0xFFFFFFFF               # the erased word the gate starts from

REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')
needs_main = pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')


def blob(name):
    with open(patch_main.blob_path(name), 'rb') as f:
        return f.read()


class Deck:
    """The Player's words the two routines read, and a record of the calls."""

    def __init__(self, armed=NONE, playing=0, state=0):
        self.cpu = cpu = Sh4()
        cpu.write(CODE, blob('gatecue'))
        cpu.write(CODE + 0x400, blob('gatecuepress'))
        cpu.put32(ARMED, armed)
        cpu.put32(CUE_ARG, 0x2A)
        cpu.write(PAUSED, bytes([int(not playing)]))
        cpu.put32(STATE, state)
        self.calls = []
        for name, at in (('clear', CLEAR_FLAG), ('stop', PLAYER_STOP), ('cue', CUE_COMMAND),
                         ('modes', MODE_UPDATE), ('loop', LOOP_TIDY)):
            cpu.stubs[at] = self.stub(name)

    def stub(self, name):
        def call(cpu):
            assert cpu.fpscr & FP_BITS == 0, name
            self.calls.append((name, cpu.r[4], cpu.r[5], cpu.r[6]) if name == 'cue' else name)
            cpu.fpscr = 0xFFFFFFFF
        return call

    def pad(self, slot, down):
        self.cpu.put32(PADS + 4 * slot, int(down))

    def playing(self, on):
        self.cpu.write(PAUSED, bytes([int(not on)]))

    @property
    def armed(self):
        return self.cpu.u32(ARMED)

    def hook(self, at, saved):
        """Run a hook the way the shared trampoline calls it: r4 = the saved r0-r7."""
        cpu = self.cpu
        cpu.write(SAVED, struct.pack('<8I', *saved))
        cpu.r[4], cpu.r[15], cpu.pr = SAVED, STACK, RET
        for k in range(8, 15):
            cpu.r[k] = 0x80000000 + k
        cpu.run(at)
        assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [STACK]
        assert list(struct.unpack('<8I', cpu.read(SAVED, 32))) == saved

    def recall(self, slot):
        """The recall's entry, slot in r4."""
        self.hook(CODE + 0x400, [0, 1, 2, 3, slot, 5, 6, 7])

    def pass_(self):
        """One Player pass reaching the pad pass's mode switch."""
        self.hook(CODE, [0, 1, 2, 3, 4, 5, 6, 7])


def back_to(slot):
    return ['clear', 'stop', ('cue', slot, 1, 0x2A), 'modes', 'loop']


# -- registry ---------------------------------------------------------------------

def test_registered_as_hooks_on_the_pad_pass_and_the_recall():
    for name, site in (('gatecue', GATE_CUE.PAD_MODES), ('gatecuepress', GATE_CUE.RECALL_ENTRY)):
        mod = patch_main.MODS[name]
        assert mod.target == 'main' and mod.hook and not mod.literal
        assert dict(sig=mod.sig, start=mod.start, end=mod.end) == site
        # the trampoline returns through pr, so the caller's pr is pushed inside the span
        assert 0x4F22 in mod.sig[mod.start:mod.end]


def test_the_registry_row_enables_both_halves_and_is_off_by_default():
    row = mod_row('gate_cue')
    assert row.env.split('+') == ['CDJ_MAIN_GATECUE', 'CDJ_MAIN_GATECUEPRESS']
    assert (row.on, row.off, row.default, row.models) == ('1', '0', 'off', 'cdj2000nxs2')


# -- the press ---------------------------------------------------------------------

@pytest.mark.parametrize('slot', range(1, 9))
def test_a_pad_recall_while_paused_arms_the_gate_with_its_slot(slot):
    deck = Deck()
    deck.pad(slot, True)
    deck.recall(slot)
    assert deck.armed == slot and deck.calls == []


def test_a_pad_recall_while_stopped_at_a_cue_arms_the_gate():
    deck = Deck(playing=1, state=7)
    deck.pad(4, True)
    deck.recall(4)
    assert deck.armed == 4


def test_a_pad_recall_while_playing_disarms_the_gate():
    deck = Deck(armed=3, playing=1)
    deck.pad(2, True)
    deck.recall(2)
    assert deck.armed == 0


def test_a_recall_without_its_pad_down_leaves_the_gate_alone():
    deck = Deck(armed=3)
    deck.pad(2, False)
    deck.recall(2)
    assert deck.armed == 3


@pytest.mark.parametrize('slot', [0, 9, 0xFF])
def test_the_main_cue_and_slots_past_h_are_not_gated(slot):
    deck = Deck(armed=NONE)
    deck.pad(slot, True)
    deck.recall(slot)
    assert deck.armed == NONE


# -- the release -------------------------------------------------------------------

@pytest.mark.parametrize('armed', [NONE, 0, 9])
def test_an_unarmed_pass_calls_nothing(armed):
    deck = Deck(armed=armed)
    deck.pass_()
    assert deck.calls == [] and deck.armed == armed


def test_a_held_pad_keeps_the_gate_open():
    deck = Deck(armed=1)
    deck.pad(1, True)
    for _ in range(3):
        deck.pass_()
    assert deck.calls == [] and deck.armed == 1


@pytest.mark.parametrize('slot', range(1, 9))
def test_the_release_runs_the_cue_keys_return_with_the_pads_slot_once(slot):
    deck = Deck(armed=slot)
    deck.pad(slot, False)
    deck.pass_()
    assert deck.calls == back_to(slot)
    assert deck.armed == 0
    deck.pass_()
    assert deck.calls == back_to(slot)


def test_the_routine_leaves_fpscr_as_the_firmware_does_after_a_call():
    deck = Deck(armed=1)
    deck.pass_()
    assert deck.cpu.fpscr == 0xFFFFFFFF & ~FP_BITS


# -- press, hold, release, as the pad pass sees them ----------------------------------

def test_a_paused_hold_and_release_goes_back_to_the_cue():
    deck = Deck()
    deck.pad(1, True)
    deck.recall(1)
    deck.playing(True)                  # the recall plays the deck
    for _ in range(5):
        deck.pass_()
    deck.pad(1, False)
    deck.pass_()
    deck.pass_()
    assert deck.calls == back_to(1)


def test_a_recall_while_playing_is_left_to_play_on():
    deck = Deck(playing=1)
    deck.pad(1, True)
    deck.recall(1)
    deck.pass_()
    deck.pad(1, False)
    deck.pass_()
    assert deck.calls == []


def test_a_second_pad_while_the_gate_plays_takes_the_gate_over():
    deck = Deck()
    deck.pad(1, True)
    deck.recall(1)
    deck.playing(True)
    deck.pad(2, True)
    deck.recall(2)
    deck.pad(1, False)
    deck.pad(2, False)
    deck.pass_()
    assert deck.calls == []


# -- the real image ----------------------------------------------------------------

@needs_main
def test_real_main_has_each_site_once_where_the_design_puts_it():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    img = sigpatch.Image(nxs2.MAIN_PROFILE, data)
    for name, at in (('gatecue', 0x08442D96), ('gatecuepress', 0x08435384)):
        mod = patch_main.MODS[name]
        assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == at


@needs_main
def test_every_address_the_routines_hold_is_found_in_real_main():
    with open(REAL_MAIN, 'rb') as f:
        img = sigpatch.Image(nxs2.MAIN_PROFILE, f.read())
    for name in ('gatecue', 'gatecuepress'):
        for stock, source in patch_main.MODS[name].imports:
            assert sigpatch.import_value(img, source) == stock, (name, hex(stock))


@needs_main
def test_real_main_takes_both_halves_with_every_other_main_mod():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    names = sorted(patch_main.MODS)
    report = {}
    out = patch_main.patch(data, names, report=report)
    assert {state for state, _ in report.values()} == {'applied'}
    for at in (0x08442D96, 0x08435384):
        off = at - 0x08000000
        assert out[off:off + 12] != data[off:off + 12]
    assert patch_main.patch(out, names) == out
