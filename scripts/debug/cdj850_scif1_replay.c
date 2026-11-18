/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Standalone SCIF1 capture reader. Build with xc/panel_display.c. */
#include "panel_display.h"
#include "vfd_render.h"

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Transaction {
    uint64_t start_ns;
    uint64_t end_ns;
    unsigned count;
    uint8_t tx[PANEL_TX_MAX][PANEL_FRAME_LEN];
} Transaction;

static bool decimal(const char **cursor, char delimiter, uint64_t *value)
{
    char *end;
    uintmax_t parsed;

    if (**cursor < '0' || **cursor > '9') {
        return false;
    }
    errno = 0;
    parsed = strtoumax(*cursor, &end, 10);
    if (errno || *end != delimiter || parsed > UINT64_MAX) {
        return false;
    }
    *value = parsed;
    *cursor = end + 1;
    return true;
}

static int hex_digit(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    return -1;
}

static bool parse_transaction(const char *line, Transaction *t)
{
    uint64_t seq, previous_ns, count;

    if (strncmp(line, "T ", 2) != 0) {
        return false;
    }
    line += 2;
    if (!decimal(&line, ' ', &seq) || seq > UINT32_MAX ||
        !decimal(&line, ' ', &t->start_ns) ||
        !decimal(&line, ' ', &t->end_ns) || t->start_ns > t->end_ns) {
        return false;
    }
    previous_ns = t->start_ns;
    /* The frame count ends the record for an empty window. */
    const char *count_end = line + strspn(line, "0123456789");

    if ((*count_end != ' ' && *count_end != '\n') ||
        !decimal(&line, *count_end, &count) || count > PANEL_TX_MAX) {
        return false;
    }
    t->count = count;
    if (!count) {
        return *count_end == '\n' && *line == '\0';
    }
    if (*count_end != ' ') {
        return false;
    }
    for (unsigned i = 0; i < count; i++) {
        uint64_t ns;

        if (!decimal(&line, ':', &ns) || ns < previous_ns || ns > t->end_ns) {
            return false;
        }
        previous_ns = ns;
        for (unsigned j = 0; j < PANEL_FRAME_LEN; j++) {
            int hi = hex_digit(*line++);

            if (hi < 0) {
                return false;
            }
            int lo = hex_digit(*line++);

            if (lo < 0) {
                return false;
            }
            t->tx[i][j] = (hi << 4) | lo;
        }
        if (*line++ != (i + 1 == count ? '\n' : ' ')) {
            return false;
        }
    }
    return *line == '\0';
}

static void report_display(const PanelDisplayState *state, PanelDisplayField field,
                           bool changed, void *opaque)
{
    const uint64_t *ns = opaque;
    char message[PANEL_DISP_LOG_LEN];

    if (changed) {
        cdj850_panel_display_format(message, sizeof(message), state, field,
                                    *ns / 1000000);
        puts(message);
    }
}

static int replay(FILE *in, PanelDisplay *display)
{
    char line[PANEL_CAPTURE_LINE_MAX];
    size_t bytes = sizeof(PANEL_CAPTURE_MAGIC) - 1;
    unsigned line_no = 1;

    if (!fgets(line, sizeof(line), in) || strcmp(line, PANEL_CAPTURE_MAGIC)) {
        fprintf(stderr, "Expected CDJ850-SCIF1 version 1 capture\n");
        return 1;
    }
    while (fgets(line, sizeof(line), in)) {
        Transaction t;
        ++line_no;
        bytes += strlen(line);
        if (bytes > PANEL_CAPTURE_MAX_BYTES) {
            fprintf(stderr, "Capture exceeds its byte limit\n");
            return 1;
        }
        if (strcmp(line, PANEL_CAPTURE_LIMIT) == 0 && fgetc(in) == EOF) {
            fprintf(stderr, "Capture stopped at its byte limit\n");
            break;
        }
        if (strncmp(line, "R ", 2) == 0) {
            const char *cursor = line + 2;
            uint64_t ns;

            if (decimal(&cursor, '\n', &ns) && *cursor == '\0') {
                cdj850_panel_display_reset_readouts(display, ns, report_display, &ns);
                continue;
            }
        }
        if (!parse_transaction(line, &t)) {
            fprintf(stderr, "Malformed capture on line %u\n", line_no);
            return 1;
        }
        cdj850_panel_display_transaction(display, t.start_ns, t.count, t.tx,
                                         report_display, &t.end_ns);
    }
    if (ferror(in) || fflush(stdout) == EOF) {
        fprintf(stderr, "Replay I/O error\n");
        return 1;
    }
    return 0;
}

static bool save_ppm(const char *path, const PanelDisplayState *state)
{
    static uint32_t pixels[FL_HEIGHT][FL_WIDTH];
    FlState frame = {
        .mode = state->header[2],
        .has_text = state->valid,
        .time = state->time,
        .pitch = state->pitch,
        .master_tempo = state->master_tempo,
        .bpm = state->bpm,
        .bpm_known = state->bpm_known,
        .track_known = state->track_known,
        .track_number = state->track_number,
    };
    FlCanvas canvas = {(uint8_t *)pixels, FL_WIDTH, FL_HEIGHT, sizeof(pixels[0])};
    FILE *out = fopen(path, "wbx");

    if (!out) {
        perror(path);
        return false;
    }
    memcpy(frame.rows, state->rows, sizeof(frame.rows));
    memcpy(frame.tags, state->tags, sizeof(frame.tags));
    fl_render(&canvas, &frame);
    fprintf(out, "P6\n%d %d\n255\n", FL_WIDTH, FL_HEIGHT);
    for (int y = 0; y < FL_HEIGHT && !ferror(out); y++) {
        uint8_t row[FL_WIDTH][3];

        for (int x = 0; x < FL_WIDTH; x++) {
            uint32_t rgb = pixels[y][x];

            row[x][0] = rgb >> 16;
            row[x][1] = rgb >> 8;
            row[x][2] = rgb;
        }
        fwrite(row, sizeof(row), 1, out);
    }
    bool write_ok = !ferror(out);

    if (fclose(out) != 0 || !write_ok) {
        perror(path);
        return false;
    }
    return true;
}

int main(int argc, char **argv)
{
    PanelDisplay display = {0};

    if (argc != 2 && (argc != 4 || strcmp(argv[2], "--ppm") != 0)) {
        fprintf(stderr, "Usage: %s CAPTURE.scif1 [--ppm OUT.ppm]\n", argv[0]);
        return 2;
    }
    FILE *in = fopen(argv[1], "rb");

    if (!in) {
        perror(argv[1]);
        return 1;
    }
    int result = replay(in, &display);

    fclose(in);
    if (result == 0 && argc == 4 && !save_ppm(argv[3], &display.state)) {
        return 1;
    }
    return result;
}
