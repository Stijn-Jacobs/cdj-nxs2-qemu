# SPDX-License-Identifier: GPL-2.0-or-later
"""beatjumplong: the Player's beat jump table gives the 2 and 4 beat slots the
beat counts of the 32 and 64 beat ones, and nothing else in MAIN changes."""
import os
import struct

import pytest

import patch_main
from helpers import path

REAL_MAIN = path('..', 'extract', 'main_unpacked.bin')
needs_main = pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')

BASE = 0x08000000
# The (beats, reverse) pair of each beat jump slot 0..13.
AMOUNTS = 0x080EA3D4


def amounts(image):
    words = struct.unpack_from('<28I', image, AMOUNTS - BASE)
    return list(zip(words[0::2], words[1::2]))


@pytest.fixture(scope='module')
def stock():
    with open(REAL_MAIN, 'rb') as f:
        return f.read()


@needs_main
def test_the_2_and_4_beat_slots_jump_32_and_64_beats(stock):
    report = {}
    out = patch_main.patch(stock, ['beatjumplong'], report=report)
    assert report['beatjumplong'] == ('applied', '')
    assert amounts(stock)[2:6] == [(2, 1), (2, 0), (4, 1), (4, 0)]
    assert amounts(out) == amounts(stock)[:2] + [(32, 1), (32, 0), (64, 1), (64, 0)] + amounts(stock)[6:]


@needs_main
def test_only_the_four_beat_counts_change(stock):
    out = patch_main.patch(stock, ['beatjumplong'])
    changed = [BASE + k for k in range(len(stock)) if stock[k] != out[k]]
    assert changed == [AMOUNTS + 8 * slot for slot in range(2, 6)]


@needs_main
def test_a_patched_image_is_recognised(stock):
    once = patch_main.patch(stock, ['beatjumplong'])
    report = {}
    assert patch_main.patch(once, ['beatjumplong'], report=report) == once
    assert report['beatjumplong'] == ('present', '')
