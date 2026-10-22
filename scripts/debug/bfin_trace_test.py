#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic-file test for bfin_trace.py: stalls, summary, dump, lost."""
import contextlib
import io
import os
import struct
import sys
import tempfile
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bfin_trace as bt

HZ = 400_000_000
NAMES = ["poll_start", "task_exec_begin", "task_exec_end", "task_ready_begin", "executor_idle"]
PS, EB, EE, RB, ID = range(5)


def ms(x):
    return int(x * HZ / 1000)


def write(path, recs, lost=None):
    with open(path, "wb") as f:
        f.write(b"BFTRACE1" + struct.pack("<II", HZ, len(NAMES)))
        for n in NAMES:
            n = "_embassy_trace_" + n
            f.write(struct.pack("<H", len(n)) + n.encode())
        for t, i, r0, r1 in recs:
            f.write(bt.REC.pack(ms(t), 0x1000 + i, r0, r1, 0, i, 0, 0))
        if lost:
            f.write(bt.REC.pack(ms(recs[-1][0]), 0, lost, 0, 0, 0, 1, 0))


def run(fn, **kw):
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        fn(types.SimpleNamespace(**kw))
    return out.getvalue()


recs = [
    (0, PS, 1, 0),
    (1, RB, 1, 0xA0), (2, EB, 1, 0xA0), (3, EE, 1, 0xA0),      # fine
    (4, RB, 1, 0xB0), (30, EB, 1, 0xB0), (31, EE, 1, 0xB0),    # ready-late 26 ms
    (32, RB, 1, 0xC0),                                          # never runs
    (33, EB, 1, 0xD0),                                          # never ends
    (100, ID, 1, 0),
    (101, PS, 2, 0), (160, ID, 2, 0),                           # poll 59 ms
    (170, PS, 3, 0),                                            # open poll
    (200, RB, 3, 0xE0),
]
with tempfile.TemporaryDirectory() as d:
    p = os.path.join(d, "t.bin")
    write(p, recs, lost=7)
    out = run(bt.cmd_stalls, out=p, ready_ms=10, exec_ms=20, poll_ms=None, sym=None)
    assert "READY-LATE executor 0x1 task 0xb0 waited 26.000 ms" in out, out
    assert "READY-OPEN" in out and "task 0xc0" in out, out
    assert "EXEC-OPEN" in out and "task 0xd0" in out, out
    assert "task 0xa0" not in out, out
    assert "POLL-LONG executor 0x2" in out and "POLL-OPEN executor 0x3" in out, out
    assert "LOST 7" in out and "6 stall(s)" in out, out
    s = run(bt.cmd_summary, out=p)
    assert "lost 7" in s and "longest exec 1.000 ms" in s, s
    dm = run(bt.cmd_dump, out=p, frm=0.030, to=0.032)
    assert "task_exec_begin" in dm and "task_ready_begin" in dm and "poll_start" not in dm, dm
print("bfin_trace_test: ok")
