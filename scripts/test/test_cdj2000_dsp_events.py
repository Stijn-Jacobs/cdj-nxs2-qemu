#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compile the production transcript writer and HPI handlers; no QEMU runs."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
BOARD = ROOT / "hw/cdj/boards/cdj2000/dsp_c6747.c"
HOST = ROOT / "hw/cdj/boards/cdj2000/dsp_host.c"


def main():
    host, board = HOST.read_text(), BOARD.read_text()
    writer = host[host.index("void cdj_dsp_host_events_init("):host.index(
        "static bool host_stopped(")]
    handlers = board[board.index("static uint64_t hpi_read("):board.index(
        "static const MemoryRegionOps hpi_ops")]
    command = host[host.index("static void host_command("):host.index(
        "/*\n * Snapshots.")]
    program = r'''
#include <glib.h>
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define HPIC_HINT 4
#define HPIC_DSPINT 2
#define HPIC_HWOB 1
#define error_report(...) fprintf(stderr, __VA_ARGS__)
typedef uint64_t hwaddr;
typedef struct c66x_stats { uint64_t packets, cycles; } c66x_stats;
typedef struct CdjDspHost {
    FILE *event_log;
    uint64_t event_sequence, commands;
    unsigned boot_phase, command, hpic;
    bool enabled;
    void *core, *chip;
    void (*set_pins)(void *, unsigned);
    bool locked;
} CdjDspHost;
typedef struct CdjC6747 {
    CdjDspHost host;
    uint32_t hpia;
    uint64_t reads, writes, dspints, dropped;
    unsigned polls;
    uint8_t ram[16];
} CdjC6747;
static void c66x_get_stats(void *core, c66x_stats *st)
{ assert(core); *st = (c66x_stats){ UINT64_MAX, UINT64_MAX - 1 }; }
static void cdj_dsp_host_lock(CdjDspHost *h)
{ assert(!h->locked); h->locked = true; }
static void cdj_dsp_host_unlock(CdjDspHost *h)
{ assert(h->locked); h->locked = false; }
G_DEFINE_AUTOPTR_CLEANUP_FUNC(CdjDspHost, cdj_dsp_host_unlock)
static uint8_t *dsp_ram(CdjC6747 *s, uint32_t a)
{ return a >= 0x11800000 && a < 0x11800010 ? s->ram + a - 0x11800000 : NULL; }
static void stl_le_p(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; i++) p[i] = v >> (8 * i); }
static uint32_t cdj_dsp_host_read_word(CdjDspHost *h, uint32_t a)
{
    uint8_t *p = dsp_ram(h->chip, a & ~3u);
    return p ? p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
               | ((uint32_t)p[3] << 24) : 0;
}
static bool cdj_dsp_host_write_word(CdjDspHost *h, uint32_t a, uint32_t v)
{
    uint8_t *p = dsp_ram(h->chip, a & ~3u);
    if (p) stl_le_p(p, v);
    return p != NULL;
}
static uint32_t cdj_dsp_host_hpic(CdjDspHost *h) { return h->hpic | 8; }
static bool cdj_dsp_host_main_hpic(CdjDspHost *h, uint32_t v)
{ h->hpic = (h->hpic & ~(v & HPIC_HINT)) | (v & HPIC_DSPINT); return v & 2; }
static void c66x_invalidate(void *core, uint32_t a, unsigned n)
{ (void)core; (void)a; (void)n; }
static void cdj_dsp_count(unsigned *polls, uint32_t a, uint32_t v)
{ (void)polls; (void)a; (void)v; }
static void hpi_note_write(CdjC6747 *s, uint32_t a) { (void)s; (void)a; }
static void dsp_start(CdjC6747 *s) { (void)s; assert(false); }
static void host_react(CdjDspHost *h) { (void)h; }
static void set_pins(void *chip, unsigned bits) { (void)chip; (void)bits; }
''' + writer + handlers + command + r'''
int main(int argc, char **argv)
{
    assert(argc == 2);
    CdjC6747 s = { 0 };
    CdjDspHost *h = &s.host;
    h->chip = &s;
    h->set_pins = set_pins;
    cdj_dsp_host_events_init(h, NULL);
    cdj_dsp_host_events_init(h, "");
    cdj_dsp_host_event(h, "disabled", 0, 0, 0, 0);
    assert(!h->event_log && !h->event_sequence);
    cdj_dsp_host_events_init(h, argv[1]);
    assert(h->event_log);
    cdj_dsp_host_event(h, "reset_assert", 0, 0, 0, 0);
    struct stat st;
    assert(!stat(argv[1], &st) && st.st_size > 9); /* flushed before close */
    h->hpic = HPIC_HINT;
    cdj_dsp_host_event(h, "rom_hpi_ready", 0, 0, 1, 0);
    hpi_write(&s, 0x40000, 0x11800000, 4);
    hpi_write(&s, 0x80000, 0xffffffff, 4);
    hpi_write(&s, 0xc0000, 0x12345678, 4);
    assert(s.hpia == 0x11800004);
    assert(hpi_read(&s, 0xc0000, 4) == 0x12345678);
    assert(hpi_read(&s, 0x80000, 4) == 0x12345678);
    assert(s.hpia == 0x11800008);
    unsigned seq = h->event_sequence;
    hpi_read(&s, 0, 4);
    hpi_read(&s, 0x40000, 4);
    assert(h->event_sequence == seq); /* polling is not recorded */
    host_command(h, 3);
    host_command(h, 3);
    host_command(h, 0);
    h->core = h; /* supplies large counters without a core or emulator */
    hpi_write(&s, 0, 6, 4);
    assert(!h->locked);
    assert(!fclose(h->event_log));
}
'''
    # Keep all generated files inside this worktree.
    with tempfile.TemporaryDirectory(prefix="dsp-events-", dir=ROOT) as directory:
        tmp = Path(directory)
        (tmp / "test.c").write_text(program)
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=gnu11", "-Wall", "-Wextra", "-Werror", str(tmp / "test.c"),
            "-o", str(tmp / "test"), *flags], check=True)
        path = tmp / "events.jsonl"
        path.write_text("existing\n")
        subprocess.run([str(tmp / "test"), str(path)], check=True)
        lines = path.read_text().splitlines()
        assert lines.pop(0) == "existing"  # append, not truncate
        expected = [
            ("reset_assert", 0, 0, 0, 0, 0, False, False),
            ("rom_hpi_ready", 0, 0, 1, 0, 0, True, False),
            ("hpi_host_address_write", 0x40000, 0, 0x11800000, 4, 0, True, False),
            ("hpi_host_data_autoincrement_write", 0x80000, 0x11800000, 0xffffffff, 4, 0, True, False),
            ("hpi_host_data_fixed_write", 0xc0000, 0x11800004, 0x12345678, 4, 0, True, False),
            ("hpi_host_data_read", 0xc0000, 0x11800004, 0x12345678, 4, 0, True, False),
            ("hpi_host_data_read", 0x80000, 0x11800004, 0x12345678, 4, 0, True, False),
            ("boot_phase", 0, 0, 3, 0, 3, True, False),
            ("boot_phase", 0, 0, 0, 0, 0, True, False),
            ("hpi_host_control_write", 0, 0x11800008, 6, 4, 0, False, True),
        ]
        assert len(lines) == len(expected)
        keys = "event offset address value size boot_phase hint dspint".split()
        for sequence, (line, fields) in enumerate(zip(lines, expected), 1):
            record = dict(sequence=sequence, **dict(zip(keys, fields)))
            record.update(packets=(2**64 - 1 if sequence == 10 else 0),
                          cycles=(2**64 - 2 if sequence == 10 else 0))
            assert line == json.dumps(record, separators=(",", ":")), line
        print("DSP transcript format, append/flush, HPI addresses and phases: PASS")


if __name__ == "__main__":
    main()
