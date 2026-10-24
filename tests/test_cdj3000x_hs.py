# SPDX-License-Identifier: GPL-2.0-or-later
"""The CDJ-3000X answer to Serato's HID handshake. x3000hs is hooked on the
entry of the host-report router; on Serato's stage 0x30 it sends the stage
0x31 identity report a CDJ-3000X v1.40 sends and leaves the handshake in the
state a verified stage 0x14 does. It is run here on the interpreter with the
firmware calls stubbed, and applied to the real 1.87 MAIN."""
import os
import struct

import pytest

import patch_main
import sigpatch
from players import cdj2000nxs2 as nxs2
from test_main_phrase import RET, STACK, Sh4 as BaseSh4

CODE = 0x08140000
REPORT = 0x0BD50FDC
STATE, AUTH = 0x0BD4B148, 0x0BD4B144
MEMSET, SEND, MAY_WAIT, LOCK, UNLOCK = 0x08533D50, 0x084F4ECA, 0x08518068, 0x0851A580, 0x0851A7C4
LOCK_ID = 0x10DE1BE4
DEVICE = 0x10DDFBA0
SERIAL = 0xA07FE000
BASE = 0x08000000
MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')
needs_main = pytest.mark.skipif(not os.path.exists(MAIN), reason='needs extract/main_unpacked.bin')

SERATO_30 = bytes.fromhex('00f0010001003005010331') + bytes(53)
# The reply Serato DJ Pro took from a deck with this identity and serial.
REPLY_31 = bytes.fromhex(
    '00f001000100313201042b73020c416c70686154686574610304004e040a43444a3330303058'
    '050e50444a303030303030315858060401280000000000000000')


class Sh4(BaseSh4):
    """The phrase test's interpreter plus bf/s."""

    def step_op(self, op, pc):
        if op & 0xFF00 == 0x8F00:
            disp = (op & 0xFF) - 0x100 if op & 0x80 else op & 0xFF
            taken = not self.t
            self.delay(pc)
            return pc + 4 + disp * 2 if taken else pc + 4
        return super().step_op(op, pc)


def blob():
    with open(patch_main.blob_path('x3000hs'), 'rb') as f:
        return f.read()


def mod():
    return patch_main.MODS['x3000hs']


# The report router hands the report in r4; the shared hook trampoline saves
# r0-r7 on the stack and passes a hook r4 = that saved block, so the routine
# reads the report pointer at offset 16.
SAVED = STACK - 0x100


class Deck:
    def __init__(self, auth=0, may_wait=0, serial=b'PDJ0000001XX', pid=0x004E):
        self.cpu = cpu = Sh4()
        self.sent, self.calls = [], []
        cpu.write(CODE, blob())
        cpu.put32(AUTH, auth)
        cpu.put32(STATE, 2)
        cpu.put32(LOCK_ID, 0x2A)
        cpu.write(DEVICE, bytes.fromhex('1201000200000040732b') + struct.pack('<H', pid) + bytes(6))
        cpu.write(SERIAL, serial + b'\xff' * 4)
        cpu.stubs.update({
            MEMSET: lambda c: c.write(c.r[4], bytes([c.r[5] & 0xFF]) * c.r[6]),
            SEND: lambda c: self.sent.append(c.read(c.r[4], c.r[5])),
            MAY_WAIT: lambda c: self.answer(c, 'may_wait', may_wait),
            LOCK: lambda c: self.answer(c, ('lock', c.r[4], c.r[5] & 0xFFFFFFFF), 0),
            UNLOCK: lambda c: self.answer(c, ('unlock', c.r[4]), 0),
        })

    def answer(self, cpu, call, value):
        self.calls.append(call)
        cpu.r[0] = value

    def report(self, data):
        cpu = self.cpu
        cpu.write(REPORT, data)
        cpu.put32(SAVED + 16, REPORT)
        cpu.r[4], cpu.r[15] = SAVED, STACK
        cpu.pr = RET
        for k in range(8, 15):
            cpu.r[k] = 0x80000000 + k
        cpu.run(CODE)
        assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [STACK]
        return self


def test_serato_stage_30_gets_the_3000x_reply_serato_took():
    deck = Deck().report(SERATO_30)
    assert deck.sent == [REPLY_31]


def test_after_the_reply_the_deck_is_authenticated_as_a_verified_0x14_leaves_it():
    deck = Deck().report(SERATO_30)
    assert deck.cpu.u32(STATE) == 6 and deck.cpu.u32(AUTH) == 1
    assert deck.calls == ['may_wait', ('lock', 0x2A, 0xFFFFFFFF), ('unlock', 0x2A)]


def test_a_caller_that_may_not_wait_sets_the_state_without_the_lock():
    deck = Deck(may_wait=1).report(SERATO_30)
    assert deck.calls == ['may_wait']
    assert deck.cpu.u32(STATE) == 6 and deck.cpu.u32(AUTH) == 1


def test_the_ids_are_the_ones_the_deck_serves():
    reply = Deck(pid=0x0051).report(SERATO_30).sent[0]
    assert reply[8:12] == bytes.fromhex('01042b73') and reply[24:28] == bytes.fromhex('03040051')


@pytest.mark.parametrize('serial', [b'ABCD1234', b'X', b'0123456789ABCDEFGHIJ'])
def test_the_serial_is_the_flash_one_up_to_sixteen_bytes(serial):
    reply = Deck(serial=serial).report(SERATO_30).sent[0]
    kept = serial[:16]
    assert reply[7] == 38 + len(kept)
    assert reply[38:40] == bytes([5, 2 + len(kept)]) and reply[40:40 + len(kept)] == kept
    assert reply[40 + len(kept):44 + len(kept)] == bytes.fromhex('06040128')
    assert reply[44 + len(kept):] == bytes(20 - len(kept))


def test_once_authenticated_a_later_stage_30_is_left_alone():
    deck = Deck(auth=1).report(SERATO_30)
    assert deck.sent == [] and deck.calls == [] and deck.cpu.u32(AUTH) == 1


@pytest.mark.parametrize('report', [
    bytes.fromhex('00f0010001001227010853657261746f') + bytes(48),     # the NXS2 host's stage 0x12
    bytes.fromhex('00f0020001003005010331') + bytes(53),               # a later fragment
    bytes.fromhex('00f0010002003005010331') + bytes(53),               # the first of two
    bytes.fromhex('0021010001003005010331') + bytes(53),               # not a handshake report
])
def test_any_other_report_goes_to_the_handler_untouched(report):
    deck = Deck().report(report)
    assert deck.sent == [] and deck.calls == [] and deck.cpu.u32(AUTH) == 0


# -- on the real image ----------------------------------------------------------------

@pytest.fixture(scope='module')
def real():
    with open(MAIN, 'rb') as f:
        return f.read()


@needs_main
def test_every_firmware_address_the_routine_holds_is_found_by_its_own_instructions(real):
    img = sigpatch.Image(nxs2.MAIN_PROFILE, real)
    for stock, anchor in mod().imports:
        assert sigpatch.anchor_value(img, anchor) == stock


@needs_main
def test_real_main_takes_the_hook_on_the_report_router_entry(real):
    out = patch_main.patch(real, ['x3000hs'])
    img = sigpatch.Image(nxs2.MAIN_PROFILE, real)
    site = sigpatch.find_sig(img, mod())
    assert img.addr_of(site) == 0x084F9504
    cave = sigpatch.largest_erased_run(img)
    changed = [i for i in range(len(real)) if real[i] != out[i]]
    assert all(site <= i < site + 12 or cave.start() <= i < cave.end() for i in changed)
    assert out.count(blob()) == 1


@needs_main
def test_an_image_that_already_carries_it_is_left_as_it_is(real):
    report = {}
    once = patch_main.patch(real, ['x3000hs'])
    assert patch_main.patch(once, ['x3000hs'], report=report) == once
    assert report['x3000hs'][0] == 'present'


@needs_main
def test_it_goes_in_with_the_other_main_mods(real):
    others = ['osc', 'oscbeat', 'tcnet', 'usbmidi']
    both = patch_main.patch(real, others + ['x3000hs'])
    assert patch_main.patch(real, ['x3000hs'] + others[::-1]) == both
