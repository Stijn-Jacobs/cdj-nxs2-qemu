# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the production FL rasterizer without QEMU or firmware."""
import shutil
import subprocess

import pytest

from helpers import path

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include "vfd_render.h"
static uint32_t pixels[FL_HEIGHT][FL_WIDTH];
static void draw(const FlState *state) {
    FlCanvas c = {(uint8_t *)pixels, FL_WIDTH, FL_HEIGHT, FL_WIDTH * 4};
    fl_render(&c, state);
}
static uint32_t dot(int x, int y) {
    return pixels[FL_MATRIX_Y + y * FL_PITCH_Y + 2]
                 [FL_MATRIX_X + x * FL_PITCH_X + 2];
}
int main(void) {
    FlState s = {0};
    draw(&s);
    assert(dot(0, 0) == FL_OFF);
    /* Unknown numeric/flag states never acquire plausible synthetic values. */
    assert(pixels[40][55] == FL_OFF);
    assert(pixels[FL_RED_Y][FL_MATRIX_X] == FL_OFF);
    s.has_text = true;
    strcpy(s.rows[0], "ALPHA");
    strcpy(s.rows[1], "BETA");
    strcpy(s.rows[2], "GAMMA");
    s.tags[0] = 0x25;
    draw(&s);
    assert(dot(0, 26) == FL_BLUE); /* left rail */
    assert(dot(99, 8) == FL_BLUE); /* entire selected band */
    assert(dot(6, 0) == FL_BLUE); /* illuminated margin before first glyph */
    assert(dot(5, 0) == FL_OFF); /* gutter separating the navigation rail */
    assert(dot(99, 17) == FL_OFF);
    assert(dot(2, 4) == FL_BLUE); /* selected part of the navigation rail */
    assert(dot(2, 13) == FL_OFF); /* outline below selected block */
    assert(fl_glyph('a', 0) != fl_glyph('A', 0));
    bool third_row = false;
    for (int y = 19; y < 26; y++) {
        for (int x = 6; x < 36; x++) third_row |= dot(x, y) == FL_BLUE;
    }
    assert(third_row); /* no longer folded into the second line */
    s.tags[0] = 1; s.tags[1] = 0x26;
    draw(&s);
    assert(dot(99, 8) == FL_OFF);
    assert(dot(99, 17) == FL_BLUE);
    /* A new idle state clears selection and old browser content. */
    memset(&s, 0, sizeof(s));
    s.has_text = true;
    strcpy(s.rows[1], "NO DISC");
    draw(&s);
    assert(dot(99, 17) == FL_OFF);
    assert(dot(6, 0) == FL_OFF);
    s.all_on = true;
    draw(&s);
    assert(dot(99, 26) == FL_BLUE);
    assert(pixels[FL_RED_Y][FL_MATRIX_X] == FL_RED);
    assert(pixels[FL_GREEN_Y][FL_MATRIX_X] == FL_GREEN);
    assert(pixels[FL_CUE_Y][FL_MATRIX_X] == FL_BLUE);
    /* Loaded mode reserves separate regions instead of displaying three
     * browser rows. Unmapped metadata and waveform must remain absent. */
    memset(&s, 0, sizeof(s));
    s.mode = 0x0a; s.has_text = true;
    strcpy(s.rows[0], "Escape");
    strcpy(s.rows[1], "UNMAPPED");
    strcpy(s.rows[2], "NOT A WAVEFORM");
    uint8_t playing_dots[FL_ROWS][FL_COLS] = {{0}};
    fl_playing_matrix(playing_dots, &s);
    assert(playing_dots[0][99] == 1); /* title band extends across glass */
    assert(playing_dots[9][99] == 0); /* gap under title */
    assert(playing_dots[11][8] == 0); /* unknown artist stays absent */
    assert(playing_dots[26][0] == 0); /* unknown overview stays absent */
    assert(fl_text_width("DJ ") + 1 == 15); /* P in photo starts at 8+15 */
    s.playing.artist_known = true;
    strcpy(s.playing.artist, "DJ Piyuli vs DJ S...");
    s.playing.waveform_known = true;
    s.playing.waveform[0] = 255; /* malformed heights clamp to five dots */
    s.playing.waveform[99] = 1;
    memset(playing_dots, 0, sizeof(playing_dots));
    fl_playing_matrix(playing_dots, &s);
    assert(playing_dots[12][2] == 1); /* artist icon */
    assert(playing_dots[22][0] == 1 && playing_dots[21][0] == 0);
    assert(playing_dots[26][99] == 1 && playing_dots[25][99] == 0);
    s.playing.strips_known = true;
    s.playing.memory[0] = 1;
    s.playing.position[99] = 1;
    s.playing.cue[0] = 1;
    s.playing.memory_lamp = true;
    s.playing.cue_lamp = true;
    draw(&s);
    assert(pixels[FL_RED_Y][FL_MATRIX_X] == FL_RED);
    assert(pixels[FL_RED_Y][FL_MATRIX_X+FL_PITCH_X] == FL_OFF);
    assert(pixels[FL_GREEN_Y][FL_MATRIX_X+99*FL_PITCH_X] == FL_GREEN);
    assert(pixels[FL_CUE_Y][FL_MATRIX_X] == FL_BLUE);
    s.mode = 0x07; /* A matrix message can overlay a loaded track. */
    draw(&s);
    assert(pixels[390][52] == FL_RED);
    assert(pixels[424][52] == FL_BLUE);
    assert(pixels[FL_RED_Y][FL_MATRIX_X] == FL_RED);
    assert(pixels[FL_CUE_Y][FL_MATRIX_X] == FL_BLUE);
    assert(pixels[FL_GREEN_Y][FL_MATRIX_X+99*FL_PITCH_X] == FL_OFF);
    s.mode = 0x0a;
    draw(&s);
    assert(pixels[314][89] == FL_OFF); /* Loaded text alone lights no INFO lamp. */
    s.mode = 0x07;
    s.info_lamp = true;
    draw(&s);
    assert(pixels[314][89] == FL_BLUE); /* INFO can stay on over a dialog. */
    assert(pixels[164][89] == FL_OFF);
    s.mode = 1;
    s.info_lamp = false;
    draw(&s);
    assert(pixels[164][89] == FL_OFF); /* Browser text alone leaves BROWSE dark. */
    s.browse_lamp = true;
    s.mode = 0x07;
    draw(&s);
    assert(pixels[164][89] == FL_BLUE); /* BROWSE can stay on over a dialog. */
    /* Pitch uses independent physical decimal apertures and a split one. */
    FlPitch zero = {{0,0,0}, false, false, false, 1};
    fl_pitch(&((FlCanvas){(uint8_t *)pixels, FL_WIDTH, FL_HEIGHT, FL_WIDTH*4}), &zero);
    assert(pixels[119][906] == FL_BLUE);
    assert(pixels[119][960] != FL_BLUE);
    assert(pixels[80][813] != FL_BLUE);
    assert(pixels[99][1018] != FL_BLUE); /* hollow percent loop */
    assert(pixels[99][1012] == FL_BLUE); /* loop stroke */
    FlPitch test = {{1,6,0}, true, true, false, 2};
    fl_pitch(&((FlCanvas){(uint8_t *)pixels, FL_WIDTH, FL_HEIGHT, FL_WIDTH*4}), &test);
    assert(pixels[119][906] != FL_BLUE);
    assert(pixels[119][960] == FL_BLUE);
    assert(pixels[73][813] == FL_BLUE); /* upper positive-sign stroke */
    assert(pixels[85][837] != FL_BLUE); /* seam in leading one */
    /* Traced legend resources must remain bounded and retain coverage
     * detail instead of silently falling back to the old stick alphabet. */
    bool remain_found = false, info_found = false;
    for (unsigned i = 0; i < sizeof(fl_legend_masks)/sizeof(fl_legend_masks[0]); i++) {
        const FlLegendMask *mask = &fl_legend_masks[i];
        assert(mask->width > 0 && mask->width <= 160);
        assert(mask->height > 0 && mask->height <= 40);
        assert(mask->count > 0);
        for (unsigned r = 0; r < mask->count; r++) {
            const uint8_t *span = mask->runs + 4*r;
            assert(span[0] < mask->height);
            assert(span[1] + span[2] <= mask->width);
            assert(span[2] > 0 && span[3] > 0);
        }
        remain_found |= !strcmp(mask->text, "REMAIN");
        info_found |= !strcmp(mask->text, "INFO");
    }
    assert(remain_found && info_found);
    /* Respect clipped surfaces and padded strides, even with long text. */
    struct { uint8_t before[32], data[12 * 16], after[32]; } guard;
    memset(&guard, 0xa5, sizeof(guard));
    s.all_on = false;
    memset(s.rows[0], 'W', sizeof(s.rows[0]) - 1);
    s.rows[0][255] = 0;
    FlCanvas c = {guard.data, 2, 12, 16};
    fl_render(&c, &s);
    for (int i = 0; i < 32; i++) {
        assert(guard.before[i] == 0xa5 && guard.after[i] == 0xa5);
    }
    for (int y = 0; y < 12; y++) {
        for (int x = 8; x < 16; x++) assert(guard.data[y * 16 + x] == 0xa5);
    }
    /* Long titles (>60 chars), inverse text, punctuation, high-byte/control
     * glyphs, and larger/smaller padded-stride canvases must stay bounded. */
    memset(&s, 0, sizeof(s));
    s.has_text = true;
    s.mode = 0x01;
    s.tags[0] = 0x25;
    s.tags[1] = 0x02;
    s.tags[2] = 0x03;
    snprintf(s.rows[0], sizeof(s.rows[0]), "%s",
             "VERY LONG TITLE WITH PUNCTUATION [01:23.45] +-_/%() \xb1 END_OVERFLOW_1234567890_ABCDEF");
    for (int i = 0; i < 64; i++) {
        s.rows[1][i] = (char)(0x80 + i * 2);
        s.rows[2][i] = (char)(1 + (i % 31));
    }
    s.rows[1][64] = '\0';
    s.rows[2][64] = '\0';
    draw(&s);
    /* Selection band on row 0 stays lit across the right edge without overflow. */
    assert(dot(99, 8) == FL_BLUE);
    assert(dot(99, 17) == FL_OFF);
    /* Loaded mode (0x0a) with >60-char title and artist containing high bytes. */
    s.mode = 0x0a;
    s.playing.artist_known = true;
    snprintf(s.playing.artist, sizeof(s.playing.artist), "%s",
             "ARTIST NAME OVER 60 CHARS LONG [128 BPM] +-_/%() \xff\xfe\x80\x01 END");
    draw(&s);
    assert(dot(99, 0) == FL_BLUE);
    /* Smaller and larger padded-stride surfaces with guard sentinels. */
    static struct { uint8_t before[64], buf[640 * (1320 * 4 + 64)], after[64]; } dyn;
    memset(&dyn, 0x5a, sizeof(dyn));
    int widths[] = {640, 1320};
    int heights[] = {240, 500};
    for (int k = 0; k < 2; k++) {
        int w = widths[k], h = heights[k], stride = w * 4 + 64;
        memset(&dyn, 0x5a, sizeof(dyn));
        FlCanvas dc = {dyn.buf, w, h, stride};
        fl_render(&dc, &s);
        for (int i = 0; i < 64; i++) {
            assert(dyn.before[i] == 0x5a && dyn.after[i] == 0x5a);
        }
        for (int y = 0; y < h; y++) {
            for (int x = w * 4; x < stride; x++) {
                assert(dyn.buf[y * stride + x] == 0x5a);
            }
        }
        uint32_t bg0 = 0;
        memcpy(&bg0, dyn.buf, 4);
        assert(bg0 == FL_BG);
        if (w > FL_WIDTH) {
            uint32_t edge = 0;
            memcpy(&edge, dyn.buf + (FL_WIDTH + 8) * 4, 4);
            assert(edge == FL_BG);
        }
    }
    puts("render checks passed");
}
'''


def test_display_rasterizer(tmp_path):
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("no C compiler")
    source = tmp_path / "render.c"
    binary = tmp_path / "render"
    source.write_text(HARNESS)
    subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                    "-I", path("hw", "cdj", "boards", "cdj850", "xc"),
                    str(source), "-o", str(binary)], check=True, capture_output=True)
    result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    assert "render checks passed" in result.stdout


@pytest.mark.parametrize("fixture_name", ["cdj850-browser-font.json", "cdj850-playing-font.json"])
def test_matrix_font_against_photo_measurements(tmp_path, fixture_name):
    """Compare logical dots with independently transcribed photo glyph masks."""
    import json

    with open(path("tests", "fixtures", fixture_name)) as reference:
        fixture = json.load(reference)
    cc = shutil.which("cc")
    if not cc:
        pytest.skip("no C compiler")
    # Generate assertions from measured masks, including each character's exact
    # location. Selected rows must be exact inverses and retain all padding.
    checks = []
    start_column = fixture["geometry"]["text_column"]
    for row in fixture["text_rows"]:
        text = row["text"]
        for inverse in (0, 1):
            checks.append(f"memset(dots, {inverse}, sizeof(dots));")
            checks.append(f"fl_matrix_text(dots, {start_column}, 1, {json.dumps(text)}, {inverse});")
            expected = [[inverse] * 100 for _ in range(9)]
            for ch, start in zip(text, row["glyph_columns"]):
                glyph = fixture["glyphs"][ch]
                for y, mask in enumerate(glyph["rows"]):
                    for col in range(glyph["width"]):
                        bit = (mask >> (glyph["width"] - 1 - col)) & 1
                        expected[1 + y][start + col] = bit ^ inverse
            for y, dots_row in enumerate(expected):
                for x, bit in enumerate(dots_row):
                    checks.append(f"assert(dots[{y}][{x}] == {bit});")
            checks.append(f"assert(fl_text_width({json.dumps(text)}) == {row['last_column'] - start_column + 1});")
    source = tmp_path / "browser.c"
    source.write_text('#include <assert.h>\n#include "vfd_render.h"\n'
                      'int main(void) { uint8_t dots[FL_ROWS][FL_COLS];\n' +
                      '\n'.join(checks) + '\nreturn 0; }\n')
    binary = tmp_path / "browser"
    # The other header-local functions are deliberately unused in this harness.
    subprocess.run([cc, "-std=c11", "-O1", "-I",
                    path("hw", "cdj", "boards", "cdj850", "xc"),
                    str(source), "-o", str(binary)], check=True, capture_output=True)
    subprocess.run([str(binary)], check=True, capture_output=True)
