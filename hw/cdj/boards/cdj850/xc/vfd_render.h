/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef CDJ850_VFD_RENDER_H
#define CDJ850_VFD_RENDER_H

/* Software reconstruction of the DATA FL glass from IMG_2290..2295.
 * 100x27 dot count measured from a rectified all-on photograph.
 * Placement is photo-calibrated; physical dimensions still need verification.
 * Visible browser letters are reconstructed from the photo; unobserved
 * glyphs remain approximations. No panel font ROM is used.
 * Pure drawing code, shared with the offline reference renderer. */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "vfd_legends.h"
#include "vfd_pitch.h"
#include "vfd_time.h"

#define FL_WIDTH 1280
#define FL_HEIGHT 480
#define FL_COLS 100
#define FL_ROWS 27
#define FL_MATRIX_X 174
#define FL_MATRIX_Y 164
#define FL_PITCH_X 9
#define FL_PITCH_Y 8
#define FL_DOT_W 7
#define FL_DOT_H 6
#define FL_RED_Y 388
#define FL_GREEN_Y 404
#define FL_CUE_Y 436
#define FL_BLUE 0xff9fe8ffu
#define FL_RED 0xffff584cu
#define FL_GREEN 0xff68e4a5u
#define FL_OFF 0xff0a121bu
#define FL_BG 0xff060b10u

/* Explicit data for future protocol mapping. These fields are only populated
 * by offline fixtures until the corresponding panel commands are decoded. */
typedef struct FlPlaying {
    bool artist_known;
    char artist[256];
    bool waveform_known;
    uint8_t waveform[FL_COLS]; /* overview heights; renderer clamps to 5 */
    bool strips_known;
    uint8_t memory[FL_COLS], position[FL_COLS], cue[FL_COLS];
    bool memory_lamp, cue_lamp;
} FlPlaying;

typedef struct FlState {
    char rows[3][256];
    FlPlaying playing;
    uint8_t tags[3];
    uint8_t mode;
    bool has_text;
    Cdj850Time time;
    Cdj850Pitch pitch;
    uint16_t bpm;
    bool bpm_known;
    bool master_tempo;
    bool track_known;
    uint8_t track_number;
    /* Synthetic all-on inspection fixture; never enabled by firmware bytes. */
    bool all_on;
} FlState;

typedef struct FlCanvas {
    uint8_t *pixels;
    int width, height, stride;
} FlCanvas;

static void fl_rect(FlCanvas *c, int x, int y, int w, int h, uint32_t color)
{
    for (int py = y < 0 ? 0 : y; py < y + h && py < c->height; py++) {
        for (int px = x < 0 ? 0 : x; px < x + w && px < c->width; px++) {
            memcpy(c->pixels + py * c->stride + px * 4, &color, 4);
        }
    }
}

/* Row bits, MSB on the left. Original geometric alphabet, not ROM data. */
static const uint8_t fl_alphabet[][7] = {
    {14,17,17,17,17,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
    {14,17,17,17,31,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {15,16,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,25,21,19,19,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
};

/* Lowercase is distinct on the loaded-track photo. Unobserved glyphs are
 * hand-drawn in the same cell and remain provisional. */
static const uint8_t fl_lowercase[26][7] = {
    {0,0,14,1,15,17,15}, {16,16,30,17,17,17,30},
    {0,0,14,16,16,16,14}, {1,1,15,17,17,17,15},
    {0,0,14,17,31,16,14}, {6,9,8,28,8,8,8},
    {0,14,17,17,15,1,14}, {16,16,30,17,17,17,17},
    {4,0,12,4,4,4,14}, {2,0,6,2,2,18,12},
    {16,16,18,20,24,20,18}, {12,4,4,4,4,4,14},
    {0,0,26,21,21,21,21}, {0,0,30,17,17,17,17},
    {0,0,14,17,17,17,14}, {0,0,30,17,30,16,16},
    {0,15,17,17,15,1,1}, {0,0,22,25,16,16,16},
    {0,0,14,16,14,1,30}, {8,8,28,8,8,9,6},
    {0,0,17,17,17,19,13}, {0,0,17,17,17,10,4},
    {0,0,17,17,21,21,10}, {0,0,17,10,4,10,17},
    {0,0,17,17,15,1,14}, {0,0,31,2,4,8,31},
};

static uint8_t fl_glyph(unsigned char ch, int row)
{
    if (ch >= 'a' && ch <= 'z') {
        return fl_lowercase[ch - 'a'][row];
    }
    if (ch >= '0' && ch <= '9') {
        return fl_alphabet[ch - '0'][row];
    }
    if (ch >= 'A' && ch <= 'Z') {
        return fl_alphabet[10 + ch - 'A'][row];
    }
    switch (ch) {
    case ' ': return 0;
    case '.': return row == 6 ? 4 : 0;
    case ':': return row == 2 || row == 5 ? 4 : 0;
    case '-': return row == 3 ? 14 : 0;
    case '_': return row == 6 ? 31 : 0;
    case '/': return 1 << (row * 4 / 6);
    case '[': {
        static const uint8_t bracket[] = {30,28,24,24,24,28,30};
        return bracket[row];
    }
    case ']': {
        static const uint8_t bracket[] = {30,14,6,6,6,14,30};
        return bracket[row];
    }
    case '(': return row == 0 || row == 6 ? 2 : 4;
    case ')': return row == 0 || row == 6 ? 8 : 4;
    case 0xb1: return row == 6 ? 31 : row == 2 ? 14 :
                      row == 1 || row == 3 ? 4 : 0;
    case '+': return row == 3 ? 14 : row >= 1 && row <= 5 ? 4 : 0;
    case '%': return row == 0 || row == 6 ? 17 : 1 << (row * 4 / 6);
    default: return row == 0 ? 14 : row == 1 ? 17 :
                    row == 2 ? 2 : row == 3 || row == 6 ? 4 : 0;
    }
}

/* Fixed glass legends are smooth strokes, not dot-matrix characters.
 * Paths are original approximations on a 5x7 design grid. */
static const char *fl_legend_path(unsigned char ch)
{
    switch (ch) {
    case 'A': return "062046;1434";
    case 'B': return "0006;003041423303;033445463606";
    case 'C': return "4130100105163645";
    case 'D': return "0006;003041453606";
    case 'E': return "40000646;0333";
    case 'F': return "400006;0333";
    case 'G': return "41301001051636454323";
    case 'I': return "1030;2026;1636";
    case 'L': return "000646";
    case 'M': return "0600224046";
    case 'N': return "06004640";
    case 'O': return "103041453606050110";
    case 'P': return "06003041423303";
    case 'R': return "06003041423303;2346";
    case 'S': return "403010010213334445463606";
    case 'T': return "0040;2026";
    case 'U': return "000516364540";
    case 'W': return "0016223640";
    case 'Y': return "002340;2326";
    case '0': return "103041453606050110";
    case '1': return "112026";
    case '6': return "4030100105063645433303";
    case '.': return "2626";
    case 0xb1: return "0323;1114;0626";
    case '%': return "0640;0010110100;3545463635";
    case ' ': return "";
    default: return NULL;
    }
}

static void fl_stroke(FlCanvas *c, int x0, int y0, int x1, int y1,
                      int radius, uint32_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int length2 = dx * dx + dy * dy;
    int xmin = (x0 < x1 ? x0 : x1) - radius;
    int xmax = (x0 > x1 ? x0 : x1) + radius;
    int ymin = (y0 < y1 ? y0 : y1) - radius;
    int ymax = (y0 > y1 ? y0 : y1) + radius;
    if (xmin < 0) xmin = 0;
    if (ymin < 0) ymin = 0;
    if (xmax >= c->width) xmax = c->width - 1;
    if (ymax >= c->height) ymax = c->height - 1;
    for (int y = ymin; y <= ymax; y++) {
        for (int x = xmin; x <= xmax; x++) {
            int ax = x - x0, ay = y - y0;
            int dot = ax * dx + ay * dy;
            int cross = ax * dy - ay * dx;
            bool inside;
            if (dot <= 0 || !length2) {
                inside = ax * ax + ay * ay <= radius * radius;
            } else if (dot >= length2) {
                ax = x - x1; ay = y - y1;
                inside = ax * ax + ay * ay <= radius * radius;
            } else {
                inside = (int64_t)cross * cross <=
                         (int64_t)radius * radius * length2;
            }
            if (inside) fl_rect(c, x, y, 1, 1, color);
        }
    }
}

/* Static legends have individual glass letterforms, not a shared stick
 * alphabet. The masks contain only source-photo letter coverage. Preserve
 * their aspect ratio inside the requested legend rectangle. */
static bool fl_label_mask(FlCanvas *c, int x, int y, const char *text,
                          int width, int height, uint32_t color)
{
    const FlLegendMask *mask = NULL;
    for (unsigned i = 0; i < sizeof(fl_legend_masks)/sizeof(fl_legend_masks[0]); i++) {
        if (!strcmp(text, fl_legend_masks[i].text)) {
            mask = &fl_legend_masks[i];
            break;
        }
    }
    if (!mask) return false;
    if (width <= 0 || height <= 0) return true;
    uint8_t coverage[40][160] = {{0}};
    for (unsigned i = 0; i < mask->count; i++) {
        const uint8_t *run = mask->runs + i*4;
        memset(&coverage[run[0]][run[1]], run[3], run[2]);
    }
    int dw = width, dh = (width * mask->height + mask->width/2) / mask->width;
    if (dh > height) {
        dh = height;
        dw = (height * mask->width + mask->height/2) / mask->height;
    }
    if (!dw || !dh) return true;
    x += (width-dw)/2; y += (height-dh)/2;
    for (int py = 0; py < dh; py++) {
        if (y+py < 0 || y+py >= c->height) continue;
        int sy = dh > 1 ? py * (mask->height-1) * 256 / (dh-1) : 0;
        int iy = sy/256, fy = sy%256;
        int iy1 = iy+1 < mask->height ? iy+1 : iy;
        for (int px = 0; px < dw; px++) {
            if (x+px < 0 || x+px >= c->width) continue;
            int sx = dw > 1 ? px * (mask->width-1) * 256 / (dw-1) : 0;
            int ix = sx/256, fx = sx%256;
            int ix1 = ix+1 < mask->width ? ix+1 : ix;
            int alpha = (coverage[iy][ix]*(256-fx)*(256-fy) +
                         coverage[iy][ix1]*fx*(256-fy) +
                         coverage[iy1][ix]*(256-fx)*fy +
                         coverage[iy1][ix1]*fx*fy + 32768) / 65536;
            if (!alpha) continue;
            uint32_t base, result = 0xff000000u;
            memcpy(&base, c->pixels+(y+py)*c->stride+(x+px)*4, 4);
            for (int shift = 0; shift <= 16; shift += 8) {
                unsigned v = (((base >> shift)&255)*(255-alpha) +
                              ((color >> shift)&255)*alpha + 127) / 255;
                result |= v << shift;
            }
            fl_rect(c, x+px, y+py, 1, 1, result);
        }
    }
    return true;
}

static void fl_label_fit(FlCanvas *c, int x, int y, const char *text,
                         int width, int height, uint32_t color)
{
    if (fl_label_mask(c, x, y, text, width, height, color)) return;
    int len = (int)strlen(text);
    if (!len) return;
    int radius = height >= 10 ? 1 : 0;
    for (int ch = 0; ch < len; ch++) {
        int left = x + ch * width / len;
        int glyph_w = width * 5 / (len * 6) - 2 * radius;
        int glyph_h = height - 1 - 2 * radius;
        const char *p = fl_legend_path((unsigned char)text[ch]);
        if (!p) continue;
        int prev_x = 0, prev_y = 0;
        bool pen = false;
        while (*p) {
            if (*p == ';') { pen = false; p++; continue; }
            int px = left + radius + (p[0] - '0') * glyph_w / 4;
            int py = y + radius + (p[1] - '0') * glyph_h / 6;
            if (pen) fl_stroke(c, prev_x, prev_y, px, py, radius, color);
            prev_x = px; prev_y = py; pen = true; p += 2;
        }
    }
}

static void fl_label_xy(FlCanvas *c, int x, int y, const char *text,
                        int sx, int sy, uint32_t color)
{
    int height = !strcmp(text, "REMAIN") ? 24 : 7 * sy;
    fl_label_fit(c, x, y, text, (int)strlen(text) * 6 * sx, height, color);
}

static void fl_box(FlCanvas *c, int x, int y, int w, int h,
                   const char *label, uint32_t color)
{
    fl_rect(c, x + 3, y, w - 6, 2, color);
    fl_rect(c, x + 3, y + h - 2, w - 6, 2, color);
    fl_rect(c, x, y + 3, 2, h - 6, color);
    fl_rect(c, x + w - 2, y + 3, 2, h - 6, color);
    for (int i = 0; i < 3; i++) {
        fl_rect(c, x + 2 - i, y + i, 2, 1, color);
        fl_rect(c, x + w - 4 + i, y + i, 2, 1, color);
        fl_rect(c, x + 2 - i, y + h - 1 - i, 2, 1, color);
        fl_rect(c, x + w - 4 + i, y + h - 1 - i, 2, 1, color);
    }
    if (!strcmp(label, "TAG LIST")) {
        fl_label_fit(c, x + (w - 48) / 2, y + 7, "TAG", 48, 23, color);
        fl_label_fit(c, x + (w - 52) / 2, y + 35, "LIST", 52, 23, color);
    } else {
        int height = h >= 40 ? 28 : 21;
        int width = w - 12;
        if (!strcmp(label, "INFO")) { width = 56; height = 24; }
        if (!strcmp(label, "MT")) { width = 74; height = 27; }
        if (!strcmp(label, "A.CUE")) { width = 126; height = 24; }
        fl_label_fit(c, x + (w - width) / 2, y + (h - height) / 2,
                     label, width, height, color);
    }
}

/* Convex silhouettes give the digits beveled ends and a slight right lean. */
static void fl_polygon(FlCanvas *c, const int points[][2], int count,
                       uint32_t color)
{
    int xmin = points[0][0], xmax = xmin, ymin = points[0][1], ymax = ymin;
    for (int i = 1; i < count; i++) {
        if (points[i][0] < xmin) xmin = points[i][0];
        if (points[i][0] > xmax) xmax = points[i][0];
        if (points[i][1] < ymin) ymin = points[i][1];
        if (points[i][1] > ymax) ymax = points[i][1];
    }
    if (xmin < 0) xmin = 0;
    if (ymin < 0) ymin = 0;
    if (xmax >= c->width) xmax = c->width - 1;
    if (ymax >= c->height) ymax = c->height - 1;
    for (int y = ymin; y <= ymax; y++) {
        for (int x = xmin; x <= xmax; x++) {
            bool positive = false, negative = false;
            for (int i = 0; i < count; i++) {
                int j = (i + 1) % count;
                int edge = (points[j][0] - points[i][0]) * (y - points[i][1]) -
                           (points[j][1] - points[i][1]) * (x - points[i][0]);
                positive |= edge > 0;
                negative |= edge < 0;
            }
            if (!(positive && negative)) fl_rect(c, x, y, 1, 1, color);
        }
    }
}

/* Seven-segment silhouettes; inactive segments remain barely visible. */
static void fl_digit(FlCanvas *c, int x, int y, int w, int h, int digit)
{
    static const uint8_t masks[] = {
        0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f
    };
    uint8_t bits = digit >= 0 && digit <= 9 ? masks[digit] : 0;
    int t = w / 6;
    int half = h / 2;
    for (int segment = 0; segment < 7; segment++) {
        uint32_t color = bits & (1 << segment) ? FL_BLUE : FL_OFF;
        bool horizontal = segment == 0 || segment == 3 || segment == 6;
        int px = segment == 1 || segment == 2 ? x + w - t : x;
        int py = segment == 0 ? y : segment == 3 ? y + h - t :
                 segment == 6 ? y + half - t / 2 :
                 segment == 2 || segment == 4 ? y + half + 2 : y + t + 2;
        int length = horizontal ? w - 2 * t : half - t - 4;
        int points[6][2];
        int shape[6][2] = {{0,t/2},{t/2,0},{length-t/2,0},
                           {length,t/2},{length-t/2,t},{t/2,t}};
        for (int i = 0; i < 6; i++) {
            int dx = horizontal ? shape[i][0] + t : shape[i][1];
            int dy = horizontal ? shape[i][1] : shape[i][0];
            points[i][0] = (horizontal ? x : px) + dx + (h - (py + dy - y)) / 22;
            points[i][1] = py + dy;
        }
        fl_polygon(c, points, 6, color);
    }
}

/* Pitch glass traced in normalized coordinates from IMG_2295, with the
 * zero/decimal arrangement cross-checked against IMG_2293. This is separate
 * from the taller track/time digits and the printed range-badge lettering. */
typedef struct FlPitch {
    int digits[3];
    bool leading_one, plus, minus;
    unsigned decimals; /* bit 0 after first full digit, bit 1 after second */
} FlPitch;

static void fl_ellipse(FlCanvas *c, int cx, int cy, int rx, int ry,
                       int thickness, uint32_t color)
{
    for (int y = -ry; y <= ry; y++) {
        for (int x = -rx; x <= rx; x++) {
            int ix = rx - thickness, iy = ry - thickness;
            bool outer = x*x*ry*ry + y*y*rx*rx <= rx*rx*ry*ry;
            bool inner = ix > 0 && iy > 0 &&
                         x*x*iy*iy + y*y*ix*ix < ix*ix*iy*iy;
            if (outer && !inner) fl_rect(c, cx+x, cy+y, 1, 1, color);
        }
    }
}

static void fl_pitch_digit(FlCanvas *c, int x, int y, int digit)
{
    /* Four-point end caps, six-point middle, and tapered side segments.
     * Every segment is isolated by a dark seam, including the zero's waist. */
    static const int segments[7][6][2] = {
        {{18,0},{82,0},{70,19},{30,19}},
        {{100,14},{83,32},{83,80},{100,96}},
        {{100,110},{83,126},{83,168},{100,186}},
        {{18,200},{82,200},{70,181},{30,181}},
        {{0,110},{17,126},{17,168},{0,186}},
        {{0,14},{17,32},{17,80},{0,96}},
        {{8,100},{24,90},{76,90},{92,100},{76,110},{24,110}},
    };
    static const unsigned masks[] = {0x3f,0x06,0x5b,0x4f,0x66,
                                     0x6d,0x7d,0x07,0x7f,0x6f};
    unsigned mask = digit >= 0 && digit <= 9 ? masks[digit] : 0;
    for (int segment = 0; segment < 7; segment++) {
        int n = segment == 6 ? 6 : 4;
        int points[6][2];
        for (int i = 0; i < n; i++) {
            int sy = segments[segment][i][1];
            points[i][0] = x + segments[segment][i][0] * 35 / 100 +
                           (200 - sy) * 5 / 200;
            points[i][1] = y + sy * 71 / 200;
        }
        fl_polygon(c, points, n, mask & (1u << segment) ? FL_BLUE : FL_OFF);
    }
}

static void fl_pitch_geometry(FlCanvas *c, const FlPitch *pitch)
{
    for (int i = 0; i < 3; i++) {
        fl_pitch_digit(c, 855 + i * 55, 50, pitch ? pitch->digits[i] : -1);
    }
    /* The leading one has the same diagonal end cuts and center seam. */
    const int one[2][4][2] = {
        {{836,52},{841,58},{839,79},{833,84}},
        {{835,90},{840,96},{838,118},{832,123}},
    };
    for (int half = 0; half < 2; half++) {
        fl_polygon(c, one[half], 4, pitch && pitch->leading_one ? FL_BLUE : FL_OFF);
    }
    uint32_t horizontal = pitch && (pitch->plus || pitch->minus) ? FL_BLUE : FL_OFF;
    uint32_t vertical = pitch && pitch->plus ? FL_BLUE : FL_OFF;
    fl_rect(c, 802, 82, 23, 6, horizontal);
    fl_rect(c, 811, 69, 5, 10, vertical);
    fl_rect(c, 811, 91, 5, 10, vertical);
    for (int i = 0; i < 2; i++) {
        fl_ellipse(c, 906 + i * 54, 119, 4, 4, 4,
                   pitch && (pitch->decimals & (1u << i)) ? FL_BLUE : FL_OFF);
    }
    uint32_t color = pitch ? FL_BLUE : FL_OFF;
    fl_ellipse(c, 1018, 99, 6, 6, 3, color);
    fl_ellipse(c, 1040, 117, 6, 6, 3, color);
    fl_stroke(c, 1017, 123, 1041, 91, 2, color);
}

/* Local optical spread softens the small pitch apertures. Retain the exact
 * core shapes; only luminous pixels contribute, never inactive segments. */
static void fl_pitch(FlCanvas *c, const FlPitch *pitch)
{
    enum { X = 794, Y = 42, W = 256, H = 88 };
    uint8_t luminous[H][W] = {{0}};
    fl_rect(c, X, Y, W, H, FL_BG);
    fl_pitch_geometry(c, pitch);
    for (int y = 0; y < H && Y+y < c->height; y++) {
        for (int x = 0; x < W && X+x < c->width; x++) {
            uint32_t pixel;
            memcpy(&pixel, c->pixels + (Y+y)*c->stride + (X+x)*4, 4);
            luminous[y][x] = pixel == FL_BLUE;
        }
    }
    static const int kernel[] = {1,3,5,3,1};
    for (int y = 0; y < H && Y+y < c->height; y++) {
        for (int x = 0; x < W && X+x < c->width; x++) {
            if (luminous[y][x]) continue;
            int spread = 0;
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (y+dy >= 0 && y+dy < H && x+dx >= 0 && x+dx < W)
                        spread += luminous[y+dy][x+dx] * kernel[dy+2] * kernel[dx+2];
                }
            }
            if (spread) {
                uint32_t base, color = 0xff000000u;
                memcpy(&base, c->pixels + (Y+y)*c->stride + (X+x)*4, 4);
                for (int shift = 0; shift <= 16; shift += 8) {
                    unsigned value = (base >> shift) & 255;
                    value += (((FL_BLUE >> shift) & 255) - value) * spread / (169 * 3);
                    color |= value << shift;
                }
                fl_rect(c, X+x, Y+y, 1, 1, color);
            }
        }
    }
}

/* Range labels are heavy printed reverse lettering. The + has a complete
 * stem and a separate long minus beneath it, unlike the readout sign. */
static void fl_pitch_range(FlCanvas *c, int range, bool active)
{
    int y = range == 16 ? 269 : range == 10 ? 333 : 398;
    uint32_t light = range == 16 ? FL_BLUE : range == 10 ? FL_RED : FL_GREEN;
    uint32_t ink = range == 16 ? 0xff06243cu :
                   range == 10 ? 0xff550e0du : 0xff093b2au;
    uint32_t face = active ? light : FL_OFF;
    fl_rect(c, 1110, y+3, 94, 44, face);
    fl_rect(c, 1113, y, 88, 50, face);
    fl_stroke(c, 1120, y+18, 1139, y+18, 2, active ? ink : FL_BG);
    fl_stroke(c, 1129, y+9, 1129, y+28, 2, active ? ink : FL_BG);
    fl_stroke(c, 1120, y+37, 1139, y+37, 2, active ? ink : FL_BG);
    if (range != 6) {
        fl_stroke(c, 1147, y+15, 1156, y+8, 2, active ? ink : FL_BG);
        fl_stroke(c, 1156, y+8, 1156, y+39, 2, active ? ink : FL_BG);
    }
    int cx = range == 6 ? 1163 : 1180;
    uint32_t color = active ? ink : FL_BG;
    if (range == 10) {
        fl_ellipse(c, cx, y+24, 11, 17, 3, color);
    } else {
        /* Curved bowl and open upper hook of the molded six. */
        fl_ellipse(c, cx, y+30, 11, 10, 3, color);
        const int hook[][2] = {{cx+9,y+13},{cx+6,y+9},{cx,y+8},
                              {cx-6,y+11},{cx-10,y+19},{cx-10,y+29}};
        for (int i = 1; i < 6; i++)
            fl_stroke(c, hook[i-1][0], hook[i-1][1], hook[i][0], hook[i][1], 2, color);
    }
}

/* Browser glyph widths measured in IMG_2292: [=4, I=3, ordinary
 * capitals=5. A single dark column separates cells. Keep the stored 5-bit
 * row masks, trimming their margins instead of stretching narrow glyphs. */
static int fl_glyph_left(unsigned char ch)
{
    return ch == 'I' || ch == 'i' || ch == 'l' ? 1 : ch == '.' || ch == ':' ? 2 : 0;
}

static int fl_glyph_width(unsigned char ch)
{
    switch (ch) {
    case '[': case ']': case 'c': return 4;
    case 'I': case 'i': case 'l': return 3;
    case ' ': return 2;
    case '.': case ':': return 1;
    default: return 5;
    }
}

static int fl_text_width(const char *text)
{
    int width = 0;
    for (; *text; text++) width += fl_glyph_width((unsigned char)*text) + 1;
    return width ? width - 1 : 0;
}

static void fl_matrix_text(uint8_t dots[FL_ROWS][FL_COLS], int x, int y,
                           const char *text, bool inverse)
{
    for (; *text; text++) {
        unsigned char ch = (unsigned char)*text;
        int width = fl_glyph_width(ch), left = fl_glyph_left(ch);
        if (x + width > FL_COLS) break; /* never draw a partial character */
        for (int r = 0; r < 7 && y + r < FL_ROWS; r++) {
            for (int col = 0; col < width; col++) {
                dots[y + r][x + col] =
                    !!(fl_glyph(ch, r) & (16 >> (left + col))) ^ inverse;
            }
        }
        x += width + 1;
    }
}

/* Loaded-track layout: inverted title/note band, artist line and a short
 * bottom-aligned overview. Unmapped data stays absent, rather than borrowing
 * browser rows as metadata or synthesizing a waveform. */
static void fl_playing_matrix(uint8_t dots[FL_ROWS][FL_COLS], const FlState *s)
{
    static const uint8_t note[] = {12,10,10,10,24,56,48};
    static const uint8_t person[] = {14,14,14,4,31,31};
    if (s->rows[0][0]) {
        for (int y = 0; y < 9; y++) memset(dots[y], 1, FL_COLS);
        for (int y = 0; y < 7; y++)
            for (int x = 0; x < 7; x++)
                dots[y+1][x] = !(note[y] & (64 >> x));
        fl_matrix_text(dots, 8, 1, s->rows[0], true);
    }
    if (s->playing.artist_known) {
        for (int y = 0; y < 6; y++)
            for (int x = 0; x < 5; x++)
                dots[y+12][x+1] = !!(person[y] & (16 >> x));
        fl_matrix_text(dots, 8, 11, s->playing.artist, false);
    }
    if (s->playing.waveform_known) {
        for (int x = 0; x < FL_COLS; x++) {
            unsigned height = s->playing.waveform[x];
            if (height > 5) height = 5;
            for (unsigned y = 0; y < height; y++) dots[FL_ROWS-1-y][x] = 1;
        }
    }
}

static void fl_render(FlCanvas *c, const FlState *s)
{
    uint8_t dots[FL_ROWS][FL_COLS] = {{0}};
    fl_rect(c, 0, 0, c->width, c->height, FL_BG);
    /* Numeric positions are calibrated against the all-on reference.
     * Unknown fixed fields stay unlit until their protocol is mapped. */
    bool track_known = s->track_known && s->track_number <= 99;
    fl_digit(c, 35, 40, 44, 86,
             s->all_on ? 8 : track_known ? s->track_number / 10 : -1);
    fl_digit(c, 101, 40, 44, 86,
             s->all_on ? 8 : track_known ? s->track_number % 10 : -1);
    bool remain = s->time.remain_known && s->time.remain;
    fl_label_xy(c, 157, 40, "REMAIN", 4, 3, s->all_on || remain ? FL_BLUE : FL_OFF);
    fl_box(c, 157, 77, 142, 49, "A.CUE", s->all_on ? FL_RED : FL_OFF);
    const int time_x[] = {316, 381, 479, 542, 632, 695};
    const int time_parts[] = {s->time.minutes, s->time.seconds, s->time.frames};
    bool time_known = s->time.known && cdj850_time_in_range(&s->time);
    for (int i = 0; i < 3; i++) {
        fl_digit(c, time_x[2 * i], 40, 44, 86,
                 s->all_on ? 8 : time_known ? time_parts[i] / 10 : -1);
        fl_digit(c, time_x[2 * i + 1], 40, 44, 86,
                 s->all_on ? 8 : time_known ? time_parts[i] % 10 : -1);
    }
    uint32_t time_labels = s->all_on || time_known ? FL_BLUE : FL_OFF;
    fl_rect(c, 449, 65, 6, 6, time_labels);
    fl_rect(c, 449, 95, 6, 6, time_labels);
    fl_label_xy(c, 430, 110, "M", 3, 2, time_labels);
    fl_label_xy(c, 590, 110, "S", 3, 2, time_labels);
    fl_label_xy(c, 742, 110, "F", 3, 2, time_labels);
    const FlPitch all_pitch = {{8,8,8}, true, true, true, 3};
    const Cdj850Pitch *pitch = &s->pitch;
    unsigned magnitude = pitch->value < 0 ? -pitch->value : pitch->value;
    bool wide = pitch->decimals == 1;
    FlPitch readout = {
        .digits = {wide && magnitude < 100 ? -1 : (int)(magnitude / 100 % 10),
                   magnitude / 10 % 10, magnitude % 10},
        .leading_one = magnitude >= 1000,
        .plus = pitch->value > 0,
        .minus = pitch->value < 0,
        .decimals = wide ? 2 : 1,
    };

    fl_pitch(c, s->all_on ? &all_pitch : pitch->decimals ? &readout : NULL);
    fl_box(c, 1050, 35, 150, 86, "", FL_BLUE);
    bool bpm_known = s->bpm_known && s->bpm > 0 && s->bpm <= 999;
    const int bpm_place[] = {100, 10, 1};
    for (int i = 0; i < 3; i++) {
        int digit = bpm_known && s->bpm >= bpm_place[i] ?
                    (s->bpm / bpm_place[i]) % 10 : -1;

        fl_digit(c, 1061 + i * 46, 47, 36, 64, s->all_on ? 8 : digit);
    }
    fl_label_xy(c, 1120, 134, "BPM", 4, 3, FL_BLUE);
    const char *left[] = {"BROWSE", "TAG LIST", "INFO"};
    for (int i = 0; i < 3; i++) {
        bool on = s->all_on || (s->has_text &&
                   (s->mode == 0x0a ? i == 2 : i == 0));
        fl_box(c, 35, 164 + i * 75, 108, 66, left[i], on ? FL_BLUE : FL_OFF);
    }
    /* MEMORY and CUE have opposite angled right edges. */
    for (int i = 0; i < 2; i++) {
        int y = 390 + i * 34;
        bool on = s->all_on || (s->mode == 0x0a &&
                  (i ? s->playing.cue_lamp : s->playing.memory_lamp));
        uint32_t color = on ? (i ? FL_BLUE : FL_RED) : FL_OFF;
        fl_rect(c, 35, y, i ? 100 : 114, 2, color);
        fl_rect(c, 35, y + 28, i ? 114 : 100, 2, color);
        fl_rect(c, 35, y, 2, 30, color);
        for (int dy = 0; dy < 30; dy++) {
            fl_rect(c, 135 + (i ? dy : 29 - dy) / 2, y + dy, 2, 1, color);
        }
        fl_label_fit(c, i ? 60 : 40, y + 5, i ? "CUE" : "MEMORY",
                     i ? 54 : 78, 21, color);
    }
    fl_box(c, 1110, 164, 98, 44, "MT",
           s->all_on || s->master_tempo ? FL_RED : FL_OFF);
    fl_label_xy(c, 1110, 227, "WIDE", 4, 5,
                s->all_on || pitch->range == CDJ850_PITCH_WIDE ? FL_RED : FL_OFF);
    fl_pitch_range(c, 16, s->all_on || pitch->range == 16);
    fl_pitch_range(c, 10, s->all_on || pitch->range == 10);
    fl_pitch_range(c, 6, s->all_on || pitch->range == 6);
    if (s->all_on) {
        memset(dots, 1, sizeof(dots));
    } else if (s->mode == 0x0a) {
        fl_playing_matrix(dots, s);
    } else if (s->has_text) {
        bool message = !s->rows[0][0] && s->rows[1][0] && !s->rows[2][0];
        {
            for (int y = 0; y < FL_ROWS; y++) {
                for (int x = 0; x < 5; x++) {
                    dots[y][x] = message || x == 0 || x == 4 ||
                                 y == 0 || y == FL_ROWS - 1 ||
                                 (s->tags[y / 9] & 0x20);
                }
            }
        }
        if (message) {
            int width = fl_text_width(s->rows[1]);
            if (width > 95) width = 95;
            fl_matrix_text(dots, 5 + (95 - width) / 2, 10, s->rows[1], false);
        } else {
            for (int r = 0; r < 3; r++) {
                bool selected = (s->tags[r] & 0x20) != 0;
                int start = 7;
                if (selected) {
                    for (int y = r * 9; y < r * 9 + 9; y++) {
                        memset(&dots[y][6], 1, FL_COLS - 6);
                    }
                }
                fl_matrix_text(dots, start, r * 9 + 1, s->rows[r], selected);
            }
        }
    }

    /* A small halo joins the apertures optically without changing the font
     * mask. Inverse and ordinary text share the same luminous glass model. */
    for (int y = 0; y < FL_ROWS; y++) {
        for (int x = 0; x < FL_COLS; x++) {
            if (dots[y][x]) {
                fl_rect(c, FL_MATRIX_X + x * FL_PITCH_X - 1,
                        FL_MATRIX_Y + y * FL_PITCH_Y - 1,
                        FL_DOT_W + 2, FL_DOT_H + 2, 0xff234251u);
            }
        }
    }
    for (int y = 0; y < FL_ROWS; y++) {
        for (int x = 0; x < FL_COLS; x++) {
            int px = FL_MATRIX_X + x * FL_PITCH_X;
            int py = FL_MATRIX_Y + y * FL_PITCH_Y;
            uint32_t color = dots[y][x] ? FL_BLUE : FL_OFF;
            fl_rect(c, px + 1, py, FL_DOT_W - 2, FL_DOT_H, color);
            fl_rect(c, px, py + 1, FL_DOT_W, FL_DOT_H - 2, color);
        }
    }
    for (int i = 0; i < FL_COLS; i++) {
        bool known = s->mode == 0x0a && s->playing.strips_known;
        fl_rect(c, FL_MATRIX_X + i * FL_PITCH_X, FL_RED_Y, 7, 12,
                s->all_on || (known && s->playing.memory[i]) ? FL_RED : FL_OFF);
        fl_rect(c, FL_MATRIX_X + i * FL_PITCH_X, FL_GREEN_Y, 7, 28,
                s->all_on || (known && s->playing.position[i]) ? FL_GREEN : FL_OFF);
        fl_rect(c, FL_MATRIX_X + i * FL_PITCH_X, FL_CUE_Y, 7, 14,
                s->all_on || (known && s->playing.cue[i]) ? FL_BLUE : FL_OFF);
    }
}
#endif
