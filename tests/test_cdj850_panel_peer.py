# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-850 SCIF1 panel peer (xc/panel_peer.c) unit tests.

Compiles the real hw/cdj/boards/cdj850/xc/panel_peer.c with minimal stub
QEMU headers (same harness style as test_cdj850_panel_seq.py and
test_cdj850_vfd.py) and tests RX button/rotary encoding, sum8ec checksums,
CDJ850_PANEL_SCRIPT scheduling, and MAIN->panel 0x80..0xAE display decoding.
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
        "#include <inttypes.h>\n"
        "typedef struct MemoryRegion MemoryRegion;\n"
        "typedef struct SuperHCPU SuperHCPU;\n"
        "typedef struct QEMUTimer QEMUTimer;\n"
        "typedef struct VMStateField { int dummy; } VMStateField;\n"
        "typedef struct VMStateDescription {\n"
        "    const char *name; int version_id; int minimum_version_id;\n"
        "    const VMStateField *fields;\n"
        "} VMStateDescription;\n"
        "#define QEMU_CLOCK_VIRTUAL 0\n"
        "#define info_report(fmt, ...) \\\n"
        "    fprintf(stderr, \"info: \" fmt \"\\n\", ##__VA_ARGS__)\n"
        "extern uint64_t stub_vms_ns;\n"
        "static inline uint64_t qemu_clock_get_ns(int c)\n"
        "{ (void)c; return stub_vms_ns; }\n"
        "static inline QEMUTimer *timer_new_ns(int c, void (*cb)(void *), void *o)\n"
        "{ (void)c; (void)cb; (void)o; return (QEMUTimer *)0x1; }\n"
        "static inline void timer_mod(QEMUTimer *t, int64_t exp)\n"
        "{ (void)t; (void)exp; }\n"
        "static inline int vmstate_register_any(void *d, const VMStateDescription *v,\n"
        "    void *o)\n"
        "{ (void)d; (void)v; (void)o; return 0; }\n"
        "#define VMSTATE_TIMER_PTR(f, s) { .dummy = 0 }\n"
        "#define VMSTATE_UINT32(f, s) { .dummy = 0 }\n"
        "#define VMSTATE_INT64(f, s) { .dummy = 0 }\n"
        "#define VMSTATE_BOOL(f, s) { .dummy = 0 }\n"
        "#define VMSTATE_UINT8_2DARRAY(f, s, a, b) { .dummy = 0 }\n"
        "#define VMSTATE_UINT16_V(f, s, v) { .dummy = 0 }\n"
        "#define VMSTATE_UINT8_ARRAY_V(f, s, n, v) { .dummy = 0 }\n"
        "#define VMSTATE_END_OF_LIST() { .dummy = 0 }\n"
        "#endif\n"
    ),
    "intc.h": (
        "#ifndef STUB_INTC_H\n#define STUB_INTC_H\n"
        "#include <stdbool.h>\n"
        "static inline void cdj850_intc_set_pin(unsigned irq, bool lvl)\n"
        "{ (void)irq; (void)lvl; }\n"
        "#endif\n"
    ),
    "port.h": (
        "#ifndef STUB_PORT_H\n#define STUB_PORT_H\n"
        "#include <stdbool.h>\n"
        "static inline bool cdj850_port_main_xbusy(void) { return true; }\n"
        "#endif\n"
    ),
    "scif.h": (
        "#ifndef STUB_SCIF_H\n#define STUB_SCIF_H\n"
        "#include <stdint.h>\n#include <stdbool.h>\n"
        "#include <string.h>\n"
        "static inline unsigned cdj850_scif1_tx_pending(void) { return 0; }\n"
        "static inline bool cdj850_scif1_tie(void) { return false; }\n"
        "static inline void cdj850_scif1_exchange(const uint8_t rx[16], uint8_t tx[16])\n"
        "{ (void)rx; memset(tx, 0, 16); }\n"
        "#endif\n"
    ),
    "trace.h": (
        "#ifndef STUB_TRACE_H\n#define STUB_TRACE_H\n"
        "#include \"cdj_common.h\"\n"
        "#include <stdio.h>\n"
        "#define info_report(fmt, ...) \\\n"
        "    fprintf(stderr, \"info: \" fmt \"\\n\", ##__VA_ARGS__)\n"
        "static inline bool cdj850_trace(void) { return false; }\n"
        "static inline uint64_t cdj850_vms(void) { return stub_vms_ns / 1000000ULL; }\n"
        "#endif\n"
    ),
    "vfd.h": (
        "#ifndef STUB_VFD_H\n#define STUB_VFD_H\n"
        "#include <stdint.h>\n"
        "void vfd_note_panel_tags(const uint8_t tags[3]);\n"
        "void vfd_note_panel_text(uint8_t mode, const char *l0, const char *l1, const char *l2);\n"
        "#endif\n"
    ),
    "_pre.h": (
        "#ifndef STUB_PANEL_PEER_PRE_H\n#define STUB_PANEL_PEER_PRE_H\n"
        "#define QEMU_CLOCK_VIRTUAL 0\n"
        "#endif\n"
    ),
}

HARNESS = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "panel_peer.h"

uint64_t stub_vms_ns = 0;
static uint8_t vfd_last_mode = 0xff;
static char vfd_last[3][256];
static unsigned vfd_calls = 0;

void vfd_note_panel_tags(const uint8_t tags[3]) { (void)tags; }

void vfd_note_panel_text(uint8_t mode, const char *l0, const char *l1,
                         const char *l2)
{
    vfd_last_mode = mode;
    snprintf(vfd_last[0], sizeof(vfd_last[0]), "%s", l0 ? l0 : "");
    snprintf(vfd_last[1], sizeof(vfd_last[1]), "%s", l1 ? l1 : "");
    snprintf(vfd_last[2], sizeof(vfd_last[2]), "%s", l2 ? l2 : "");
    vfd_calls++;
}

static void dump_frame(const char *tag, const uint8_t f[16])
{
    printf("%s:", tag);
    for (int i = 0; i < 16; i++) {
        printf(" %02x", f[i]);
    }
    printf("\n");
}

static int parse_hex(const char *s, uint8_t out[16])
{
    for (int i = 0; i < 16; i++) {
        unsigned v = 0;
        if (sscanf(s + i * 2, "%2x", &v) != 1) {
            return -1;
        }
        out[i] = (uint8_t)v;
    }
    return 0;
}

int main(int argc, char **argv)
{
    uint8_t f1[16], f2[16];

    cdj850_panel_peer_init();
    if (argc < 2) {
        return 2;
    }
    if (strcmp(argv[1], "idle") == 0) {
        cdj850_panel_peer_build_rx(0, f1);
        cdj850_panel_peer_build_rx(1, f2);
        dump_frame("F1", f1);
        dump_frame("F2", f2);
        return 0;
    }
    if (strcmp(argv[1], "buttons") == 0) {
        printf("bogus=%d\n", cdj850_panel_peer_set_button("nonexistent", true) ? 1 : 0);
        for (int i = 2; i < argc; i++) {
            const char *arg = argv[i];
            if (arg[0] == '+') {
                printf("set(%s)=%d\n", arg, cdj850_panel_peer_set_button(arg + 1, true) ? 1 : 0);
            } else if (arg[0] == '-') {
                printf("clr(%s)=%d\n", arg, cdj850_panel_peer_set_button(arg + 1, false) ? 1 : 0);
            }
            cdj850_panel_peer_build_rx(0, f1);
            cdj850_panel_peer_build_rx(1, f2);
            dump_frame("F1", f1);
            dump_frame("F2", f2);
        }
        return 0;
    }
    if (strcmp(argv[1], "rotate") == 0) {
        for (int i = 2; i < argc; i++) {
            cdj850_panel_peer_rotate(atoi(argv[i]));
            cdj850_panel_peer_build_rx(0, f1);
            printf("rot=%u:", (unsigned)cdj850_panel_peer_rotary());
            dump_frame("F1", f1);
        }
        return 0;
    }
    if (strcmp(argv[1], "script") == 0) {
        for (int i = 2; i < argc; i++) {
            uint64_t ms = strtoull(argv[i], NULL, 10);
            stub_vms_ns = ms * 1000000ULL;
            cdj850_panel_peer_build_rx(0, f1);
            cdj850_panel_peer_build_rx(1, f2);
            printf("t=%llu rot=%u rx1_13=%02x rx2_6=%02x\n",
                   (unsigned long long)ms,
                   (unsigned)cdj850_panel_peer_rotary(),
                   f1[13], f2[6]);
        }
        return 0;
    }
    if (strcmp(argv[1], "display") == 0) {
        for (int i = 2; i < argc; i++) {
            uint8_t tx[16];
            if (parse_hex(argv[i], tx) != 0) {
                return 3;
            }
            cdj850_panel_peer_feed_display_tx(tx);
        }
        printf("mode=0x%02x updates=%u vfd=%u\n",
               cdj850_panel_peer_display_mode(),
               cdj850_panel_peer_display_updates(),
               vfd_calls);
        for (unsigned r = 0; r < 3; r++) {
            printf("R%u:attr=%02x:%s\n", r,
                   cdj850_panel_peer_display_attr(r),
                   cdj850_panel_peer_display_line(r));
        }
        printf("VFD:mode=0x%02x:[%s]|[%s]|[%s]\n",
               vfd_last_mode, vfd_last[0], vfd_last[1], vfd_last[2]);
        return 0;
    }
    return 2;
}
"""


def sum8ec(data):
    s = 0
    for b in data:
        s += b
        if s > 0xFF:
            s = (s + 1) & 0xFF
    return s


def make_display_burst(seq, mode, rows, tags=None):
    """Build SCIF1 MAIN->panel 0x80 header + 0x81.. payload frames."""
    hdr = bytearray(16)
    hdr[0] = 0x80
    hdr[1] = seq & 0xFF
    hdr[2] = mode & 0xFF
    hdr[15] = sum8ec(hdr[:15])

    payload = bytearray()
    for idx, (attr0, attr1, attr2, text) in enumerate(rows):
        btext = text.encode("ascii")
        tag = (0x11 + idx) if tags is None else tags[idx]
        payload.extend([tag, attr0, attr1, attr2, len(btext)])
        payload.extend(btext)
    while len(payload) % 14 != 0:
        payload.append(0)

    frames = [bytes(hdr)]
    for i in range(0, len(payload), 14):
        chunk = bytearray(16)
        chunk[0] = 0x81 + (i // 14)
        chunk[1:15] = payload[i:i + 14]
        chunk[15] = sum8ec(chunk[:15])
        frames.append(bytes(chunk))
    return frames


@pytest.fixture(scope="module")
def panel_peer_exe(tmp_path_factory):
    cc = (os.environ.get("CC") or shutil.which("gcc")
          or shutil.which("cc") or shutil.which("clang"))
    if not cc:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("panel_peer")
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
         str(main_c), path("hw", "cdj", "boards", "cdj850", "xc", "panel_peer.c"),
         "-o", str(exe)],
        capture_output=True, text=True, timeout=120)
    if r.returncode != 0:
        pytest.fail("cannot compile panel_peer.c harness:\n" + r.stderr[-3000:])

    def run(extra_env, *args):
        env = dict(os.environ)
        env.pop("CDJ850_PANEL_SCRIPT", None)
        if extra_env:
            env.update(extra_env)
        res = subprocess.run([str(exe)] + list(args), capture_output=True,
                             text=True, timeout=60, env=env)
        assert res.returncode == 0, res.stderr[-2000:]
        return res.stdout, res.stderr

    return run


def parse_frame_line(line):
    _, hexpart = line.split(":", 1)
    return [int(x, 16) for x in hexpart.strip().split()]


def test_idle_frames_and_checksum(panel_peer_exe):
    out, _ = panel_peer_exe(None, "idle")
    lines = out.strip().splitlines()
    f1 = parse_frame_line(lines[0])
    f2 = parse_frame_line(lines[1])
    assert f1 == [0x01, 0x01, 0x00] + [0x00] * 12 + [0x02]
    assert f2 == [0x02] + [0x00] * 11 + [0x02, 0x00, 0x00, 0x00]
    assert f1[15] == sum8ec(f1[:15])
    assert f2[12] == sum8ec(f2[:12])


def test_button_encodings(panel_peer_exe):
    out, _ = panel_peer_exe(
        None, "buttons",
        "+usb", "-usb", "+disc", "-disc", "+link", "-link",
        "+enter", "-enter", "+browse", "+back", "-browse", "-back",
        "+play", "+cue", "-play", "-cue",
    )
    lines = out.strip().splitlines()
    assert lines[0] == "bogus=0"
    steps = {}
    i = 1
    while i < len(lines):
        label = lines[i]
        f1 = parse_frame_line(lines[i + 1])
        f2 = parse_frame_line(lines[i + 2])
        assert f1[15] == sum8ec(f1[:15])
        assert f2[12] == sum8ec(f2[:12])
        steps[label] = (f1, f2)
        i += 3

    assert steps["set(+usb)=1"][1][6] == 0x02
    assert steps["clr(-usb)=1"][1][6] == 0x00
    assert steps["set(+disc)=1"][1][6] == 0x04
    assert steps["set(+link)=1"][1][6] == 0x01
    assert steps["set(+enter)=1"][0][13] == 0x01
    assert steps["clr(-enter)=1"][0][13] == 0x00
    assert steps["set(+browse)=1"][1][7] == 0x01
    assert steps["set(+back)=1"][1][7] == 0x11
    assert steps["clr(-browse)=1"][1][7] == 0x10
    assert steps["clr(-back)=1"][1][7] == 0x00
    assert steps["set(+play)=1"][1][1] == 0x02
    assert steps["set(+cue)=1"][1][1] == 0x06
    assert steps["clr(-cue)=1"][1][1] == 0x00


def test_rotary_encoder_steps(panel_peer_exe):
    out, _ = panel_peer_exe(None, "rotate", "1", "-2")
    lines = out.strip().splitlines()
    f1_plus = parse_frame_line("F1:" + lines[0].split("F1:", 1)[1])
    assert f1_plus[3] == 0x00 and f1_plus[4] == 0x01
    assert f1_plus[15] == sum8ec(f1_plus[:15])

    f1_minus = parse_frame_line("F1:" + lines[1].split("F1:", 1)[1])
    assert f1_minus[3] == 0xFF and f1_minus[4] == 0xFF
    assert f1_minus[15] == sum8ec(f1_minus[:15])


def test_scripted_timeline(panel_peer_exe):
    out, err = panel_peer_exe(
        {"CDJ850_PANEL_SCRIPT": "100:usb:50,200:rot+2,300:enter:100"},
        "script", "0", "99", "100", "149", "150", "200", "300", "399", "400",
    )
    rows = {}
    for ln in out.strip().splitlines():
        parts = dict(kv.split("=") for kv in ln.split())
        rows[int(parts["t"])] = parts

    assert rows[99]["rx2_6"] == "00"
    assert rows[100]["rx2_6"] == "02"
    assert rows[149]["rx2_6"] == "02"
    assert rows[150]["rx2_6"] == "00"
    assert rows[150]["rot"] == "0"
    assert rows[200]["rot"] == "2"
    assert rows[200]["rx1_13"] == "00"
    assert rows[300]["rx1_13"] == "01"
    assert rows[399]["rx1_13"] == "01"
    assert rows[400]["rx1_13"] == "00"
    assert "input press usb at 100 ms" in err
    assert "input rotate +2 -> 2 at 200 ms" in err


def test_display_decoder_no_disc_and_three_line_browse(panel_peer_exe):
    no_disc = make_display_burst(
        seq=3, mode=0x08,
        rows=[(0, 0, 0, ""), (0, 0, 0, "NO DISC"), (0, 0, 0, "")],
    )
    # Append a corrupted frame (wrong checksum) to verify it is ignored.
    bad = bytearray(no_disc[-1])
    bad[15] ^= 0x55

    out1, _ = panel_peer_exe(None, "display", *(f.hex() for f in no_disc), bad.hex())
    assert "mode=0x08 updates=1 vfd=1\n" in out1
    assert "R0:attr=00:\n" in out1
    assert "R1:attr=00:NO DISC\n" in out1
    assert "R2:attr=00:\n" in out1
    assert "VFD:mode=0x08:[]|[NO DISC]|[]\n" in out1

    browse = make_display_burst(
        seq=4, mode=0x01,
        rows=[
            (0x00, 0x00, 0x01, "ALPHA.wav"),
            (0x01, 0x00, 0x02, "BETA.wav"),
            (0x00, 0x00, 0x03, "GAMMA.wav"),
        ],
    )
    combined = [f.hex() for f in (no_disc + browse)]
    out2, err2 = panel_peer_exe(None, "display", *combined)
    assert "mode=0x01 updates=2 vfd=2\n" in out2
    assert "R0:attr=00:ALPHA.wav\n" in out2
    assert "R1:attr=01:BETA.wav\n" in out2
    assert "R2:attr=00:GAMMA.wav\n" in out2
    assert "VFD:mode=0x01:[ALPHA.wav]|[BETA.wav]|[GAMMA.wav]\n" in out2
    assert "[ALPHA.wav] | [BETA.wav] | [GAMMA.wav]" in err2


def test_display_decoder_multiframe_and_sub1_retry_reset(panel_peer_exe):
    # Partial burst (0x80 + 0x81 only) leaves disp_next_sub == 2; a retry
    # starting with 0x81 without repeating 0x80 must reset and decode cleanly,
    # including >3-frame bursts (0x80 + 0x81..0x85).
    partial = make_display_burst(
        seq=5, mode=0x01,
        rows=[(0, 0, 0, "STALE_0"), (0, 0, 0, "DISC"), (0, 0, 0, "STALE_2")],
    )[:2]
    retry_multiframe = make_display_burst(
        seq=6, mode=0x01,
        rows=[
            (0x00, 0x00, 0x01, "01 ALPHA_LONG.wav"),
            (0x01, 0x00, 0x02, "02 BETA_LONG.wav"),
            (0x00, 0x00, 0x03, "03 GAMMA_LONG.wav"),
        ],
    )
    assert len(retry_multiframe) == 6
    stream = [f.hex() for f in (partial + retry_multiframe[1:])]
    out, err = panel_peer_exe(None, "display", *stream)
    assert "mode=0x01 updates=1 vfd=1\n" in out
    assert "R0:attr=00:01 ALPHA_LONG.wav\n" in out
    assert "R1:attr=01:02 BETA_LONG.wav\n" in out
    assert "R2:attr=00:03 GAMMA_LONG.wav\n" in out
    assert "STALE" not in out
    assert "[01 ALPHA_LONG.wav] | [02 BETA_LONG.wav] | [03 GAMMA_LONG.wav]" in err


def test_display_decoder_browse_tags_and_cursor_move(panel_peer_exe):
    # Firmware browse lists use row tags 0x25/0x02/0x03 then 0x01/0x26/0x03
    # when the highlight moves from row 0 to row 1 with unchanged row text.
    rows = [
        (0x04, 0x00, 0x00, "ALPHA.wav"),
        (0x04, 0x00, 0x00, "BETA.wav"),
        (0x04, 0x00, 0x00, "GAMMA.wav"),
    ]
    b0 = make_display_burst(seq=10, mode=0x01, rows=rows, tags=(0x25, 0x02, 0x03))
    b1 = make_display_burst(seq=12, mode=0x01, rows=rows, tags=(0x01, 0x26, 0x03))
    out, err = panel_peer_exe(None, "display", *(f.hex() for f in (b0 + b1)))
    assert "mode=0x01 updates=2 vfd=2\n" in out
    assert "tag=25/02/03 attr=04:00:00/04:00:00/04:00:00 [ALPHA.wav] | [BETA.wav] | [GAMMA.wav]" in err
    assert "tag=01/26/03 attr=04:00:00/04:00:00/04:00:00 [ALPHA.wav] | [BETA.wav] | [GAMMA.wav]" in err
