# SPDX-License-Identifier: GPL-2.0-or-later
"""emulator/mods/dsppatch.py: patching the DSP program a player's MAIN carries
as data, and the checks validate.py makes of it.

The synthetic tests build a tiny MAIN with a small DSP program in it. The ones
on the XDJ-700 skip when its update is not present, like the rest of the
suite."""
import os
import struct
import sys

import pytest

import dsppatch
import modelfw
import patch_main
import patch_update
import sigpatch
import validate
from helpers import path

REAL_XDJ700 = path("..", "fw", "crossmodel", "XDJ700_v115", "XDJ700.UPD")
needs_xdj700 = pytest.mark.skipif(not os.path.exists(REAL_XDJ700), reason="XDJ700.UPD 1.15 not present")

LOAD = 0xC0000000
HEAD = tuple(0x11110000 + k for k in range(8))
PROFILE = dsppatch.DspProfile(load=LOAD, length=0x4000, head=HEAD, free=(LOAD + 0x1000, LOAD + 0x3000))
START = 0x300           # where the program sits in the synthetic MAIN
EDIT = LOAD + 0x100     # a word the mods below edit
CODE = LOAD + 0x1400    # inside the free run, past its ends


def program():
    """A DSP program of 0x4000 bytes: words that decode as nothing a scan looks
    for, the head, and an erased run from 0x1000 to 0x3000."""
    words = [0x00000000] * 0x1000
    words[:8] = HEAD
    words[0x40] = 0xCAFE0001
    words[0x400:0xC00] = [0xFFFFFFFF] * 0x800
    return bytearray(struct.pack('<4096I', *words))


def main_image(blob=None):
    return bytearray(b'\x55' * START) + (blob or program()) + bytearray(b'\x66' * 0x100)


def word(image, addr):
    return struct.unpack_from('<I', image, START + addr - LOAD)[0]


def put(blob, addr, *values):
    struct.pack_into('<%dI' % len(values), blob, addr - LOAD, *values)


def mod(edits=((EDIT, 0xCAFE0001, 0xCAFE0002),), code=((CODE, (1, 2, 3)),)):
    return dsppatch.DspMod(what='dsp test mod', target='dsp', fw_versions=('9.99',), edits=edits, code=code)


def run(data, mods, names=None, spans=None):
    report = {}
    out = dsppatch.patch(PROFILE, bytes(data), mods, names or sorted(mods), report=report, spans=spans)
    return out, report


# -- finding the program ------------------------------------------------------------

def test_the_program_is_found_by_its_first_words():
    assert dsppatch.find_blob(bytes(main_image()), PROFILE) == START


def test_a_program_found_twice_or_not_at_all_is_refused():
    with pytest.raises(sigpatch.NotApplied):
        dsppatch.find_blob(bytes(main_image()) + program(), PROFILE)
    with pytest.raises(sigpatch.NotApplied):
        dsppatch.find_blob(b'\x00' * 0x5000, PROFILE)


# -- applying ------------------------------------------------------------------------

def test_the_edits_and_the_code_go_in_and_are_declared():
    data, spans = main_image(), {}
    out, report = run(data, {'t': mod()}, spans=spans)
    assert report == {'t': ('applied', '')}
    assert word(out, EDIT) == 0xCAFE0002
    assert [word(out, CODE + 4 * k) for k in range(3)] == [1, 2, 3]
    assert spans == {'t': [(START + 0x100, 4, 'dsp'), (START + 0x1400, 12, 'dspfree')]}
    changed = [i for i in range(len(data)) if data[i] != out[i]]
    assert all(START + 0x100 <= i < START + 0x104 or START + 0x1400 <= i < START + 0x140C for i in changed)


def test_a_patched_image_is_left_as_it_is():
    once, _ = run(main_image(), {'t': mod()})
    twice, report = run(once, {'t': mod()})
    assert twice == once
    assert report == {'t': ('present', '')}


def test_a_mod_is_added_to_an_image_that_has_another():
    other = mod(edits=((LOAD + 0x104, 0, 7),), code=((CODE + 0x100, (9,)),))
    once, _ = run(main_image(), {'a': mod()})
    both, report = run(once, {'a': mod(), 'b': other})
    assert report == {'a': ('present', ''), 'b': ('applied', '')}
    assert word(both, LOAD + 0x104) == 7 and word(both, CODE + 0x100) == 9 and word(both, EDIT) == 0xCAFE0002


def test_a_word_that_is_neither_stock_nor_new_refuses_the_mod():
    data = main_image()
    struct.pack_into('<I', data, START + 0x100, 0x12345678)
    out, report = run(data, {'t': mod()})
    assert out == bytes(data)
    assert report['t'][0] == 'skipped' and '0x12345678' in report['t'][1]


def test_a_half_applied_mod_is_refused():
    once, _ = run(main_image(), {'t': mod()})
    half = bytearray(once)
    struct.pack_into('<3I', half, START + 0x1400, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF)
    out, report = run(half, {'t': mod()})
    assert out == bytes(half)
    assert report['t'] == ('skipped', 'it is half applied')


def test_code_outside_the_free_run_or_at_its_ends_is_refused():
    for addr in (LOAD + 0x800, PROFILE.free[0] + 0x100, PROFILE.free[1] - 0x100, PROFILE.free[1] + 0x100):
        out, report = run(main_image(), {'t': mod(code=((addr, (1, 2)),))})
        assert report['t'][0] == 'skipped' and 'free run' in report['t'][1]


def test_code_over_bytes_that_are_not_erased_is_refused():
    data = main_image()
    struct.pack_into('<I', data, START + 0x1404, 5)
    out, report = run(data, {'t': mod()})
    assert report['t'][0] == 'skipped' and 'taken' in report['t'][1]


def test_an_edit_outside_the_program_is_refused():
    out, report = run(main_image(), {'t': mod(edits=((LOAD + 0x4000, 0, 1),))})
    assert report['t'][0] == 'skipped' and 'outside' in report['t'][1]


def test_two_mods_that_write_the_same_word_do_not_both_go_in():
    clash = mod(edits=((EDIT, 0xCAFE0001, 0xCAFE0003),), code=())
    out, report = run(main_image(), {'a': mod(), 'b': clash})
    assert report['a'][0] == 'applied' and report['b'][0] == 'skipped'
    assert word(out, EDIT) == 0xCAFE0002


def test_code_the_program_points_at_is_not_placed_there():
    blob = program()
    put(blob, LOAD + 0x200, CODE + 4)       # a pointer table entry
    out, report = run(main_image(blob), {'t': mod()})
    assert report['t'][0] == 'skipped' and 'points into' in report['t'][1]


# -- what points into a range --------------------------------------------------------

def branch(pc, target):
    return ((target - (pc & ~31)) // 4 & 0x1FFFFF) << 7 | 0x10


def mvk(dst, const, high=False, side=0):
    return const << 7 | dst << 23 | side << 1 | (0x68 if high else 0x28)


def refs(blob, lo=CODE, hi=CODE + 16):
    return dsppatch.references(bytes(blob), LOAD, lo, hi)


def test_a_branch_a_pointer_and_a_pair_of_constants_into_the_range_are_found():
    blob = program()
    put(blob, LOAD + 0x200, branch(LOAD + 0x200, CODE + 8))
    put(blob, LOAD + 0x300, CODE + 4)
    put(blob, LOAD + 0x500, mvk(3, CODE & 0xFFFF), mvk(3, CODE >> 16, high=True))
    assert refs(blob) == [LOAD + 0x200, LOAD + 0x300, LOAD + 0x504]


def test_a_branch_out_of_the_range_or_a_pair_for_another_register_is_no_hit():
    blob = program()
    put(blob, LOAD + 0x200, branch(LOAD + 0x200, CODE + 16))
    put(blob, LOAD + 0x500, mvk(3, CODE & 0xFFFF), mvk(4, CODE >> 16, high=True))
    assert refs(blob) == []


def test_half_words_of_a_compact_word_are_not_read_as_a_branch():
    blob = program()
    put(blob, LOAD + 0x200, branch(LOAD + 0x200, CODE))
    assert refs(blob) == [LOAD + 0x200]
    put(blob, LOAD + 0x21C, 0xE0000000 | 1 << 21)          # the packet's header: word 0 is compact
    assert refs(blob) == []


# -- the XDJ-700 -----------------------------------------------------------------------

def test_the_xdj700_program_is_where_the_notes_put_it(xdj700_main):
    profile = modelfw.dsp_profile('xdj700')
    assert dsppatch.find_blob(xdj700_main, profile) == 0xE770


def test_the_xdj700_free_run_is_erased_and_the_program_does_not_point_into_it(xdj700_main):
    profile = modelfw.dsp_profile('xdj700')
    start = dsppatch.find_blob(xdj700_main, profile)
    blob = xdj700_main[start:start + profile.length]
    run = blob[profile.free[0] - profile.load:profile.free[1] - profile.load]
    assert set(run) == {0xFF}
    lo, hi = profile.free[0] + dsppatch.EDGE, profile.free[1] - dsppatch.EDGE
    assert dsppatch.references(blob, profile.load, lo, hi) == []


def test_the_xdj700_scan_finds_the_nine_calls_of_the_slot_flag_setter(xdj700_main):
    profile = modelfw.dsp_profile('xdj700')
    start = dsppatch.find_blob(xdj700_main, profile)
    blob = xdj700_main[start:start + profile.length]
    assert dsppatch.references(blob, profile.load, 0xC003B5AC, 0xC003B5B0) == [
        0xC0032CD4, 0xC0034C80, 0xC0034D60, 0xC0034DA0, 0xC0034DD4, 0xC0034F2C, 0xC0035000, 0xC003750C, 0xC00377A8]


def run_on(image, profile, mods):
    report = {}
    return dsppatch.patch(profile, image, mods, sorted(mods), report=report), report


def real_mod(profile):
    """The record writer's early return for ids above 8 and a few words of
    code, as a mod of the XDJ-700 would carry them."""
    return dsppatch.DspMod(what='dsp test mod', target='dsp', fw_versions=('1.15',),
                           edits=((0xC003A850, 0x00292A58, 0x00290BD8),),
                           code=((profile.free[0] + 0x1000, (0x00000000, 0x00008000)),))


def test_the_xdj700_program_takes_a_mod_once(xdj700_main):
    profile = modelfw.dsp_profile('xdj700')
    mods = {'dsptest': real_mod(profile)}
    out, report = run_on(xdj700_main, profile, mods)
    assert report == {'dsptest': ('applied', '')} and out != xdj700_main
    assert len(out) == len(xdj700_main)
    again, report = run_on(out, profile, mods)
    assert again == out and report == {'dsptest': ('present', '')}


@needs_xdj700
def test_a_patched_xdj700_update_passes_the_validator(tmp_path, monkeypatch, capsys):
    profile = modelfw.dsp_profile('xdj700')
    monkeypatch.setitem(patch_main.DSP_SETS, 'xdj700',
                        sigpatch.PatchSet(profile, {'dsptest': real_mod(profile)}))
    out = tmp_path / 'out.UPD'
    monkeypatch.setattr(sys, 'argv', ['patch_update.py', '--model', 'xdj700', REAL_XDJ700, str(out), 'dsptest'])
    patch_update.main()
    text = capsys.readouterr().out
    assert 'dsp: 1 words of the DSP program edited, 8 bytes of code placed' in text
    assert 'FAIL' not in text and 'all checks passed' in text


def test_the_validator_fails_code_placed_off_the_erased_run_or_where_the_program_points():
    blob = program()
    put(blob, LOAD + 0x200, CODE + 0x40)
    image = bytes(main_image(blob))
    for addr, why in ((LOAD + 0x100, 'not erased'), (PROFILE.free[0] + 0x40, 'bytes from its ends'),
                      (CODE + 0x40, 'points into')):
        lo = START + addr - LOAD
        rep = validate.Report()
        validate.check_dsp_writes(image, [('m', 'dspfree', lo, lo + 8)], PROFILE, START, rep)
        assert rep.failed and why in rep.text()
    lo = START + CODE - LOAD
    rep = validate.Report()
    validate.check_dsp_writes(image, [('m', 'dspfree', lo, lo + 8)], PROFILE, START, rep)
    assert not rep.failed


def patch_main_profile():
    return sigpatch.ImageProfile(flash_base=0x08000000, loadtab_off=0, loadtab_n=0,
                                 ram_base=None, ram_end=None, byteorder='<')


def test_a_change_with_no_declaration_is_told_apart_by_where_it_is():
    stock = bytes(main_image())
    flat = validate.flat_spans(None, [(START + 0x100, START + 0x102), (START + 0x1400, START + 0x1408), (0x10, 0x12)],
                               stock, None, None, (START, START + 0x4000))
    assert [kind for _, kind, _, _ in flat] == ['dsp', 'dspfree', 'site']


def test_a_change_to_the_program_nobody_declared_fails_the_validator():
    stock = bytes(main_image())
    out = bytearray(stock)
    struct.pack_into('<I', out, START + 0x100, 5)
    rep = validate.Report()
    validate.check_writes('main', stock, bytes(out), patch_main_profile(), {'m': []}, rep, dsp=(START, START + 0x4000))
    assert rep.failed and 'no mod declared' in rep.text()
