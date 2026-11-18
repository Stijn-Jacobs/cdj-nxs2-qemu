#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Render synthetic DATA FL fixtures with the emulator's actual drawing code.

No firmware, emulator build, photos or third-party Python packages required.
These fixtures exercise the layout; they are not captured firmware states.
--photo-readouts adds the photographed numeric values for visual comparison only.
It does not change emulator defaults or decode additional firmware fields.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import zlib

ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "hw/cdj/boards/cdj850/xc"
HARNESS = r'''
#include <stdio.h>
#include "vfd_render.h"
static uint32_t pixels[FL_HEIGHT][FL_WIDTH];
int main(int argc, char **argv) {
    FlState state = {0};
    FlCanvas canvas = {(uint8_t *)pixels, FL_WIDTH, FL_HEIGHT, FL_WIDTH * 4};
    if (argc != 3) return 2;
    state.all_on = !strcmp(argv[1], "all-on");
    state.has_text = true;
    state.browse_lamp = strcmp(argv[1], "loaded") != 0;
    if (!strcmp(argv[1], "no-disc")) strcpy(state.rows[1], "NO DISC");
    if (!strcmp(argv[1], "no-usb")) strcpy(state.rows[1], "NO USB");
    if (!strcmp(argv[1], "browser")) {
        strcpy(state.rows[0], "[ARTIST]");
        strcpy(state.rows[1], "[ALBUM]");
        strcpy(state.rows[2], "[TRACK]");
        state.tags[0] = 0x25; state.tags[1] = 2; state.tags[2] = 3;
        state.mode = 1;
    }
    if (!strcmp(argv[1], "loaded")) {
        strcpy(state.rows[0], "EXAMPLE.WAV");
        state.mode = 0x0a;
        state.info_lamp = true;
        if (!strcmp(argv[2], "photo")) {
            /* PLAYING_FIXTURE */
        }
    }
    fl_render(&canvas, &state);
    /* Offline-only reference values. Never injected into the live adapter. */
    if (!strcmp(argv[2], "photo") && !state.all_on) {
        bool loaded = !strcmp(argv[1], "loaded");
        int track[] = {0, loaded ? 1 : 0};
        int digits[] = {0, loaded ? 5 : 0, loaded ? 4 : 0,
                        loaded ? 4 : 0, loaded ? 2 : 0, loaded ? 8 : 0};
        const int positions[] = {316,381,479,542,632,695};
        for (int i = 0; i < 2; i++)
            fl_digit(&canvas, 35 + i * 66, 40, 44, 86, track[i]);
        for (int i = 0; i < 6; i++)
            fl_digit(&canvas, positions[i], 40, 44, 86, digits[i]);
        fl_label_xy(&canvas, 157, 40, "REMAIN", 4, 3, FL_BLUE);
        fl_box(&canvas, 157, 77, 142, 49, "A.CUE", FL_RED);
        fl_rect(&canvas, 449, 65, 6, 6, FL_BLUE);
        fl_rect(&canvas, 449, 95, 6, 6, FL_BLUE);
        fl_label_xy(&canvas, 430, 110, "M", 3, 2, FL_BLUE);
        fl_label_xy(&canvas, 590, 110, "S", 3, 2, FL_BLUE);
        fl_label_xy(&canvas, 742, 110, "F", 3, 2, FL_BLUE);
        const FlPitch zero_pitch = {{0,0,0}, false, false, false, 1};
        fl_pitch(&canvas, &zero_pitch);
        if (loaded) {
            int bpm[] = {1,6,0};
            for (int i = 0; i < 3; i++)
                fl_digit(&canvas, 1061 + i * 46, 47, 36, 64, bpm[i]);
        }
        fl_pitch_range(&canvas, 10, true);
    }
    for (int y = 0; y < FL_HEIGHT; y++) {
        for (int x = 0; x < FL_WIDTH; x++) {
            unsigned char rgb[] = {pixels[y][x] >> 16, pixels[y][x] >> 8,
                                   pixels[y][x]};
            fwrite(rgb, 1, 3, stdout);
        }
    }
    return ferror(stdout) ? 1 : 0;
}
'''


def playing_fixture_code():
    """Reference-only data; never copied into the emulator's display state."""
    fixture = json.loads((ROOT / "tests/fixtures/cdj850-now-playing.json").read_text())
    wave = fixture["waveform"]
    if len(wave) != 100 or any(not isinstance(v, int) or not 0 <= v <= 5 for v in wave):
        raise ValueError("invalid reference overview")
    lines = [f'strcpy(state.rows[0], {json.dumps(fixture["title"])});',
             f'strcpy(state.playing.artist, {json.dumps(fixture["artist"])});',
             'state.playing.artist_known = true;',
             'state.playing.waveform_known = true;',
             'state.playing.strips_known = true;',
             'state.playing.memory_lamp = state.playing.cue_lamp = true;',
             'const uint8_t overview[100] = {' + ','.join(map(str, wave)) + '};',
             'memcpy(state.playing.waveform, overview, sizeof(overview));']
    for field in ("memory", "position", "cue"):
        for column in fixture[field + "_columns"]:
            if not isinstance(column, int) or not 0 <= column < 100:
                raise ValueError("invalid strip column")
            lines.append(f'state.playing.{field}[{column}] = 1;')
    return '\n'.join(lines)


def png_chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data +
            struct.pack(">I", zlib.crc32(kind + data)))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("state", choices=["all-on", "no-disc", "no-usb", "browser", "loaded"])
    parser.add_argument("output", type=Path)
    parser.add_argument("--photo-readouts", action="store_true",
                        help="add offline-only numeric/indicator values from the photos")
    args = parser.parse_args()
    cc = shutil.which("cc")
    if not cc:
        parser.error("a C compiler is required")
    with tempfile.TemporaryDirectory(prefix="cdj850-fl-") as directory:
        source = Path(directory) / "preview.c"
        binary = Path(directory) / "preview"
        source.write_text(HARNESS.replace("/* PLAYING_FIXTURE */", playing_fixture_code()))
        subprocess.run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                        "-I", str(HEADER), str(source), "-o", str(binary)], check=True)
        rgb = subprocess.check_output([str(binary), args.state,
                                       "photo" if args.photo_readouts else "unknown"])
    width, height = 1280, 480
    if len(rgb) != width * height * 3:
        raise ValueError("unexpected renderer dimensions")
    rows = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    data = (b"\x89PNG\r\n\x1a\n" +
            png_chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
            png_chunk(b"IDAT", zlib.compress(rows)) + png_chunk(b"IEND", b""))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print(args.output.resolve())


if __name__ == "__main__":
    main()
