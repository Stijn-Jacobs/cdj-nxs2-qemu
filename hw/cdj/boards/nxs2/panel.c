/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/*
 * SCIF2 (0xFFE20000): the front-panel MCU link, with the panel modelled as the
 * peer. Keys, jog, touch and the browse encoder reach MAIN this way.
 *
 * Firmware tasks PnlCom_SndTASK (0x0844F3C4) and PnlCom_RcvTASK (0x0844C990),
 * task table at 0x080EA78C. MAIN initiates a full-duplex 40-byte exchange over
 * DMA, set up by 0x083EAEC2(mode=1, rx=0xA9000000, tx=0xA9000028, len=0x28):
 *
 *   FIFO resets, then
 *   ch2 (tx): SAR=0xA9000028  DAR=0xFFE2000C (SCFTDR2)  TCR=0x28  CHCR=0x40001800
 *   ch1 (rx): SAR=0xFFE20014 (SCFRDR2)  DAR=0xA9000000  TCR=0x28  CHCR=0x40004800
 *   DMAOR |= 1, then CHCR_2 DE, then CHCR_1 DE, then SCSCR2 |= REIE|RE|TE.
 *
 * cdj_dmac_run() runs synchronously on the CHCR DE store and tx is armed
 * before rx, so the 40th SCFTDR2 write builds the reply before the rx channel
 * reads it. The panel only ever answers; it never sends unsolicited frames.
 *
 * Frame (validator 0x0844CA24..0x0844CA8A, builder 0x0844F740):
 *   [0x00..0x25] payload, [0x26] checksum, [0x27] sync = 0x8F.
 * A bad frame takes the reject path at 0x0844CBA8, which bumps a counter
 * (saturating at 0x50) and waits for the next one.
 *
 * Keys are active high (0x0844D8D6), so an all-zero payload means nothing
 * pressed. The non-key payload bytes of the idle frame are a guess; a real
 * panel's idle frame is probably not all zeros.
 */
#define CDJ_PNL_SIZE        0x1000
#define CDJ_PNL_FRAME       0x28    /* whole frame, both directions          */
#define CDJ_PNL_PAYLOAD     0x26    /* bytes covered by the checksum         */
QEMU_BUILD_BUG_ON(CDJ_STATEOUT_PNL_FRAME != CDJ_PNL_FRAME);
#define CDJ_PNL_SYNC        0x8F

#define CDJ_PNL_SCSMR       0x00
#define CDJ_PNL_SCBRR       0x04
#define CDJ_PNL_SCSCR       0x08
#define CDJ_PNL_SCFTDR      0x0C    /* write-only: transmit FIFO data        */
#define CDJ_PNL_SCFSR       0x10
#define CDJ_PNL_SCFRDR      0x14    /* read-only: receive FIFO data          */
#define CDJ_PNL_SCFCR       0x18
#define CDJ_PNL_SCFDR       0x1C    /* read-only: FIFO fill levels           */
#define CDJ_PNL_SCLSR       0x24

#define CDJ_PNL_SCFSR_RDF   0x0002
#define CDJ_PNL_SCFSR_TDFE  0x0020
#define CDJ_PNL_SCFSR_TEND  0x0040
/* Status bits the peer owns; a guest write may clear them but never set them. */
#define CDJ_PNL_SCFSR_HW    0x009F  /* ER|BRK|FER|PER|RDF|DR                 */

#define CDJ_PNL_SCFCR_RFRST 0x0002
#define CDJ_PNL_SCFCR_TFRST 0x0004

/* One scheduled key press, from CDJ_PANEL_PRESS=<off>:<mask>:<at_ms>:<dur_ms>
 * (comma-separated list). Offsets are into the 38-byte report; BROWSE is
 * 0x14:0x01. */
typedef struct CdjPnlPress {
    unsigned off;
    uint8_t mask;
    int64_t at_ms;
    int64_t dur_ms;
    bool seen;
    /* Down until a matching "rel"; dur_ms is ignored meanwhile. A separate
     * flag because a zero-duration CDJ_PANEL_PRESS must stay a no-op. */
    bool held;
} CdjPnlPress;

typedef struct CdjPnlState {
    MemoryRegion iomem;
    uint16_t reg[CDJ_PNL_SIZE / 4];
    uint8_t rx[CDJ_PNL_FRAME];
    unsigned rxhead;                /* next byte the guest will pop          */
    unsigned rxlen;                 /* bytes still queued                    */
    unsigned txlen;                 /* bytes staged toward the next frame    */
    uint8_t tx[CDJ_PNL_FRAME];
    CdjPnlPress *press;
    unsigned npress;
    int keyfd;                      /* live key socket; -2 = not opened yet   */
    unsigned rot_off;               /* report byte the select knob counts in  */
    uint8_t rot_val;
    bool rot_live;
    uint8_t lvl_val[CDJ_PNL_PAYLOAD];   /* analogue fields: absolute, held    */
    bool lvl_live[CDJ_PNL_PAYLOAD];
    uint64_t frames_sent;           /* replies the panel loaded              */
    uint64_t frames_recv;           /* complete frames MAIN transmitted      */
    uint64_t starved;               /* SCFRDR reads with an empty FIFO       */
    uint64_t reads;
    uint64_t writes;
    Notifier exit;
} CdjPnlState;

static CdjPnlState *cdj_pnl;
static bool cdj_pnl_reglog;
static uint64_t cdj_pnl_dbg_first = 1;
static uint64_t cdj_pnl_dbg_n;
static bool cdj_pnl_fill_on;
static uint8_t cdj_pnl_fill;
static unsigned cdj_pnl_fill_lo, cdj_pnl_fill_hi = 0xFFFF;
static uint64_t cdj_pnl_wcount[64];
static uint64_t cdj_pnl_wnz[64];

/* 8-bit sum with end-around carry, as the firmware's validator computes it. */
static uint8_t cdj_pnl_checksum(const uint8_t *frame)
{
    unsigned sum = 0;
    unsigned i;

    for (i = 0; i < CDJ_PNL_PAYLOAD; i++) {
        sum += frame[i];
        if (sum & 0x100) {
            sum = (sum & 0xFF) + 1;
        }
    }
    return (uint8_t)sum;
}

static void cdj_pnl_parse_presses(CdjPnlState *s)
{
    const char *spec = getenv("CDJ_PANEL_PRESS");
    g_auto(GStrv) items = NULL;
    unsigned i;

    if (!spec || !*spec) {
        return;
    }
    items = g_strsplit(spec, ",", 0);
    for (i = 0; items[i]; i++) {
        g_auto(GStrv) f = g_strsplit(items[i], ":", 0);

        if (!f[0] || !f[1] || !f[2] || !f[3]) {
            warn_report("panel: ignoring malformed CDJ_PANEL_PRESS entry '%s' "
                        "(want <off>:<mask>:<at_ms>:<dur_ms>)", items[i]);
            continue;
        }
        s->press = g_renew(CdjPnlPress, s->press, s->npress + 1);
        s->press[s->npress] = (CdjPnlPress) {
            .off    = (unsigned)strtoul(f[0], NULL, 0),
            .mask   = (uint8_t)strtoul(f[1], NULL, 0),
            .at_ms  = (int64_t)strtoll(f[2], NULL, 0),
            .dur_ms = (int64_t)strtoll(f[3], NULL, 0),
        };
        if (s->press[s->npress].off >= CDJ_PNL_PAYLOAD) {
            warn_report("panel: CDJ_PANEL_PRESS offset 0x%x is outside the "
                        "38-byte report, ignored", s->press[s->npress].off);
            continue;
        }
        s->npress++;
    }
}

/*
 * A PLAY press requested by the decoder model so the transport follows the
 * cue key (see cdj_dsp_engine_key()). Delayed 60 ms so it never shares a
 * report with the cue press.
 */
static void cdj_pnl_inject_play(CdjPnlState *s, int64_t now)
{
    s->press = g_renew(CdjPnlPress, s->press, s->npress + 1);
    s->press[s->npress++] = (CdjPnlPress) {
        .off = cdj_dsp_eng.play_off, .mask = (uint8_t)cdj_dsp_eng.play_mask,
        .at_ms = now + 60, .dur_ms = 120,
    };
    info_report("cue key: PLAY toggle injected for %" PRId64 " ms", now + 60);
}

/*
 * Touch screen (CDJ_TOUCH=1). The panel MCU sends two raw 10-bit values:
 * X = BE16 report[0x16..0x17], Y = BE16 report[0x18..0x19], read by the key
 * decoder at 0x0844E164. Below 32 on either axis is pen-up. Host pixels are
 * converted with the inverse of the firmware's default calibration
 * (0x084C37E4).
 *
 * The firmware's filter (0x0844F016) drops the first three pen-down frames and
 * repeats the last position for up to 12 pen-up frames, so each press is held
 * for at least CDJ_TOUCH_MIN_FRAMES frames and presses are spaced by
 * CDJ_TOUCH_GAP_FRAMES pen-up frames.
 */
#define CDJ_TOUCH_PEN_UP_BELOW   32
#define CDJ_TOUCH_RAW_X          0x16
#define CDJ_TOUCH_RAW_Y          0x18

typedef struct CdjPnlTouch {
    bool on;                        /* CDJ_TOUCH set                          */
    CdjTouch host;                  /* latest message, LCD pixels             */
    uint16_t rx, ry;                /* host position in raw panel units       */
    bool want_down;                 /* the host's finger is down              */
    bool latched;                   /* a press not yet reported to the guest  */
    int64_t tap_until_ms;           /* timed tap: release at; 0 = none        */
    bool down;                      /* what the report is carrying            */
    unsigned down_frames, up_frames;
    unsigned min_frames, gap_frames;
    uint64_t presses;
} CdjPnlTouch;

static CdjPnlTouch cdj_pnl_touch;

/* Raw -> pixel as 0x084C37E4 does with its default constants; 0 off-screen. */
static int cdj_touch_cal_x(int rx)
{
    int x = (int)(794.0 - (790.0 * rx - 23700.0) / 961.0);

    return x >= 0 && x <= CDJ_TOUCH_W ? x : 0;
}

static int cdj_touch_cal_y(int ry)
{
    int y = (int)((469.0 * ry - 18760.0) / 921.0 + 5.0);

    return y >= 0 && y <= CDJ_TOUCH_H ? y : 0;
}

/* Invert one axis, nudging by one count so the firmware's truncating forward
 * transform lands on the pixel. (400,240) -> 01 fd 01 f6. */
static uint16_t cdj_touch_invert(int px, double approx, int (*cal)(int))
{
    static const int nudge[] = { 0, 1, -1 };
    int raw = (int)(approx + 0.5);
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(nudge); i++) {
        if (raw + nudge[i] >= CDJ_TOUCH_PEN_UP_BELOW
            && cal(raw + nudge[i]) == px) {
            return (uint16_t)(raw + nudge[i]);
        }
    }
    return (uint16_t)MAX(raw, CDJ_TOUCH_PEN_UP_BELOW);
}

static void cdj_touch_px_to_raw(unsigned x, unsigned y,
                                uint16_t *rx, uint16_t *ry)
{
    *rx = cdj_touch_invert(x, (23700.0 + 961.0 * (794.0 - x)) / 790.0,
                           cdj_touch_cal_x);
    *ry = cdj_touch_invert(y, (18760.0 + 921.0 * ((double)y - 5.0)) / 469.0,
                           cdj_touch_cal_y);
}

static unsigned cdj_touch_env_frames(const char *name, unsigned dflt)
{
    const char *v = getenv(name);

    return v && *v ? (unsigned)strtoul(v, NULL, 0) : dflt;
}

static void cdj_pnl_touch_init(CdjPnlTouch *t)
{
    t->on = cdj_touch_enabled();
    if (!t->on) {
        return;
    }
    /* 8 > the 4 frames the filter needs; 16 > its 12-frame release tail. */
    t->min_frames = cdj_touch_env_frames("CDJ_TOUCH_MIN_FRAMES", 8);
    t->gap_frames = cdj_touch_env_frames("CDJ_TOUCH_GAP_FRAMES", 16);
    t->up_frames = t->gap_frames;
    info_report("panel: touch screen live (report 0x16..0x19), a press lasts "
                ">= %u frames, presses >= %u frames apart",
                t->min_frames, t->gap_frames);
}

/* One touch message from the key socket. */
static void cdj_pnl_touch_msg(CdjPnlTouch *t, int64_t now)
{
    bool edge = t->host.down != t->want_down || t->host.tap_ms;

    cdj_touch_px_to_raw(t->host.x, t->host.y, &t->rx, &t->ry);
    t->want_down = t->host.down;
    t->latched |= t->host.down;
    t->tap_until_ms = t->host.tap_ms ? now + t->host.tap_ms : 0;
    if (!edge) {
        return;             /* a drag: one line per move would bury the log */
    }
    info_report("panel: touch %s (%u, %u) raw (%u, %u)%s at %" PRId64 " ms",
                t->host.down ? "down" : "up", t->host.x, t->host.y,
                t->rx, t->ry, t->host.tap_ms ? " tap" : "", now);
}

/* Runs last each frame so CDJ_PANEL_FILL cannot clobber a zero high byte.
 * Pen-up writes nothing; the idle 0 is already below the threshold. */
static void cdj_pnl_touch_apply(CdjPnlTouch *t, uint8_t *rx, int64_t now)
{
    if (!t->on) {
        return;
    }
    if (t->tap_until_ms && now >= t->tap_until_ms) {
        t->want_down = false;
        t->tap_until_ms = 0;
    }
    if (t->down && !t->want_down && t->down_frames >= t->min_frames) {
        t->down = false;
        t->up_frames = 0;
        info_report("panel: touch released after %u frames at %" PRId64 " ms",
                    t->down_frames, now);
    } else if (!t->down && (t->want_down || t->latched)
               && t->up_frames >= t->gap_frames) {
        t->down = true;
        t->latched = false;
        t->down_frames = 0;
        t->presses++;
        info_report("panel: touch pressed, report[0x16..0x19] = %02x %02x "
                    "%02x %02x at %" PRId64 " ms", t->rx >> 8, t->rx & 0xFF,
                    t->ry >> 8, t->ry & 0xFF, now);
    }
    if (!t->down) {
        t->up_frames = MIN(t->up_frames + 1, UINT_MAX - 1);
        return;
    }
    stw_be_p(rx + CDJ_TOUCH_RAW_X, t->rx);
    stw_be_p(rx + CDJ_TOUCH_RAW_Y, t->ry);
    t->down_frames = MIN(t->down_frames + 1, UINT_MAX - 1);
}

/* Live presses from the key socket (cdj_panelkeys.h), drained once per frame. */
static void cdj_pnl_drain_keys(CdjPnlState *s, int64_t now)
{
    char msg[64];
    ssize_t n;

    if (s->keyfd == -2) {
        s->keyfd = cdj_panelkey_bind(getenv(CDJ_PANELKEY_ENV));
        if (s->keyfd >= 0) {
            info_report("panel: live keys on %s (udp %u)",
                        getenv(CDJ_PANELKEY_ENV),
                        cdj_panelsock_port(getenv(CDJ_PANELKEY_ENV)));
        } else if (getenv(CDJ_PANELKEY_ENV)) {
            /* Usually a port inside a Windows reserved range. */
            warn_report("panel: could NOT bind %s (udp %u) -- this deck will "
                        "ignore every key. On Windows check "
                        "'netsh int ipv4 show excludedportrange protocol=udp'",
                        getenv(CDJ_PANELKEY_ENV),
                        cdj_panelsock_port(getenv(CDJ_PANELKEY_ENV)));
        }
    }
    if (s->keyfd < 0) {
        return;
    }
    while ((n = recv(s->keyfd, msg, sizeof(msg) - 1, 0)) > 0) {
        char op[16] = "or";
        unsigned off, dur;
        int val;

        msg[n] = '\0';
        /* Touch first: its x would fail the key offset range check. */
        if (cdj_pnl_touch.on && cdj_touch_parse(msg, &cdj_pnl_touch.host)) {
            cdj_pnl_touch_msg(&cdj_pnl_touch, now);
            continue;
        }
        if (sscanf(msg, "%i:%i:%u:%15s", &off, &val, &dur, op) < 3) {
            warn_report("panel: bad live key '%s'", msg);
            continue;
        }
        if (off >= CDJ_PNL_PAYLOAD) {
            warn_report("panel: live key offset 0x%x is outside the report", off);
            continue;
        }
        info_report("panel: live key '%s' at %" PRId64 " ms", msg,
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        if (!strcmp(op, "rot")) {
            /* The select knob is a counter the firmware diffs between
             * frames, so it persists. Moving it to a new offset restarts it. */
            if (s->rot_off != off) {
                s->rot_off = off;
                s->rot_val = 0;
                s->rot_live = true;
                info_report("panel: rotary now on report[0x%02x]", off);
            }
            s->rot_val = (uint8_t)(s->rot_val + val);
            info_report("panel: rotary %+d -> report[0x%02x] = 0x%02x",
                        val, off, s->rot_val);
            continue;
        }
        if (!strcmp(op, "lvl")) {
            /*
             * Analogue field: an absolute byte value held until changed. The
             * panel decoder (0x0844D400) passes whole bytes to:
             *   byte 0x02  ->  0x084E1654  ReleaseStartMax
             *   byte 0x04  ->  0x084E1668  TempoSliderMax
             */
            s->lvl_val[off] = (uint8_t)val;
            if (!s->lvl_live[off]) {
                s->lvl_live[off] = true;
                info_report("panel: report[0x%02x] is now an analogue field",
                            off);
            }
            continue;
        }
        /*
         * Held keys (CUE held, search scrub):
         *   op "hold"  press and keep it down (dur_ms ignored)
         *   op "rel"   release it
         */
        if (!strcmp(op, "rel")) {
            unsigned j;

            for (j = 0; j < s->npress; j++) {
                CdjPnlPress *p = &s->press[j];

                if (p->held && p->off == off && p->mask == (uint8_t)val) {
                    p->held = false;
                    /* At least 25 ms, so a tap shorter than a panel frame
                     * is still seen. */
                    p->dur_ms = MAX(now - p->at_ms, 25);
                    info_report("panel: report[0x%02x] &= ~0x%02x at %" PRId64
                                " ms (held %" PRId64 " ms)",
                                p->off, p->mask, now, p->dur_ms);
                }
            }
            if (cdj_dsp_engine_key_up(off, (unsigned)val, now)) {
                cdj_pnl_inject_play(s, now);
            }
            continue;
        }
        if (!cdj_dsp_key_armed && cdj_dsp.key_off == (int)off
            && cdj_dsp.key_val == val) {
            cdj_dsp_key_armed = true;
            info_report("dsp: panel key 0x%02x:0x%02x down at %" PRId64
                        " ms, frame %" PRIu64 " -- replies now carry id %u",
                        off, (unsigned)val, now, cdj_dsp.frames,
                        cdj_dsp.reply_id);
        }
        /* CUE and PLAY also go to the decoder model, which owns the play
         * position. */
        if (cdj_dsp_engine_key(off, (unsigned)val, dur, !strcmp(op, "hold"),
                               now)) {
            cdj_pnl_inject_play(s, now);
        }
        s->press = g_renew(CdjPnlPress, s->press, s->npress + 1);
        s->press[s->npress++] = (CdjPnlPress) {
            .off = off, .mask = (uint8_t)val, .at_ms = now,
            .dur_ms = dur ? dur : 120,
            .held = !strcmp(op, "hold"),
        };
    }
}

/* Build the panel's answer to the frame MAIN just sent. */
static void cdj_pnl_build_reply(CdjPnlState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / SCALE_MS;
    unsigned i;

    cdj_pnl_drain_keys(s, now);
    if (cdj_dsp_engine_key_poll(now)) {
        cdj_pnl_inject_play(s, now);
    }
    memset(s->rx, 0, sizeof(s->rx));
    /* Analogue fields assign, so they go before the keys, which OR. */
    /*
     * Tempo slider: bytes 4-5 position, bytes 6-7 centre reference (decoded
     * at 0x0844D44C). 0x0844646A skips the conversion while the centre is 0.
     * Both default to mid-travel (0.00 %); a live lvl overrides them.
     */
    s->rx[4] = 0x80;
    s->rx[6] = 0x80;
    for (i = 0; i < CDJ_PNL_PAYLOAD; i++) {
        if (s->lvl_live[i]) {
            s->rx[i] = s->lvl_val[i];
        }
    }
    for (i = 0; i < s->npress; i++) {
        CdjPnlPress *p = &s->press[i];

        /* A held key is down from at_ms until its "rel" arrives. */
        if (now >= p->at_ms && (p->held || now < p->at_ms + p->dur_ms)) {
            s->rx[p->off] |= p->mask;
            if (!p->seen) {
                p->seen = true;
                info_report("panel: report[0x%02x] |= 0x%02x at %" PRId64 " ms",
                            p->off, p->mask, now);
            }
        }
    }
    if (s->rot_live && s->rot_off < CDJ_PNL_PAYLOAD) {
        s->rx[s->rot_off] = s->rot_val;
    }
    /*
     * CDJ_PANEL_FILL=<byte>[:<lo>:<hi>]: fill zero reply bytes in [lo,hi)
     * with <byte>, for finding undocumented fields (e.g. the panel version,
     * which reads Ver0.00 with an all-zero reply).
     */
    if (cdj_pnl_fill_on) {
        unsigned i;

        for (i = cdj_pnl_fill_lo; i < cdj_pnl_fill_hi && i < CDJ_PNL_PAYLOAD;
             i++) {
            if (!s->rx[i]) {
                s->rx[i] = cdj_pnl_fill;
            }
        }
    }
    cdj_pnl_touch_apply(&cdj_pnl_touch, s->rx, now);
    s->rx[CDJ_PNL_PAYLOAD] = cdj_pnl_checksum(s->rx);
    s->rx[CDJ_PNL_PAYLOAD + 1] = CDJ_PNL_SYNC;
    s->rxhead = 0;
    s->rxlen = CDJ_PNL_FRAME;
    s->frames_sent++;
}

static uint64_t cdj_pnl_read(void *opaque, hwaddr off, unsigned size)
{
    CdjPnlState *s = opaque;
    unsigned idx = (off & (CDJ_PNL_SIZE - 1)) >> 2;

    s->reads++;
    switch (off & 0xFFC) {
    case CDJ_PNL_SCFRDR:
        if (!s->rxlen) {
            /* Nothing queued; MAIN's frame will fail its sync check. */
            s->starved++;
            return 0;
        }
        s->rxlen--;
        return s->rx[s->rxhead++];

    case CDJ_PNL_SCFSR:
        return s->reg[idx] | (s->rxlen ? CDJ_PNL_SCFSR_RDF : 0);

    case CDJ_PNL_SCFDR:
        /* RX count in bits 6:0, TX count in bits 14:8 (TX always empty). */
        return s->rxlen & 0x7F;

    default:
        return s->reg[idx];
    }
}

static void cdj_pnl_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjPnlState *s = opaque;
    unsigned idx = (off & (CDJ_PNL_SIZE - 1)) >> 2;

    s->writes++;
    /* CDJ_PANEL_REGLOG: per-register write census, printed at exit. */
    if (cdj_pnl_reglog) {
        unsigned o = (off & 0xFFC) >> 2;

        if (o < 64) {
            cdj_pnl_wcount[o]++;
            if (val) {
                cdj_pnl_wnz[o]++;
            }
        }
    }
    switch (off & 0xFFC) {
    case CDJ_PNL_SCFTDR:
        s->tx[s->txlen++] = (uint8_t)val;
        if (s->txlen == CDJ_PNL_FRAME) {
            s->txlen = 0;
            s->frames_recv++;
            /* CDJ_PANEL_DEBUG=<first>[:<count>]: log MAIN frames from
             * #first (default count 3). */
            if (cdj_pnl_dbg_n
                && s->frames_recv >= cdj_pnl_dbg_first
                && s->frames_recv < cdj_pnl_dbg_first + cdj_pnl_dbg_n) {
                char hex[3 * CDJ_PNL_FRAME + 1];
                unsigned k;

                for (k = 0; k < CDJ_PNL_FRAME; k++) {
                    snprintf(hex + 3 * k, 4, "%02x ", s->tx[k]);
                }
                info_report("panel: MAIN frame #%" PRIu64 ": %s",
                            s->frames_recv, hex);
            }
            cdj_stateout_panel(s->tx);
            cdj_pnl_build_reply(s);
        }
        return;

    case CDJ_PNL_SCFSR:
        /* Hardware-owned bits are write-0-to-clear only. */
        s->reg[idx] &= (uint16_t)val | (uint16_t)~CDJ_PNL_SCFSR_HW;
        /* TX drains instantly, so TEND and TDFE stay set. */
        s->reg[idx] |= CDJ_PNL_SCFSR_TEND | CDJ_PNL_SCFSR_TDFE;
        return;

    case CDJ_PNL_SCFCR:
        if (val & CDJ_PNL_SCFCR_RFRST) {
            s->rxlen = 0;
            s->rxhead = 0;
        }
        if (val & CDJ_PNL_SCFCR_TFRST) {
            s->txlen = 0;
        }
        s->reg[idx] = (uint16_t)val;
        return;

    case CDJ_PNL_SCLSR:
        s->reg[idx] &= (uint16_t)val;       /* ORER is write-0-to-clear */
        return;

    default:
        s->reg[idx] = (uint16_t)val;
        return;
    }
}

static const MemoryRegionOps cdj_pnl_ops = {
    .read = cdj_pnl_read,
    .write = cdj_pnl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_pnl_summary(Notifier *n, void *unused)
{
    if (cdj_pnl_reglog) {
        unsigned i;

        for (i = 0; i < 64; i++) {
            if (cdj_pnl_wcount[i]) {
                info_report("panel reg: off 0x%03X  %" PRIu64 " writes, "
                            "%" PRIu64 " non-zero", i * 4,
                            cdj_pnl_wcount[i], cdj_pnl_wnz[i]);
            }
        }
    }
    CdjPnlState *s = container_of(n, CdjPnlState, exit);
    uint8_t err[8];

    info_report("panel: %" PRIu64 " reads, %" PRIu64 " writes, "
                "%" PRIu64 " frames from MAIN, %" PRIu64 " replies loaded, "
                "%" PRIu64 " starved SCFRDR reads",
                s->reads, s->writes, s->frames_recv, s->frames_sent,
                s->starved);
    /* 0x0B06C77C: the reject path's saturating error counter.
     * 0x0B06C774 / 0x0B06C778: calibrated touch X / Y. */
    cpu_physical_memory_read(0x0B06C778, err, sizeof(err));
    info_report("panel: PnlCom reject errors=%u (last touch y=%u)",
                (unsigned)(err[4] | (err[5] << 8) | (err[6] << 16) | (err[7] << 24)),
                (unsigned)(err[0] | (err[1] << 8)));
    if (cdj_pnl_touch.on) {
        info_report("panel: touch %" PRIu64 " messages, %" PRIu64 " presses "
                    "reported to the guest", cdj_pnl_touch.host.events,
                    cdj_pnl_touch.presses);
    }
}

void cdj_pnl_init(MemoryRegion *sysmem, hwaddr addr)
{
    CdjPnlState *s = g_new0(CdjPnlState, 1);
    MemoryRegion *alias = g_new(MemoryRegion, 1);

    s->keyfd = -2;                  /* bound lazily, on the first panel frame */
    cdj_pnl_reglog = getenv("CDJ_PANEL_REGLOG") != NULL;
    {
        const char *f = getenv("CDJ_PANEL_FILL");

        if (f) {
            cdj_pnl_fill_on = true;
            cdj_pnl_fill = (uint8_t)strtoul(f, (char **)&f, 0);
            if (*f == ':') {
                cdj_pnl_fill_lo = strtoul(f + 1, (char **)&f, 0);
                if (*f == ':') {
                    cdj_pnl_fill_hi = strtoul(f + 1, NULL, 0);
                }
            }
            warn_report("panel: filling reply bytes [%u,%u) with 0x%02X",
                        cdj_pnl_fill_lo, cdj_pnl_fill_hi, cdj_pnl_fill);
        }
    }
    {
        const char *d = getenv("CDJ_PANEL_DEBUG");

        if (d) {
            cdj_pnl_dbg_first = strtoull(d, (char **)&d, 0);
            if (!cdj_pnl_dbg_first) {
                cdj_pnl_dbg_first = 1;
            }
            cdj_pnl_dbg_n = (*d == ':') ? strtoull(d + 1, NULL, 0) : 3;
            if (!cdj_pnl_dbg_n) {
                cdj_pnl_dbg_n = 3;
            }
        }
    }
    cdj_pnl_parse_presses(s);
    cdj_pnl_touch_init(&cdj_pnl_touch);
    s->reg[CDJ_PNL_SCFSR >> 2] = CDJ_PNL_SCFSR_TEND | CDJ_PNL_SCFSR_TDFE;
    s->exit.notify = cdj_pnl_summary;
    qemu_add_exit_notifier(&s->exit);

    memory_region_init_io(&s->iomem, NULL, &cdj_pnl_ops, s,
                          "sh7724.scif2-panel", CDJ_PNL_SIZE);
    memory_region_add_subregion(sysmem, addr, &s->iomem);
    /* Needed: cdj_dmac_run() addresses through A7ADDR(). */
    memory_region_init_alias(alias, NULL, "sh7724.scif2-panel-a7", &s->iomem,
                             0, CDJ_PNL_SIZE);
    memory_region_add_subregion(sysmem, A7ADDR(addr), alias);
    cdj_pnl = s;
}

