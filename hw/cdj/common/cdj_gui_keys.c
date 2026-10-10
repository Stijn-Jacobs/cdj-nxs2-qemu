/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "ui/input.h"
#include "cdj_panelkeys.h"
#include "cdj_gui_keys.h"

#define CDJ_GUI_ROTARY      0x0E        /* select knob counter byte          */

/*
 * Nudge: a platter turned by hand. The firmware's jog engine takes motion
 * from the rolling position counter in report bytes 8-9 and speed from the
 * pulse period in bytes 10-11 (27778 / P = platter speed %), so while the key
 * is held the counter is stepped and the period held. The counter has to move
 * at least 42 per 30 firmware passes before the bend engages; 2000 pulses/s
 * clears that. Measured on the real DSP, P 278 bent the deck to 1.06x and
 * P 139 to 1.18x.
 */
#define CDJ_NUDGE_TICK_MS   40
#define CDJ_NUDGE_STEP      80
#define CDJ_NUDGE_PERIOD    278
#define CDJ_NUDGE_HARD      139

static struct {
    QEMUTimer *timer;
    const char *sock;
    int dir;                    /* -1 slower, +1 faster, 0 idle */
    bool hard;
    uint16_t count;
} cdj_nudge;

static void cdj_nudge_send(void)
{
    unsigned period = cdj_nudge.dir ? (cdj_nudge.hard ? CDJ_NUDGE_HARD
                                                      : CDJ_NUDGE_PERIOD) : 0;
    unsigned bits = 0x80 | (cdj_nudge.dir > 0 ? 0x40 : 0);

    if (cdj_nudge.dir) {
        cdj_nudge.count += cdj_nudge.dir * CDJ_NUDGE_STEP;
        cdj_panelkey_send_op(cdj_nudge.sock, 0x08, cdj_nudge.count >> 8, 0, "lvl");
        cdj_panelkey_send_op(cdj_nudge.sock, 0x09, cdj_nudge.count & 0xff, 0,
                             "lvl");
    }
    cdj_panelkey_send_op(cdj_nudge.sock, 0x0A, period >> 8, 0, "lvl");
    cdj_panelkey_send_op(cdj_nudge.sock, 0x0B, period & 0xff, 0, "lvl");
    if (cdj_nudge.dir) {
        cdj_panelkey_send_op(cdj_nudge.sock, 0x0F, bits, CDJ_NUDGE_TICK_MS * 3,
                             "or");
    }
}

static void cdj_nudge_tick(void *opaque)
{
    if (!cdj_nudge.dir) {
        return;
    }
    cdj_nudge_send();
    timer_mod(cdj_nudge.timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                               + CDJ_NUDGE_TICK_MS);
}

static void cdj_nudge_set(const char *sock, int dir, bool hard)
{
    if (dir == cdj_nudge.dir && hard == cdj_nudge.hard) {
        return;
    }
    if (!cdj_nudge.timer) {
        cdj_nudge.timer = timer_new_ms(QEMU_CLOCK_REALTIME, cdj_nudge_tick, NULL);
    }
    cdj_nudge.sock = sock;
    cdj_nudge.dir = dir;
    cdj_nudge.hard = hard;
    if (dir) {
        cdj_nudge_tick(NULL);
    } else {
        timer_del(cdj_nudge.timer);
        cdj_nudge_send();       /* period 0: the platter has stopped */
    }
}

static unsigned cdj_gui_sweep_off = 0x15;   /* byte under F1-F8 */

static const CdjGuiKey *cdj_gui_table;
static size_t cdj_gui_count;
static bool *cdj_gui_held;

static void cdj_gui_key_event(DeviceState *dev, QemuConsole *src,
                              InputEvent *evt)
{
    static bool shift, nudge_down[2];
    const char *sock = getenv(CDJ_PANELKEY_ENV);
    InputKeyEvent *k = evt->u.key.data;
    int qcode = qemu_input_key_value_to_qcode(k->key);
    unsigned i;

    if (!sock) {
        return;
    }
    if (qcode == Q_KEY_CODE_SHIFT || qcode == Q_KEY_CODE_SHIFT_R) {
        shift = k->down;
        if (cdj_nudge.dir) {
            cdj_nudge_set(sock, cdj_nudge.dir, shift);
        }
        return;
    }
    if (qcode == Q_KEY_CODE_MINUS || qcode == Q_KEY_CODE_EQUAL) {
        nudge_down[qcode == Q_KEY_CODE_EQUAL] = k->down;
        cdj_nudge_set(sock, nudge_down[1] - nudge_down[0], shift);
        return;
    }
    /*
     * Held keys send hold on key-down and rel on key-up, so a key is down as
     * long as the finger is; host auto-repeat is swallowed.
     */
    for (i = 0; i < cdj_gui_count; i++) {
        if (cdj_gui_table[i].qcode != qcode) {
            continue;
        }
        if (k->down == cdj_gui_held[i]) {
            return;                     /* auto-repeat, or a stray release */
        }
        cdj_gui_held[i] = k->down;
        cdj_panelkey_send_op(sock, cdj_gui_table[i].off, cdj_gui_table[i].mask,
                             0, k->down ? "hold" : "rel");
        if (k->down) {
            info_report("panel key: %s (report[0x%02x] 0x%02x)%s",
                        cdj_gui_table[i].name, cdj_gui_table[i].off,
                        cdj_gui_table[i].mask,
                        cdj_gui_table[i].confirmed ? "" : "   [unverified]");
        }
        return;
    }
    if (!k->down) {
        return;                         /* the rest are taps, not held keys */
    }
    /* The select knob repeats with the host's auto-repeat, like a turn. */
    if (qcode == Q_KEY_CODE_UP || qcode == Q_KEY_CODE_DOWN ||
        qcode == Q_KEY_CODE_PGUP || qcode == Q_KEY_CODE_PGDN) {
        int step = (qcode == Q_KEY_CODE_PGUP || qcode == Q_KEY_CODE_PGDN)
                   ? 10 : 1;

        if (qcode == Q_KEY_CODE_UP || qcode == Q_KEY_CODE_PGUP) {
            step = -step;
        }
        cdj_panelkey_send_op(sock, CDJ_GUI_ROTARY, step, 0, "rot");
        return;
    }
    if (!getenv("CDJ_PANEL_SWEEP")) {
        return;
    }
    if (qcode == Q_KEY_CODE_F9 || qcode == Q_KEY_CODE_F10) {
        cdj_gui_sweep_off += (qcode == Q_KEY_CODE_F10) ? 1 : -1;
        cdj_gui_sweep_off &= 0x1F;
        info_report("panel sweep: byte is now 0x%02x (F1-F8 press its bits)",
                    cdj_gui_sweep_off);
        return;
    }
    if (qcode >= Q_KEY_CODE_F1 && qcode <= Q_KEY_CODE_F8) {
        unsigned mask = 1u << (qcode - Q_KEY_CODE_F1);

        cdj_panelkey_send(sock, cdj_gui_sweep_off, mask, 150);
        info_report("panel sweep: report[0x%02x] |= 0x%02x",
                    cdj_gui_sweep_off, mask);
    }
}

static QemuInputHandler cdj_gui_kbd = {
    .name  = "CDJ front panel",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = cdj_gui_key_event,
};

/* Only here so that the window counts as having an absolute pointer: with
 * none, Cocoa, GTK and SDL grab the host mouse on the first click. */
static void cdj_gui_pointer_event(DeviceState *dev, QemuConsole *src,
                                  InputEvent *evt)
{
}

static QemuInputHandler cdj_gui_pointer = {
    .name  = "CDJ window pointer",
    .mask  = INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_BTN,
    .event = cdj_gui_pointer_event,
};

void cdj_gui_pointer_init(void)
{
    qemu_input_handler_register(NULL, &cdj_gui_pointer);
}

void cdj_gui_keys_init(const CdjGuiKey *keys, size_t count, const char *board)
{
    if (!getenv(CDJ_PANELKEY_ENV)) {
        return;
    }
    cdj_gui_table = keys;
    cdj_gui_count = count;
    cdj_gui_held = g_new0(bool, count);
    qemu_input_handler_register(NULL, &cdj_gui_kbd);
    info_report("%s: keyboard live -- Space play/pause, C cue, "
                "Up/Down browse, Enter load, Esc back, - = nudge "
                "(emulator/README.md lists every key)", board);
}
