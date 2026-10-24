# SPDX-License-Identifier: GPL-2.0-or-later
"""x3000keepwave: the display's detail waveform (0x37) handler no longer zeroes
the renderer's point count on the first and the last chunk of a publish."""
import os

import pytest

import patch_gui
from helpers import path

REAL_GUI = path('..', 'extract', 'gui_unpacked.bin')
needs_gui = pytest.mark.skipif(not os.path.exists(REAL_GUI), reason='extract/gui_unpacked.bin not present')

# Image offsets of the two count stores, 0x0E502082 and 0x0E5020E4.
COUNT_STORES = (0x2182, 0x21E4)


@needs_gui
def test_only_the_two_count_stores_become_nops():
    with open(REAL_GUI, 'rb') as f:
        stock = f.read()
    report = {}
    out = patch_gui.patch(stock, ['x3000keepwave'], 'cdj2000nxs2', report)
    assert report['x3000keepwave'] == ('applied', '')
    changed = [k for k in range(len(stock)) if stock[k] != out[k]]
    assert changed == [at + k for at in COUNT_STORES for k in (0, 1)]
    assert all(out[at:at + 2] == b'\x00\x09' for at in COUNT_STORES)


@needs_gui
def test_a_patched_image_is_recognised():
    with open(REAL_GUI, 'rb') as f:
        once = patch_gui.patch(f.read(), ['x3000keepwave'], 'cdj2000nxs2')
    report = {}
    assert patch_gui.patch(once, ['x3000keepwave'], 'cdj2000nxs2', report) == once
    assert report['x3000keepwave'] == ('present', '')
