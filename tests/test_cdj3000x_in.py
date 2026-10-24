# SPDX-License-Identifier: GPL-2.0-or-later
"""The deck's state report in the CDJ-3000X layout. x3000in is hooked in the
HID IN task where it takes the next report from the sender's ring; a 0x20
report goes out with its hot cues and analog block one byte further on and the
reverse bits the right way round, as a 3000X sends it. Run here on the
interpreter, and applied to the real 1.87 MAIN."""
import os
import struct

import pytest

import patch_main
import sigpatch
from players import cdj2000nxs2 as nxs2
from test_cdj3000x_rx import RET, STACK, Sh4

CODE = 0x08140000
SLOTS = 0x0BD4B124
SLOT = 0x0BD5109C
SAVED = STACK - 0x100
BASE = 0x08000000
MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')
needs_main = pytest.mark.skipif(not os.path.exists(MAIN), reason='needs extract/main_unpacked.bin')

# State reports the deck sent Serato, the second with the browse encoder turned.
IDLE = bytes.fromhex('0020000000006000') + bytes(56)
CUES = bytes.fromhex('00200000000060') + bytes(8) + bytes([0x81]) + bytes(48)
TURNED = bytes.fromhex('00208000000060000000000000000000000000020000000000000000000000') + bytes(33)


def blob():
    with open(patch_main.blob_path('x3000in'), 'rb') as f:
        return f.read()


def send(report, length=None):
    """Run the hook the way the trampoline calls it after the ring swap: the
    report in slot 1 of the ring, and return the slot as it is sent."""
    length = len(report) if length is None else length
    cpu = Sh4()
    cpu.write(CODE, blob())
    cpu.write(SLOT, report)
    cpu.put32(SLOTS + 4, SLOT)
    cpu.write(SAVED, struct.pack('<8I', 4, 1, 2, 3, 0x2A, 5, length, SLOTS))
    cpu.r[4], cpu.r[15], cpu.pr = SAVED, STACK, RET
    for k in range(8, 15):
        cpu.r[k] = 0x80000000 + k
    cpu.run(CODE)
    assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [STACK]
    assert struct.unpack('<8I', cpu.read(SAVED, 32)) == (4, 1, 2, 3, 0x2A, 5, length, SLOTS)
    return cpu.read(SLOT, len(report))


def in_3000x_layout(report):
    out = bytearray(report[:19] + b'\0' + report[19:29] + report[30:])
    out[6] ^= 0x60
    out[15:17] = bytes([0, report[15]])
    return bytes(out)


@pytest.mark.parametrize('report', [IDLE, TURNED, CUES])
def test_a_state_report_goes_out_in_the_3000x_layout(report):
    assert send(report) == in_3000x_layout(report)


def test_the_encoder_lands_where_a_3000x_has_it():
    sent = send(TURNED)
    assert sent[19] == 0 and sent[20:22] == TURNED[19:21]


@pytest.mark.parametrize('report,length', [
    (bytes.fromhex('00f0010001003132') + bytes(56), 64),     # the handshake
    (bytes.fromhex('0021') + bytes(62), 64),                 # not a state report
    (TURNED, 29),                                            # too short to hold the block
    (TURNED + bytes(64), 128),                               # longer than a report
])
def test_any_other_report_is_sent_as_it_is(report, length):
    assert send(report, length) == report


# -- on the real image ----------------------------------------------------------------

@pytest.fixture(scope='module')
def real():
    with open(MAIN, 'rb') as f:
        return f.read()


@needs_main
def test_real_main_takes_the_hook_in_the_hid_in_task(real):
    img = sigpatch.Image(nxs2.MAIN_PROFILE, real)
    mod = patch_main.MODS['x3000in']
    assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == 0x084F70C2
    for stock, source in patch_main.MODS['x3000in'].imports:
        assert sigpatch.import_value(img, source) == stock


@needs_main
def test_an_image_that_already_carries_it_is_left_as_it_is(real):
    report = {}
    once = patch_main.patch(real, ['x3000in', 'x3000hs'])
    assert patch_main.patch(once, ['x3000in', 'x3000hs'], report=report) == once
    assert report['x3000in'][0] == 'present'
