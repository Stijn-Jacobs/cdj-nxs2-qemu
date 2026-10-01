# SPDX-License-Identifier: GPL-2.0-or-later
"""One mouse click must reach the firmware as one press. The touch state
machine in hw/cdj/boards/nxs2/panel.c (and the message parser in
cdj_panelkeys.h) are cut out and compiled on their own, then fed the messages
a click produces, including moves while the button is held. Skips when there
is no C compiler."""
import os
import shutil
import subprocess
import sys

import pytest

from helpers import path

STUBS = r"""
#include <stdbool.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define info_report(...) ((void)0)
#define CDJ_TOUCH_ENV "CDJ_TOUCH"
static void stw_be_p(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
"""

MAIN = r"""
int main(void)
{
    char line[64];
    CdjPnlTouch t = { .on = true, .min_frames = 8, .gap_frames = 16 };
    uint8_t report[0x20];
    int64_t now = 0;

    t.up_frames = t.gap_frames;
    while (fgets(line, sizeof(line), stdin)) {
        int frames;

        if (sscanf(line, "frames %d", &frames) == 1) {
            while (frames--) {
                memset(report, 0, sizeof(report));
                cdj_pnl_touch_apply(&t, report, now++);
            }
        } else if (cdj_touch_parse(line, &t.host)) {
            cdj_pnl_touch_msg(&t, now);
        }
    }
    printf("%" PRIu64 "\n", t.presses);
    return 0;
}
"""


def between(src, start, end):
    i = src.index(start)
    return src[i:src.index(end, i)]


@pytest.fixture(scope="module")
def presses(tmp_path_factory):
    cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if not cc:
        pytest.skip("no C compiler")
    with open(path("hw", "cdj", "common", "cdj_panelkeys.h"), encoding="utf-8") as fh:
        header = between(fh.read(), "#define CDJ_TOUCH_W", "static inline void cdj_touch_send")
    with open(path("hw", "cdj", "boards", "nxs2", "panel.c"), encoding="utf-8") as fh:
        panel = between(fh.read(), "#define CDJ_TOUCH_PEN_UP_BELOW", "static void cdj_pnl_on_frame")
    d = tmp_path_factory.mktemp("touch")
    prog = d / "touch.c"
    prog.write_text(STUBS + header + panel + MAIN, encoding="utf-8")
    exe = d / ("touch.exe" if os.name == "nt" else "touch")
    env = dict(os.environ, TMP=str(d), TEMP=str(d), TMPDIR=str(d))
    env["PATH"] = os.path.dirname(os.path.abspath(cc)) + os.pathsep + env.get("PATH", "")
    r = subprocess.run([cc, "-pipe", "-std=gnu11", "-O1", str(prog), "-o", str(exe)],
                       capture_output=True, text=True, env=env)
    if r.returncode != 0:
        if sys.platform == "win32":
            pytest.skip("the C compiler did not run here: " + r.stderr[:200])
        pytest.fail("cannot compile the touch state machine:\n" + r.stderr)

    def run(script):
        out = subprocess.run([str(exe)], input=script, capture_output=True, text=True,
                             check=True, env=env).stdout
        return int(out)
    return run


def test_a_click_is_one_press(presses):
    assert presses("100:100:1:touch\nframes 3\n100:100:0:touch\nframes 60\n") == 1


def test_a_move_while_held_does_not_queue_a_second_press(presses):
    assert presses("100:100:1:touch\nframes 2\n101:100:1:touch\nframes 2\n"
                   "101:101:1:touch\nframes 2\n101:101:0:touch\nframes 60\n") == 1


def test_a_timed_tap_is_one_press(presses):
    assert presses("100:100:40:tap\nframes 60\n") == 1


def test_two_clicks_are_two_presses(presses):
    click = "100:100:1:touch\nframes 3\n100:100:0:touch\nframes 40\n"
    assert presses(click + click) == 2
