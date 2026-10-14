# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-850 VFD frame decode (Task VFD-1): synthetic-frame render tests.

The panel link carries binary control frames, not text (see
notes/cdj850-bringup.md, Task VFD-1 section): the 6-byte panel exchange
(TX staging 0x0C7F671B, TX[0].bit7 always set by the TX-prep helper at
0x0C005D54) plus 16-bit command words. The panel CPU renders glass content
from these; MAIN sends indices only. vfd.c therefore passes the frame
bytes through as hex (index passthrough), synced on the builder-proven
TX[0].bit7 invariant. No firmware bytes appear here: every stimulus frame
below is hand-built and distinct from observed traffic.

Method: the test compiles the REAL hw/cdj/boards/cdj850/xc/vfd.c with
stub QEMU headers (no emulator needed, skipped without a C compiler),
feeds synthetic TX byte streams through vfd_note_tx(), and asserts the
exact vfd_text() strings.
"""
import os
import shutil
import subprocess
import sys

import pytest

from helpers import path

STUBS = {
    "cdj_common.h": (
        "#ifndef STUB_CDJ_COMMON_H\n#define STUB_CDJ_COMMON_H\n"
        "#include <stdint.h>\n#include <stdbool.h>\n"
        "#include <stdio.h>\n#include <string.h>\n"
        "#endif\n"
    ),
    "ui/console.h": (
        "#ifndef STUB_UI_CONSOLE_H\n#define STUB_UI_CONSOLE_H\n"
        "typedef struct QemuConsole QemuConsole;\n"
        "typedef struct DisplaySurface DisplaySurface;\n"
        "typedef struct GraphicHwOps {\n"
        "    void (*invalidate)(void *opaque);\n"
        "    void (*gfx_update)(void *opaque);\n"
        "} GraphicHwOps;\n"
        "static inline QemuConsole *graphic_console_init(void *a, int b,\n"
        "    const GraphicHwOps *c, void *d)\n"
        "{ (void)a; (void)b; (void)c; (void)d; return (QemuConsole *)0x1; }\n"
        "static inline void *qemu_create_displaysurface(int w, int h)\n"
        "{ (void)w; (void)h; return (void *)0x2; }\n"
        "static inline void dpy_gfx_replace_surface(QemuConsole *c, void *s)\n"
        "{ (void)c; (void)s; }\n"
        "static inline void qemu_console_resize(QemuConsole *c, int w, int h)\n"
        "{ (void)c; (void)w; (void)h; }\n"
        "static inline DisplaySurface *qemu_console_surface(QemuConsole *c)\n"
        "{ (void)c; return (DisplaySurface *)0x3; }\n"
        "static inline void dpy_gfx_update(QemuConsole *c, int x, int y,\n"
        "    int w, int h)\n"
        "{ (void)c; (void)x; (void)y; (void)w; (void)h; }\n"
        "#endif\n"
    ),
    "ui/surface.h": (
        "#ifndef STUB_UI_SURFACE_H\n#define STUB_UI_SURFACE_H\n"
        "static inline int surface_width(void *s) { (void)s; return 320; }\n"
        "static inline int surface_height(void *s) { (void)s; return 40; }\n"
        "static unsigned char stub_pixels[320 * 40 * 4];\n"
        "static inline void *surface_data(void *s)\n"
        "{ (void)s; return stub_pixels; }\n"
        "static inline int surface_stride(void *s) { (void)s; return 320 * 4; }\n"
        "#endif\n"
    ),
    "ui/vgafont.h": (
        "#ifndef STUB_UI_VGAFONT_H\n#define STUB_UI_VGAFONT_H\n"
        "static unsigned char vgafont16[256 * 16];\n"
        "#endif\n"
    ),
    "trace.h": (
        "#ifndef STUB_TRACE_H\n#define STUB_TRACE_H\n"
        "#include <stdio.h>\n"
        "#define info_report(fmt, ...) \\\n"
        "    fprintf(stderr, \"info: \" fmt \"\\n\", ##__VA_ARGS__)\n"
        "#endif\n"
    ),
    "_pre.h": (
        "#ifndef STUB_VFD_PRE_H\n#define STUB_VFD_PRE_H\n"
        "#include <stdlib.h>\n#include <stdint.h>\n"
        "#include <stdbool.h>\n#include <stdio.h>\n#include <string.h>\n"
        "#define QEMU_CLOCK_VIRTUAL 0\n"
        "static inline unsigned long long qemu_clock_get_ns(int c)\n"
        "{ (void)c; return 0ULL; }\n"
        "#ifndef info_report\n"
        "#define info_report(fmt, ...) \\\n"
        "    fprintf(stderr, \"info: \" fmt \"\\n\", ##__VA_ARGS__)\n"
        "#endif\n"
        "#endif\n"
    ),
}

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vfd.h"

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Feed one hex segment (spaces ignored), then print both lines. */
static void run_segment(const char *hex)
{
    for (const char *p = hex; *p; ) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        int hi = hexval(*p++);
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) { fprintf(stderr, "odd hex\n"); exit(2); }
        int lo = hexval(*p++);
        if (hi < 0 || lo < 0) { fprintf(stderr, "bad hex\n"); exit(2); }
        vfd_note_tx((unsigned char)((hi << 4) | lo));
    }
    char l0[64], l1[64];
    vfd_text(l0, sizeof l0, l1, sizeof l1);
    printf("L0:[%s]\nL1:[%s]\n", l0, l1);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    vfd_init();
    if (argc < 2) {
        char l0[64], l1[64];
        vfd_text(l0, sizeof l0, l1, sizeof l1);
        printf("L0:[%s]\nL1:[%s]\n", l0, l1);
        return 0;
    }
    for (int i = 1; i < argc; i++)
        run_segment(argv[i]);
    return 0;
}
"""


@pytest.fixture(scope="module")
def vfd_exe(tmp_path_factory):
    cc = (os.environ.get("CC") or shutil.which("gcc")
          or shutil.which("cc") or shutil.which("clang"))
    if not cc:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("vfd")
    for name, text in STUBS.items():
        p = d / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text, encoding="utf-8")
    main_c = d / "harness.c"
    main_c.write_text(HARNESS, encoding="utf-8")
    exe = d / ("harness.exe" if os.name == "nt" else "harness")
    r = subprocess.run(
        [cc, "-pipe", "-std=gnu11", "-O1", "-include", str(d / "_pre.h"),
         "-I", str(d),
         "-I", path("hw", "cdj", "boards", "cdj850", "xc"),
         str(main_c), path("hw", "cdj", "boards", "cdj850", "xc", "vfd.c"),
         "-o", str(exe)],
        capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        pytest.fail("cannot compile vfd.c harness:\n" + r.stderr[-3000:])

    def run(*segments):
        r = subprocess.run([str(exe)] + list(segments), capture_output=True,
                           text=True, timeout=60)
        assert r.returncode == 0, r.stderr[-2000:]
        return r.stdout, r.stderr
    return run


def last_lines(out):
    l0 = l1 = None
    for line in out.splitlines():
        if line.startswith("L0:[") and line.endswith("]"):
            l0 = line[4:-1]
        if line.startswith("L1:[") and line.endswith("]"):
            l1 = line[4:-1]
    return l0, l1


def test_initial_window(vfd_exe):
    out, err = vfd_exe()
    l0, l1 = last_lines(out)
    assert l0 == "CDJ-850 PANEL LINK -- BOOT"
    assert l1 == "FL: waiting for frames"


def test_exchange_frame_passthrough(vfd_exe):
    # Synthetic 6-byte exchange frame (bit7-set TX[0], binary payload).
    out, err = vfd_exe("C3 01 02 03 04 05")
    l0, l1 = last_lines(out)
    assert l0 == "CDJ-850 PANEL LINK -- BOOT"
    assert l1 == "TX C3 01 02 03 04 05 #1"
    assert "?" not in l1
    assert err == ""


def test_flag_byte_variants(vfd_exe):
    # TX[0] flag-bit shapes from the TX-prep builder (bset/bclr #6/#4/#1):
    # bit6 clear, bit4 set, bit1 set variants must pass through exactly.
    out, err = vfd_exe("83 11 22 33 44 55", "D2 2A 3B 4C 5D 6E")
    segs = [ln for ln in out.splitlines() if ln.startswith("L1:[")]
    assert segs[0] == "L1:[TX 83 11 22 33 44 55 #1]"
    assert segs[1] == "L1:[TX D2 2A 3B 4C 5D 6E #2]"


def test_state_value_field(vfd_exe):
    # TX[3..5] 24-bit state value passes through (jog/lamp state lives here).
    out, err = vfd_exe("C2 00 00 08 00 01", "C2 7F 7E 7F 7F 7F")
    segs = [ln for ln in out.splitlines() if ln.startswith("L1:[")]
    assert segs[0] == "L1:[TX C2 00 00 08 00 01 #1]"
    assert segs[1] == "L1:[TX C2 7F 7E 7F 7F 7F #2]"


def test_resync_on_flag_byte(vfd_exe):
    # Bytes without bit7 set (upload/16-bit interleave shapes) never form a
    # frame and never corrupt the displayed one; any bit7-set byte resyncs.
    out, err = vfd_exe("12 34 56", "C3 01 02 03 04 05", "13 00 FF",
                        "82 10 20 30 40 50")
    segs = [ln for ln in out.splitlines() if ln.startswith("L1:[")]
    assert segs[0] == "L1:[FL: waiting for frames]"
    assert segs[1] == "L1:[TX C3 01 02 03 04 05 #1]"
    assert segs[2] == "L1:[TX C3 01 02 03 04 05 #1]"
    assert segs[3] == "L1:[TX 82 10 20 30 40 50 #2]"


def test_no_question_glyphs_any_byte(vfd_exe):
    # Every byte value 0x00-0xFF renders as hex; '?' never appears.
    stream = " ".join("%02X" % b for b in range(256))
    out, err = vfd_exe(stream)
    assert "?" not in out
    assert err == ""
