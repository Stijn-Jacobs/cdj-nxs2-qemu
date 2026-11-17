# SPDX-License-Identifier: GPL-2.0-or-later
"""1024-byte HID Output reports for the CDJ-3000X disguise. x3000out declares
the larger Output report, x3000rxbuf has the HID OUT task read each whole
report into a buffer of its own, and x3000rxview hands the stock code the
first 64 bytes in the task's buffer with the count capped at 64. The two
routines run here on the interpreter; all three are applied to the real 1.87
MAIN."""
import os
import struct

import pytest

import patch_main
import sigpatch
from players import cdj2000nxs2 as nxs2
from test_cdj3000x_hs import Sh4 as BaseSh4
from test_main_phrase import RET, STACK

CODE = 0x08140000
RX_BUFFER = 0x081FFBE0
STOCK_BUFFER = 0x0BD50FDC
LENGTH = 0x0BD4B108
BASE = 0x08000000
MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')
needs_main = pytest.mark.skipif(not os.path.exists(MAIN), reason='needs extract/main_unpacked.bin')
SAVED = STACK - 0x100
NAMES = ['x3000out', 'x3000rxbuf', 'x3000rxview']


class Sh4(BaseSh4):
    """The handshake test's interpreter plus movt, tst and xor #imm, mov.l @(r0,Rm), mov.b @-Rn, extu.w, the
    signed compares and the mov.w forms."""

    def step_op(self, op, pc):
        n, m, d = op >> 8 & 15, op >> 4 & 15, op & 15
        signed = [v - (1 << 32) if v & 0x80000000 else v for v in (self.r[n], self.r[m])]
        if op & 0xF0FF == 0x0029:
            self.r[n] = self.t
        elif op & 0xF00F == 0x3003:
            self.t = int(signed[0] >= signed[1])
        elif op & 0xF00F == 0x3007:
            self.t = int(signed[0] > signed[1])
        elif op & 0xF00F == 0x000E:
            self.r[n] = self.u32(self.r[0] + self.r[m])
        elif op & 0xF00F == 0x2004:
            self.r[n] = (self.r[n] - 1) & 0xFFFFFFFF
            self.write(self.r[n], bytes([self.r[m] & 0xFF]))
        elif op & 0xFF00 == 0xCA00:
            self.r[0] ^= op & 0xFF
        elif op & 0xFF00 == 0xC800:
            self.t = int(self.r[0] & op & 0xFF == 0)
        elif op & 0xF00F == 0x600D:
            self.r[n] = self.r[m] & 0xFFFF
        elif op & 0xF00F == 0x6005:
            self.r[n] = self.s16(self.r[m])
            self.r[m] += 2
        elif op & 0xF00F == 0x2001:
            self.write(self.r[n], struct.pack('<H', self.r[m] & 0xFFFF))
        elif op >> 12 == 0x9:
            self.r[n] = self.s16(pc + 4 + (op & 0xFF) * 2) & 0xFFFFFFFF
        elif op & 0xFF00 == 0x8500:
            self.r[0] = self.s16(self.r[m] + d * 2) & 0xFFFFFFFF
        elif op & 0xFF00 == 0x8100:
            self.write(self.r[m] + d * 2, struct.pack('<H', self.r[0] & 0xFFFF))
        else:
            return super().step_op(op, pc)
        return pc + 2


def blob(name):
    with open(patch_main.blob_path(name), 'rb') as f:
        return f.read()


def run(name, saved, t=0, r13=0x80000000 + 13):
    """Run a hook the way the shared trampoline calls it: r4 = the saved r0-r7."""
    cpu = Sh4()
    cpu.write(CODE, blob(name))
    cpu.write(SAVED, struct.pack('<8I', *saved))
    cpu.r[4], cpu.r[15], cpu.pr, cpu.t = SAVED, STACK, RET, t
    for k in range(8, 15):
        cpu.r[k] = 0x80000000 + k
    cpu.r[13] = r13
    cpu.run(CODE)
    assert [cpu.r[k] for k in range(8, 15)] == [0x80000000 + k for k in range(8, 13)] + [r13, 0x8000000E]
    assert cpu.r[15] == STACK
    return cpu, list(struct.unpack('<8I', cpu.read(SAVED, 32)))


def test_the_read_goes_to_the_large_buffer_and_asks_for_1024_bytes():
    saved = [20, 1, 2, 3, 0x0BD4A000, STOCK_BUFFER, 6, 7]
    cpu, after = run('x3000rxbuf', saved, r13=LENGTH)
    assert after == saved[:5] + [RX_BUFFER] + saved[6:]
    assert cpu.u32(LENGTH) == 1024


REPORT = bytes(range(256)) * 4


def view(count, t=0):
    cpu = Sh4()
    cpu.write(RX_BUFFER, REPORT)
    cpu.write(STOCK_BUFFER, b'\xee' * 128)
    cpu.write(CODE, blob('x3000rxview'))
    saved = [count & 0xFFFFFFFF, 0x00080001, 2, 3, 4, count & 0xFFFFFFFF, 6, 7]
    cpu.write(SAVED, struct.pack('<8I', *saved))
    cpu.r[4], cpu.r[15], cpu.pr, cpu.t = SAVED, STACK, RET, t
    for k in range(8, 15):
        cpu.r[k] = 0x80000000 + k
    cpu.run(CODE)
    assert [cpu.r[k] for k in range(8, 15)] == [0x80000000 + k for k in range(8, 15)]
    return cpu, list(struct.unpack('<8I', cpu.read(SAVED, 32))), saved


def test_a_whole_report_is_seen_as_its_first_64_bytes():
    cpu, after, saved = view(1024)
    assert cpu.read(STOCK_BUFFER, 128) == REPORT[:64] + b'\xee' * 64
    assert after == saved[:5] + [64] + saved[6:]


@pytest.mark.parametrize('count', [1, 40, 64])
def test_a_short_report_keeps_its_count(count):
    cpu, after, saved = view(count)
    assert cpu.read(STOCK_BUFFER, 64) == REPORT[:64]
    assert after == saved


@pytest.mark.parametrize('count', [-4, -5])
def test_an_error_leaves_the_buffer_and_the_count_alone(count):
    cpu, after, saved = view(count, t=int(count == -5))
    assert cpu.read(STOCK_BUFFER, 128) == b'\xee' * 128
    assert after == saved


@pytest.mark.parametrize('count,t', [(1024, 0), (-5, 1), (40, 0)])
def test_t_is_what_the_compare_with_minus_5_left(count, t):
    cpu, _, _ = view(count, t=t)
    assert cpu.t == t


LIST = 0x081FEFE0
HIDO_STATE, AUTH = 0x0BD50E90, 0x0BD4B144
FRAMEWORK = 0x084F9DB2


def record(row, text, flags=0, b2=0, b3=0):
    """A list record as Serato sends a CDJ-3000X one: four NXS2 header bytes,
    four more, then the text."""
    return bytes([row, flags, b2, b3]) + bytes(4) + text.encode('utf-16-le') + bytes(2)


def nxs2_record(row, text, flags=0, b2=0, b3=0):
    return bytes([row, flags, b2, b3]) + text.encode('utf-16-le') + bytes(2)


# The deck-select prompt as Serato sent it to the deck with the CDJ-3000X identity.
PROMPT = [(0, 'Connect to SDJ Pro', 0), (1, 'Turn the encoder to select a deck', 0x80),
          (2, 'DECK 1', 0), (3, 'DECK 2', 0), (4, 'DECK 3', 0), (5, 'DECK 4', 0)]


def list_report(records, index=1, count=1):
    body = b''.join(records)
    return (bytes([0, 0x2A]) + struct.pack('<HH', index, count) + body).ljust(1024, b'\0')


class ListDeck:
    """A deck fed the reports one after the other; the stock view and the count are the last one's."""

    def __init__(self, *reports, state=2, auth=1, count=1024):
        self.injected = []
        cpu = self.cpu = Sh4()
        cpu.put32(HIDO_STATE, state)
        cpu.put32(AUTH, auth)
        cpu.write(CODE, blob('x3000rxview'))
        cpu.stubs[FRAMEWORK] = lambda c: self.injected.append(c.read(c.r[4], c.r[5]))
        for report in reports:
            cpu.write(RX_BUFFER, report)
            cpu.write(STOCK_BUFFER, b'\xee' * 128)
            cpu.write(SAVED, struct.pack('<8I', count, 1, 2, 3, 4, count, 6, 7))
            cpu.r[4], cpu.r[15], cpu.pr, cpu.t = SAVED, STACK, RET, 0
            for k in range(8, 15):
                cpu.r[k] = 0x80000000 + k
            cpu.run(CODE)
            assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [STACK]
            assert cpu.t == 0
        self.count = struct.unpack('<8I', cpu.read(SAVED, 32))[5]


def test_the_prompt_reaches_the_framework_in_the_nxs2_record_form():
    deck = ListDeck(list_report([record(r, t, f) for r, t, f in PROMPT]))
    body = b''.join(nxs2_record(r, t, f) for r, t, f in PROMPT)
    assert deck.injected == [bytes([0, 0x2A, 1, 0, 1, 0]) + body]
    assert deck.cpu.read(STOCK_BUFFER, 2) == b'\x00\x3f'
    assert deck.count == 64


def test_the_clear_before_a_list_translates_to_the_stock_clear():
    deck = ListDeck(list_report([bytes([1, 0x80]) + bytes(8)]))
    assert deck.injected == [bytes([0, 0x2A, 1, 0, 1, 0, 1, 0x80, 0, 0, 0, 0])]


def test_rows_past_the_nxs2s_nine_are_left_out():
    deck = ListDeck(list_report([record(8, 'last'), record(9, 'tenth'), record(2, 'after')]))
    assert deck.injected == [bytes([0, 0x2A, 1, 0, 1, 0]) + nxs2_record(8, 'last') + nxs2_record(2, 'after')]


def test_a_long_text_keeps_127_characters():
    deck = ListDeck(list_report([record(0, 'x' * 200)]))
    assert deck.injected == [bytes([0, 0x2A, 1, 0, 1, 0]) + nxs2_record(0, 'x' * 127)]


@pytest.mark.parametrize('state,auth', [(1, 1), (2, 0)])
def test_the_list_is_left_to_the_stock_path_unless_the_deck_is_authenticated(state, auth):
    report = list_report([record(0, 'hello')])
    deck = ListDeck(report, state=state, auth=auth)
    assert deck.injected == []
    assert deck.cpu.read(STOCK_BUFFER, 64) == report[:64]


# Lists Serato sent the deck with the CDJ-3000X identity (record header, text):
# the root of the library with the cursor on a crate, a crate's first page,
# and a page further down the same crate with the cursor held on row 5.
ROOT = [
    ('0000000000000000', 'Serato'),
    ('0102000000000000', 'Stems'),
    ('0202000000000000', 'Spotify'),
    ('0302000000000000', 'Crate 1'),
    ('0402000000000000', 'SetFlow - Peak Time - Hardcore - 60min - 20 tracks - 2026-08-02 (1)'),
    ('0582000000000000', 'Uptempo selection'),
    ('0602000000000000', 'dnb dumb'),
    ('0702000000000000', 'Uptempo soundcloud'),
    ('0802000000000000', 'DnB'),
    ('0902000000000000', 'euphoric selection'),
    ('0a02000000000000', 'new'),
    ('0b02000000000000', 'sefa albums'),
]
CRATE = [
    ('0000000000000000', 'Uptempo selection'),
    ('01040a0164000200', 'CRYEXGENIC PART 2.5 (Extended Mix)'),
    ('02840a0566001800', 'Wanna Dance (Extended Mix)'),
    ('03040a0566001600', 'In My Head (Extended Mix)'),
    ('04040a0064001800', 'Already Gone (Extended Mix)'),
    ('05040a0069001400', 'Voices In My Head (Extended Mix)'),
    ('06040a0967001600', 'Save You (Extended Mix)'),
    ('07040a0566001600', 'Control The Night (Extended Mix)'),
    ('08040a0566001600', 'Move It (Extended Mix)'),
    ('09040a0064001600', 'Jump Me Crazy (Extended Mix)'),
    ('0a040a0069001800', 'Party Jumping (Extended Mix)'),
    ('0b040a00be000200', 'This Feeling (Extended Mix)'),
]
PAGE = [
    ('0000000000000000', 'Uptempo selection'),
    ('01040a0069001400', 'Voices In My Head (Extended Mix)'),
    ('02040a0967001600', 'Save You (Extended Mix)'),
    ('03040a0566001600', 'Control The Night (Extended Mix)'),
    ('04040a0566001600', 'Move It (Extended Mix)'),
    ('05840a0064001600', 'Jump Me Crazy (Extended Mix)'),
    ('06040a0069001800', 'Party Jumping (Extended Mix)'),
    ('07040a00be000200', 'This Feeling (Extended Mix)'),
    ('08040a0566001400', 'Unholy'),
    ('09040a0566000200', "Get's Me High (Extended Mix)"),
    ('0a040a0567001000', "Can't Stop (Extended Mix)"),
    ('0b040a0069001200', 'Funky Music (Extended Mix)'),
]

# b6 is the key, the minor ones even from A minor (2) a semitone apart.
CAMELOT = [8, 3, 10, 5, 12, 7, 2, 9, 4, 11, 6, 1]


def body(rows):
    return b''.join(bytes.fromhex(header) + text.encode('utf-16-le') + bytes(2) for header, text in rows)


def shown(header, text):
    """A row's text on the NXS2: a track's ends in its BPM and, for a minor key, the Camelot key."""
    bpm = header[4] | header[5] << 8
    if header[1] & 0x7F != 4 or not bpm:
        return text[:127]
    key = header[6]
    tail = ' %d.%d' % (bpm, header[3])
    if key and not key & 1 and key <= 24:
        tail += ' %dA' % CAMELOT[key // 2 - 1]
    return text[:115] + tail


def nxs2_list(rows, first=1):
    """The list as the framework gets it: row 0, then rows first..first+7 as rows 1-8."""
    out = bytes([0, 0x2A, 1, 0, 1, 0])
    for header, text in rows:
        header = bytes.fromhex(header)
        row = header[0]
        if row:
            if not first <= row < first + 8:
                continue
            row -= first - 1
        out += bytes([row]) + header[1:4] + shown(header, text).encode('utf-16-le') + bytes(2)
    return out


def texts(message):
    """The text of each NXS2 record in a list message."""
    out, i = [], 6
    while i < len(message):
        end = i + 4
        while message[end:end + 2] != b'\0\0':
            end += 2
        out.append(message[i + 4:end].decode('utf-16-le'))
        i = end + 2
    return out


def test_the_library_root_shows_the_header_and_crates_1_to_8_with_the_cursor_on_5():
    deck = ListDeck(list_report([body(ROOT)]))
    assert deck.injected == [nxs2_list(ROOT)]
    assert texts(deck.injected[0]) == [text for _, text in ROOT[:9]]
    assert deck.injected[0].count(bytes([5, 0x82, 0, 0]) + 'Uptempo'.encode('utf-16-le')) == 1


def test_a_crate_shows_each_track_with_its_bpm_and_key():
    deck = ListDeck(list_report([body(CRATE)]))
    assert deck.injected == [nxs2_list(CRATE)]
    assert texts(deck.injected[0])[:5] == ['Uptempo selection', 'CRYEXGENIC PART 2.5 (Extended Mix) 100.1 8A',
                                           'Wanna Dance (Extended Mix) 102.5 1A', 'In My Head (Extended Mix) 102.5 6A',
                                           'Already Gone (Extended Mix) 100.0 1A']


def test_a_page_down_the_crate_is_rows_1_to_8_as_serato_sent_them():
    deck = ListDeck(list_report([body(PAGE)]))
    assert deck.injected == [nxs2_list(PAGE)]
    assert texts(deck.injected[0])[8] == 'Unholy 102.5 11A'


def with_cursor(rows, cursor):
    """The rows with the cursor flag on row `cursor` instead."""
    out = []
    for header, text in rows:
        header = bytearray.fromhex(header)
        header[1] = header[1] & 0x7F | (0x80 if header[0] == cursor else 0)
        out.append((header.hex(), text))
    return out


@pytest.mark.parametrize('cursor,first', [(1, 1), (5, 1), (6, 2), (7, 3), (8, 4), (9, 4), (11, 4)])
def test_the_window_keeps_the_cursor_on_screen_four_rows_down(cursor, first):
    rows = with_cursor(PAGE, cursor)
    deck = ListDeck(list_report([body(rows)]))
    assert deck.injected == [nxs2_list(rows, first)]
    assert deck.injected[0].count(bytes([cursor - first + 1, 0x84])) == 1


@pytest.mark.parametrize('cursor', [1, 6])
def test_a_list_of_up_to_nine_rows_is_never_moved(cursor):
    rows = with_cursor(PAGE[:7], cursor)
    deck = ListDeck(list_report([body(rows)]))
    assert deck.injected == [nxs2_list(rows)]


@pytest.mark.parametrize('key,tail', [(0x0F, ' 102.5'), (0, ' 102.5'), (0x1A, ' 102.5'), (0x04, ' 102.5 3A')])
def test_only_a_minor_key_is_shown(key, tail):
    deck = ListDeck(list_report([body([('0000000000000000', 'crate'), ('01040a056600%02x00' % key, 'track')])]))
    assert texts(deck.injected[0]) == ['crate', 'track' + tail]


def test_a_track_without_a_bpm_shows_its_title_only():
    deck = ListDeck(list_report([body([('0000000000000000', 'crate'), ('01040a0000001800', 'track')])]))
    assert texts(deck.injected[0]) == ['crate', 'track']


def test_a_long_track_title_keeps_115_characters_before_its_bpm_and_key():
    deck = ListDeck(list_report([body([('0000000000000000', 'crate'), ('01040a05e7031800', 'x' * 200)])]))
    assert texts(deck.injected[0]) == ['crate', 'x' * 115 + ' 999.5 1A']


def fragments(message):
    """A list message as Serato splits one too long for a report: 1018 bytes a fragment."""
    parts = [message[i:i + 1018] for i in range(0, len(message), 1018)]
    return [(bytes([0, 0x2A]) + struct.pack('<HH', k + 1, len(parts)) + part).ljust(1024, b'\0')
            for k, part in enumerate(parts)]


def long_list(characters):
    rows = [('0000000000000000', 'A crate with long titles')]
    rows += [('%02x%02x0a0566001800' % (row, 0x84 if row == 5 else 4), '%02d ' % row + 'y' * (characters - 3))
             for row in range(1, 12)]
    return rows


@pytest.mark.parametrize('characters,parts', [(60, 2), (140, 4)])
def test_a_list_in_fragments_is_put_together_first(characters, parts):
    rows = long_list(characters)
    reports = fragments(body(rows))
    assert len(reports) == parts
    deck = ListDeck(*reports)
    assert deck.injected == [nxs2_list(rows)]
    assert deck.cpu.read(STOCK_BUFFER, 2) == b'\x00\x3f'
    assert ListDeck(*reports[:-1]).injected == []


def test_every_fragment_is_kept_from_the_stock_path():
    for report in fragments(body(long_list(60))):
        deck = ListDeck(report)
        assert deck.cpu.read(STOCK_BUFFER, 2) == b'\x00\x3f' and deck.count == 64


def test_a_list_started_again_is_taken_from_its_new_first_fragment():
    rows = long_list(60)
    first, second = fragments(body(rows))
    other = fragments(body(long_list(70)))[0]
    assert ListDeck(other, first, second).injected == [nxs2_list(rows)]


@pytest.mark.parametrize('order', [[1], [0, 0], [1, 0], [0, 1, 1]])
def test_a_fragment_out_of_turn_is_dropped(order):
    reports = fragments(body(long_list(60)))
    deck = ListDeck(*[reports[k] for k in order])
    assert deck.injected == ([nxs2_list(long_list(60))] if order == [0, 1, 1] else [])


def test_a_fragment_after_a_whole_list_is_dropped():
    deck = ListDeck(list_report([body(PAGE)]), fragments(body(long_list(60)))[1])
    assert deck.injected == [nxs2_list(PAGE)]


@pytest.mark.parametrize('index,count', [(1, 5), (5, 5), (2, 1), (0, 1)])
def test_a_list_of_more_than_four_fragments_or_a_wrong_index_is_dropped(index, count):
    deck = ListDeck(list_report([body(PAGE)], index, count))
    assert deck.injected == [] and deck.cpu.read(STOCK_BUFFER, 2) == b'\x00\x3f'


def test_other_reports_are_only_viewed():
    report = bytes([0, 0x33]) + bytes(1022)
    deck = ListDeck(report)
    assert deck.injected == [] and deck.cpu.read(STOCK_BUFFER, 64) == report[:64]


# Control reports Serato sent the deck with the CDJ-3000X identity: playing at
# 0:25.150, and just after a load at -0:00.075, the seconds byte's bit 7 the sign.
PLAYING = bytes.fromhex(
    '0021c001290401a204b580001996000308aa020100006900000000008000000029000080194300000000000000ffff'
    '00006900000000000000180d0000000000').ljust(1024, b'\0')
AFTER_LOAD = bytes.fromhex(
    '0021c001290401a204b58000804b000308aa020100006900000000008000000029000000009f00000000000000ffff'
    '00006900000000000000180d0000000000').ljust(1024, b'\0')


def nxs2_control(report, elapsed):
    """The control report as the NXS2 reads it: bytes 22-42 one byte lower,
    zeros from 42, the top bytes of the loop in and out times and the second
    time's minutes without their flag, byte 6 bit 0 clear, a loop size only on
    a beat loop that exists and inside the NXS2's table, and the elapsed time
    given."""
    view = bytearray(report[:64])
    view[6] &= 0xFE
    view[11:15] = elapsed
    view[21:42] = report[22:43]
    view[42:64] = bytes(22)
    for k in (27, 30, 34):
        view[k] &= 0x7F
    if not (view[9] & 8 and 3 <= view[10] <= 21):
        view[10] = 0x80
    return bytes(view)


class ControlDeck(ListDeck):
    def __init__(self):
        self.injected = []
        cpu = self.cpu = Sh4()
        cpu.put32(HIDO_STATE, 2)
        cpu.put32(AUTH, 1)
        cpu.write(CODE, blob('x3000rxview'))
        cpu.stubs[FRAMEWORK] = lambda c: self.injected.append(c.read(c.r[4], c.r[5]))

    def feed(self, report):
        cpu = self.cpu
        cpu.write(RX_BUFFER, report)
        cpu.write(SAVED, struct.pack('<8I', 1024, 1, 2, 3, 4, 1024, 6, 7))
        cpu.r[4], cpu.r[15], cpu.pr, cpu.t = SAVED, STACK, RET, 0
        for k in range(8, 15):
            cpu.r[k] = 0x80000000 + k
        cpu.run(CODE)
        assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [STACK]
        return cpu.read(STOCK_BUFFER, 64)


def test_the_control_report_reaches_the_nxs2_in_its_own_layout():
    view = ControlDeck().feed(PLAYING)
    assert view == nxs2_control(PLAYING, PLAYING[11:15])
    assert view[21] == 0x69 and view[22] == 0x00     # BPM 105.0 where the NXS2 reads it


def test_a_negative_elapsed_shows_as_the_start():
    deck = ControlDeck()
    deck.feed(PLAYING)
    assert deck.feed(AFTER_LOAD) == nxs2_control(AFTER_LOAD, bytes(4))
    assert deck.feed(PLAYING) == nxs2_control(PLAYING, PLAYING[11:15])


def test_quantize_reaches_the_nxs2():
    report = bytearray(PLAYING)
    report[6] |= 0x08
    assert ControlDeck().feed(bytes(report))[6] == 0x08


# Loop sizes in beats by byte 10's low 7 bits: the 3000X's table, and what the
# NXS2's two loop size getters 0x084FBE06 (numerator) and 0x084FBF12
# (denominator) make of the same value; for any other the numerator is 0.
LOOP_3000X = {3: (1, 128), 4: (1, 64), 5: (1, 32), 6: (1, 16), 7: (1, 8), 8: (1, 4), 9: (1, 3), 10: (1, 2),
              11: (3, 4), 12: (1, 1), 13: (2, 1), 14: (4, 1), 15: (8, 1), 16: (16, 1), 17: (32, 1), 18: (64, 1),
              19: (128, 1), 20: (256, 1), 21: (512, 1), 23: (3, 2), 24: (3, 1), 25: (5, 1), 26: (6, 1),
              27: (7, 1), 28: (9, 1)}
LOOP_NXS2 = {v: LOOP_3000X[v] for v in range(3, 22)}


def loop_report(b9, b10, loop_in=0, loop_out=0):
    report = bytearray(PLAYING)
    report[9], report[10] = b9, b10
    report[26:29] = loop_in.to_bytes(3, 'little')
    report[29:32] = loop_out.to_bytes(3, 'little')
    return bytes(report)


def test_a_loop_size_is_passed_only_where_both_tables_agree():
    deck = ControlDeck()
    for size in range(128):
        shown = size in LOOP_NXS2 and LOOP_NXS2[size] == LOOP_3000X.get(size)
        assert deck.feed(loop_report(0xB5 | 8, size))[10] == (size if shown else 0x80)


@pytest.mark.parametrize('b9,b10', [(0xB5, 14), (0xB5 | 8, 0x80 | 14)])
def test_no_size_without_a_beat_loop(b9, b10):
    assert ControlDeck().feed(loop_report(b9, b10))[10] == 0x80


def test_the_loop_times_lose_the_flag_in_their_top_byte():
    view = ControlDeck().feed(loop_report(0xB5 | 8, 14, 0x800000 | 130137, 0x800000 | 131000))
    assert view[25:31] == (130137).to_bytes(3, 'little') + (131000).to_bytes(3, 'little')


def test_the_control_report_is_left_alone_before_authentication():
    deck = ControlDeck()
    deck.cpu.put32(AUTH, 0)
    assert deck.feed(PLAYING) == PLAYING[:64]


# -- on the real image ----------------------------------------------------------------

@pytest.fixture(scope='module')
def real():
    with open(MAIN, 'rb') as f:
        return f.read()


def output_bytes(descriptor):
    """The Output report's size in bytes, from the descriptor's global items."""
    size = count = 0
    i = 0
    while i < len(descriptor):
        prefix = descriptor[i]
        n = (0, 1, 2, 4)[prefix & 3]
        value = int.from_bytes(descriptor[i + 1:i + 1 + n], 'little')
        tag = prefix & 0xFC
        if tag == 0x74:
            size = value
        elif tag == 0x94:
            count = value
        elif tag == 0x90:
            return size * count // 8
        i += 1 + n
    raise AssertionError('no Output item')


@needs_main
def test_the_descriptor_declares_1024_bytes_out_in_the_same_52(real):
    out = patch_main.patch(real, ['x3000out'])
    at = 0x0856F328 - BASE
    assert output_bytes(real[at:at + 52]) == 64
    assert output_bytes(out[at:at + 52]) == 1024
    assert out[at + 52:at + 56] == real[at + 52:at + 56]
    changed = [i for i in range(len(real)) if real[i] != out[i]]
    assert changed and all(at + 32 <= i < at + 52 for i in changed)
    for length in (0x0856F122, 0x0856F21C):
        assert out[length - BASE:length - BASE + 2] == b'\x34\x00'


@needs_main
def test_every_firmware_address_the_routines_hold_is_found(real):
    img = sigpatch.Image(nxs2.MAIN_PROFILE, real)
    for name in ('x3000rxbuf', 'x3000rxview'):
        for stock, source in patch_main.MODS[name].imports:
            assert sigpatch.import_value(img, source) == stock


@needs_main
def test_the_hooks_sit_round_the_hido_read_call(real):
    img = sigpatch.Image(nxs2.MAIN_PROFILE, real)
    sites = {}
    for name in ('x3000rxbuf', 'x3000rxview'):
        mod = patch_main.MODS[name]
        sites[name] = img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start)
    assert sites == {'x3000rxbuf': 0x084F6ABC, 'x3000rxview': 0x084F6ACE}
    out = patch_main.patch(real, NAMES)
    call = 0x084F6AC8 - BASE
    assert out[call:call + 6] == real[call:call + 6]


@needs_main
def test_an_image_that_already_carries_them_is_left_as_it_is(real):
    report = {}
    once = patch_main.patch(real, NAMES)
    assert patch_main.patch(once, NAMES, report=report) == once
    assert {name: report[name][0] for name in NAMES} == dict.fromkeys(NAMES, 'present')


@needs_main
def test_they_go_in_with_the_handshake_and_the_other_main_mods(real):
    others = ['x3000ident', 'x3000bcd', 'x3000hs', 'osc', 'oscbeat', 'tcnet', 'usbmidi']
    both = patch_main.patch(real, others + NAMES)
    assert patch_main.patch(real, NAMES[::-1] + others[::-1]) == both
