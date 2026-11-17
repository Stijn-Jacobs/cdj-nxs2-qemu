# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-850 panel sequenced hold (Task 8l): per-frame RX[0] cycling tests.

The sequenced hold extends the Task 8k static hold (CDJ850_PANEL_RX six hex
bytes, default OFF) with a SEQUENCED mode: new env keywords (seq40 / seq20 /
seq10 / seqall, default OFF) that cycle the RX[0] route selector through a
bank triple per 6-byte exchange so the cddabuff tail consumer can fire.
RX[1]/RX[2] stay 0x01 (routed value 0x0101, as the 8k active-frame probes)
and tail bytes stay 0x01 (DjControl gate passes). All held bytes are
synthetic test vectors, never panel-matrix key codes.

Method: compile the REAL hw/cdj/boards/cdj850/xc/panel_link.c with stub
QEMU headers (no emulator needed, skipped without a C compiler), drive it
through panel_exchange_start() + panel_ssu_rx_byte(), assert exact frames.
"""
import os
import shutil
import subprocess

import pytest

from helpers import path

STUBS = {
    "cdj_common.h": (
        "#ifndef STUB_CDJ_COMMON_H\n#define STUB_CDJ_COMMON_H\n"
        "#include <stdint.h>\n#include <stdbool.h>\n"
        "#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#endif\n"
    ),
    "trace.h": (
        "#ifndef STUB_TRACE_H\n#define STUB_TRACE_H\n"
        "#include <stdio.h>\n"
        "#define info_report(fmt, ...) \\\n"
        "    fprintf(stderr, \"info: \" fmt \"\\n\", ##__VA_ARGS__)\n"
        "#endif\n"
    ),
    "vfd.h": (
        "#ifndef STUB_VFD_H\n#define STUB_VFD_H\n"
        "#include <stdint.h>\n"
        "void vfd_note_tx(uint8_t b);\n"
        "#endif\n"
    ),
    "_pre.h": (
        "#ifndef STUB_PANEL_PRE_H\n#define STUB_PANEL_PRE_H\n"
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
#include "panel_link.h"

void vfd_note_tx(uint8_t b) { (void)b; }

int main(int argc, char **argv)
{
    panel_link_init();
    if (argc >= 2 && strcmp(argv[1], "restart") == 0) {
        /* Partial frame, then exchange restart: pins the sequencing rule
         * (advance happens at the byte-0 slot; restart only re-aligns). */
        panel_exchange_start();
        printf("%02x ", panel_ssu_rx_byte());
        printf("%02x | ", panel_ssu_rx_byte());
        panel_exchange_start();
        for (int i = 0; i < 6; i++)
            printf("%02x%c", panel_ssu_rx_byte(), i < 5 ? ' ' : '\n');
        return 0;
    }
    int n = argc >= 2 ? atoi(argv[1]) : 1;
    if (n < 1) n = 1;
    if (n > 32) n = 32;
    for (int f = 0; f < n; f++) {
        panel_exchange_start();
        for (int i = 0; i < 6; i++)
            printf("%02x%c", panel_ssu_rx_byte(), i < 5 ? ' ' : '\n');
    }
    return 0;
}
"""


@pytest.fixture(scope="module")
def panel_exe(tmp_path_factory):
    cc = (os.environ.get("CC") or shutil.which("gcc")
          or shutil.which("cc") or shutil.which("clang"))
    if not cc:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("panel_seq")
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
         str(main_c), path("hw", "cdj", "boards", "cdj850", "xc", "panel_link.c"),
         "-o", str(exe)],
        capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        pytest.fail("cannot compile panel_link.c harness:\n" + r.stderr[-3000:])

    def run(rx_value, *args):
        env = dict(os.environ)
        if rx_value is None:
            env.pop("CDJ850_PANEL_RX", None)
        else:
            env["CDJ850_PANEL_RX"] = rx_value
        r = subprocess.run([str(exe)] + list(args), capture_output=True,
                           text=True, timeout=60, env=env)
        assert r.returncode == 0, r.stderr[-2000:]
        return r.stdout, r.stderr
    return run


def frames(out):
    return [ln.strip() for ln in out.splitlines() if ln.strip()]


def test_idle_default_off(panel_exe):
    out, _ = panel_exe(None, "2")
    assert frames(out) == ["00 00 00 01 01 01", "00 00 00 01 01 01"]


def test_static_hold_byte_identical(panel_exe):
    out, _ = panel_exe("40:01:01:01:01:01", "2")
    assert frames(out) == ["40 01 01 01 01 01", "40 01 01 01 01 01"]


def test_seq40_cycles_bank_triple(panel_exe):
    out, err = panel_exe("seq40", "4")
    assert frames(out) == [
        "40 01 01 01 01 01",
        "41 01 01 01 01 01",
        "42 01 01 01 01 01",
        "40 01 01 01 01 01",
    ]
    assert "seq" in err.lower()


def test_seqall_cycles_all_banks(panel_exe):
    out, _ = panel_exe("seqall", "10")
    assert frames(out) == [
        "40 01 01 01 01 01",
        "41 01 01 01 01 01",
        "42 01 01 01 01 01",
        "20 01 01 01 01 01",
        "21 01 01 01 01 01",
        "22 01 01 01 01 01",
        "10 01 01 01 01 01",
        "11 01 01 01 01 01",
        "12 01 01 01 01 01",
        "40 01 01 01 01 01",
    ]


def test_seq20_seq10_cheap(panel_exe):
    out, _ = panel_exe("seq20", "4")
    assert frames(out) == [
        "20 01 01 01 01 01",
        "21 01 01 01 01 01",
        "22 01 01 01 01 01",
        "20 01 01 01 01 01",
    ]
    out, _ = panel_exe("seq10", "4")
    assert frames(out) == [
        "10 01 01 01 01 01",
        "11 01 01 01 01 01",
        "12 01 01 01 01 01",
        "10 01 01 01 01 01",
    ]


def test_seq_restart_realigns_without_repeat(panel_exe):
    # Partial frame (40 01) then restart: next frame is 41, not 40 again.
    out, _ = panel_exe("seq40", "restart")
    assert frames(out) == ["40 01 | 41 01 01 01 01 01"]


def test_bad_value_stays_off(panel_exe):
    out, _ = panel_exe("bogus", "2")
    assert frames(out) == ["00 00 00 01 01 01", "00 00 00 01 01 01"]
