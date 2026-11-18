/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "panel_display.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

uint8_t cdj850_panel_sum8ec(const uint8_t *p, unsigned n)
{
    unsigned sum = 0;

    for (unsigned i = 0; i < n; i++) {
        sum += p[i];
        sum = (sum & 0xff) + (sum >> 8);
    }
    return sum;
}

/* Skip the transport type, sequence and checksum in change-only reporting. */
static bool header_changed(const PanelDisplay *display)
{
    return !display->state.valid ||
           memcmp(display->state.header + 2,
                  display->pending_header + 2, 13) != 0;
}

static void publish_header(PanelDisplay *display, PanelDisplayUpdate update,
                           void *opaque)
{
    bool changed = header_changed(display);

    display->header_only = false;
    memcpy(display->state.header, display->pending_header, PANEL_FRAME_LEN);
    display->state.valid = true;
    if (changed) {
        update(&display->state, PANEL_DISPLAY_HEADER, true, opaque);
    }
}

static void try_publish_text(PanelDisplay *display, PanelDisplayUpdate update,
                             void *opaque)
{
    PanelDisplayState next = {
        .updates = display->state.updates + 1,
        .time = display->state.time,
        .pitch = display->state.pitch,
        .master_tempo = display->state.master_tempo,
        .bpm = display->state.bpm,
        .bpm_known = display->state.bpm_known,
        .track_known = display->state.track_known,
        .track_number = display->state.track_number,
        .valid = true,
    };
    const uint8_t *payload = display->payload;
    unsigned pos = 0;

    for (unsigned row = 0; row < PANEL_DISP_ROWS; row++) {
        unsigned len;

        if (pos + 5 > display->payload_len || (payload[pos] & 3) != row + 1) {
            return;
        }
        len = payload[pos + 4];
        if (pos + 5 + len > display->payload_len) {
            return;
        }
        next.tags[row] = payload[pos];
        memcpy(next.attrs[row], payload + pos + 1, sizeof(next.attrs[row]));
        for (unsigned i = 0; i < len; i++) {
            uint8_t ch = payload[pos + 5 + i];

            next.rows[row][i] = ch >= 0x20 && ch <= 0x7e ? (char)ch : '.';
        }
        pos += 5 + len;
    }

    bool changed = header_changed(display) ||
                   memcmp(next.tags, display->state.tags, sizeof(next.tags)) ||
                   memcmp(next.attrs, display->state.attrs, sizeof(next.attrs)) ||
                   memcmp(next.rows, display->state.rows, sizeof(next.rows));

    memcpy(next.header, display->pending_header, sizeof(next.header));
    display->state = next;
    display->next_sub = 0;
    update(&display->state, PANEL_DISPLAY_ROWS, changed, opaque);
}

void cdj850_panel_display_feed(PanelDisplay *display, const uint8_t tx[16],
                              PanelDisplayUpdate update, void *opaque)
{
    unsigned sub;
    bool valid;

    if (!tx || tx[0] < 0x80 || tx[0] > 0xaf) {
        return;
    }
    valid = cdj850_panel_sum8ec(tx, 15) == tx[15];
    if (tx[0] == 0x80) {
        if (!valid) {
            /* A new corrupt header cannot lend the old header to its rows. */
            display->have_header = false;
            display->header_only = false;
            return;
        }
        if (display->header_only) {
            publish_header(display, update, opaque);
        }
        memcpy(display->pending_header, tx, PANEL_FRAME_LEN);
        display->payload_len = 0;
        display->next_sub = 1;
        display->quiet_transactions = 0;
        display->have_header = true;
        display->header_only = true;
        return;
    }

    if (!display->have_header) {
        return;
    }
    /* Even a damaged continuation identifies this as an attempted text
     * burst. Its header must not replace the last complete display state. */
    display->header_only = false;
    sub = tx[0] - 0x80;
    if (sub == 1) {
        display->next_sub = 1;
        display->payload_len = 0;
    }
    if (!valid || sub != display->next_sub) {
        return;
    }
    if (display->payload_len + 14 > sizeof(display->payload)) {
        display->next_sub = 0;
        return;
    }
    memcpy(display->payload + display->payload_len, tx + 1, 14);
    display->payload_len += 14;
    display->next_sub++;
    try_publish_text(display, update, opaque);
}

static void publish_track(PanelDisplay *display, bool known, uint8_t number,
                          PanelDisplayUpdate update, void *opaque)
{
    PanelDisplayState *state = &display->state;

    if (state->track_known == known && state->track_number == number) {
        return;
    }
    state->track_known = known;
    state->track_number = number;
    update(state, PANEL_DISPLAY_TRACK, true, opaque);
}

static void publish_time(PanelDisplay *display, const Cdj850Time *next,
                         PanelDisplayUpdate update, void *opaque)
{
    const Cdj850Time *last = &display->state.time;

    if (last->known == next->known && last->minutes == next->minutes &&
        last->seconds == next->seconds && last->frames == next->frames &&
        last->remain_known == next->remain_known && last->remain == next->remain) {
        return;
    }
    display->state.time = *next;
    update(&display->state, PANEL_DISPLAY_TIME, true, opaque);
}

static void publish_bpm(PanelDisplay *display, uint16_t number,
                        PanelDisplayUpdate update, void *opaque)
{
    PanelDisplayState *state = &display->state;
    bool known = number > 0 && number <= 999;
    uint16_t bpm = known ? number : 0;

    if (state->bpm_known == known && state->bpm == bpm) {
        return;
    }
    state->bpm_known = known;
    state->bpm = bpm;
    update(state, PANEL_DISPLAY_BPM, true, opaque);
}

static void publish_pitch(PanelDisplay *display, Cdj850Pitch next,
                          PanelDisplayUpdate update, void *opaque)
{
    Cdj850Pitch *last = &display->state.pitch;

    if (last->value == next.value && last->decimals == next.decimals &&
        last->range == next.range) {
        return;
    }
    *last = next;
    update(&display->state, PANEL_DISPLAY_PITCH, true, opaque);
}

static void panel_pitch(PanelDisplay *display, unsigned magnitude,
                        PanelDisplayUpdate update, void *opaque)
{
    Cdj850Pitch pitch = {0};
    unsigned sign = display->pitch_sign & 3;

    /* 0x0c063fd6 sets the range lamp and sign before 0x0c065dd4
     * serializes the magnitude. The flags are on the preceding page 2. */
    if (display->pitch_range_flags & 2) {
        switch (display->pitch_range_flags & 0xe4) {
        case 0x80:
            pitch.range = 6;
            break;
        case 0x40:
            pitch.range = 10;
            break;
        case 0x20:
            pitch.range = 16;
            break;
        case 0x04:
            pitch.range = CDJ850_PITCH_WIDE;
            break;
        }
    }

    bool wide = pitch.range == CDJ850_PITCH_WIDE;
    unsigned limit = pitch.range * (wide ? 10u : 100u);

    if (pitch.range && magnitude <= limit &&
        (display->pitch_decimal_flags & 0x30) == (wide ? 0x20 : 0x10) &&
        (magnitude ? (sign == 2 || sign == 3) : sign == 0)) {
        pitch.decimals = wide ? 1 : 2;
        pitch.value = sign == 2 ? -(int)magnitude : (int)magnitude;
    }

    publish_pitch(display, pitch, update, opaque);
}

static void publish_master_tempo(PanelDisplay *display, bool enabled,
                                 PanelDisplayUpdate update, void *opaque)
{
    PanelDisplayState *state = &display->state;

    if (state->master_tempo != enabled) {
        state->master_tempo = enabled;
        update(state, PANEL_DISPLAY_MASTER_TEMPO, true, opaque);
    }
}

void cdj850_panel_display_reset_readouts(PanelDisplay *display, uint64_t ns,
                                        PanelDisplayUpdate update, void *opaque)
{
    display->readout_reset_ns = ns;
    publish_track(display, false, 0, update, opaque);
    publish_time(display, &(Cdj850Time){0}, update, opaque);
    publish_bpm(display, 0, update, opaque);
    display->have_pitch_flags = false;
    publish_pitch(display, (Cdj850Pitch){0}, update, opaque);
    publish_master_tempo(display, false, update, opaque);
}

static void panel_status(PanelDisplay *display, const uint8_t tx[PANEL_FRAME_LEN],
                         PanelDisplayUpdate update, void *opaque)
{
    Cdj850Time time = display->state.time;

    /* MAIN TX checksums are at byte 15 on both pages. */
    if ((tx[0] != 0x01 && tx[0] != 0x02) ||
        cdj850_panel_sum8ec(tx, 15) != tx[15]) {
        display->have_pitch_flags = false;
        return;
    }
    if (tx[0] == 0x01) {
        unsigned number = tx[4];
        bool known = number <= 99;

        /* MAIN 0x0c065d9e copies the track and binary M/S/F bytes. */
        publish_track(display, known, known ? number : 0, update, opaque);
        /* 0x0c065dc4..0x0c065dd0 sends MAIN's already rounded BPM word.
         * 0x0c064118 uses 0xffff when there is no BPM to display. */
        publish_bpm(display, (tx[8] << 8) | tx[9], update, opaque);
        if (display->have_pitch_flags) {
            panel_pitch(display, (tx[10] << 8) | tx[11], update, opaque);
            display->have_pitch_flags = false;
        }
        time.minutes = tx[5];
        time.seconds = tx[6];
        time.frames = tx[7];
        time.known = cdj850_time_in_range(&time);
        if (!time.known) {
            time.minutes = time.seconds = time.frames = 0;
        }
    } else {
        display->pitch_sign = tx[11];
        display->pitch_decimal_flags = tx[12];
        display->pitch_range_flags = tx[14];
        display->have_pitch_flags = true;
        /* MAIN 0x0c06405a sets MT in the flags copied at 0x0c065e42. */
        publish_master_tempo(display, (tx[11] & 0x80) != 0, update, opaque);
        /* MAIN 0x0c063bce copies the TIME mode to the page 2 flags. */
        time.remain_known = true;
        time.remain = (tx[13] & 1) != 0;
    }
    publish_time(display, &time, update, opaque);
}

void cdj850_panel_display_transaction(PanelDisplay *display, uint64_t start_ns,
                                     unsigned count,
                                     const uint8_t tx[][PANEL_FRAME_LEN],
                                     PanelDisplayUpdate update, void *opaque)
{
    if (count < 2) {
        display->have_pitch_flags = false;
        return;
    }
    if (!display->readout_reset_ns || start_ns > display->readout_reset_ns) {
        panel_status(display, tx[0], update, opaque);
    }
    for (unsigned i = 0; i < count; i++) {
        cdj850_panel_display_feed(display, tx[i], update, opaque);
    }
    /* A solitary header is itself an update. Wait as the panel peer does
     * because the first row chunk can arrive in a subsequent transaction. */
    if (display->header_only && ++display->quiet_transactions >= 2) {
        publish_header(display, update, opaque);
    }
}

void cdj850_panel_display_format(char *out, size_t size,
                                const PanelDisplayState *state, PanelDisplayField field,
                                uint64_t ms)
{
    const uint8_t *h = state->header;

    if (field == PANEL_DISPLAY_TRACK) {
        char track[3] = "--";

        if (state->track_known) {
            snprintf(track, sizeof(track), "%02u", (unsigned)state->track_number);
        }
        snprintf(out, size, "cdj850-panel: display track=%s (%" PRIu64 " ms)",
                 track, ms);
    } else if (field == PANEL_DISPLAY_TIME) {
        const Cdj850Time *time = &state->time;
        char digits[9] = "--:--:--";
        char remain = time->remain_known ? (time->remain ? '1' : '0') : '?';

        if (time->known) {
            snprintf(digits, sizeof(digits), "%02u:%02u:%02u",
                     (unsigned)time->minutes, (unsigned)time->seconds,
                     (unsigned)time->frames);
        }
        snprintf(out, size, "cdj850-panel: display time=%s remain=%c (%" PRIu64 " ms)",
                 digits, remain, ms);
    } else if (field == PANEL_DISPLAY_BPM) {
        char digits[4] = "---";

        if (state->bpm_known) {
            snprintf(digits, sizeof(digits), "%u", (unsigned)state->bpm);
        }
        snprintf(out, size, "cdj850-panel: display bpm=%s (%" PRIu64 " ms)",
                 digits, ms);
    } else if (field == PANEL_DISPLAY_PITCH) {
        const Cdj850Pitch *pitch = &state->pitch;
        char digits[12] = "---";
        char range[5] = "--";

        if (pitch->range == CDJ850_PITCH_WIDE) {
            memcpy(range, "WIDE", sizeof(range));
        } else if (pitch->range) {
            snprintf(range, sizeof(range), "%u", (unsigned)pitch->range);
        }
        if (pitch->decimals) {
            unsigned abs_value = pitch->value < 0 ? -pitch->value : pitch->value;
            int decimals = pitch->decimals == 1 ? 1 : 2;
            unsigned scale = decimals == 1 ? 10 : 100;
            const char *sign = pitch->value < 0 ? "-" : pitch->value > 0 ? "+" : "";

            snprintf(digits, sizeof(digits), "%s%u.%0*u", sign, abs_value / scale,
                     decimals, abs_value % scale);
        }
        snprintf(out, size, "cdj850-panel: display pitch=%s range=%s (%" PRIu64 " ms)",
                 digits, range, ms);
    } else if (field == PANEL_DISPLAY_MASTER_TEMPO) {
        snprintf(out, size, "cdj850-panel: display master_tempo=%u (%" PRIu64 " ms)",
                 (unsigned)state->master_tempo, ms);
    } else if (field == PANEL_DISPLAY_ROWS) {
        const uint8_t (*a)[3] = state->attrs;

        snprintf(out, size,
                 "cdj850-panel: display mode=0x%02x tag=%02x/%02x/%02x "
                 "attr=%02x:%02x:%02x/%02x:%02x:%02x/%02x:%02x:%02x "
                 "[%s] | [%s] | [%s] (%" PRIu64 " ms)",
                 h[2], state->tags[0], state->tags[1], state->tags[2],
                 a[0][0], a[0][1], a[0][2], a[1][0], a[1][1], a[1][2],
                 a[2][0], a[2][1], a[2][2],
                 state->rows[0], state->rows[1], state->rows[2], ms);
    } else {
        snprintf(out, size,
                 "cdj850-panel: display hdr mode=0x%02x "
                 "bytes=%02x %02x %02x %02x %02x %02x %02x %02x "
                 "%02x %02x %02x %02x (%" PRIu64 " ms)",
                 h[2], h[3], h[4], h[5], h[6], h[7], h[8],
                 h[9], h[10], h[11], h[12], h[13], h[14], ms);
    }
}
