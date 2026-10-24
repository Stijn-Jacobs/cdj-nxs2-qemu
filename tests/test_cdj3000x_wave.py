# SPDX-License-Identifier: GPL-2.0-or-later
"""The overview, beat grid and detail waveform Serato sends a CDJ-3000X.
x3000rxview hands the report framework each overview and grid fragment whole,
cut at the end of the message, turns the detail length and windows into the
stock detail header and its points, and arms the detail trains to the display
itself, each cut to the chunks with new points. Run here on the interpreter
with the reports Serato sent around one track load; the framework is a stub
that records what it is handed and, for the detail, builds the header the way
the stock arm does and asks the publisher for the whole detail, and the SPI
tick is modelled after every report: the publisher takes a request, and a
train that is armed goes through."""
import os
import struct

import pytest

from test_cdj3000x_rx import RET, STACK, Sh4 as RxSh4, blob

CODE = 0x08140000
RX_BUFFER = 0x081FFBE0
STOCK_BUFFER = 0x0BD50FDC
HIDO_STATE, AUTH = 0x0BD50E90, 0x0BD4B144
FRAMEWORK, MEMSET = 0x084F9DB2, 0x08533D50
OVERVIEW, GRID, HEADER, POINTS = 0x0BE53428, 0x0BF5BFFC, 0x0BE54510, 0x0BE5452C
REQUEST, SEND, TRAIN, GRID_SEND, RECORD = 0x0A35F404, 0x09FF85EC, 0x10DC1544, 0x09FF8608, 0x0B55059C
ARMER, GRID_ARMER = 0x08391D7C, 0x08391F2E
# the grid the record holds: 1,000 beats of 20 bytes after a 32-byte header, 3 chunks
GRID_BYTES = 32 + 20 * 1000
SR = 0x40000000
SAVED = STACK - 0x100
DATA = os.path.join(os.path.dirname(__file__), 'data')


def pc_relative(op):
    return op >> 12 in (0x9, 0xD) or op & 0xFF00 == 0xC700


class Sh4(RxSh4):
    """The rx test's interpreter plus bt/s, extu.w, shll16, shlr16, shlr8, mov.b Rm,@(r0,Rn) and the status
    register (stc sr, ldc sr; T lives in the interpreter's own flag). A PC-relative load in a delay slot is a
    slot illegal instruction on the deck's SH-4, as it is here."""

    sr = SR

    def delay(self, pc):
        assert not pc_relative(self.fetch_op(pc + 2)), 'slot illegal instruction at %#x' % pc
        super().delay(pc)

    def step_op(self, op, pc):
        n, m = op >> 8 & 15, op >> 4 & 15
        if op & 0xFF00 == 0x8D00:
            disp = (op & 0xFF) - 0x100 if op & 0x80 else op & 0xFF
            taken = self.t
            self.delay(pc)
            return pc + 4 + disp * 2 if taken else pc + 4
        if op & 0xF00F == 0x600D:
            self.r[n] = self.r[m] & 0xFFFF
        elif op & 0xF0FF == 0x4028:
            self.r[n] = self.r[n] << 16 & 0xFFFFFFFF
        elif op & 0xF0FF == 0x4029:
            self.r[n] >>= 16
        elif op & 0xF00F == 0x0004:
            self.write(self.r[0] + self.r[n], bytes([self.r[m] & 0xFF]))
        elif op & 0xF0FF == 0x4019:
            self.r[n] >>= 8
        elif op & 0xF0FF == 0x0002:
            self.r[n] = self.sr | self.t
        elif op & 0xF0FF == 0x400E:
            self.sr, self.t = self.r[n] & ~1, self.r[n] & 1
        else:
            return super().step_op(op, pc)
        return pc + 2


def load(name):
    with open(os.path.join(DATA, name)) as f:
        return [bytes.fromhex(line).ljust(1024, b'\0') for line in f if not line.startswith('#')]


REPORTS = load('cdj3000x_load.txt')
FIRST_LOAD = load('cdj3000x_first_load.txt')
DECK_SELECT = load('cdj3000x_deck_select.txt')


def of_type(kind):
    return [r for r in REPORTS if r[1] == kind]


class Deck:
    """`trains` holds (first chunk, last chunk) of every detail train x3000rxview armed and the link sent,
    `ends` the byte each ended at, `armed` how many it armed, `whole` the header length the publisher read for each whole detail the
    framework asked for. With `busy` set a train stays armed, as on a link still sending it."""

    def __init__(self):
        self.injected, self.cleared, self.trains, self.ends, self.grids, self.whole = [], [], [], [], [], []
        self.busy, self.armed = False, 0
        cpu = self.cpu = Sh4()
        cpu.put32(HIDO_STATE, 2)
        cpu.put32(AUTH, 1)
        cpu.write(CODE, blob('x3000rxview'))
        cpu.stubs[FRAMEWORK] = self.framework
        cpu.stubs[MEMSET] = lambda c: self.cleared.append((c.r[4], c.r[5], c.r[6]))
        cpu.stubs[ARMER] = self.armer
        cpu.stubs[GRID_ARMER] = self.grid_armer

    def framework(self, cpu):
        report = cpu.read(cpu.r[4], cpu.r[5])
        self.injected.append(report)
        if report[1] == 0x2E:
            n = struct.unpack_from('<I', report, 6)[0]
            cpu.put32(HEADER, max(n, 12))
            cpu.put32(HEADER + 4, 2 * max(n, 12))
            cpu.put32(HEADER + 16, POINTS)
            cpu.put32(REQUEST, 1)
        elif report[1] == 0x2F and report[2] == 1:
            cpu.write(GRID + 16, report[6:8])
        elif report[1] == 0x2C and report[2] == 1:
            cpu.write(OVERVIEW, report[6:7])

    def armer(self, cpu):
        """The stock armer: the whole detail of the record's length, from chunk 1."""
        assert cpu.sr & 0xF0 == 0xF0
        self.armed += 1
        length = cpu.u32(RECORD + 32)
        cpu.write(SEND, bytes([1]))
        cpu.write(SEND + 4, struct.pack('<6I', 0x09EF06D8, length, 0, length, (length + 0x1FFF) >> 13, 0))

    def grid_armer(self, cpu):
        assert cpu.sr & 0xF0 == 0xF0
        cpu.write(GRID_SEND, bytes([1]))
        cpu.write(GRID_SEND + 4, struct.pack('<6I', 0x09DF762C, GRID_BYTES, 0, GRID_BYTES, 3, 0))

    def tick(self):
        """The SPI tick between two reports."""
        cpu = self.cpu
        if cpu.u32(REQUEST):
            self.whole.append(cpu.u32(HEADER + 4))
            cpu.put32(REQUEST, 0)
        if cpu.read(SEND, 1)[0] and not self.busy:
            progress, remaining, chunks, index = struct.unpack('<4I', cpu.read(SEND + 12, 16))
            assert cpu.read(TRAIN, 2) == bytes([1, 0])
            end = progress + remaining
            assert progress == index * 0x2000 and chunks == (end + 0x1FFF) >> 13 and end <= cpu.u32(SEND + 8)
            self.trains.append((index + 1, chunks))
            self.ends.append(end)
            progress, remaining, chunks, index = struct.unpack('<4I', cpu.read(GRID_SEND + 12, 16))
            assert progress == index * 0x2000 and progress + remaining == GRID_BYTES
            self.grids.append((index + 1, chunks, cpu.u32(RECORD + 44), cpu.u32(RECORD + 32)))
            cpu.write(SEND, bytes([0]))
            cpu.write(GRID_SEND, bytes([0]))
            cpu.write(TRAIN, bytes([0, 2]))
            cpu.put32(RECORD + 44, 0)

    def feed(self, report):
        cpu = self.cpu
        cpu.write(RX_BUFFER, report)
        cpu.write(SAVED, struct.pack('<8I', 1024, 1, 2, 3, 4, 1024, 6, 7))
        cpu.r[4], cpu.r[15], cpu.pr, cpu.t = SAVED, STACK, RET, 0
        for k in range(8, 15):
            cpu.r[k] = 0x80000000 + k
        cpu.run(CODE)
        assert [cpu.r[k] for k in range(8, 16)] == [0x80000000 + k for k in range(8, 15)] + [STACK]
        assert cpu.t == 0 and cpu.sr == SR
        assert struct.unpack('<8I', cpu.read(SAVED, 32))[5] == 64
        view = cpu.read(STOCK_BUFFER, 64)
        if view[1] == 0x2C and view[2] == 1:
            cpu.write(OVERVIEW, view[6:7])
        self.tick()
        return view


def message(fragments):
    return b''.join(f[6:] for f in fragments)


def test_the_overview_reaches_the_framework_whole_and_cut_at_its_600th_column():
    deck = Deck()
    overview = of_type(0x2C)[:5]
    views = [deck.feed(r) for r in overview]
    assert [len(r) for r in deck.injected] == [1024] * 4 + [6 + 4201 - 4 * 1018]
    assert [r[:6] for r in deck.injected] == [r[:6] for r in overview]
    sent = message(deck.injected)
    assert sent == message(overview)[:1 + 600 * 7]
    assert sent[0] == 0xC0 and len(sent) <= 0x1090
    assert all(v[1] == 0x3F for v in views)


def test_the_beat_grid_reaches_the_framework_with_every_downbeat_numbered_1():
    deck = Deck()
    grid = of_type(0x2F)[:2]
    for r in grid:
        deck.feed(r)
    stream = message(grid)
    count = struct.unpack_from('<H', stream)[0]
    assert count == 313
    sent = message(deck.injected)
    assert len(sent) == 2 + 4 * count
    beats = [sent[2 + 4 * k] for k in range(count)]
    assert beats == [1, 2, 3, 4] * 78 + [1]
    expected = bytearray(stream[:2 + 4 * count])
    for k in range(count):
        expected[2 + 4 * k] = expected[2 + 4 * k] or 1
    assert sent == bytes(expected)
    assert [stream[2 + 4 * k] for k in range(5)] == [1, 2, 3, 4, 0]


def test_a_grid_longer_than_the_record_is_held_to_24000_entries():
    deck = Deck()
    deck.feed(bytes([0, 0x2F, 1, 0, 1, 0]) + struct.pack('<H', 30000) + bytes(1016))
    assert struct.unpack_from('<H', deck.injected[0], 6)[0] == 24000


def test_the_detail_length_builds_an_empty_detail_of_its_points():
    deck = Deck()
    view = deck.feed(of_type(0x35)[0])
    assert deck.injected == [bytes([0, 0x2E, 1, 0, 1, 0]) + struct.pack('<I', 27405)]
    assert deck.cleared == [(POINTS, 0, 2 * 27405)]
    assert deck.trains == [] and view[1] == 0x3F


def test_the_whole_detail_the_framework_asks_for_is_read_as_the_header_alone():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    assert deck.whole == [0] and deck.cpu.u32(HEADER + 4) == 0
    deck.feed(window(0))
    assert deck.cpu.u32(HEADER + 4) == 2 * 27405
    assert deck.trains == [(1, 1)]


def test_the_same_length_again_keeps_the_points_already_in():
    deck = Deck()
    first, again = of_type(0x35)[:2]
    deck.feed(first)
    window = of_type(0x36)[2]
    deck.feed(window)
    deck.feed(again)
    assert len(deck.injected) == 1 and len(deck.cleared) == 1
    start = struct.unpack_from('<I', window, 10)[0]
    assert deck.cpu.read(POINTS + 2 * start, 38) == window[14:52]


def test_a_length_past_the_buffer_is_held_to_540007_points():
    deck = Deck()
    deck.feed(bytes([0, 0x35]) + struct.pack('<I', 1080000) + bytes(1018))
    assert deck.injected[0][6:10] == struct.pack('<I', 540007)


def test_an_unload_leaves_the_stock_no_wave_path_to_the_framework():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    deck.feed(bytes([0, 0x35]) + bytes(1022))
    assert deck.injected[1] == bytes([0, 0x2E, 1, 0, 1, 0, 0, 0, 0, 0])


def test_the_windows_land_at_their_start():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    windows = of_type(0x36)
    for r in windows:
        assert deck.feed(r)[1] == 0x3F
    # the first window (at 9,993) goes at once and from chunk 1, as every first
    # train of a track; Serato then starts again from point 0, away from the
    # front, which moves the count to 1,050, so 450 points later the next
    # train goes, again from chunk 1
    assert deck.trains == [(1, 3), (1, 3)]
    for r in windows[3:]:
        count, start = struct.unpack_from('<II', r, 6)
        assert count == 19
    points = {}
    for r in windows:
        start = struct.unpack_from('<I', r, 10)[0]
        for k in range(19):
            points[start + k] = r[14 + 2 * k:16 + 2 * k]
    for index, point in points.items():
        assert deck.cpu.read(POINTS + 2 * index, 2) == point
    assert deck.cpu.read(POINTS + 2 * max(points) + 2, 2) == b'\0\0'


def window(start, count=19):
    return (bytes([0, 0x36, 1, 0, 1, 0]) + struct.pack('<II', count, start)
            + bytes(k & 0xFF for k in range(1, 2 * count + 1)))[:1024].ljust(1024, b'\0')


@pytest.mark.parametrize('start,count', [(27405 - 18, 19), (27406, 1), (0, 0), (0, 506), (0xFFFFFFF0, 19)])
def test_a_window_outside_the_track_or_the_report_is_dropped(start, count):
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    deck.feed(window(start, count))
    assert deck.trains == []
    assert deck.cpu.read(POINTS, 2 * 19) == bytes(38)


def test_the_last_points_of_the_track_are_taken():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    deck.feed(window(27405 - 19))
    assert deck.cpu.read(POINTS + 2 * (27405 - 19), 38) == bytes(range(1, 39))
    # the length, 2N + 20, leaves the last 6 points out, as the stock train does
    assert deck.trains == [(1, 7)] and deck.ends == [2 * 27405 + 20]


def test_a_window_before_any_length_is_dropped():
    deck = Deck()
    deck.feed(of_type(0x36)[0])
    assert deck.trains == [] and deck.injected == []


def test_the_waveforms_are_left_to_the_stock_path_before_authentication():
    deck = Deck()
    deck.cpu.put32(AUTH, 0)
    for r in of_type(0x2C)[:1] + of_type(0x2F)[:1] + of_type(0x35)[:1] + of_type(0x36)[:1]:
        assert deck.feed(r) == r[:64]
    assert deck.injected == [] and deck.trains == []


def test_the_first_load_goes_through_in_the_order_serato_sent_it():
    deck = Deck()
    views = [deck.feed(r) for r in FIRST_LOAD]
    empty, early, overview, length, grid = (FIRST_LOAD[:9], FIRST_LOAD[9], FIRST_LOAD[10:15], FIRST_LOAD[15],
                                            FIRST_LOAD[16])
    assert early[1] == 0x36 and length[1] == 0x35 and grid[1] == 0x2F
    assert empty[0][6] == 0x80 and views[:9] == [r[:64] for r in empty]
    assert all(v[1] == 0x3F for v in views[9:])
    assert [len(r) for r in deck.injected[:5]] == [1024] * 4 + [6 + 4201 - 4 * 1018]
    assert message(deck.injected[:5]) == message(overview)[:4201]
    assert deck.injected[5] == bytes([0, 0x2E, 1, 0, 1, 0]) + struct.pack('<I', 0x5413)
    assert deck.cleared == [(POINTS, 0, 2 * 0x5413)]
    count = struct.unpack_from('<H', grid, 6)[0]
    assert count == 0xEF and deck.injected[6][:8] == grid[:8]
    sent = deck.injected[6][6:]
    assert len(sent) == 2 + 4 * count and 0 not in sent[2::4]
    assert len(deck.injected) == 7 and deck.trains == []


def test_the_deck_select_prompt_is_drawn_between_the_empty_overviews():
    deck = Deck()
    views = [deck.feed(r) for r in DECK_SELECT]
    lists = [r for r in DECK_SELECT if r[1] == 0x2A]
    assert len(lists) == 4 and [r[1] for r in deck.injected] == [0x2A] * 4
    assert all(v == r[:64] for v, r in zip(views, DECK_SELECT) if r[1] == 0x2C)
    assert deck.injected[1][6:10] == bytes([0, 0, 0, 0]) and deck.injected[1][10:12] == 'C'.encode('utf-16-le')


def test_a_real_overview_after_an_empty_one_is_taken_whole():
    deck = Deck()
    for r in DECK_SELECT[:9] + of_type(0x2C)[:5]:
        deck.feed(r)
    assert message(deck.injected) == message(of_type(0x2C)[:5])[:4201]


def test_a_train_goes_once_10_s_of_points_have_come_in_from_the_chunk_of_the_first_new_one():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    for k in range(474):
        deck.feed(window(19 * k))
    # a point p is byte 32 + 2p of the train: 4,503 is in chunk 2, 9,006 in chunk 3,
    # and each train ends just past the highest point
    assert deck.trains == [(1, 1), (1, 1), (1, 1), (1, 2), (2, 2), (2, 2), (2, 3)]
    assert deck.ends == [32 + 2 * 19 * k for k in (1, 79, 158, 237, 316, 395, 474)]
    deck.feed(of_type(0x35)[0])
    deck.feed(window(19 * 474))
    assert len(deck.trains) == 7


def test_the_grid_after_a_train_is_its_last_chunk_and_asks_for_the_beat_ticks():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    for k in range(80):
        deck.feed(window(19 * k))
    assert deck.grids == [(3, 3, 1, 2 * 27405 + 20)] * 2


def test_a_train_waits_until_the_link_has_sent_the_last_one():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    deck.busy = True
    for k in range(160):
        deck.feed(window(19 * k))
    assert deck.armed == 1
    deck.busy = False
    deck.tick()
    deck.feed(window(19 * 160))
    assert deck.armed == 2 and deck.trains == [(1, 1), (1, 1)]


def test_a_new_track_starts_the_count_again():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    for k in range(200):
        deck.feed(window(19 * k))
    deck.feed(bytes([0, 0x35]) + struct.pack('<I', 28302) + bytes(1018))
    for k in range(200):
        deck.feed(window(19 * k))
    assert deck.trains == [(1, 1)] * 6 and deck.whole == [0, 0]


def stream(deck, start, windows):
    for k in range(windows):
        deck.feed(window(start + 18 * k))


def test_a_seek_past_what_the_display_has_asks_for_a_publish_450_points_later():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    stream(deck, 0, 100)
    deck.feed(window(9000))
    stream(deck, 9018, 22)
    assert len(deck.trains) == 2
    deck.feed(window(9000 + 18 * 23))
    assert deck.trains[2:] == [(1, 3)]


def test_a_seek_back_into_what_the_last_publish_covered_asks_for_nothing():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    stream(deck, 0, 250)
    assert len(deck.trains) == 4
    stream(deck, 4500, 3)
    deck.feed(window(1000))
    stream(deck, 1018, 40)
    assert len(deck.trains) == 4


def test_a_new_track_forgets_what_the_last_one_published():
    deck = Deck()
    deck.feed(of_type(0x35)[0])
    stream(deck, 0, 250)
    deck.feed(bytes([0, 0x35]) + struct.pack('<I', 28302) + bytes(1018))
    stream(deck, 0, 3)
    deck.feed(window(1000))
    stream(deck, 1018, 23)
    assert deck.trains[4:] == [(1, 1), (1, 1)]

