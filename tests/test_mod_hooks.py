# SPDX-License-Identifier: GPL-2.0-or-later
"""Several hook mods on one firmware site: one trampoline that runs the
replaced span once and then calls each selected hook in registry order,
run through the SH-4 interpreter of test_main_oscbeat."""
import os
import struct

import pytest

import patch_main
import sigpatch
from sigpatch import Mod
from test_main_oscbeat import Sh4

SIG_OFF = 0x100
SPAN = SIG_OFF + 4
CAVE = (0x400, 0x1800)
COUNTER = 0x3000
FRAME = 0xF000

# A made-up span: bump the word at COUNTER (through a pc-relative literal, so
# the move has to re-point it), then a bf that is taken to the span's end,
# skipping the mov #9,r7 before it.
SPAN_OPS = [0xD220,             # mov.l @(0x80,pc),r2
            0x6322,             # mov.l @r2,r3
            0x7301,             # add #1,r3
            0x2232,             # mov.l r3,@r2
            0xE602,             # mov #2,r6
            0x4610,             # dt r6          (T = 0)
            0x8B00,             # bf  -> the span's end
            0xE709]             # mov #9,r7      (skipped)
SITE = dict(sig=[0x0009, 0x0009] + SPAN_OPS + [0x0009], start=2, end=2 + len(SPAN_OPS))

HOOKS = {
    # block r0 = (r0 + 1) * 4
    'a': [0x6142, 0x7101, 0x4108, 0x2412, 0x000B, 0x0009],
    # block r0 += 3, block r1 = r5 (the site's r15)
    'b': [0x6142, 0x7103, 0x2412, 0x1451, 0x000B, 0x0009],
}


def hook_mod(**site):
    return Mod(what='hook', target='main', fw_versions=('1.87',), args=[],
               literal=False, hook=True, **site)


MODS = {'a': hook_mod(**SITE), 'b': hook_mod(**SITE)}


@pytest.fixture
def blobs(tmp_path):
    for name, ops in HOOKS.items():
        (tmp_path / name).write_bytes(struct.pack('<%dH' % len(ops), *ops))
    return lambda name: str(tmp_path / name)


def build_image():
    d = bytearray(0x2000)
    for k, op in enumerate(SITE['sig']):
        struct.pack_into('<H', d, SIG_OFF + 2 * k, op)
    struct.pack_into('<I', d, (SPAN & ~3) + 4 + 0x20 * 4, COUNTER)
    d[CAVE[0]:CAVE[1]] = b'\xff' * (CAVE[1] - CAVE[0])
    profile = sigpatch.ImageProfile(flash_base=0, loadtab_off=0, loadtab_n=0,
                                    ram_base=CAVE[0], ram_end=CAVE[1], byteorder='<')
    return profile, bytes(d)


def run(image):
    cpu = Sh4(image, 0)
    cpu.r[0], cpu.r[7], cpu.r[15] = 0, 0x77, FRAME
    cpu.r[8:15] = [0xA0000000 + k for k in range(8, 15)]
    cpu.run(SPAN, stop=SPAN + 2 * len(SPAN_OPS))
    return cpu


def test_two_hooks_share_one_trampoline_and_the_span_runs_once(blobs):
    profile, data = build_image()
    out = sigpatch.patch(profile, blobs, data, MODS, ['b', 'a'])
    tramp = struct.unpack_from('<I', out, SPAN + 8)[0]
    offsets, _ = sigpatch.layout(blobs, MODS)
    assert tramp == CAVE[0] + offsets[sigpatch.site_key(MODS['a'])]

    cpu = run(out)
    assert cpu.u32(COUNTER) == 1
    assert cpu.r[0] == (0 + 1) * 4 + 3          # a, then b
    assert cpu.r[1] == FRAME
    assert cpu.r[7] == 0x77                     # the span's own branch was kept
    assert cpu.r[15] == FRAME
    assert cpu.r[8:15] == [0xA0000000 + k for k in range(8, 15)]


def test_one_hook_alone_runs_on_the_same_trampoline(blobs):
    profile, data = build_image()
    alone = sigpatch.patch(profile, blobs, data, MODS, ['a'])
    both = sigpatch.patch(profile, blobs, data, MODS, ['a', 'b'])
    assert alone[SPAN:SPAN + 16] == both[SPAN:SPAN + 16]
    offsets, _ = sigpatch.layout(blobs, MODS)
    at = CAVE[0] + offsets['a']
    assert alone[at:at + 12] == both[at:at + 12]

    cpu = run(alone)
    assert cpu.u32(COUNTER) == 1 and cpu.r[0] == 4


def test_selection_order_does_not_change_the_image(blobs):
    profile, data = build_image()
    assert (sigpatch.patch(profile, blobs, data, MODS, ['a', 'b'])
            == sigpatch.patch(profile, blobs, data, MODS, ['b', 'a']))


def test_a_hook_and_an_owner_mod_on_one_span_are_refused(blobs):
    profile, data = build_image()
    mods = {'a': MODS['a'], 'b': MODS['b']._replace(hook=False)}
    with pytest.raises(SystemExit, match='overlap'):
        sigpatch.patch(profile, blobs, data, mods, ['a', 'b'])


@pytest.mark.parametrize('ops, why', [
    ([0xE000, 0x8B05, 0x0009], 'leaves the span'),       # bf past the end
    ([0xB010, 0x0009, 0x0009], 'cannot be moved'),       # bsr
    ([0xC701, 0x0009, 0x0009], 'cannot be moved'),       # mova
    ([0x0009, 0x0009, 0x410B], 'delayed branch'),        # jsr without its slot
])
def test_spans_that_cannot_run_elsewhere_are_refused(ops, why):
    with pytest.raises(SystemExit, match=why):
        sigpatch.trampoline(ops, [], [0x1000], 0x2000, 0x400, byteorder='<')


REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')


@pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
def test_real_main_takes_every_main_mod_at_once_deterministically():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    names = sorted(patch_main.MODS)
    out = patch_main.patch(data, names)
    assert out == patch_main.patch(data, list(reversed(names)))
    alone = patch_main.patch(data, ['oscbeat'])
    mod = patch_main.MODS['oscbeat']
    span = sigpatch.find_sig(sigpatch.Image(patch_main.MAIN_PROFILE, data), mod) + 2 * mod.start
    assert alone[span:span + 2 * (mod.end - mod.start)] == out[span:span + 2 * (mod.end - mod.start)]
