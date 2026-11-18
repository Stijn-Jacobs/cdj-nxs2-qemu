/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SCIF1 display status and text shared by the live peer and host replay. */
#ifndef CDJ850_PANEL_DISPLAY_H
#define CDJ850_PANEL_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "panel_wire.h"
#include "vfd_pitch.h"
#include "vfd_playing.h"
#include "vfd_time.h"

#define PANEL_DISP_ROWS 3
#define PANEL_DISP_BUF (47 * 14)
#define PANEL_DISP_LOG_LEN 1024

typedef struct PanelDisplayState {
    uint8_t header[PANEL_FRAME_LEN];
    uint8_t tags[PANEL_DISP_ROWS];
    uint8_t attrs[PANEL_DISP_ROWS][3];
    char rows[PANEL_DISP_ROWS][256];
    unsigned updates;
    Cdj850Time time;
    Cdj850Pitch pitch;
    Cdj850Playing playing;
    uint16_t bpm;
    bool bpm_known;
    bool master_tempo;
    bool auto_cue;
    bool info_lamp;
    bool browse_lamp;
    bool tag_list_lamp;
    bool menu_lamp;
    uint8_t track_number;
    bool track_known;
    bool valid;
} PanelDisplayState;

typedef struct PanelDisplay {
    PanelDisplayState state;
    uint64_t readout_reset_ns;
    uint8_t pending_header[PANEL_FRAME_LEN];
    uint8_t payload[PANEL_DISP_BUF];
    unsigned payload_len;
    unsigned next_sub;
    unsigned quiet_transactions;
    bool have_header;
    bool header_only;
    bool have_pitch_flags;
    uint8_t pitch_sign, pitch_decimal_flags, pitch_range_flags;
    uint8_t pending_waveform[CDJ850_OVERVIEW_COLUMNS];
    unsigned next_wave_part;
} PanelDisplay;

typedef enum PanelDisplayField {
    PANEL_DISPLAY_HEADER,
    PANEL_DISPLAY_ROWS,
    PANEL_DISPLAY_TRACK,
    PANEL_DISPLAY_TIME,
    PANEL_DISPLAY_BPM,
    PANEL_DISPLAY_PITCH,
    PANEL_DISPLAY_MASTER_TEMPO,
    PANEL_DISPLAY_AUTO_CUE,
    PANEL_DISPLAY_INFO,
    PANEL_DISPLAY_BROWSE,
    PANEL_DISPLAY_TAG_LIST,
    PANEL_DISPLAY_MENU,
    PANEL_DISPLAY_PLAYING,
} PanelDisplayField;

typedef void (*PanelDisplayUpdate)(const PanelDisplayState *state,
                                   PanelDisplayField field, bool changed,
                                   void *opaque);

uint8_t cdj850_panel_sum8ec(const uint8_t *p, unsigned n);
void cdj850_panel_display_feed(PanelDisplay *display, const uint8_t tx[16],
                              PanelDisplayUpdate update, void *opaque);
void cdj850_panel_display_reset_readouts(PanelDisplay *display, uint64_t ns,
                                     PanelDisplayUpdate update, void *opaque);
void cdj850_panel_display_transaction(PanelDisplay *display, uint64_t start_ns,
                                     unsigned count,
                                     const uint8_t tx[][PANEL_FRAME_LEN],
                                     PanelDisplayUpdate update, void *opaque);
void cdj850_panel_display_format(char *out, size_t size,
                                const PanelDisplayState *state, PanelDisplayField field,
                                uint64_t ms);

#endif
