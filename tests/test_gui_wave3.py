# SPDX-License-Identifier: GPL-2.0-or-later
"""The 3-band display mods, executed: the centre routine (gui_wave3.s) and the
overview routine (gui_wave3ov.s) run on an SH-2A interpreter and their pixels
are compared with a model written from the drawing rules in their headers.
Against the real display image, the stock instructions they replace are run
too, so a word or record the mods do not take as 3-band must come out exactly
as it does in the unpatched firmware."""
import os
import random
import struct

import pytest

import patch_gui
import sigpatch
from helpers import path
from sh2a import Memory, Sh2a

REAL_GUI = path('..', 'extract', 'gui_unpacked.bin')
needs_real_gui = pytest.mark.skipif(not os.path.exists(REAL_GUI),
                                    reason='extract/gui_unpacked.bin not present')

CANVAS = 0x1C200000
CODE = 0x1C300000
STACK = 0x1C3F0000
ARRAY = 0x1C400000
RECORDS = 0x1C500000

CANVAS_COLUMNS = 764
CANVAS_ROW_BYTES = 768 * 2
CENTRE_ROW = 70
CANVAS_ROWS = 72
UNTOUCHED = 0x1234

OVERVIEW_COLUMNS = 600
OVERVIEW_ROWS = 40

# Which bands reach a row -> the colour, 24-bit. Low blue, mid amber, high
# white alone; the overlaps are blends.
BLEND = {
    (1, 0, 0): 0x0055E1, (0, 1, 0): 0xFFA600, (0, 0, 1): 0xFFFFFF,
    (1, 1, 0): 0xB4690A, (1, 0, 1): 0xD2DCFA, (0, 1, 1): 0xFFF0D7,
    (1, 1, 1): 0xF5EBD7,
}
BLUE, AMBER, WHITE = 0x0055E1, 0xFFA600, 0xFFFFFF


def rgb565(rgb):
    return ((rgb >> 19 & 31) << 11) | ((rgb >> 10 & 63) << 5) | (rgb >> 3 & 31)


def flagged_word(low, mid, high):
    return high << 11 | mid << 6 | low << 1 | 1


def band_rows(field):
    return max(1, 3 * field >> 1)


def stock_colour(word):
    return (word & 0xE000) | (word >> 2 & 0x700) | (word >> 5 & 0x1C)


def with_canvas(blob):
    return blob[:-4] + struct.pack('>I', CANVAS)


def blob(name):
    with open(patch_gui.blob_path(name), 'rb') as f:
        return f.read()


def centre_memory(words, frame_first, frame_count, code, code_at=CODE):
    mem = Memory()
    mem.add(CANVAS, struct.pack('>H', UNTOUCHED) * (CANVAS_ROW_BYTES // 2 * CANVAS_ROWS))
    mem.add(STACK, bytes(0x100))
    mem.add(ARRAY, struct.pack('>%dH' % len(words), *words))
    mem.add(code_at, code)
    mem.put32(STACK + 40, frame_first)
    mem.put32(STACK + 12, frame_count)
    return mem


def canvas_pixels(mem):
    raw = mem.read(CANVAS, CANVAS_ROW_BYTES * CANVAS_ROWS)
    return struct.unpack('>%dH' % (len(raw) // 2), raw)


def call_centre(mem, column, peak, height, code_at=CODE, sentinel=0x55AA0000):
    cpu = Sh2a(mem)
    cpu.r[15] = STACK
    cpu.r[4], cpu.r[5], cpu.r[6], cpu.r[7], cpu.r[12] = column, peak, height, ARRAY, height
    for k in (8, 9, 10, 11, 13, 14):
        cpu.r[k] = sentinel + k
    cpu.run(code_at, Sh2a.RET)
    assert [cpu.r[k] for k in (8, 9, 10, 11, 13, 14, 15)] == \
        [sentinel + k for k in (8, 9, 10, 11, 13, 14)] + [STACK]
    return cpu


def model_centre(words, first_entries, per_col, columns):
    """The canvas the drawing rules give for 3-band words: each band's rows
    from its largest field over the column's group, coloured by which bands
    reach the row."""
    fb = {}
    tallest = {}
    for col in columns:
        group = words[first_entries[col]:first_entries[col] + per_col]
        low, mid, high = (max(w >> s & 31 for w in group) for s in (1, 6, 11))
        heights = [band_rows(v) for v in (low, mid, high)]
        tallest[col] = max(heights)
        for d in range(tallest[col]):
            key = tuple(int(d < h) for h in heights)
            fb[CENTRE_ROW - d, col] = rgb565(BLEND[key])
    return fb, tallest


def expect_canvas(fb):
    cells = [UNTOUCHED] * (CANVAS_ROW_BYTES // 2 * CANVAS_ROWS)
    for (row, col), px in fb.items():
        cells[row * CANVAS_ROW_BYTES // 2 + col] = px
    return tuple(cells)


@pytest.mark.parametrize('per_col', [1, 2, 4, 8, 16])
def test_centre_draws_the_3band_words_by_the_drawing_rules(per_col):
    rng = random.Random(per_col)
    count = CANVAS_COLUMNS * per_col + 40
    words = [flagged_word(rng.choice((0, 1, 5, 31, rng.randrange(32))),
                          rng.choice((0, 2, 17, 31, rng.randrange(32))),
                          rng.choice((0, 3, 9, 31, rng.randrange(32)))) for _ in range(count)]
    columns = range(0, CANVAS_COLUMNS, 5)
    first = {col: 10 + col * per_col for col in columns}
    mem = centre_memory(words, 0, per_col, with_canvas(blob('wave3')))
    for col in columns:
        mem.put32(STACK + 40, first[col])
        peak = first[col] + rng.randrange(per_col)
        cpu = call_centre(mem, col, peak, rng.randrange(1, 56))
        fb, tallest = model_centre(words, first, per_col, [col])
        assert cpu.r[12] == tallest[col]
    everything = {}
    for col in columns:
        everything.update(model_centre(words, first, per_col, [col])[0])
    assert canvas_pixels(mem) == expect_canvas(everything)


def test_centre_paints_a_column_of_silent_bands_one_row_high():
    mem = centre_memory([flagged_word(0, 0, 0)], 0, 1, with_canvas(blob('wave3')))
    cpu = call_centre(mem, 3, 0, 20)
    assert cpu.r[12] == 1
    assert canvas_pixels(mem) == expect_canvas({(CENTRE_ROW, 3): rgb565(BLEND[1, 1, 1])})


def test_centre_draws_stock_words_as_the_stock_fill_does():
    rng = random.Random(7)
    words = [rng.randrange(0x10000) & ~3 for _ in range(200)]
    mem = centre_memory(words, 0, 4, with_canvas(blob('wave3')))
    expected = {}
    for col in range(0, 60, 3):
        peak, height = col, rng.randrange(1, 56)
        first = col
        mem.put32(STACK + 40, first)
        cpu = call_centre(mem, col, peak, height)
        assert cpu.r[12] == height
        for d in range(height):
            expected[CENTRE_ROW - d, col] = stock_colour(words[peak])
    assert canvas_pixels(mem) == expect_canvas(expected)


def overview_memory(records, code):
    mem = Memory()
    mem.add(CANVAS, struct.pack('>H', UNTOUCHED) * (OVERVIEW_COLUMNS * OVERVIEW_ROWS))
    mem.add(STACK, bytes(0x100))
    mem.add(RECORDS, b''.join(records))
    mem.add(CODE, code)
    mem.put32(STACK, OVERVIEW_COLUMNS)
    mem.put32(STACK + 4, CANVAS)
    return mem


def call_overview(mem, column):
    cpu = Sh2a(mem)
    cpu.r[15] = STACK
    cpu.r[5] = RECORDS + 6 * column
    cpu.r[6] = 0xFFFFFFFC
    cpu.r[8] = column
    for k in (9, 10, 11, 12, 13, 14):
        cpu.r[k] = 0x66000000 + k
    cpu.run(CODE, Sh2a.RET)
    assert [cpu.r[k] for k in (8, 9, 10, 11, 12, 13, 14)] == \
        [column] + [0x66000000 + k for k in (9, 10, 11, 12, 13, 14)]
    return cpu


def overview_record(total, mid_and_low, low):
    return bytes([total, mid_and_low, low, 0x3B, 0xFF, 0xFF])


def random_extents(rng):
    total = rng.randrange(1, OVERVIEW_ROWS + 1)
    mid = rng.randrange(1, total + 1)
    return total, mid, rng.randrange(1, mid + 1)


def model_overview(records, first=0):
    fb = {}
    for col, rec in enumerate(records, first):
        total, mid, low = rec[0], rec[1], rec[2]
        for row in range(total):
            colour = BLUE if row < low else AMBER if row < mid else WHITE
            fb[OVERVIEW_ROWS - 1 - row, col] = rgb565(colour)
    return fb


def overview_cells(fb):
    cells = [UNTOUCHED] * (OVERVIEW_COLUMNS * OVERVIEW_ROWS)
    for (row, col), px in fb.items():
        cells[row * OVERVIEW_COLUMNS + col] = px
    return tuple(cells)


def overview_pixels(mem):
    raw = mem.read(CANVAS, OVERVIEW_COLUMNS * OVERVIEW_ROWS * 2)
    return struct.unpack('>%dH' % (len(raw) // 2), raw)


def test_overview_paints_3band_records_bottom_aligned_and_ends_the_column():
    rng = random.Random(3)
    records = [overview_record(*random_extents(rng)) for _ in range(OVERVIEW_COLUMNS)]
    records[0] = overview_record(40, 40, 40)
    records[1] = overview_record(1, 1, 1)
    mem = overview_memory(records, blob('wave3ov'))
    for col in range(OVERVIEW_COLUMNS):
        assert call_overview(mem, col).r[2] == 0
    assert overview_pixels(mem) == overview_cells(model_overview(records))


def stock_decode(record):
    """What the six replaced instructions leave in r7, r0 and r2."""
    height = record[4] >> 2
    return height, (record[4] << 8 | record[5]) >> 4 & 63, height & 0xFF


def test_overview_decodes_any_other_record_as_the_stock_code_does():
    rng = random.Random(5)
    records = [bytes(rng.randrange(256) for _ in range(6)) for _ in range(OVERVIEW_COLUMNS)]
    records[0] = bytes([9, 9, 9, 0x3B, 0xFF, 0xFE])
    records[1] = bytes([9, 9, 9, 0x3A, 0xFF, 0xFF])
    records[2] = bytes([0, 0, 0, 0x3B, 0, 0])
    mem = overview_memory(records, blob('wave3ov'))
    for col, rec in enumerate(records):
        cpu = call_overview(mem, col)
        assert (cpu.r[7], cpu.r[0], cpu.r[2]) == stock_decode(rec)
    assert overview_pixels(mem) == overview_cells({})


# -- against the real display image ----------------------------------------------

CENTRE_SITE, CENTRE_RESUME = 0x1C009CB6, 0x1C009D4A
OVERVIEW_SITE, OVERVIEW_RESUME = 0x1C007B8E, 0x1C007B9E


def real_memory(image, front):
    """Every segment the load table copies, behind the scratch regions the
    caller added first (find() takes the first region that holds an address)."""
    img = sigpatch.Image(patch_gui.GUI_PROFILE, image)
    for _, dest, src, size in img.copies():
        if dest < 0xFFF80000:
            front.add(dest, image[src:src + size])
    return front


@pytest.fixture(scope='module')
def real_images():
    with open(REAL_GUI, 'rb') as f:
        stock = f.read()
    return stock, patch_gui.patch(stock, ['phrase', 'wave3', 'wave3ov'])


@needs_real_gui
def test_real_image_takes_all_three_display_mods_at_once(real_images):
    stock, patched = real_images
    img = sigpatch.Image(patch_gui.GUI_PROFILE, stock)
    for name in ('wave3', 'wave3ov', 'phrase'):
        assert patch_gui.MODS[name].fw_versions == ('1.81',)
        assert sigpatch.find_sig(img, patch_gui.MODS[name]) is not None
    assert patched != stock


def real_centre(image, words, peak, height, first, per_col):
    mem = Memory()
    mem.add(CANVAS, struct.pack('>H', UNTOUCHED) * (CANVAS_ROW_BYTES // 2 * CANVAS_ROWS))
    mem.add(STACK, bytes(0x100))
    mem.add(ARRAY, struct.pack('>%dH' % len(words), *words))
    real_memory(image, mem)
    mem.put32(STACK + 40, first)
    mem.put32(STACK + 12, per_col)
    cpu = Sh2a(mem)
    cpu.r[15] = STACK
    cpu.r[5], cpu.r[8], cpu.r[12] = peak, ARRAY, height
    mem.put32(STACK + 44, 11)
    cpu.r[13], cpu.r[14] = 0x77000000 + 13, CANVAS_ROW_BYTES
    cpu.run(CENTRE_SITE, CENTRE_RESUME)
    return cpu, mem


@needs_real_gui
def test_real_image_stock_words_leave_the_firmware_output_unchanged(real_images):
    stock, patched = real_images
    rng = random.Random(11)
    words = [rng.randrange(0x10000) & ~3 for _ in range(64)]
    for _ in range(40):
        peak, height, per_col = rng.randrange(64), rng.randrange(1, 56), rng.choice((1, 4, 16))
        a, mem_a = real_centre(stock, words, peak, height, peak, per_col)
        b, mem_b = real_centre(patched, words, peak, height, peak, per_col)
        assert canvas_pixels(mem_a) == canvas_pixels(mem_b)
        assert [a.r[k] for k in (8, 12, 13, 14, 15)] == [b.r[k] for k in (8, 12, 13, 14, 15)]
        assert a.r[12] == height


@needs_real_gui
def test_real_image_3band_words_reach_the_mod_through_the_patched_call(real_images):
    _, patched = real_images
    words = [flagged_word(4, 20, 9), flagged_word(10, 2, 31)]
    cpu, mem = real_centre(patched, words, 0, 7, 0, 2)
    fb, tallest = model_centre(words, {11: 0}, 2, [11])
    assert cpu.r[12] == tallest[11]
    assert canvas_pixels(mem) == expect_canvas(fb)


def real_overview(image, record, column=17):
    mem = Memory()
    mem.add(CANVAS, struct.pack('>H', UNTOUCHED) * (OVERVIEW_COLUMNS * OVERVIEW_ROWS))
    mem.add(STACK, bytes(0x100))
    mem.add(0x0E5BB45C, bytes(6 * 600))
    real_memory(image, mem)
    mem.put32(STACK, OVERVIEW_COLUMNS)
    mem.put32(STACK + 4, CANVAS)
    mem.write(0x0E5BB45C + 6 * column, record)
    cpu = Sh2a(mem)
    cpu.r[15] = STACK
    cpu.r[5], cpu.r[6], cpu.r[8] = 0x0E5BB45C + 6 * column, 0xFFFFFFFC, column
    cpu.run(OVERVIEW_SITE, OVERVIEW_RESUME)
    return cpu, mem


@needs_real_gui
def test_real_image_other_overview_records_decode_as_in_the_firmware(real_images):
    stock, patched = real_images
    rng = random.Random(13)
    records = [bytes(rng.randrange(256) for _ in range(6)) for _ in range(30)]
    records += [bytes([5, 5, 5, 0x3B, 0xFF, 0xFE]), bytes([5, 5, 5, 0x3C, 0xFF, 0xFF])]
    for record in records:
        a, mem_a = real_overview(stock, record)
        b, mem_b = real_overview(patched, record)
        assert [a.r[k] for k in (0, 2, 7)] == [b.r[k] for k in (0, 2, 7)]
        assert mem_a.read(CANVAS, 100) == mem_b.read(CANVAS, 100)


@needs_real_gui
def test_real_image_3band_overview_record_is_painted_and_ends_the_column(real_images):
    _, patched = real_images
    record = overview_record(30, 18, 7)
    cpu, mem = real_overview(patched, record, column=17)
    assert cpu.r[2] == 0
    assert overview_pixels(mem) == overview_cells(model_overview([record], first=17))
