#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host-only CUE/E0 regression check; no QEMU process or firmware needed.

Compile the production parser and pure status helper with GLib. The existing
BF531 host harness has no QEMU dependencies, so use only these independent
sections of the ATAPI source instead of mocking its timers and MMIO.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "hw/cdj/boards/cdj2000/cdj2000_ata.c"


def main():
    source = SOURCE.read_text()
    media = source[source.index("#define CD_RAW"):source.index(
        "/* The track an LBA falls in")]
    state = source[source.index("static void ata_state_reply("):source.index(
        "/* Run the CDB.")]
    program = """
#include <glib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
""" + media + state + r'''
int main(int argc, char **argv)
{
    char *err = NULL;
    CdjDisc *d;
    uint8_t out[4];
    const int states[] = { 1, 2, 3, 9 };
    unsigned i;

    g_assert_cmpint(argc, ==, 2);
    g_assert_cmpint(chdir(argv[1]), ==, 0);
    d = cdj_disc_open("mixed.cue", &err);
    g_assert_null(err);
    g_assert_nonnull(d);
    g_assert_cmpint(d->ntracks, ==, 2);
    g_assert_cmpint(d->track[0].control, ==, 0);
    g_assert_cmpint(d->track[1].control, ==, 4);
    g_assert_cmpint(d->track[1].index0, ==, 2);
    g_assert_cmpint(d->track[1].start, ==, 3);
    g_assert_cmpuint(d->track[1].file_offset, ==, 2 * 2352);
    g_assert_cmpuint(d->leadout, ==, 6);
    cdj_disc_free(d);

    d = cdj_disc_open("multi.cue", &err);
    g_assert_nonnull(d);
    g_assert_null(err);
    g_assert_cmpint(d->nfiles, ==, 2);
    g_assert_cmpint(d->track[1].start, ==, 6);
    g_assert_cmpuint(d->leadout, ==, 8);
    cdj_disc_free(d);

    d = cdj_disc_open("data.iso", &err);
    g_assert_nonnull(d);
    g_assert_cmpint(d->track[0].sector, ==, 2048);
    g_assert_cmpuint(d->leadout, ==, 2);
    cdj_disc_free(d);

    const char *bad[] = { "gap.cue", "notrack.cue", "noindex.cue",
                          "badtime.cue", "missing.cue" };
    for (i = 0; i < G_N_ELEMENTS(bad); i++) {
        d = cdj_disc_open(bad[i], &err);
        g_assert_null(d);
        g_assert_nonnull(err);
        g_clear_pointer(&err, g_free);
    }
    for (i = 0; i < G_N_ELEMENTS(states); i++) {
        memset(out, 0xff, sizeof(out));
        ata_state_reply(out, '<', states[i]);
        g_assert_cmpmem(out, 3, "<\0\1", 3);
        g_assert_cmpint(out[3], ==, states[i]);
    }
    ata_state_reply(out, '?', 9);
    g_assert_cmpmem(out, 4, "?\0\1\0", 4);
    puts("CUE parser and E0 state reply: PASS");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="cdj-atapi-test-") as directory:
        tmp = Path(directory)
        (tmp / "audio.bin").write_bytes(bytes(6 * 2352))
        (tmp / "data.iso").write_bytes(bytes(2 * 2048))
        base = 'FILE "audio.bin" BINARY\n TRACK 01 AUDIO\n'
        cues = {
            "mixed.cue": base + ' INDEX 01 00:00:00\n TRACK 02 MODE1/2352\n INDEX 00 00:00:02\n INDEX 01 00:00:03\n',
            "multi.cue": base + ' INDEX 01 00:00:00\nFILE "data.iso" BINARY\n TRACK 02 MODE1/2048\n INDEX 01 00:00:00\n',
            "gap.cue": base + ' PREGAP 00:02:00\n',
            "notrack.cue": 'FILE "audio.bin" BINARY\nFILE "audio.bin" BINARY\n',
            "noindex.cue": base,
            "badtime.cue": base + ' INDEX 01 00:60:00\n',
        }
        for name, contents in cues.items():
            (tmp / name).write_text(contents)
        (tmp / "test.c").write_text(program)
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(tmp / "test.c"),
            "-o", str(tmp / "test"), *flags], check=True)
        subprocess.run([str(tmp / "test"), directory], check=True)


if __name__ == "__main__":
    main()
