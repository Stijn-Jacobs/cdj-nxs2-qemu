/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
#include "cdj_pnl_link.h"
/*
 * SCIF2 (0xFFE20000): the front-panel MCU link, with the panel modelled as the
 * peer. Keys, jog, touch and the browse encoder reach MAIN this way. The
 * register protocol, the DMA-fed frame exchange and the key-schedule /
 * key-socket plumbing are common/cdj_pnl_link.c (shared with the CDJ-2000 and
 * CDJ-2000NXS, whose panel is the same protocol at a shorter frame length
 * and with no touch screen); this file is what is specific to the NXS2 --
 * the touch screen, the tempo-slider default, and the DSP-transport model
 * reacting to specific keys.
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
#define CDJ_PNL_FRAME       0x28    /* whole frame, both directions          */
#define CDJ_PNL_PAYLOAD     0x26    /* bytes covered by the checksum         */
QEMU_BUILD_BUG_ON(CDJ_STATEOUT_PNL_FRAME != CDJ_PNL_FRAME);
#define CDJ_PNL_SYNC        0x8F

/*
 * A PLAY press requested by the decoder model so the transport follows the
 * cue key (see cdj_dsp_engine_key()). Delayed 60 ms so it never shares a
 * report with the cue press.
 */
static void cdj_pnl_inject_play(CdjPnlLinkState *s, int64_t now)
{
    cdj_pnl_link_press(s, cdj_dsp_eng.play_off, (uint8_t)cdj_dsp_eng.play_mask,
                       now + 60, 120);
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

/* Everything this board keeps beyond the common panel state. */
typedef struct CdjNxsPanelExtra {
    CdjPnlTouch touch;
} CdjNxsPanelExtra;

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

static void cdj_pnl_on_frame(CdjPnlLinkState *s, void *extra, const uint8_t *frame)
{
    cdj_stateout_panel(frame);
}

static void cdj_pnl_poll(CdjPnlLinkState *s, void *extra, int64_t now)
{
    if (cdj_dsp_engine_key_poll(now)) {
        cdj_pnl_inject_play(s, now);
    }
}

/*
 * Tempo slider: bytes 4-5 position, bytes 6-7 centre reference (decoded
 * at 0x0844D44C). 0x0844646A skips the conversion while the centre is 0.
 * Both default to mid-travel (0.00 %); a live lvl overrides them.
 */
static void cdj_pnl_build_defaults(CdjPnlLinkState *s, void *extra, uint8_t *rx)
{
    rx[4] = 0x80;
    rx[6] = 0x80;
}

static void cdj_pnl_build_last(CdjPnlLinkState *s, void *extra, uint8_t *rx,
                               int64_t now)
{
    CdjNxsPanelExtra *e = extra;

    cdj_pnl_touch_apply(&e->touch, rx, now);
}

/* Touch first: its x would fail the key offset range check. */
static bool cdj_pnl_drain_extra(CdjPnlLinkState *s, void *extra, const char *msg,
                                int64_t now)
{
    CdjNxsPanelExtra *e = extra;

    if (e->touch.on && cdj_touch_parse(msg, &e->touch.host)) {
        cdj_pnl_touch_msg(&e->touch, now);
        return true;
    }
    return false;
}

static void cdj_pnl_key_down(CdjPnlLinkState *s, void *extra, unsigned off,
                             unsigned mask, unsigned dur_ms, bool held,
                             int64_t now)
{
    if (!cdj_dsp_key_armed && cdj_dsp.key_off == (int)off
        && cdj_dsp.key_val == (int)mask) {
        cdj_dsp_key_armed = true;
        info_report("dsp: panel key 0x%02x:0x%02x down at %" PRId64
                    " ms, frame %" PRIu64 " -- replies now carry id %u",
                    off, mask, now, cdj_dsp.frames, cdj_dsp.reply_id);
    }
    /* CUE and PLAY also go to the decoder model, which owns the play
     * position. */
    if (cdj_dsp_engine_key(off, mask, dur_ms, held, now)) {
        cdj_pnl_inject_play(s, now);
    }
}

static void cdj_pnl_key_up(CdjPnlLinkState *s, void *extra, unsigned off,
                           unsigned mask, int64_t now)
{
    if (cdj_dsp_engine_key_up(off, mask, now)) {
        cdj_pnl_inject_play(s, now);
    }
}

static void cdj_pnl_summary(CdjPnlLinkState *s, void *extra)
{
    CdjNxsPanelExtra *e = extra;
    uint8_t err[8];

    /* 0x0B06C77C: the reject path's saturating error counter.
     * 0x0B06C774 / 0x0B06C778: calibrated touch X / Y. */
    cpu_physical_memory_read(0x0B06C778, err, sizeof(err));
    info_report("panel: PnlCom reject errors=%u (last touch y=%u)",
                (unsigned)(err[4] | (err[5] << 8) | (err[6] << 16) | (err[7] << 24)),
                (unsigned)(err[0] | (err[1] << 8)));
    if (e->touch.on) {
        info_report("panel: touch %" PRIu64 " messages, %" PRIu64 " presses "
                    "reported to the guest", e->touch.host.events,
                    e->touch.presses);
    }
}

void cdj_pnl_init(MemoryRegion *sysmem, hwaddr addr)
{
    static const CdjPnlLinkHooks hooks = {
        .on_frame = cdj_pnl_on_frame,
        .poll = cdj_pnl_poll,
        .build_defaults = cdj_pnl_build_defaults,
        .build_last = cdj_pnl_build_last,
        .drain_extra = cdj_pnl_drain_extra,
        .key_down = cdj_pnl_key_down,
        .key_up = cdj_pnl_key_up,
        .summary = cdj_pnl_summary,
    };
    CdjNxsPanelExtra *e = cdj_pnl_link_init(sysmem, addr, "sh7724.scif2-panel",
                                           CDJ_PNL_FRAME, CDJ_PNL_SYNC, &hooks,
                                           sizeof(*e));

    cdj_pnl_touch_init(&e->touch);
}
