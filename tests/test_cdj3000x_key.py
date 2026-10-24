# SPDX-License-Identifier: GPL-2.0-or-later
"""The key (0x27) Serato sends a CDJ-3000X. x3000key, on the host-report
router's entry, hands it to the report framework as the musical key message.
Run here on the interpreter with the keys of two recorded sessions; where the
real 1.87 MAIN is extracted, the firmware's own framework and key parser run on
what it produces."""
import struct

import pytest

import patch_main
import sigpatch
import validate
from players import cdj2000nxs2 as nxs2
from test_cdj3000x_length import TRACK_INFO, MainSh4, firmware_cpu, main_image
from test_cdj3000x_rx import RET, STACK, blob, needs_main
from test_cdj3000x_wave import CODE, FRAMEWORK, AUTH, SAVED, STOCK_BUFFER

KEY_ID_PARSER = 0x084FCB40
REAL_CODE = 0x0A200000

KEYS = [r for r in TRACK_INFO if r[1] == 0x27]

# Camelot 10 to 12 as the note names the firmware's parser takes
NOTE_NAMES = {'10A': 'Bm', '10B': 'D', '11A': 'F#m', '11B': 'A', '12A': 'C#m', '12B': 'E'}


def key_text(report):
    """The key as the text of a 0x27 report: UTF-16LE from byte 6, ended by a
    unit whose low byte is zero."""
    text = ''
    for k in range(6, len(report), 2):
        if report[k] == 0:
            break
        text += chr(report[k])
    return text


def camelot_id(text):
    """The key number the firmware gives a Camelot code: 2n - 1 for A, 2n for B."""
    if not text:
        return 0
    return 2 * int(text[:-1]) - (1 if text[-1] == 'A' else 0)


class KeyDeck:
    def __init__(self, cpu=None, code=CODE, authenticated=True):
        self.injected = []
        self.cpu = cpu = cpu or MainSh4()
        self.code = code
        cpu.put32(AUTH, int(authenticated))
        cpu.write(code, blob('x3000key'))

    def feed(self, report, count=64):
        """The router is entered with the 64-byte view of the report in r4 and its length in r5."""
        cpu = self.cpu
        cpu.write(STOCK_BUFFER, report[:64])
        saved = [0, 1, 2, 3, STOCK_BUFFER, count & 0xFFFFFFFF, 6, 7]
        cpu.write(SAVED, struct.pack('<8I', *saved))
        cpu.r[4], cpu.r[15], cpu.pr = SAVED, STACK, RET
        for k in range(8, 15):
            cpu.r[k] = 0x80000000 + k
        cpu.run(self.code)
        assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [STACK]
        assert list(struct.unpack('<8I', cpu.read(SAVED, 32))) == saved
        return self


def stub_framework(deck):
    deck.cpu.stubs[FRAMEWORK] = lambda c: deck.injected.append(c.read(c.r[4], c.r[5]))
    return deck


def key_message(name):
    return bytes([0, 0x41]) + name.encode().ljust(5, b'\0') + bytes(17)


def test_the_key_reports_are_in_the_recording():
    assert {key_text(r) for r in KEYS} == {'', '1A', '6A', '11A'}


@pytest.mark.parametrize('report', KEYS, ids=key_text)
def test_a_key_reaches_the_framework_as_the_musical_key_message(report):
    deck = stub_framework(KeyDeck()).feed(report)
    text = key_text(report)
    assert deck.injected == [key_message(NOTE_NAMES.get(text, text))]


@needs_main
@pytest.mark.parametrize('report', KEYS, ids=key_text)
def test_the_firmware_parser_gives_the_key_number_of_the_camelot_code(report):
    cpu = firmware_cpu()
    KeyDeck(cpu, code=REAL_CODE).feed(report)
    cpu.r[15], cpu.pr = STACK - 0x400, RET
    cpu.run(KEY_ID_PARSER)
    assert cpu.r[0] == camelot_id(key_text(report))


@needs_main
@pytest.mark.parametrize('code', ['%d%s' % (n, letter) for n in range(1, 13) for letter in 'AB'])
def test_every_camelot_code_gives_its_key_number(code):
    cpu = firmware_cpu()
    report = struct.pack('<BBHH', 0, 0x27, 1, 1) + code.encode('utf-16-le')
    KeyDeck(cpu, code=REAL_CODE).feed(report.ljust(1024, b'\0'))
    cpu.r[15], cpu.pr = STACK - 0x400, RET
    cpu.run(KEY_ID_PARSER)
    assert cpu.r[0] == camelot_id(code)


def report_27(text, index=1, total=1):
    return (struct.pack('<BBHH', 0, 0x27, index, total) + text.encode('utf-16-le')).ljust(1024, b'\0')


def test_a_note_name_is_passed_as_it_is():
    deck = stub_framework(KeyDeck()).feed(report_27('F#m'))
    assert deck.injected == [key_message('F#m')]


def test_a_long_text_keeps_its_first_five_characters():
    deck = stub_framework(KeyDeck()).feed(report_27('ABCDEFG'))
    assert deck.injected == [key_message('ABCDE')]


@pytest.mark.parametrize('text', ['1♯', 'F♯m', 'Ⅰ'])
def test_a_character_that_is_not_ascii_leaves_the_key_empty(text):
    deck = stub_framework(KeyDeck()).feed(report_27(text))
    assert deck.injected == [key_message('')]


@pytest.mark.parametrize('text', ['13A', '10C', '1A0', '20A'])
def test_a_text_that_is_not_a_camelot_code_above_nine_is_passed_as_it_is(text):
    deck = stub_framework(KeyDeck()).feed(report_27(text))
    assert deck.injected == [key_message(text)]


@pytest.mark.parametrize('report', [report_27('8A', index=2), report_27('8A', total=2),
                                    bytes([0, 0x2A]) + report_27('8A')[2:]])
def test_only_a_whole_key_report_is_taken(report):
    assert stub_framework(KeyDeck()).feed(report).injected == []


def test_nothing_is_taken_before_the_deck_is_authenticated():
    assert stub_framework(KeyDeck(authenticated=False)).feed(report_27('8A')).injected == []


@pytest.mark.parametrize('count', [0, 11])
def test_a_short_report_is_left_alone(count):
    assert stub_framework(KeyDeck()).feed(report_27('8A'), count=count).injected == []


NAMES = ['x3000hs', 'x3000key', 'x3000out', 'x3000rxbuf', 'x3000rxview']


def test_the_hook_runs_after_the_handshake_that_shares_its_site():
    assert sorted(patch_main.MODS).index('x3000key') > sorted(patch_main.MODS).index('x3000hs')


@pytest.fixture(scope='module')
def real():
    return main_image()


@needs_main
def test_every_firmware_address_the_routine_holds_is_found(real):
    img = sigpatch.Image(nxs2.MAIN_PROFILE, real)
    for stock, source in patch_main.MODS['x3000key'].imports:
        assert sigpatch.import_value(img, source) == stock


@needs_main
def test_the_image_passes_the_validator_with_the_key_hook_beside_the_handshake(real):
    spans = {}
    out = patch_main.patch(real, NAMES, spans=spans)
    rep = validate.Report()
    validate.check_main_image(real, out, nxs2.MAIN_PROFILE, spans, rep)
    assert not rep.failed, [text for state, text in rep.lines if state == 'FAIL']
    report = {}
    assert patch_main.patch(out, NAMES, report=report) == out
    assert {name: report[name][0] for name in NAMES} == dict.fromkeys(NAMES, 'present')
