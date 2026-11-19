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
    if (display->state.header[2] != display->pending_header[2]) {
        display->state.playing.artist_known = false;
        display->state.playing.artist[0] = '\0';
    }
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
        .playing = display->state.playing,
        .master_tempo = display->state.master_tempo,
        .auto_cue = display->state.auto_cue,
        .info_lamp = display->state.info_lamp,
        .browse_lamp = display->state.browse_lamp,
        .tag_list_lamp = display->state.tag_list_lamp,
        .menu_lamp = display->state.menu_lamp,
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
    /* MAIN's loaded-track row 2 uses attribute 0x07 for the artist.
     * Both text records and their header are published in this update. */
    next.playing.artist_known = next.header[2] == 0x0a &&
                                next.attrs[1][0] == 0x07 && next.rows[1][0];
    if (next.playing.artist_known) {
        memcpy(next.playing.artist, next.rows[1], sizeof(next.playing.artist));
    } else {
        next.playing.artist[0] = '\0';
    }
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

static void clear_playing(PanelDisplay *display, PanelDisplayUpdate update,
                          void *opaque)
{
    Cdj850Playing *playing = &display->state.playing;

    display->next_wave_part = 0;
    if (playing->artist_known || playing->waveform_known ||
        playing->strips_known || playing->memory_lamp) {
        memset(playing, 0, sizeof(*playing));
        update(&display->state, PANEL_DISPLAY_PLAYING, true, opaque);
    }
}

static void publish_track(PanelDisplay *display, bool known, uint8_t number,
                          PanelDisplayUpdate update, void *opaque)
{
    PanelDisplayState *state = &display->state;

    if (state->track_known == known && state->track_number == number) {
        return;
    }
    clear_playing(display, update, opaque);
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

static void publish_auto_cue(PanelDisplay *display, bool enabled,
                             PanelDisplayUpdate update, void *opaque)
{
    PanelDisplayState *state = &display->state;

    if (state->auto_cue != enabled) {
        state->auto_cue = enabled;
        update(state, PANEL_DISPLAY_AUTO_CUE, true, opaque);
    }
}

static void publish_info(PanelDisplay *display, bool enabled,
                         PanelDisplayUpdate update, void *opaque)
{
    PanelDisplayState *state = &display->state;

    if (state->info_lamp != enabled) {
        state->info_lamp = enabled;
        update(state, PANEL_DISPLAY_INFO, true, opaque);
    }
}

static void publish_browse(PanelDisplay *display, bool enabled,
                           PanelDisplayUpdate update, void *opaque)
{
    PanelDisplayState *state = &display->state;

    if (state->browse_lamp != enabled) {
        state->browse_lamp = enabled;
        update(state, PANEL_DISPLAY_BROWSE, true, opaque);
    }
}

static void publish_menu(PanelDisplay *display, bool enabled,
                           PanelDisplayUpdate update, void *opaque)
{
    PanelDisplayState *state = &display->state;

    if (state->menu_lamp != enabled) {
        state->menu_lamp = enabled;
        update(state, PANEL_DISPLAY_MENU, true, opaque);
    }
}

static void publish_tag_list(PanelDisplay *display, bool enabled,
                           PanelDisplayUpdate update, void *opaque)
{
    PanelDisplayState *state = &display->state;

    if (state->tag_list_lamp != enabled) {
        state->tag_list_lamp = enabled;
        update(state, PANEL_DISPLAY_TAG_LIST, true, opaque);
    }
}

void cdj850_panel_display_reset_readouts(PanelDisplay *display, uint64_t ns,
                                        PanelDisplayUpdate update, void *opaque)
{
    display->readout_reset_ns = ns;
    display->have_header = false;
    display->header_only = false;
    display->next_sub = 0;
    display->payload_len = 0;
    display->quiet_transactions = 0;
    publish_track(display, false, 0, update, opaque);
    publish_time(display, &(Cdj850Time){0}, update, opaque);
    publish_bpm(display, 0, update, opaque);
    display->have_pitch_flags = false;
    publish_pitch(display, (Cdj850Pitch){0}, update, opaque);
    publish_master_tempo(display, false, update, opaque);
    publish_auto_cue(display, false, update, opaque);
    publish_info(display, false, update, opaque);
    publish_browse(display, false, update, opaque);
    clear_playing(display, update, opaque);
    publish_tag_list(display, false, update, opaque);
    publish_menu(display, false, update, opaque);
}

static void panel_playing_status(PanelDisplay *display, const uint8_t *tx,
                                 PanelDisplayUpdate update, void *opaque)
{
    Cdj850Playing *playing = &display->state.playing;
    /* MAIN 0x0c0637cc sends elapsed columns and playing-address flags.
     * Byte 9 bits 1 and 0 select slow and fast flash; bit 2 is the lit
     * phase. In steady playback bit 2 is set in REMAIN, clear in elapsed. */
    unsigned progress = tx[4];
    bool remain = (tx[13] & 1) != 0;
    bool flashing = (tx[9] & 0x03) != 0;
    bool phase_on = (tx[9] & 0x04) != 0;
    bool address_on = progress <= CDJ850_OVERVIEW_COLUMNS &&
                      (phase_on || (!remain && !flashing));
    /* MAIN 0x0c0642cc emits one column for the current cue or loop-in. */
    unsigned cue = tx[10] <= CDJ850_OVERVIEW_COLUMNS ? tx[10] : 0;
    bool memory_lamp = (tx[11] & 0x10) != 0;
    bool changed = playing->memory_lamp != memory_lamp;

    for (unsigned i = 0; i < CDJ850_OVERVIEW_COLUMNS; i++) {
        bool address_here = address_on && (remain ? i >= progress : i < progress);
        bool cue_here = i + 1 == cue;

        changed |= playing->position[i] != address_here || playing->cue[i] != cue_here;
        playing->position[i] = address_here;
        playing->cue[i] = cue_here;
    }
    playing->memory_lamp = memory_lamp;
    playing->cue_lamp = cue != 0;
    if (changed) {
        playing->strips_known = true;
        update(&display->state, PANEL_DISPLAY_PLAYING, true, opaque);
    }
}

static void panel_auxiliary(PanelDisplay *display, const uint8_t *tx,
                            PanelDisplayUpdate update, void *opaque)
{
    Cdj850Playing *playing = &display->state.playing;

    if (tx[0] != 0x60 && (tx[0] < 0x70 || tx[0] > 0x77)) {
        return;
    }
    if (cdj850_panel_sum8ec(tx, 15) != tx[15]) {
        if (tx[0] != 0x60) {
            display->next_wave_part = 0;
        }
        return;
    }

    if (tx[0] == 0x60) {
        /* 0x0c06664e packs saved cue and loop start columns MSB first. */
        bool changed = false;

        for (unsigned i = 0; i < CDJ850_OVERVIEW_COLUMNS; i++) {
            bool lit = (tx[2 + i / 8] & (0x80 >> (i % 8))) != 0;

            changed |= playing->memory[i] != lit;
            playing->memory[i] = lit;
        }
        if (changed) {
            playing->strips_known = true;
            update(&display->state, PANEL_DISPLAY_PLAYING, true, opaque);
        }
        return;
    }

    /* 0x0c0662ae sends a 100-column overview starting at 0x70 byte 3.
     * A new first frame supersedes incomplete data. Subsequent frames
     * contribute bytes 1..14, with the final four samples on 0x77. */
    unsigned part = tx[0] - 0x70;
    if (part && part != display->next_wave_part) {
        display->next_wave_part = 0;
        return;
    }
    unsigned offset = part ? 12 + 14 * (part - 1) : 0;
    unsigned length = part == 7 ? 4 : part ? 14 : 12;
    const uint8_t *samples = tx + (part ? 1 : 3);

    for (unsigned i = 0; i < length; i++) {
        if (samples[i] > 15) {
            display->next_wave_part = 0;
            return;
        }
        display->pending_waveform[offset + i] = (samples[i] + 2) / 3;
    }
    display->next_wave_part = part < 7 ? part + 1 : 0;
    if (part == 7 && memcmp(playing->waveform, display->pending_waveform,
                            sizeof(playing->waveform)) != 0) {
        memcpy(playing->waveform, display->pending_waveform,
               sizeof(playing->waveform));
        playing->waveform_known = false;
        for (unsigned i = 0; i < CDJ850_OVERVIEW_COLUMNS; i++) {
            playing->waveform_known |= playing->waveform[i] != 0;
        }
        update(&display->state, PANEL_DISPLAY_PLAYING, true, opaque);
    }
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
        /* MAIN 0x0c0642c6 sets the A.CUE bit copied at 0x0c065e4e. */
        publish_auto_cue(display, (tx[12] & 1) != 0, update, opaque);
        /* MAIN 0x0c063444..0x0c063480 sets the four navigation LEDs.
         * The copy at 0x0c065e26 puts those bits in this byte. */
        publish_browse(display, (tx[7] & 0x10) != 0, update, opaque);
        publish_tag_list(display, (tx[7] & 0x20) != 0, update, opaque);
        publish_info(display, (tx[7] & 0x40) != 0, update, opaque);
        publish_menu(display, (tx[7] & 0x80) != 0, update, opaque);
        panel_playing_status(display, tx, update, opaque);
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
    bool fresh = !display->readout_reset_ns || start_ns > display->readout_reset_ns;

    if (count < 2) {
        display->have_pitch_flags = false;
        display->next_wave_part = 0;
        return;
    }
    if (fresh) {
        panel_status(display, tx[0], update, opaque);
    }
    for (unsigned i = 0; i < count; i++) {
        if (fresh) {
            panel_auxiliary(display, tx[i], update, opaque);
        }
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
    } else if (field == PANEL_DISPLAY_AUTO_CUE) {
        snprintf(out, size, "cdj850-panel: display auto_cue=%u (%" PRIu64 " ms)",
                 (unsigned)state->auto_cue, ms);
    } else if (field == PANEL_DISPLAY_INFO) {
        snprintf(out, size, "cdj850-panel: display info=%u (%" PRIu64 " ms)",
                 (unsigned)state->info_lamp, ms);
    } else if (field == PANEL_DISPLAY_BROWSE) {
        snprintf(out, size, "cdj850-panel: display browse=%u (%" PRIu64 " ms)",
                 (unsigned)state->browse_lamp, ms);
    } else if (field == PANEL_DISPLAY_MENU) {
        snprintf(out, size, "cdj850-panel: display menu=%u (%" PRIu64 " ms)",
                 (unsigned)state->menu_lamp, ms);
    } else if (field == PANEL_DISPLAY_TAG_LIST) {
        snprintf(out, size, "cdj850-panel: display tag_list=%u (%" PRIu64 " ms)",
                 (unsigned)state->tag_list_lamp, ms);
    } else if (field == PANEL_DISPLAY_PLAYING) {
        const Cdj850Playing *playing = &state->playing;
        unsigned memory = 0, position = 0, cue = 0;

        for (unsigned i = 0; i < CDJ850_OVERVIEW_COLUMNS; i++) {
            memory += playing->memory[i];
            position += playing->position[i];
            if (playing->cue[i]) {
                cue = i + 1;
            }
        }
        snprintf(out, size,
                 "cdj850-panel: display playing wave=%u memory=%u position=%u "
                 "cue=%u (%" PRIu64 " ms)",
                 (unsigned)playing->waveform_known, memory, position, cue, ms);
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
