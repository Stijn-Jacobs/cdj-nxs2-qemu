# SPDX-License-Identifier: GPL-2.0-or-later
"""The panel socket port must be computed identically by QEMU
(cdj_panelsock_port() in hw/cdj/cdj_panelkeys.h) and by the host tools
(port_for() in scripts/run/cdj_panelsock.py): the two processes only meet on
that number. The C function is cut out of the header and compiled on its own,
so no QEMU tree is needed; the test skips when there is no C compiler."""
import os
import shutil
import subprocess
import sys

import pytest

import cdj_panelsock
from helpers import path

NAMES = [
    "/tmp/cdj-panel-keys-show1.sock",
    "C:/msys64/tmp/cdj-panel-keys-show1.sock",
    "C:\\msys64\\tmp\\cdj-panel-keys-show2.sock",
    "/tmp/cdj-panel-state-show1.sock",
    "/tmp/cdj-show1-gui-mon.sock",
    "cdj-panel-keys-warm1",
    "/tmp/x:7202",
    "7202",
    "x:1023",
    "x:70000",
    "x:",
    "",
]


def c_function():
    with open(path("hw", "cdj", "cdj_panelkeys.h"), encoding="utf-8") as fh:
        src = fh.read()
    start = src.index("static inline uint16_t cdj_panelsock_port(")
    depth, i = 0, src.index("{", start)
    for j in range(i, len(src)):
        depth += {"{": 1, "}": -1}.get(src[j], 0)
        if depth == 0:
            return src[start:j + 1]
    raise AssertionError("unbalanced braces in cdj_panelsock_port")


@pytest.fixture(scope="module")
def c_port(tmp_path_factory):
    cc = os.environ.get("CC") or shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")
    if not cc:
        pytest.skip("no C compiler")
    d = tmp_path_factory.mktemp("cport")
    prog = d / "port.c"
    prog.write_text(
        "#include <stdint.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n"
        + c_function()
        + "\nint main(int argc, char **argv)\n{\n"
          "    for (int i = 1; i < argc; i++) printf(\"%u\\n\", cdj_panelsock_port(argv[i]));\n"
          "    return 0;\n}\n", encoding="utf-8")
    exe = d / ("port.exe" if os.name == "nt" else "port")
    env = dict(os.environ, TMP=str(d), TEMP=str(d), TMPDIR=str(d))
    # A MinGW gcc found late on PATH loads the wrong runtime DLLs and fails silently.
    env["PATH"] = os.path.dirname(os.path.abspath(cc)) + os.pathsep + env.get("PATH", "")
    r = subprocess.run([cc, "-pipe", "-std=gnu11", "-O1", str(prog), "-o", str(exe)],
                       capture_output=True, text=True, env=env)
    if r.returncode != 0:
        if sys.platform == "win32":
            pytest.skip("the C compiler did not run here: " + r.stderr[:200])
        pytest.fail("cannot compile the header's cdj_panelsock_port:\n" + r.stderr)

    def port(names):
        out = subprocess.run([str(exe)] + names, capture_output=True, text=True, check=True,
                             env=env).stdout
        return [int(x) for x in out.split()]
    return port


def test_c_and_python_agree(c_port):
    assert c_port(NAMES) == [cdj_panelsock.port_for(n) for n in NAMES]


@pytest.mark.xfail(strict=True, reason="known divergence: Python strips .sock before looking for "
                                       "an explicit port, C looks for it in the unstripped name")
def test_c_and_python_agree_on_a_numeric_stem(c_port):
    assert c_port(["/tmp/7202.sock"]) == [cdj_panelsock.port_for("/tmp/7202.sock")]

