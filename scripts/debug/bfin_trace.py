#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline reader for CDJ_BF531_TRACE files (embassy-executor `trace` hooks).

File (little endian): "BFTRACE1", u32 cclk_hz, u32 npoints, npoints x (u16
len + name); then 32-byte records to EOF: u64 cycles, u32 pc, r0, r1, r2,
u16 id, u16 kind (0 hit, 1 LOST: r0|r1<<32 hits dropped), u32 pad.
Embassy hooks: r0 = executor id, r1 = task id (poll_start/idle: r0 only).

  bfin_trace.py points ELF [--sym NAME ...] [-o FILE]   points file for QEMU
  bfin_trace.py summary OUT
  bfin_trace.py stalls OUT --ready-ms N --exec-ms N [--poll-ms N] [--sym ELF]
  bfin_trace.py dump OUT [--from S] [--to S]
"""
import argparse
import os
import re
import struct
import subprocess
import sys
from collections import defaultdict

NM = os.path.expanduser("~/.local/share/cdj-toolchain/binutils-bfin/bin/bfin-elf-nm")
REC = struct.Struct("<QIIIIHHI")


def read_trace(path):
    """Returns (hz, names, recs); recs are (sec, kind, event, pc, r0, r1, r2).
    kind is 'hit' or 'lost'; event is the hook name without its prefix."""
    data = open(path, "rb").read()
    if data[:8] != b"BFTRACE1":
        sys.exit(f"{path}: not a BFTRACE1 file")
    hz, n = struct.unpack_from("<II", data, 8)
    off, names = 16, []
    for _ in range(n):
        (ln,) = struct.unpack_from("<H", data, off)
        names.append(data[off + 2:off + 2 + ln].decode())
        off += 2 + ln
    ev = [re.sub(r"^.*_embassy_trace_", "", x) for x in names]
    recs = []
    body = data[off:]
    for cyc, pc, r0, r1, r2, i, kind, _ in REC.iter_unpack(body[:len(body) // REC.size * REC.size]):
        if kind == 1:
            recs.append((cyc / hz, "lost", "lost", 0, r0 | r1 << 32, 0, 0))
        else:
            recs.append((cyc / hz, "hit", ev[i] if i < len(ev) else f"id{i}", pc, r0, r1, r2))
    return hz, names, recs


def nm(elf, *flags):
    out = subprocess.run([NM, "-C", *flags, elf], capture_output=True, text=True, check=True).stdout
    return [l.split(None, 3) for l in out.splitlines()]


def cmd_points(a):
    want = set(a.sym)
    lines, seen = [], set()
    for f in nm(a.elf):
        if len(f) == 3 and f[1] in "Tt" and (("_embassy_trace_" in f[2]) or f[2] in want):
            if f[0] not in seen:
                seen.add(f[0])
                lines.append(f"{f[0]} {f[2]}")
    missing = want - {l.split(None, 1)[1] for l in lines}
    if missing:
        sys.exit(f"symbols not found: {sorted(missing)}")
    if not lines:
        sys.exit("no _embassy_trace_* symbols (is the trace feature on?)")
    text = "\n".join(lines) + "\n"
    open(a.o, "w").write(text) if a.o else sys.stdout.write(text)


def namer(elf):
    """task id -> 'sym+off' using the nearest data symbol at or below it."""
    if not elf:
        return lambda x: ""
    syms = []
    for f in nm(elf, "-S", "-n"):
        try:
            if len(f) == 4 and f[2] in "dDbBrR":
                syms.append((int(f[0], 16), int(f[1], 16), f[3]))
            elif len(f) == 3 and f[1] in "dDbBrR":
                syms.append((int(f[0], 16), 0, f[2]))
        except ValueError:
            pass

    def name(x):
        best = None
        for s in syms:
            if s[0] <= x:
                best = s
            else:
                break
        if best and (not best[1] or x < best[0] + best[1]):
            return f" ({best[2]}+{x - best[0]:#x})"
        return ""
    return name


def cmd_summary(a):
    hz, names, recs = read_trace(a.out)
    counts = defaultdict(lambda: defaultdict(int))
    longest = {}
    open_ = {}
    lost = 0
    for t, kind, ev, pc, r0, r1, r2 in recs:
        if kind == "lost":
            lost = r0
            continue
        key = (r0, r1) if ev.startswith("task_") else (r0, None)
        counts[key][ev] += 1
        if ev == "task_exec_begin":
            open_[key] = t
        elif ev == "task_exec_end" and key in open_:
            d = t - open_.pop(key)
            longest[key] = max(d, longest.get(key, 0))
    print(f"{len(recs)} records, {recs[-1][0] if recs else 0:.6f} s, lost {lost}")
    for (ex, task), c in sorted(counts.items(), key=lambda k: (k[0][0], -1 if k[0][1] is None else k[0][1])):
        what = f"executor {ex:#x}" if task is None else f"  task {task:#x}"
        extra = ""
        if (ex, task) in longest:
            extra = f"  longest exec {longest[(ex, task)] * 1e3:.3f} ms"
        print(f"{what}: " + " ".join(f"{k}={v}" for k, v in sorted(c.items())) + extra)


def cmd_stalls(a):
    hz, names, recs = read_trace(a.out)
    nm_ = namer(a.sym)
    end = recs[-1][0] if recs else 0.0
    ready, running, polling = {}, {}, {}
    found = 0
    poll_ms = a.poll_ms if a.poll_ms is not None else a.exec_ms

    def tn(task):
        return f"task {task:#x}{nm_(task)}"

    def rep(msg):
        nonlocal found
        found += 1
        print(msg)

    for t, kind, ev, pc, r0, r1, r2 in recs:
        if kind == "lost":
            print(f"{t:.6f}s NOTE: LOST {r0} hits; stalls around here may be false")
            continue
        k = (r0, r1)
        if ev == "task_ready_begin":
            ready.setdefault(k, t)
        elif ev == "task_exec_begin":
            if k in ready:
                d = (t - ready.pop(k)) * 1e3
                if d > a.ready_ms:
                    rep(f"{t:.6f}s READY-LATE executor {r0:#x} {tn(r1)} waited {d:.3f} ms")
            running[k] = t
        elif ev == "task_exec_end":
            if k in running:
                d = (t - running.pop(k)) * 1e3
                if d > a.exec_ms:
                    rep(f"{t:.6f}s EXEC-LONG executor {r0:#x} {tn(r1)} ran {d:.3f} ms")
        elif ev == "poll_start":
            polling.setdefault(r0, t)
        elif ev == "executor_idle":
            if r0 in polling:
                d = (t - polling.pop(r0)) * 1e3
                if d > poll_ms:
                    rep(f"{t:.6f}s POLL-LONG executor {r0:#x} polled {d:.3f} ms before idle")
    for k, t in ready.items():
        if (end - t) * 1e3 > a.ready_ms:
            rep(f"{end:.6f}s READY-OPEN executor {k[0]:#x} {tn(k[1])} readied at {t:.6f}s, never ran ({(end - t) * 1e3:.3f} ms)")
    for k, t in running.items():
        if (end - t) * 1e3 > a.exec_ms:
            rep(f"{end:.6f}s EXEC-OPEN executor {k[0]:#x} {tn(k[1])} began {t:.6f}s, no exec_end ({(end - t) * 1e3:.3f} ms)")
    for ex, t in polling.items():
        if (end - t) * 1e3 > poll_ms:
            rep(f"{end:.6f}s POLL-OPEN executor {ex:#x} poll_start at {t:.6f}s, no idle ({(end - t) * 1e3:.3f} ms)")
    print(f"{found} stall(s); trace ends {end:.6f}s")


def cmd_dump(a):
    hz, names, recs = read_trace(a.out)
    for t, kind, ev, pc, r0, r1, r2 in recs:
        if a.frm <= t <= a.to:
            if kind == "lost":
                print(f"{t:.6f} LOST {r0} hits dropped")
            else:
                print(f"{t:.6f} {ev:<18} pc={pc:#x} r0={r0:#x} r1={r1:#x} r2={r2:#x}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    s = p.add_subparsers(dest="cmd", required=True)
    x = s.add_parser("points")
    x.add_argument("elf")
    x.add_argument("--sym", action="append", default=[])
    x.add_argument("-o")
    x.set_defaults(f=cmd_points)
    x = s.add_parser("summary")
    x.add_argument("out")
    x.set_defaults(f=cmd_summary)
    x = s.add_parser("stalls")
    x.add_argument("out")
    x.add_argument("--ready-ms", type=float, required=True)
    x.add_argument("--exec-ms", type=float, required=True)
    x.add_argument("--poll-ms", type=float, help="poll_start -> idle limit (default: --exec-ms)")
    x.add_argument("--sym", metavar="ELF")
    x.set_defaults(f=cmd_stalls)
    x = s.add_parser("dump")
    x.add_argument("out")
    x.add_argument("--from", dest="frm", type=float, default=0.0)
    x.add_argument("--to", type=float, default=float("inf"))
    x.set_defaults(f=cmd_dump)
    a = p.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
