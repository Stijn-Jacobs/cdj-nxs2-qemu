/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_pnl_touch.h"
#include "qemu/timer.h"
#include "ui/input.h"

/*
 * The panel MCU sends two raw 10-bit values: X = BE16 report[0x16..0x17],
 * Y = BE16 report[0x18..0x19]. MAIN's key decoder reads them there on the
 * XDJ-1000 (fw 1.13, 0x08CF90B2), the XDJ-1000MK2 (fw 1.45, 0x0949CD3A) and
 * the XDJ-700 (fw 1.15, 0x08D52340), calls the pen-up filter and then the
 * calibration (0x08D4FB38 in 1.13), whose default constants are the NXS2's:
 *
 *      X = 794 - (790 * rx - 23700) / 961      Y = (469 * ry - 18760) / 921 + 5
 *
 * Either raw value below 32 is pen-up. The filter drops the first three
 * pen-down frames and repeats the last position for up to 12 pen-up frames, so
 * each press is held for at least CDJ_TOUCH_MIN_FRAMES frames and presses are
 * spaced by CDJ_TOUCH_GAP_FRAMES pen-up frames. CDJ_TOUCH=0 turns the model off.
 */
#define PEN_UP_BELOW   32
#define RAW_X          0x16
#define RAW_Y          0x18

/* Raw -> pixel as the firmware's default calibration does; 0 off-screen. */
static int cal_x(int rx)
{
    int x = (int)(794.0 - (790.0 * rx - 23700.0) / 961.0);

    return x >= 0 && x <= CDJ_TOUCH_W ? x : 0;
}

static int cal_y(int ry)
{
    int y = (int)((469.0 * ry - 18760.0) / 921.0 + 5.0);

    return y >= 0 && y <= CDJ_TOUCH_H ? y : 0;
}

/* Invert one axis, nudging by one count so the firmware's truncating forward
 * transform lands on the pixel. */
static uint16_t invert(int px, double approx, int (*cal)(int))
{
    static const int nudge[] = { 0, 1, -1 };
    int raw = (int)(approx + 0.5);
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(nudge); i++) {
        if (raw + nudge[i] >= PEN_UP_BELOW && cal(raw + nudge[i]) == px) {
            return (uint16_t)(raw + nudge[i]);
        }
    }
    return (uint16_t)MAX(raw, PEN_UP_BELOW);
}

static unsigned env_frames(const char *name, unsigned dflt)
{
    const char *v = getenv(name);

    return v && *v ? (unsigned)strtoul(v, NULL, 0) : dflt;
}

void cdj_pnl_touch_init(CdjPnlTouch *t)
{
    const char *v = getenv(CDJ_TOUCH_ENV);

    t->on = !(v && !strcmp(v, "0"));
    if (!t->on) {
        return;
    }
    /* 8 > the 4 frames the filter needs; 16 > its 12-frame release tail. */
    t->min_frames = env_frames("CDJ_TOUCH_MIN_FRAMES", 8);
    t->gap_frames = env_frames("CDJ_TOUCH_GAP_FRAMES", 16);
    t->up_frames = t->gap_frames;
    info_report("panel: touch screen live (report 0x16..0x19), a press lasts "
                ">= %u frames, presses >= %u frames apart",
                t->min_frames, t->gap_frames);
}

/* The host moved, pressed or released; now is virtual ms. */
static void touch_changed(CdjPnlTouch *t, int64_t now)
{
    bool edge = t->host.down != t->want_down || t->host.tap_ms;

    t->rx = invert(t->host.x, (23700.0 + 961.0 * (794.0 - t->host.x)) / 790.0,
                   cal_x);
    t->ry = invert(t->host.y,
                   (18760.0 + 921.0 * ((double)t->host.y - 5.0)) / 469.0,
                   cal_y);
    t->want_down = t->host.down;
    /* Only a new press latches: a move while held must not queue a second one. */
    t->latched |= edge && t->host.down;
    t->tap_until_ms = t->host.tap_ms ? now + t->host.tap_ms : 0;
    if (edge) {
        info_report("panel: touch %s (%u, %u) raw (%u, %u)%s at %" PRId64 " ms",
                    t->host.down ? "down" : "up", t->host.x, t->host.y,
                    t->rx, t->ry, t->host.tap_ms ? " tap" : "", now);
    }
}

bool cdj_pnl_touch_drain(CdjPnlTouch *t, const char *msg, int64_t now)
{
    if (!t->on || !cdj_touch_parse(msg, &t->host)) {
        return false;
    }
    touch_changed(t, now);
    return true;
}

void cdj_pnl_touch_apply(CdjPnlTouch *t, uint8_t *rx, int64_t now)
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
    stw_be_p(rx + RAW_X, t->rx);
    stw_be_p(rx + RAW_Y, t->ry);
    t->down_frames = MIN(t->down_frames + 1, UINT_MAX - 1);
}

void cdj_pnl_touch_summary(const CdjPnlTouch *t)
{
    if (t->on) {
        info_report("panel: touch %" PRIu64 " messages, %" PRIu64 " presses "
                    "reported to the guest", t->host.events, t->presses);
    }
}

static CdjPnlTouch *pointer_target;

static void pointer_event(DeviceState *dev, QemuConsole *src, InputEvent *evt)
{
    CdjTouch *host = &pointer_target->host;

    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *m = evt->u.abs.data;

        /* The axis spans the 800x480 console at any window zoom. */
        if (m->axis == INPUT_AXIS_X) {
            host->x = qemu_input_scale_axis(m->value, INPUT_EVENT_ABS_MIN,
                                            INPUT_EVENT_ABS_MAX, 0,
                                            CDJ_TOUCH_W - 1);
        } else if (m->axis == INPUT_AXIS_Y) {
            host->y = qemu_input_scale_axis(m->value, INPUT_EVENT_ABS_MIN,
                                            INPUT_EVENT_ABS_MAX, 0,
                                            CDJ_TOUCH_H - 1);
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *b = evt->u.btn.data;

        if (b->button == INPUT_BUTTON_LEFT) {
            host->down = b->down;
            host->tap_ms = 0;
            host->events++;
        }
        break;
    }
    default:
        break;
    }
}

/* A move and its button arrive as one batch; act once the batch is whole.
 * Hovering is not forwarded. */
static void pointer_sync(DeviceState *dev)
{
    CdjPnlTouch *t = pointer_target;

    if (t->on && (t->host.down || t->want_down)) {
        touch_changed(t, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / SCALE_MS);
    }
}

static QemuInputHandler pointer_handler = {
    .name  = "CDJ touch screen",
    .mask  = INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_BTN,
    .event = pointer_event,
    .sync  = pointer_sync,
};

void cdj_pnl_touch_pointer_init(CdjPnlTouch *t)
{
    pointer_target = t;
    qemu_input_handler_register(NULL, &pointer_handler);
}
