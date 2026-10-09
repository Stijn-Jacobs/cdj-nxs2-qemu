/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj2000/sh7763.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "m16c/m16c63.h"
#include "display_m16c.h"
/*
 * CDJ-900 display processor: the M16C/63 behind MAIN's SCIF2, in its own
 * window. The core is plain C (hw/cdj/m16c/); this file is the QEMU side, a
 * timer that runs the chip on its own 20 MHz clock and a surface for what
 * the chip scans out.
 *
 * The chip scans the glass itself: 60 rows, one every 113.6 us, each sent as
 * 44 bytes out of its UART at 0x2A8. Bytes 0-35 are the 288 data lines of
 * the row (bit n of the row is bit n%8 of byte n/8); bytes 36-43 are a 64-bit
 * little-endian row select with two adjacent bits set, bits 62-r and 63-r for
 * row r. A second UART (0x272) clocks out the lamp rows, of which this window
 * draws the time digits, see m16c_lamp_byte().
 *
 * The glass is a 180 x 45 dot matrix turned on its side: scan row r carries
 * dot columns 3r to 3r+2, and the 45 dot rows are five bands of nine, each
 * band a 7-byte stretch of the data lines, except that the top band starts
 * one byte higher: byte 28 of every row holds a fixed pattern the chip stores
 * after the bands, and the top band's dots are in bytes 29-35. The top band
 * is the source line (disc or USB icon and its name), the three below it the
 * text lines, and the bottom band the overview waveform with its playhead.
 * The chip converts each nine-dot
 * column word to line bits through a table in its own image (word bit b
 * lands on line base-6b, with a base per slot); even scan rows use the first
 * three slots and odd rows the other three, so every dot has exactly one
 * data bit, see lcd_dot().
 *
 * MAIN's link is SCIF2 (cdj_pnl_link.c routes it here while the deck selects
 * the chip): 66-byte frames clocked in one byte at a time, the reply to each
 * byte coming back at once.
 */

#define M16C_BASE       0xC0000
#define M16C_HZ         20000000
#define M16C_QUANTUM_NS (2 * SCALE_MS)
/* The chip may fall this far behind MAIN's clock before it skips ahead. */
#define M16C_MAX_LAG    (M16C_HZ / 10)
/* One link byte on the wire: SCIF2 runs SCBRR 12 at 54 MHz, 8 bits. */
#define M16C_BYTE_CYCLES 160

#define LCD_COLS        180
#define LCD_ROWS        45
/* The time readout under the glass, in glass dots. */
#define TIME_ROWS       14
#define TIME_DIGITS     7
#define LCD_BANDS       5
#define SCAN_ROWS       60
#define SCAN_BYTES      44
#define SCAN_COLS_BYTES 36
#define SCALE           5
#define M16C_FRAME_BYTES 66
/* The longest exchange logged; MAIN's extension blocks are longer than a frame. */
#define M16C_LINK_LOG_BYTES 1024
/* The lamp serial line sends rows of six data bytes and a 16-bit select. */
#define LAMP_ROW_BYTES  8
#define LAMP_SEL_MIN_SEC 0x0001
#define LAMP_SEL_FRAMES  0x0006

typedef struct CdjM16cGui {
    m16c63 *chip;
    QemuConsole *con;
    QEMUTimer *tick;
    bool failed;
    uint8_t scan[SCAN_BYTES];
    unsigned scan_len;
    uint8_t glass[SCAN_ROWS][SCAN_COLS_BYTES];
    uint64_t rows;
    bool dirty;
    unsigned link_log;
    uint8_t link_tx[M16C_LINK_LOG_BYTES], link_rx[M16C_LINK_LOG_BYTES];
    unsigned link_len;
    uint8_t lamp[LAMP_ROW_BYTES];
    unsigned lamp_len;
    uint16_t time_words[2][3];
} CdjM16cGui;

static CdjM16cGui cdj_m16c;

static void m16c_sync(CdjM16cGui *s)
{
    uint64_t due = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000 *
                   (M16C_HZ / 1000000);
    uint64_t now = m16c_cycles(m16c63_cpu(s->chip));
    m16c_stop stop;

    if (s->failed || due <= now) {
        return;
    }
    if (due - now > M16C_MAX_LAG) {
        m16c_skip_cycles(m16c63_cpu(s->chip), due - now - M16C_MAX_LAG);
        now = due - M16C_MAX_LAG;
    }
    stop = m16c63_run(s->chip, due - now);
    if (stop == M16C_STOP_UNDEF) {
        error_report("cdj900 gui: undefined instruction at 0x%05x",
                     m16c_trap_pc(m16c63_cpu(s->chip)));
        s->failed = true;
    }
}

void cdj900_gui_sync(void)
{
    if (cdj_m16c.chip) {
        m16c_sync(&cdj_m16c);
    }
}

static int row_of_select(const uint8_t *sel)
{
    uint64_t v = ldq_le_p(sel);
    unsigned row;

    for (row = 0; row < SCAN_ROWS; row++) {
        if (v == 3ull << (62 - row)) {
            return row;
        }
    }
    return -1;
}

static void m16c_scan_byte(CdjM16cGui *s, uint8_t byte)
{
    int row;

    s->scan[s->scan_len++] = byte;
    if (s->scan_len < SCAN_BYTES) {
        return;
    }
    row = row_of_select(s->scan + SCAN_COLS_BYTES);
    if (row < 0) {
        /* Out of step with the chip's byte count: slide by one byte. */
        memmove(s->scan, s->scan + 1, SCAN_BYTES - 1);
        s->scan_len = SCAN_BYTES - 1;
        return;
    }
    if (memcmp(s->glass[row], s->scan, SCAN_COLS_BYTES)) {
        memcpy(s->glass[row], s->scan, SCAN_COLS_BYTES);
        s->dirty = true;
    }
    s->rows++;
    s->scan_len = 0;
}

/*
 * The chip's second serial line (0x272) clocks out 15 rows of 8 bytes: three
 * little-endian data words, then the row select. Two rows carry the time as
 * seven-segment digits: select 0x0001 holds the minutes and seconds digits
 * (four places), select 0x0006 the frame digits (tens, units and the half
 * frame, which is 0 or 5). The chip builds a row by OR-ing its digit font
 * (ten entries of three words at 0xC469E) into the words, place p shifted
 * right by p; the font sets only bits 3, 7 and 11 of each word. Of the
 * assignments of those seven bit positions to segments, exactly one draws
 * 0-9 as seven-segment digits, time_seg below.
 */
static const struct {
    uint8_t word, bit;
} time_seg[7] = {
    [0] = { 2, 7 },   /* a, top */
    [1] = { 2, 3 },   /* b, top right */
    [2] = { 1, 3 },   /* c, bottom right */
    [3] = { 0, 7 },   /* d, bottom */
    [4] = { 0, 11 },  /* e, bottom left */
    [5] = { 1, 11 },  /* f, top left */
    [6] = { 1, 7 },   /* g, middle */
};

static void m16c_lamp_byte(CdjM16cGui *s, uint8_t byte)
{
    unsigned sel, row;

    s->lamp[s->lamp_len++] = byte;
    if (s->lamp_len < LAMP_ROW_BYTES) {
        return;
    }
    s->lamp_len = 0;
    sel = lduw_le_p(s->lamp + 6);
    if (sel != LAMP_SEL_MIN_SEC && sel != LAMP_SEL_FRAMES) {
        return;
    }
    row = sel == LAMP_SEL_FRAMES;
    for (unsigned k = 0; k < 3; k++) {
        uint16_t word = lduw_le_p(s->lamp + 2 * k);

        if (s->time_words[row][k] != word) {
            s->time_words[row][k] = word;
            s->dirty = true;
        }
    }
}

static void m16c_uart_tx(void *opaque, unsigned unit, uint8_t byte)
{
    if (unit == 2) {
        m16c_scan_byte(opaque, byte);
    } else if (unit == 1) {
        m16c_lamp_byte(opaque, byte);
    }
}

/* The link restart tests P6 bit 4 (link idle). */
#define M16C_P6_LINK_IDLE 0x10

/* The chip's answer-valid line to MAIN is its port 5 bit 6: the link receive
 * handler sets it for a frame that arrived with a good CRC and clears it for
 * one that did not. */
#define M16C_P5_ANSWER_VALID 0x40

static uint8_t m16c_p5_out;

static void m16c_port_out(void *opaque, unsigned port, uint8_t latch,
                          uint8_t dir)
{
    if (port == 5) {
        m16c_p5_out = latch & dir;
    }
}

bool cdj900_gui_answer_valid(void)
{
    m16c_sync(&cdj_m16c);
    return m16c_p5_out & M16C_P5_ANSWER_VALID;
}

/* Port 5 bit 0 selects the run mode at start: read low, the chip stays in a
 * pattern loop driven by the four port 4 inputs (0x0CE358 in the image) and
 * never reaches the command loop that draws MAIN's screens; read high, it
 * runs the command loop (0x0CCBEA). On the board it is a strap, high. */
#define M16C_P5_RUN_COMMAND_LOOP 0x01

static uint8_t m16c_port_in(void *opaque, unsigned port)
{
    if (port == 5) {
        return M16C_P5_RUN_COMMAND_LOOP;
    }
    if (port == 6 && cdj900_gui_link_idle()) {
        return M16C_P6_LINK_IDLE;
    }
    return 0;
}

/* The data line holding dot (x, y) in scan row x / 3. */
static bool lcd_dot(const CdjM16cGui *s, unsigned x, unsigned y)
{
    static const uint8_t slot_bit[6] = { 60, 62, 63, 61, 59, 58 };
    static const int8_t band_byte[LCD_BANDS] = { 28, 20, 13, 6, -1 };
    unsigned row = x / 3;
    unsigned slot = (row & 1) * 3 + x % 3;
    unsigned line = band_byte[y / 9] * 8 + slot_bit[slot] - 6 * (y % 9);

    return s->glass[row][line / 8] >> (line % 8) & 1;
}

/* Whether digit place d of the time readout lights segment seg: places 0-3
 * are the minutes and seconds row, 4-6 the frames row. */
static bool time_segment(const CdjM16cGui *s, unsigned d, unsigned seg)
{
    unsigned row = d >= 4;
    unsigned place = row ? d - 4 : d;

    return s->time_words[row][time_seg[seg].word] >>
           (time_seg[seg].bit - place) & 1;
}

/* Dot (x, y) of the readout: seven digits of 7 x 12 dots, spaced to group
 * minutes, seconds and frames. */
static bool time_dot(const CdjM16cGui *s, unsigned x, unsigned y)
{
    static const uint8_t digit_x[TIME_DIGITS] = { 36, 45, 63, 72, 90, 99, 108 };
    static const uint8_t seg_rect[7][4] = {
        { 1, 0, 5, 2 }, { 5, 1, 2, 5 }, { 5, 6, 2, 5 }, { 1, 10, 5, 2 },
        { 0, 6, 2, 5 }, { 0, 1, 2, 5 }, { 1, 5, 5, 2 },
    };

    for (unsigned d = 0; d < TIME_DIGITS; d++) {
        if (x < digit_x[d] || x >= digit_x[d] + 7 || y < 1 || y > 12) {
            continue;
        }
        for (unsigned seg = 0; seg < 7; seg++) {
            const uint8_t *r = seg_rect[seg];

            if (x - digit_x[d] >= r[0] && x - digit_x[d] < r[0] + r[2] &&
                y - 1 >= r[1] && y - 1 < r[1] + r[3] &&
                time_segment(s, d, seg)) {
                return true;
            }
        }
    }
    return false;
}

static void m16c_update(void *opaque)
{
    CdjM16cGui *s = opaque;
    DisplaySurface *ds = qemu_console_surface(s->con);
    uint32_t *dst = surface_data(ds);
    unsigned y, x, sy, sx;

    if (!s->dirty) {
        return;
    }
    s->dirty = false;
    for (y = 0; y < LCD_ROWS + TIME_ROWS; y++) {
        for (sy = 0; sy < SCALE; sy++) {
            for (x = 0; x < LCD_COLS; x++) {
                bool lit = y < LCD_ROWS ? lcd_dot(s, x, y) :
                                          time_dot(s, x, y - LCD_ROWS);
                uint32_t px = lit ? 0xFF9FE8FF : 0xFF10202A;

                for (sx = 0; sx < SCALE; sx++) {
                    dst[(y * SCALE + sy) * LCD_COLS * SCALE + x * SCALE + sx] = px;
                }
            }
        }
    }
    dpy_gfx_update(s->con, 0, 0, LCD_COLS * SCALE,
                   (LCD_ROWS + TIME_ROWS) * SCALE);
}

static void m16c_invalidate(void *opaque)
{
    ((CdjM16cGui *)opaque)->dirty = true;
}

static const GraphicHwOps m16c_ops = {
    .invalidate = m16c_invalidate,
    .gfx_update = m16c_update,
};

static void m16c_tick(void *opaque)
{
    CdjM16cGui *s = opaque;

    m16c_sync(s);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + M16C_QUANTUM_NS);
}

static void m16c_report(Notifier *n, void *opaque)
{
    info_report("cdj900 gui: %" PRIu64 " display rows scanned",
                cdj_m16c.rows);
}

static void m16c_log_frame(const char *who, const uint8_t *frame, unsigned len)
{
    g_autoptr(GString) hex = g_string_new(NULL);
    unsigned i;

    for (i = 0; i < len; i++) {
        g_string_append_printf(hex, "%02x", frame[i]);
    }
    info_report("cdj900 gui link: %s %s", who, hex->str);
}

uint8_t cdj900_gui_link_byte(uint8_t tx)
{
    CdjM16cGui *s = &cdj_m16c;
    uint8_t rx;

    m16c_sync(s);
    rx = m16c63_link_clock(s->chip, tx);
    m16c63_run(s->chip, M16C_BYTE_CYCLES);
    if (s->link_log && s->link_len < M16C_LINK_LOG_BYTES) {
        s->link_tx[s->link_len] = tx;
        s->link_rx[s->link_len++] = rx;
    }
    return rx;
}

void cdj900_gui_link_end(void)
{
    CdjM16cGui *s = &cdj_m16c;

    if (s->link_log && s->link_len) {
        m16c_log_frame("MAIN", s->link_tx, s->link_len);
        m16c_log_frame("GUI ", s->link_rx, s->link_len);
        s->link_log--;
    }
    s->link_len = 0;
}

/* CDJ_M16C_GUI=<GUI flash image, based at 0xC0000> is the display
 * processor's firmware; MAIN waits on its answers, so the board refuses to
 * start without it. CDJ_M16C_LINK_LOG=<n> logs the
 * first n exchanges with it, each way. */
bool cdj900_gui_init(void)
{
    static Notifier exit_notifier = { .notify = m16c_report };
    static const m16c63_ops ops = {
        .opaque = &cdj_m16c,
        .uart_tx = m16c_uart_tx,
        .port_in = m16c_port_in,
        .port_out = m16c_port_out,
    };
    CdjM16cGui *s = &cdj_m16c;
    const char *path = getenv("CDJ_M16C_GUI");
    uint8_t *flash;
    gsize len;

    if (!path) {
        error_report("cdj900: CDJ_M16C_GUI must name the display processor's "
                     "flash image, MAIN cannot run without it");
        exit(1);
    }
    if (getenv("CDJ_M16C_LINK_LOG")) {
        s->link_log = strtoul(getenv("CDJ_M16C_LINK_LOG"), NULL, 0);
    }
    if (!g_file_get_contents(path, (gchar **)&flash, &len, NULL)) {
        error_report("cdj900 gui: cannot read '%s'", path);
        return false;
    }
    s->chip = m16c63_new(flash, M16C_BASE, len, &ops);

    s->con = graphic_console_init(NULL, 0, &m16c_ops, s);
    dpy_gfx_replace_surface(s->con, qemu_create_displaysurface(
        LCD_COLS * SCALE, (LCD_ROWS + TIME_ROWS) * SCALE));
    qemu_console_resize(s->con, LCD_COLS * SCALE,
                        (LCD_ROWS + TIME_ROWS) * SCALE);
    cdj_gui_pointer_init();
    s->dirty = true;
    qemu_add_exit_notifier(&exit_notifier);

    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, m16c_tick, s);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    return true;
}
