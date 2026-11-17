# SPDX-License-Identifier: GPL-2.0-or-later
"""The deck's HID report descriptor travels QEMU -> bridge as a hex line.

cdj_usbf_host_tell() in hw/cdj/boards/nxs2/usbf_host.c encodes it with
sprintf("%02x"); the bridge's Deck.status() decodes with bytes.fromhex(). The
two only meet on those bytes -- a format mismatch would hand the host a corrupt
report descriptor and a wrong virtual device -- so the C encode loop is cut out
of the file, compiled on its own and run, and its output decoded the way the
bridge does it. Skips when there is no C compiler."""
import os
import shutil
import subprocess

import pytest

import usb_hid_port as up
from helpers import path


def c_encode_loop():
    with open(path("hw", "cdj", "boards", "nxs2", "usbf_host.c"), encoding="utf-8") as fh:
        src = fh.read()
    start = src.index("for (i = 0; i < h->hid_desc_len; i++)")
    depth, i = 0, src.index("{", start)
    for j in range(i, len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if depth == 0:
            return src[start:j + 1]
    raise AssertionError("unbalanced braces in cdj_usbf_host_tell's hid_desc loop")


def encoded(desc, tmp_path_factory):
    cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if not cc:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("hiddesc")
    prog = d / "hex.c"
    prog.write_text(
        "#include <stdint.h>\n#include <stdio.h>\n#include <string.h>\n"
        "struct H { uint8_t hid_desc[512]; unsigned hid_desc_len; };\n"
        "int main(void)\n{\n"
        "    static struct H h_;\n"
        "    struct H *h = &h_;\n"
        "    char out[1100], *p = out;\n"
        "    unsigned i;\n"
        "    h->hid_desc_len = %d;\n"
        "    memcpy(h->hid_desc, \"%s\", %d);\n"
        % (len(desc), "".join("\\x%02x" % b for b in desc), len(desc))
        + c_encode_loop()
        + "\n    fwrite(out, 1, p - out, stdout);\n    return 0;\n}\n", encoding="utf-8")
    exe = d / ("hex.exe" if os.name == "nt" else "hex")
    env = dict(os.environ, TMP=str(d), TEMP=str(d), TMPDIR=str(d))
    # A MinGW gcc found late on PATH loads the wrong runtime DLLs and fails silently.
    env["PATH"] = os.path.dirname(os.path.abspath(cc)) + os.pathsep + env.get("PATH", "")
    build = subprocess.run([cc, "-pipe", "-std=gnu11", "-O0", str(prog), "-o", str(exe)],
                           capture_output=True, text=True, env=env, timeout=120)
    if build.returncode != 0:
        msg = (build.stderr or build.stdout).strip()[-200:] or "no compiler output"
        pytest.fail("cannot compile cdj_usbf_host_tell's hid_desc loop:\n" + msg)
    out = subprocess.run([str(exe)], capture_output=True, text=True, env=env, timeout=30)
    return out.stdout


def test_c_encode_decodes_the_way_the_bridge_does(tmp_path_factory):
    desc = bytes.fromhex("05 01 09 06 a1 01 05 07 19 e0 29 e7")
    deck = up.Deck(lambda report, vt: None)
    deck.datagram(b"\x00hid_desc=" + encoded(desc, tmp_path_factory).encode(),
                  ("127.0.0.1", 1))
    assert deck.hid_desc == desc
