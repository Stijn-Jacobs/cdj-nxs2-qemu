/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "intc.h"
#include "panel_peer.h"
#include "port.h"
#include "scif.h"
#include "trace.h"
#include "vfd.h"

/*
 * Panel CPU (IC1007) as the clock master of the SCIF1 link to MAIN.
 *
 * Established (static reverse engineering of the MAIN firmware; addresses
 * are in the MAIN image, load 0x0C000000):
 *   - Pins (schematic): PE3/SCK1 = TSCK, PE5/TXD1 = TSO, PE4/RXD1 = TSI,
 *     PE9/IRQ5 = PNL_XBUSY (panel to MAIN), PC14 = MAIN_XBUSY (MAIN to
 *     panel). MAIN runs SCIF1 as a clock-synchronous slave on external SCK.
 *   - MAIN sets MAIN_XBUSY once, at the end of the link setup 0x0C0F9F14.
 *     Before that its ISRs are not registered.
 *   - A rising edge of PNL_XBUSY runs the IRQ5 ISR 0x0C0FA05E, which
 *     restarts SCIF1 and preloads 16 TX bytes with TIE on. Each time those
 *     bytes are clocked out TXI1 (ISR 0x0C0FA13C) drains up to 16 RX bytes,
 *     validates them and, depending on its page counter, preloads another
 *     16 TX bytes with TIE on or ends the transaction with TIE off. So a
 *     transaction is 2 or 3 back-to-back 16-byte full-duplex frames.
 *   - Frame 1 must be type 1 and frame 2 type 2; frame 3 is not checked.
 *     Type 1: byte 0 = 0x01, byte 15 = sum of bytes 0..14. Type 2: byte 0 =
 *     0x02, byte 12 = sum of bytes 0..11. The sum is the 8-bit end-around
 *     carry sum of the firmware routine 0x0C0F9EE8. Type 1 bytes 1-2 are
 *     the panel firmware version, shown by MAIN as "Panel : X.YZ".
 *   - Type 1 bytes 3..4 hold the 16-bit big-endian rotary encoder counter,
 *     bytes 5..6 the jog counter, bytes 7..10 two 16-bit ADC values, and
 *     bytes 13..14 button word 0. Type 2 bytes 1..8 hold button words 1..4
 *     (unpacked in MAIN 0x0C0D610C, button table 0x0C194EA4 / 0x0C194FD8).
 *   - MAIN->panel display updates arrive across frame 2 and frame 3 as
 *     0x80 (header/mode) followed by 0x81..0xAE (14-byte payload chunks
 *     carrying 3 records tagged 0x11, 0x12, 0x13 with 3 attribute bytes,
 *     length byte, and text bytes; built in MAIN 0x0C066046 / 0x0C066338).
 *   - A new PNL_XBUSY edge in the middle of a transaction restarts MAIN's
 *     state machine, so this peer never raises one.
 *   - The service manual (10.14, items 7-10) shows PNL_XBUSY as one high
 *     pulse of roughly 0.6 ms and three TSCK bursts of roughly 90 us about
 *     0.75 ms apart, in separately triggered captures.
 *
 * Assumed (nothing in the firmware or the manual decides these):
 *   - The transaction period. The real rate is unknown; CDJ850_PANEL_PERIOD_MS
 *     (default 10) is the idle gap from the end of one transaction to the
 *     next PNL_XBUSY rising edge.
 *   - Intra-transaction timing: frame 1 is clocked 200 us after the edge,
 *     each further frame 750 us after the previous one (the TSCK spacing
 *     from the capture above). If MAIN's ISR has not yet loaded the TX
 *     bytes when a frame is due (emulated interrupt latency is not bounded
 *     by these delays) the peer polls every 50 us for up to 20 ms. PNL_XBUSY
 *     stays high for the whole transaction and drops once MAIN ends it
 *     (TIE off after the last TXI1), or when MAIN did not take part.
 *   - Whether a transaction has 2 or 3 frames is decided here by "MAIN
 *     reloaded 16 TX bytes with TIE set after the previous frame"; how the
 *     real panel decides is not known.
 *   - The panel starts only once MAIN_XBUSY is high, and polls for that at
 *     the transaction period.
 */
#define CDJ850_PANEL_TYPE1      0x01
#define CDJ850_PANEL_TYPE2      0x02
#define CDJ850_PANEL_VER_MAJ    0x01
#define CDJ850_PANEL_VER_MIN    0x00
#define PANEL_FRAME_LEN         16
#define PANEL_NFRAMES           3
#define PANEL_TX_MAX            64      /* up to 48 display frames + hdr/idle */
#define PANEL_FIRST_DELAY_NS    200000
#define PANEL_GAP_NS            750000
#define PANEL_POLL_NS           50000     /* re-check while MAIN's ISR is late */
#define PANEL_POLL_MAX_NS       20000000  /* ... for at most this long */
#define PANEL_PERIOD_MS_DEFAULT 10
#define PNL_XBUSY_IRQ           5
#define PANEL_DISP_ROWS         3
#define PANEL_DISP_BUF          700       /* up to 47 * 14 payload bytes */
#define PANEL_SCRIPT_MAX        256
#define PANEL_HOLD_DEFAULT_MS   120

typedef enum {
    PANEL_IDLE,         /* waiting for MAIN_XBUSY / the next period */
    PANEL_WAIT_FRAME,   /* PNL_XBUSY high, next frame due when the timer fires */
} PanelState;

typedef struct {
    const char *name;
    uint8_t word;       /* 0 -> btn_rx1[0..1], 1..4 -> btn_rx2[(word-1)*2..] */
    uint16_t mask;
} PanelButtonDef;

typedef enum {
    PANEL_ACT_PRESS,
    PANEL_ACT_RELEASE,
    PANEL_ACT_ROTATE,
} PanelActionKind;

typedef struct {
    uint64_t at_ms;
    uint32_t order;
    PanelActionKind kind;
    int32_t delta;
    char name[24];
} PanelScriptEvent;

/* Button map verified against MAIN 0x0C194EA4 / 0x0C194FD8 and UI handlers. */
static const PanelButtonDef panel_buttons[] = {
    /* Word 0 -> frame 1 bytes 13..14 */
    { "enter",          0, 0x0100 },
    { "push",           0, 0x0100 },
    { "jog_touch",      0, 0x2000 },
    { "jog_fwd",        0, 0x4000 },
    { "jog_move",       0, 0x8000 },
    /* Word 1 -> frame 2 bytes 1..2 */
    { "usb_stop",       1, 0x0100 },
    { "play",           1, 0x0200 },
    { "cue",            1, 0x0400 },
    { "loop_in",        1, 0x0800 },
    { "loop_out",       1, 0x1000 },
    { "reloop",         1, 0x2000 },
    { "beat_1",         1, 0x0001 },
    { "beat_2",         1, 0x0002 },
    { "beat_4",         1, 0x0004 },
    { "beat_8",         1, 0x0008 },
    { "beat_select",    1, 0x0010 },
    { "rev",            1, 0x0020 },
    /* Word 2 -> frame 2 bytes 3..4 */
    { "eject",          2, 0x0100 },
    { "vinyl",          2, 0x0200 },
    { "tempo",          2, 0x0800 },
    { "master_tempo",   2, 0x1000 },
    { "call_prev",      2, 0x0001 },
    { "call_next",      2, 0x0002 },
    { "delete",         2, 0x0004 },
    { "memory",         2, 0x0008 },
    /* Word 3 -> frame 2 bytes 5..6 */
    { "time",           3, 0x0100 },
    { "tr_search_prev", 3, 0x0200 },
    { "prev",           3, 0x0200 },
    { "tr_search_next", 3, 0x0400 },
    { "next",           3, 0x0400 },
    { "search_prev",    3, 0x0800 },
    { "search_next",    3, 0x1000 },
    { "link",           3, 0x0001 },
    { "pc",             3, 0x0001 },
    { "usb",            3, 0x0002 },
    { "disc",           3, 0x0004 },
    { "cd",             3, 0x0004 },
    /* Word 4 -> frame 2 bytes 7..8 */
    { "browse",         4, 0x0100 },
    { "tag",            4, 0x0200 },
    { "info",           4, 0x0400 },
    { "menu",           4, 0x0800 },
    { "back",           4, 0x1000 },
    { "tag_track",      4, 0x2000 },
};

static struct {
    QEMUTimer *timer;
    bool enabled;
    PanelState state;
    uint32_t frame;             /* frames exchanged in this transaction */
    uint64_t period_ns;
    uint32_t transactions;      /* completed (>= 2 frames) */
    uint32_t aborted;           /* no frame 1 */
    uint32_t timeouts;          /* MAIN never answered a frame */
    int64_t due_ns;             /* when the pending frame was due */
    uint32_t seq;               /* transaction number, 1-based, for logs */
    bool reported;
    uint8_t last_tx[PANEL_NFRAMES][PANEL_FRAME_LEN];
    uint8_t cur_tx[PANEL_TX_MAX][PANEL_FRAME_LEN];

    /* Panel inputs (frame 1 and frame 2 payloads) */
    uint16_t rotary;
    uint16_t jog;
    uint8_t btn_rx1[2];         /* rx1[13..14] */
    uint8_t btn_rx2[8];         /* rx2[1..8] */

    /* Scripted input schedule */
    PanelScriptEvent script[PANEL_SCRIPT_MAX];
    unsigned script_n;
    unsigned script_next;

    /* Reassembly state for MAIN->panel 0x80..0xAE display frames */
    uint8_t disp_pending_seq;
    uint8_t disp_pending_mode;
    uint8_t disp_next_sub;
    uint8_t disp_buf[PANEL_DISP_BUF];
    unsigned disp_buf_len;
    uint8_t disp_hdr[PANEL_FRAME_LEN];
    bool disp_hdr_pending;
    uint8_t disp_hdr_wait;

    /* Decoded 3-line display state */
    uint8_t disp_mode;
    uint8_t disp_tag[PANEL_DISP_ROWS];
    uint8_t disp_attr[PANEL_DISP_ROWS][3];
    char disp_line[PANEL_DISP_ROWS][256];
    uint32_t disp_updates;

    /* Last logged display state (for change-only info_report) */
    bool disp_logged;
    uint8_t logged_hdr[PANEL_FRAME_LEN];
    uint8_t logged_tag[PANEL_DISP_ROWS];
    uint8_t logged_attr[PANEL_DISP_ROWS][3];
    char logged_line[PANEL_DISP_ROWS][256];
} panel;

static bool panel_strieq(const char *a, const char *b)
{
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z') {
            ca += 'a' - 'A';
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb += 'a' - 'A';
        }
        if (ca != cb) {
            return false;
        }
    }
    return *a == '\0' && *b == '\0';
}

static const PanelButtonDef *panel_find_button(const char *name)
{
    unsigned i;

    if (!name || !*name) {
        return NULL;
    }
    for (i = 0; i < sizeof(panel_buttons) / sizeof(panel_buttons[0]); i++) {
        if (panel_strieq(name, panel_buttons[i].name)) {
            return &panel_buttons[i];
        }
    }
    return NULL;
}

uint8_t cdj850_panel_sum8ec(const uint8_t *p, unsigned n)
{
    unsigned s = 0, i;

    for (i = 0; i < n; i++) {
        s += p[i];
        if (s > 0xFF) {
            s = (s + 1) & 0xFF;
        }
    }
    return s;
}

bool cdj850_panel_peer_set_button(const char *name, bool pressed)
{
    const PanelButtonDef *def = panel_find_button(name);
    uint8_t hi, lo, *pair;

    if (!def) {
        return false;
    }
    hi = (uint8_t)(def->mask >> 8);
    lo = (uint8_t)(def->mask & 0xFF);
    pair = (def->word == 0) ? panel.btn_rx1
                            : &panel.btn_rx2[(def->word - 1) * 2];
    if (pressed) {
        pair[0] |= hi;
        pair[1] |= lo;
    } else {
        pair[0] &= (uint8_t)~hi;
        pair[1] &= (uint8_t)~lo;
    }
    return true;
}

void cdj850_panel_peer_rotate(int delta)
{
    panel.rotary = (uint16_t)((int)panel.rotary + delta);
}

uint16_t cdj850_panel_peer_rotary(void)
{
    return panel.rotary;
}

static void cdj850_panel_build_rx1(uint8_t f[PANEL_FRAME_LEN])
{
    memset(f, 0, PANEL_FRAME_LEN);
    f[0] = CDJ850_PANEL_TYPE1;
    f[1] = CDJ850_PANEL_VER_MAJ;
    f[2] = CDJ850_PANEL_VER_MIN;
    f[3] = (uint8_t)(panel.rotary >> 8);
    f[4] = (uint8_t)panel.rotary;
    f[5] = (uint8_t)(panel.jog >> 8);
    f[6] = (uint8_t)panel.jog;
    f[13] = panel.btn_rx1[0];
    f[14] = panel.btn_rx1[1];
    f[15] = cdj850_panel_sum8ec(f, 15);
}

static void cdj850_panel_build_rx2(uint8_t f[PANEL_FRAME_LEN])
{
    memset(f, 0, PANEL_FRAME_LEN);
    f[0] = CDJ850_PANEL_TYPE2;
    memcpy(&f[1], panel.btn_rx2, 8);
    f[12] = cdj850_panel_sum8ec(f, 12);
}

static void panel_script_run(uint64_t now_ms);

void cdj850_panel_peer_build_rx(unsigned frame, uint8_t f[16])
{
    if (frame == 0) {
        panel_script_run(cdj850_vms());
        cdj850_panel_build_rx1(f);
    } else if (frame == 1) {
        cdj850_panel_build_rx2(f);
    } else {
        memset(f, 0, PANEL_FRAME_LEN);
    }
}

static int panel_script_cmp(const void *a, const void *b)
{
    const PanelScriptEvent *ea = a;
    const PanelScriptEvent *eb = b;

    if (ea->at_ms != eb->at_ms) {
        return ea->at_ms < eb->at_ms ? -1 : 1;
    }
    return ea->order < eb->order ? -1 : (ea->order > eb->order ? 1 : 0);
}

static void panel_script_add(uint64_t at_ms, PanelActionKind kind,
                             const char *name, int32_t delta)
{
    PanelScriptEvent *ev;

    if (panel.script_n >= PANEL_SCRIPT_MAX) {
        info_report("cdj850-panel: script event limit (%u) reached",
                    PANEL_SCRIPT_MAX);
        return;
    }
    ev = &panel.script[panel.script_n];
    memset(ev, 0, sizeof(*ev));
    ev->at_ms = at_ms;
    ev->order = panel.script_n;
    ev->kind = kind;
    ev->delta = delta;
    if (name) {
        snprintf(ev->name, sizeof(ev->name), "%.*s",
                 (int)sizeof(ev->name) - 1, name);
    }
    panel.script_n++;
}

static bool panel_parse_rot_prefix(const char *s, int32_t *delta_out)
{
    char *end = NULL;
    long v;

    if ((s[0] == 'r' || s[0] == 'R') && (s[1] == 'o' || s[1] == 'O') &&
        (s[2] == 't' || s[2] == 'T')) {
        s += 3;
    } else if ((s[0] == 'e' || s[0] == 'E') && (s[1] == 'n' || s[1] == 'N') &&
               (s[2] == 'c' || s[2] == 'C')) {
        s += 3;
    } else {
        return false;
    }
    if (*s != '+' && *s != '-') {
        return false;
    }
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0') {
        return false;
    }
    *delta_out = (int32_t)v;
    return true;
}

static void panel_script_parse(const char *spec)
{
    const char *p = spec;

    if (!spec || !*spec) {
        return;
    }
    while (*p) {
        char buf[64];
        size_t len = 0;
        char *c1, *c2, *end = NULL;
        unsigned long long at_ms;
        const char *action;

        while (*p == ',' || *p == ';' || *p == ' ' || *p == '\t') {
            p++;
        }
        if (!*p) {
            break;
        }
        while (*p && *p != ',' && *p != ';' && *p != ' ' && *p != '\t') {
            if (len + 1 < sizeof(buf)) {
                buf[len++] = *p;
            }
            p++;
        }
        buf[len] = '\0';

        c1 = strchr(buf, ':');
        if (!c1 || c1 == buf || !c1[1]) {
            info_report("cdj850-panel: ignoring malformed script token '%s'",
                        buf);
            continue;
        }
        *c1 = '\0';
        at_ms = strtoull(buf, &end, 10);
        if (!buf[0] || *end != '\0') {
            info_report("cdj850-panel: invalid timestamp in token '%s'", buf);
            continue;
        }
        action = c1 + 1;
        c2 = strchr(action, ':');
        if (c2) {
            *c2 = '\0';
        }

        if (action[0] == '+' || action[0] == '-') {
            bool pressed = (action[0] == '+');
            const char *bname = action + 1;

            if (!panel_find_button(bname)) {
                info_report("cdj850-panel: unknown button '%s'", bname);
                continue;
            }
            panel_script_add(at_ms,
                             pressed ? PANEL_ACT_PRESS : PANEL_ACT_RELEASE,
                             bname, 0);
        } else {
            int32_t rot_delta = 0;

            if (panel_parse_rot_prefix(action, &rot_delta)) {
                panel_script_add(at_ms, PANEL_ACT_ROTATE, NULL, rot_delta);
            } else {
                unsigned long hold_ms = PANEL_HOLD_DEFAULT_MS;

                if (!panel_find_button(action)) {
                    info_report("cdj850-panel: unknown script action '%s'",
                                action);
                    continue;
                }
                if (c2 && c2[1]) {
                    hold_ms = strtoul(c2 + 1, &end, 10);
                    if (*end != '\0' || hold_ms == 0) {
                        hold_ms = PANEL_HOLD_DEFAULT_MS;
                    }
                }
                panel_script_add(at_ms, PANEL_ACT_PRESS, action, 0);
                panel_script_add(at_ms + hold_ms, PANEL_ACT_RELEASE, action, 0);
            }
        }
    }
    if (panel.script_n > 1) {
        qsort(panel.script, panel.script_n, sizeof(panel.script[0]),
              panel_script_cmp);
    }
    if (panel.script_n > 0) {
        info_report("cdj850-panel: loaded %u scripted panel event(s)",
                    panel.script_n);
    }
}

static void panel_script_run(uint64_t now_ms)
{
    while (panel.script_next < panel.script_n &&
           panel.script[panel.script_next].at_ms <= now_ms) {
        const PanelScriptEvent *ev = &panel.script[panel.script_next++];

        switch (ev->kind) {
        case PANEL_ACT_PRESS:
            cdj850_panel_peer_set_button(ev->name, true);
            info_report("cdj850-panel: input press %s at %" PRIu64 " ms",
                        ev->name, now_ms);
            break;
        case PANEL_ACT_RELEASE:
            cdj850_panel_peer_set_button(ev->name, false);
            info_report("cdj850-panel: input release %s at %" PRIu64 " ms",
                        ev->name, now_ms);
            break;
        case PANEL_ACT_ROTATE:
            cdj850_panel_peer_rotate(ev->delta);
            info_report("cdj850-panel: input rotate %+d -> %u at %" PRIu64 " ms",
                        (int)ev->delta, (unsigned)panel.rotary, now_ms);
            break;
        }
    }
}

static bool panel_try_decode_display(void)
{
    unsigned p0 = 0, p1, p2, end;
    unsigned pos[PANEL_DISP_ROWS];
    unsigned r;
    bool changed = !panel.disp_logged;

    if (panel.disp_buf_len < 15) {
        return false;
    }
    if ((panel.disp_buf[p0] & 0x03) != 1) {
        return false;
    }
    p1 = p0 + 5u + panel.disp_buf[p0 + 4];
    if (p1 + 5u > panel.disp_buf_len || (panel.disp_buf[p1] & 0x03) != 2) {
        return false;
    }
    p2 = p1 + 5u + panel.disp_buf[p1 + 4];
    if (p2 + 5u > panel.disp_buf_len || (panel.disp_buf[p2] & 0x03) != 3) {
        return false;
    }
    end = p2 + 5u + panel.disp_buf[p2 + 4];
    if (panel.disp_buf_len < end) {
        return false;
    }

    pos[0] = p0;
    pos[1] = p1;
    pos[2] = p2;
    for (r = 0; r < PANEL_DISP_ROWS; r++) {
        unsigned pr = pos[r];
        unsigned len = panel.disp_buf[pr + 4];
        unsigned i;

        panel.disp_tag[r] = panel.disp_buf[pr];
        panel.disp_attr[r][0] = panel.disp_buf[pr + 1];
        panel.disp_attr[r][1] = panel.disp_buf[pr + 2];
        panel.disp_attr[r][2] = panel.disp_buf[pr + 3];
        for (i = 0; i < len && i < sizeof(panel.disp_line[r]) - 1; i++) {
            uint8_t ch = panel.disp_buf[pr + 5 + i];

            panel.disp_line[r][i] = (ch >= 0x20 && ch <= 0x7e) ? (char)ch
                                                               : '.';
        }
        panel.disp_line[r][i] = '\0';
        if (panel.disp_tag[r] != panel.logged_tag[r] ||
            memcmp(panel.disp_attr[r], panel.logged_attr[r], 3) != 0 ||
            strcmp(panel.disp_line[r], panel.logged_line[r]) != 0) {
            changed = true;
        }
    }

    panel.disp_mode = panel.disp_pending_mode;
    panel.disp_updates++;
    panel.disp_next_sub = 0;
    panel.disp_hdr_pending = false;
    panel.disp_hdr_wait = 0;
    vfd_note_panel_text(panel.disp_mode, panel.disp_line[0],
                        panel.disp_line[1], panel.disp_line[2]);

    if (panel.disp_logged &&
        memcmp(panel.disp_hdr + 2, panel.logged_hdr + 2, 13) != 0) {
        changed = true;
    }
    if (changed) {
        panel.disp_logged = true;
        memcpy(panel.logged_hdr, panel.disp_hdr, sizeof(panel.logged_hdr));
        memcpy(panel.logged_tag, panel.disp_tag, sizeof(panel.logged_tag));
        memcpy(panel.logged_attr, panel.disp_attr, sizeof(panel.logged_attr));
        memcpy(panel.logged_line, panel.disp_line, sizeof(panel.logged_line));
        info_report("cdj850-panel: display mode=0x%02x tag=%02x/%02x/%02x "
                    "attr=%02x:%02x:%02x/%02x:%02x:%02x/%02x:%02x:%02x "
                    "[%s] | [%s] | [%s] (%" PRIu64 " ms)",
                    panel.disp_mode,
                    panel.disp_tag[0], panel.disp_tag[1], panel.disp_tag[2],
                    panel.disp_attr[0][0], panel.disp_attr[0][1],
                    panel.disp_attr[0][2],
                    panel.disp_attr[1][0], panel.disp_attr[1][1],
                    panel.disp_attr[1][2],
                    panel.disp_attr[2][0], panel.disp_attr[2][1],
                    panel.disp_attr[2][2],
                    panel.disp_line[0], panel.disp_line[1],
                    panel.disp_line[2], cdj850_vms());
    }
    return true;
}

static void panel_flush_hdr_only(void)
{
    if (!panel.disp_hdr_pending) {
        return;
    }
    panel.disp_hdr_pending = false;
    panel.disp_hdr_wait = 0;
    if (!panel.disp_logged ||
        memcmp(panel.disp_hdr + 2, panel.logged_hdr + 2, 13) != 0) {
        panel.disp_logged = true;
        memcpy(panel.logged_hdr, panel.disp_hdr, sizeof(panel.logged_hdr));
        vfd_note_panel_text(panel.disp_mode, panel.disp_line[0],
                            panel.disp_line[1], panel.disp_line[2]);
        info_report("cdj850-panel: display hdr mode=0x%02x "
                    "bytes=%02x %02x %02x %02x %02x %02x %02x %02x %02x "
                    "%02x %02x %02x (%" PRIu64 " ms)",
                    panel.disp_mode,
                    panel.disp_hdr[3], panel.disp_hdr[4], panel.disp_hdr[5],
                    panel.disp_hdr[6], panel.disp_hdr[7], panel.disp_hdr[8],
                    panel.disp_hdr[9], panel.disp_hdr[10], panel.disp_hdr[11],
                    panel.disp_hdr[12], panel.disp_hdr[13], panel.disp_hdr[14],
                    cdj850_vms());
    }
}

void cdj850_panel_peer_feed_display_tx(const uint8_t tx[16])
{
    uint8_t sub;

    if (!tx || cdj850_panel_sum8ec(tx, 15) != tx[15]) {
        return;
    }
    if (tx[0] == 0x80) {
        if (panel.disp_hdr_pending) {
            panel_flush_hdr_only();
        }
        panel.disp_pending_seq = tx[1];
        panel.disp_pending_mode = tx[2];
        panel.disp_mode = tx[2];
        panel.disp_buf_len = 0;
        panel.disp_next_sub = 1;
        memcpy(panel.disp_hdr, tx, PANEL_FRAME_LEN);
        panel.disp_hdr_pending = true;
        panel.disp_hdr_wait = 0;
        return;
    }
    if (tx[0] >= 0x81 && tx[0] <= 0xAF) {
        sub = (uint8_t)(tx[0] - 0x80);
        if (sub == 1 && panel.disp_next_sub != 1) {
            panel.disp_buf_len = 0;
            panel.disp_next_sub = 1;
        }
        if (panel.disp_next_sub == 0 || sub != panel.disp_next_sub) {
            return;
        }
        panel.disp_hdr_pending = false;
        panel.disp_hdr_wait = 0;
        if (panel.disp_buf_len + 14u > sizeof(panel.disp_buf)) {
            panel.disp_next_sub = 0;
            panel.disp_buf_len = 0;
            return;
        }
        memcpy(panel.disp_buf + panel.disp_buf_len, &tx[1], 14);
        panel.disp_buf_len += 14u;
        panel.disp_next_sub++;
        panel_try_decode_display();
    }
}

uint8_t cdj850_panel_peer_display_mode(void)
{
    return panel.disp_mode;
}

const char *cdj850_panel_peer_display_line(unsigned row)
{
    return row < PANEL_DISP_ROWS ? panel.disp_line[row] : "";
}

uint8_t cdj850_panel_peer_display_attr(unsigned row)
{
    return row < PANEL_DISP_ROWS ? panel.disp_attr[row][0] : 0;
}

unsigned cdj850_panel_peer_display_updates(void)
{
    return panel.disp_updates;
}

static void panel_hex(char *out, const uint8_t *f)
{
    unsigned i;

    for (i = 0; i < PANEL_FRAME_LEN; i++) {
        sprintf(out + 3 * i, "%02x ", f[i]);
    }
    out[3 * PANEL_FRAME_LEN - 1] = 0;
}

static void panel_schedule(int64_t delay_ns)
{
    timer_mod(panel.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static void panel_end_transaction(void)
{
    cdj850_intc_set_pin(PNL_XBUSY_IRQ, false);
    if (panel.frame >= 2) {
        unsigned i;

        panel.transactions++;
        memcpy(panel.last_tx, panel.cur_tx, sizeof(panel.last_tx));
        for (i = 0; i < panel.frame; i++) {
            cdj850_panel_peer_feed_display_tx(panel.cur_tx[i]);
        }
        if (panel.disp_hdr_pending && ++panel.disp_hdr_wait >= 2) {
            panel_flush_hdr_only();
        }
        if (!panel.reported) {
            panel.reported = true;
            info_report("cdj850: panel peer: first full transaction "
                        "completed (%u frames) at %" PRIu64 " ms",
                        panel.frame, cdj850_vms());
        }
    } else {
        panel.aborted++;
    }
    panel.state = PANEL_IDLE;
    panel_schedule(panel.period_ns);
}

static void panel_exchange(void)
{
    uint8_t rx[PANEL_FRAME_LEN] = { 0 };
    uint8_t tx[PANEL_FRAME_LEN];
    unsigned n = panel.frame;       /* 0-based index of this frame */

    cdj850_panel_peer_build_rx(n, rx);
    cdj850_scif1_exchange(rx, tx);
    memcpy(panel.cur_tx[n], tx, PANEL_FRAME_LEN);
    if (cdj850_trace()) {
        char hx[3 * PANEL_FRAME_LEN];

        panel_hex(hx, tx);
        info_report("cdj850: panel txn %u frame %u MAIN->panel %s %"
                    PRIu64 " ms", panel.seq, n + 1, hx, cdj850_vms());
        panel_hex(hx, rx);
        info_report("cdj850: panel txn %u frame %u panel->MAIN %s %"
                    PRIu64 " ms", panel.seq, n + 1, hx, cdj850_vms());
    }
    panel.frame++;
}

static bool panel_main_ready(void)
{
    return cdj850_scif1_tx_pending() == PANEL_FRAME_LEN &&
           cdj850_scif1_tie();
}

/*
 * A frame is due. MAIN's ISR may not have run yet (interrupt latency in the
 * emulated CPU is not bounded by the 200/750 us delays), so the peer polls
 * for MAIN's answer up to PANEL_POLL_MAX_NS before it decides:
 *   MAIN reloaded 16 bytes with TIE set  -> clock the next frame;
 *   TIE dropped (the TXI1 ISR ended the transaction, spec section 3, or a
 *   frame-1 request that never came)       -> transaction over;
 *   neither within the limit               -> transaction over.
 * TIE still set with an empty TX FIFO means the TXI1 ISR has not run yet.
 */
static void panel_tick(void *opaque)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    (void)opaque;

    if (panel.state == PANEL_IDLE) {
        if (!cdj850_port_main_xbusy()) {
            panel_schedule(panel.period_ns);
            return;
        }
        panel.seq++;
        panel.frame = 0;
        memset(panel.cur_tx, 0, sizeof(panel.cur_tx));
        panel.state = PANEL_WAIT_FRAME;
        panel.due_ns = now + PANEL_FIRST_DELAY_NS;
        cdj850_intc_set_pin(PNL_XBUSY_IRQ, true);
        panel_schedule(PANEL_FIRST_DELAY_NS);
        return;
    }

    if (panel_main_ready() && panel.frame < PANEL_TX_MAX) {
        panel_exchange();
        panel.due_ns = now + PANEL_GAP_NS;
        panel_schedule(PANEL_GAP_NS);
        return;
    }
    if (now < panel.due_ns) {       /* woke early (restored timer) */
        panel_schedule(panel.due_ns - now);
        return;
    }
    if ((panel.frame == 0 || cdj850_scif1_tie()) &&
        now - panel.due_ns < PANEL_POLL_MAX_NS) {
        panel_schedule(PANEL_POLL_NS);
        return;
    }
    if (panel.frame == 0 && cdj850_trace()) {
        info_report("cdj850: panel txn %u: MAIN has not loaded frame 1 "
                    "(tx=%u tie=%d), dropping PNL_XBUSY %" PRIu64 " ms",
                    panel.seq, cdj850_scif1_tx_pending(), cdj850_scif1_tie(),
                    cdj850_vms());
    }
    if (panel.frame > 0 && cdj850_scif1_tie()) {
        panel.timeouts++;       /* MAIN left TIE set: no end-of-transaction */
        if (cdj850_trace()) {
            info_report("cdj850: panel txn %u: no answer from MAIN after "
                        "frame %u, ending %" PRIu64 " ms", panel.seq,
                        panel.frame, cdj850_vms());
        }
    }
    panel_end_transaction();
}

static const VMStateDescription vmstate_cdj850_panel = {
    .name = "cdj850-panel-peer",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, typeof(panel)),
        VMSTATE_UINT32(state, typeof(panel)),
        VMSTATE_UINT32(frame, typeof(panel)),
        VMSTATE_UINT32(transactions, typeof(panel)),
        VMSTATE_UINT32(aborted, typeof(panel)),
        VMSTATE_UINT32(timeouts, typeof(panel)),
        VMSTATE_INT64(due_ns, typeof(panel)),
        VMSTATE_UINT32(seq, typeof(panel)),
        VMSTATE_BOOL(reported, typeof(panel)),
        VMSTATE_UINT8_2DARRAY(last_tx, typeof(panel), PANEL_NFRAMES,
                              PANEL_FRAME_LEN),
        VMSTATE_UINT8_2DARRAY(cur_tx, typeof(panel), PANEL_TX_MAX,
                              PANEL_FRAME_LEN),
        VMSTATE_UINT16_V(rotary, typeof(panel), 2),
        VMSTATE_UINT16_V(jog, typeof(panel), 2),
        VMSTATE_UINT8_ARRAY_V(btn_rx1, typeof(panel), 2, 2),
        VMSTATE_UINT8_ARRAY_V(btn_rx2, typeof(panel), 8, 2),
        VMSTATE_END_OF_LIST()
    }
};

unsigned cdj850_panel_peer_transactions(void)
{
    return panel.transactions;
}

const uint8_t *cdj850_panel_peer_last_tx(unsigned n)
{
    return n < PANEL_NFRAMES ? panel.last_tx[n] : NULL;
}

void cdj850_panel_peer_init(void)
{
    const char *e = getenv("CDJ850_PANEL");
    const char *p = getenv("CDJ850_PANEL_PERIOD_MS");
    const char *s = getenv("CDJ850_PANEL_SCRIPT");
    unsigned ms = PANEL_PERIOD_MS_DEFAULT;

    memset(&panel, 0, sizeof(panel));
    if (e && *e == '0') {
        info_report("cdj850: panel peer disabled (CDJ850_PANEL=0)");
        return;
    }
    if (p && *p) {
        ms = strtoul(p, NULL, 0);
        if (!ms) {
            ms = PANEL_PERIOD_MS_DEFAULT;
        }
    }
    panel.enabled = true;
    panel.period_ns = (uint64_t)ms * 1000000;
    panel.state = PANEL_IDLE;
    panel_script_parse(s);
    panel.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, panel_tick, NULL);
    vmstate_register_any(NULL, &vmstate_cdj850_panel, &panel);
    panel_schedule(panel.period_ns);
}
