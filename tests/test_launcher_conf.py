# SPDX-License-Identifier: GPL-2.0-or-later
"""cdj.conf as launcher/conf.py reads and writes it: the files the bash
setup.sh wrote (printf %q) must read the same, and a rewrite keeps keys it
does not know."""

import os
import subprocess
import sys

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import conf, host  # noqa: E402

OLD = r"""# Written by ./setup.sh on 2026-09-20 10:00. ./start.sh reads it.
# Edit it, or run ./setup.sh --reconfigure.
CDJ_DECKS=2
CDJ_NAME=club
CDJ_DJLINK=1
CDJ_AUDIO=1
CDJ_CONTROLLER=roland-dj-202
CDJ_RELAY_PORT=7573
CDJ_GROUP=239.77.77.1:45000
CDJ_MIDI_PYTHON=/c/Program\ Files/Python311/python.exe
CDJ_TOOLS_PYTHON=''
QEMU_BUILD=/c/qemu-build-mingw
QEMU_EB_BUILD=/c/qemu-build-mingw-eb
CDJ_APP=1
"""


def test_reads_what_bash_setup_wrote():
    v = conf.parse(OLD)
    assert v["CDJ_DECKS"] == "2"
    assert v["CDJ_MIDI_PYTHON"] == "/c/Program Files/Python311/python.exe"
    assert v["CDJ_TOOLS_PYTHON"] == ""
    assert v["CDJ_APP"] == "1"


def test_rewrite_keeps_unknown_keys_and_reads_back():
    v = conf.parse(OLD)
    text = conf.render(v)
    assert conf.parse(text) == v
    assert text.index("CDJ_APP=") > text.index("QEMU_EB_BUILD=")


def test_defaults_fill_what_the_file_leaves_out():
    c = conf.with_start_defaults({"CDJ_NAME": "x"})
    assert (c["CDJ_NAME"], c["CDJ_DECKS"], c["CDJ_RELAY_PORT"]) == ("x", "1", "7202")


def test_bad_lines_are_reported_not_fatal():
    seen = []
    v = conf.parse("CDJ_DECKS=1\nif true; then\nCDJ_NAME='unbalanced\n", warn=seen.append)
    assert v == {"CDJ_DECKS": "1"} and len(seen) == 2


@pytest.mark.skipif(not host.find_bash(), reason="needs bash")
def test_bash_sources_what_it_writes():
    v = conf.parse(OLD)
    script = conf.render(v) + 'printf "%s|%s" "$CDJ_MIDI_PYTHON" "$CDJ_NAME"\n'
    out = subprocess.run([host.find_bash(), "-c", script], capture_output=True, text=True).stdout
    assert out == "/c/Program Files/Python311/python.exe|club"
