#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One-shot readable dump of the CDJ GUI's Rust/Embassy runtime state.

Reads memory live over the Blackfin GDB remote server (CDJ_BF531_GDB) or from
a CDJ_BF531_DUMP directory.  Every address is resolved from symbol names in
the ELF given on the command line (bfin-elf-nm -C -S); nothing is built in.

  gui_dump.py ELF --gdb /path/to.sock|host:port|port [--stay]
  gui_dump.py ELF --gdb ... --watch SYM[,SYM] [--max N]
  gui_dump.py ELF --offline DIR
"""
import argparse
import bisect
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import time

TEXT_TYPES = "TtWw"
SP_REG, RETS_REG, RETI_REG, PC_REG, NUM_REGS = 14, 37, 49, 53, 54
FLAG_SPAWNED, FLAG_RUN_QUEUED = 1, 2


# ---- symbols ---------------------------------------------------------------
class Symbols:
    def __init__(self, nm_text):
        self.by_name = []          # (name, addr, size, type)
        for line in nm_text.splitlines():
            m = re.match(r"^([0-9a-fA-F]+) (?:([0-9a-fA-F]+) )?(\w) (.+)$", line)
            if m:
                self.by_name.append((m.group(4).strip(), int(m.group(1), 16),
                                     int(m.group(2) or "0", 16), m.group(3)))
        self.sorted = sorted((a, s, n, t) for n, a, s, t in self.by_name)
        self.addrs = [x[0] for x in self.sorted]

    @classmethod
    def from_elf(cls, elf, nm=None):
        nm = nm or os.environ.get("BFIN_NM") or shutil.which("bfin-elf-nm")
        if not nm:
            sys.exit("bfin-elf-nm not found: put it on PATH, set BFIN_NM or pass --nm")
        return cls(subprocess.check_output([nm, "-C", "-S", elf], text=True))

    def find(self, suffix):
        """(addr, size) of the symbol whose name is `suffix` or ends in ::suffix."""
        hit = [x for x in self.by_name
               if x[0] == suffix or x[0].endswith("::" + suffix)]
        for n, a, s, t in hit:
            if n == suffix:
                return a, s
        return (hit[0][1], hit[0][2]) if hit else None

    def _cover(self, addr, text_only):
        i = bisect.bisect_right(self.addrs, addr) - 1
        for a, s, n, t in self.sorted[max(i - 4, 0):i + 1][::-1]:
            if text_only and t not in TEXT_TYPES:
                continue
            # unsized asm labels: accept a short distance past them
            if addr == a or (s and addr < a + s) or (not s and t in TEXT_TYPES
                                                     and addr - a < 0x400):
                return n if addr == a else "%s+0x%x" % (n, addr - a)
        return None

    def where(self, addr):
        """'name+0xoff' for an address inside a symbol, else None."""
        return self._cover(addr, False)

    def code(self, w):
        """Symbolize w only if it looks like a code address (even, in text)."""
        return None if w & 1 else self._cover(w, True)


# ---- targets ---------------------------------------------------------------
class OfflineTarget:
    BANKS = (("sdram.bin", 0), ("l1a.bin", 0xFF800000),
             ("l1b.bin", 0xFF900000), ("l1s.bin", 0xFFB00000))

    def __init__(self, d):
        self.banks = []
        for f, base in self.BANKS:
            p = os.path.join(d, f)
            if os.path.exists(p):
                with open(p, "rb") as fh:
                    self.banks.append((base, fh.read()))
        if not self.banks:
            sys.exit("no sdram.bin/l1*.bin in " + d)

    def read(self, addr, n):
        for base, data in self.banks:
            if base <= addr and addr + n <= base + len(data):
                return data[addr - base:addr - base + n]
        raise IOError("unmapped 0x%x+%d" % (addr, n))

    def regs(self):
        return None


class RspError(Exception):
    pass


class LiveTarget:
    def __init__(self, spec, timeout=5.0):
        if "/" in spec or os.path.exists(spec):
            self.s = socket.socket(socket.AF_UNIX)
            self.s.connect(spec)
        else:
            host, _, port = spec.rpartition(":")
            self.s = socket.create_connection((host or "127.0.0.1", int(port)))
        self.timeout = timeout
        self.buf = b""
        self.noack = False
        self.running = False
        self.send("QStartNoAckMode")    # accepted or not, both are handled
        self.noack = True               # assume accepted, as this server does

    # framing
    def _put(self, data):
        self.s.sendall(b"$" + data + b"#%02x" % (sum(data) & 0xFF))

    def _get(self, timeout):
        """Next packet payload, or None on timeout."""
        end = time.time() + timeout
        while True:
            i = self.buf.find(b"$")
            if i >= 0:
                j = self.buf.find(b"#", i)
                if j >= 0 and len(self.buf) >= j + 3:
                    pkt = self.buf[i + 1:j]
                    self.buf = self.buf[j + 3:]
                    if not self.noack:
                        self.s.sendall(b"+")
                    return pkt
            left = end - time.time()
            if left <= 0:
                return None
            self.s.settimeout(left)
            try:
                d = self.s.recv(65536)
            except socket.timeout:
                return None
            if not d:
                raise RspError("connection closed")
            self.buf += d

    def send(self, cmd):
        self._put(cmd.encode())
        r = self._get(self.timeout)
        if r is None:
            raise RspError("timeout waiting for reply to " + cmd)
        return r.decode()

    def interrupt(self):
        """Halt the target; returns the server's stop reply (verbatim)."""
        self.s.sendall(b"\x03")
        r = self._get(self.timeout)
        self.running = False
        return r.decode() if r is not None else None

    def cont(self):
        self._put(b"c")
        self.running = True

    def wait_stop(self, timeout):
        r = self._get(timeout)
        if r is not None:
            self.running = False
            return r.decode()
        return None

    def read(self, addr, n):
        out = b""
        while n:
            k = min(n, 0x1000)
            r = self.send("m%x,%x" % (addr, k))
            if r.startswith("E") or len(r) != 2 * k:
                raise IOError("m%x,%x -> %s" % (addr, k, r[:16]))
            out += bytes.fromhex(r)
            addr, n = addr + k, n - k
        return out

    def regs(self):
        r = self.send("g")
        return list(struct.unpack("<%dI" % NUM_REGS, bytes.fromhex(r)[:NUM_REGS * 4]))

    def watch(self, addr, n, on=True):
        return self.send("%s2,%x,%x" % ("Z" if on else "z", addr, n))

    def detach(self):
        try:
            self._put(b"D")
            self._get(1.0)
        except (OSError, RspError):
            pass


# ---- printers (pure: bytes in, lines out) ----------------------------------
def words(b):
    return list(struct.unpack("<%dI" % (len(b) // 4), b[:len(b) // 4 * 4]))


def note(sy, w):
    return sy.where(w) if w > 0xFFF else None


def raw_lines(sy, b, base=0):
    out = []
    ws = words(b)
    for i in range(0, len(ws), 8):
        row = ws[i:i + 8]
        notes = ["w%d=%s" % (i + k, n) for k, w in enumerate(row)
                 for n in [note(sy, w)] if n]
        out.append("  +%03x: %s%s" % (base + i * 4, " ".join("%08x" % w for w in row),
                                     "  ; " + " ".join(notes) if notes else ""))
    return out


def fmt_executor(sy, b):
    ws = words(b)
    head = ws[0] if ws else 0
    return ["  run-queue head: %s" % (("0x%08x %s" % (head, note(sy, head) or "")).strip()
                                      if head else "empty (0)")] + raw_lines(sy, b)


def fmt_pool(sy, b):
    out = []
    for t in range(len(b) // 32):
        w = words(b[t * 32:t * 32 + 32])
        st = [n for f, n in ((FLAG_SPAWNED, "SPAWNED"), (FLAG_RUN_QUEUED, "RUN_QUEUED"))
              if w[0] & f]
        rest = " ".join("%08x" % x for x in w[4:])
        out.append("  task %d: state=0x%x [%s] next=%s exec=%s poll=%s | %s" % (
            t, w[0], "|".join(st) or "idle",
            "0x%08x" % w[1] if w[1] else "-",
            ("0x%08x %s" % (w[2], note(sy, w[2]) or "")).strip() if w[2] else "-",
            ("0x%08x %s" % (w[3], sy.code(w[3]) or "?")).strip() if w[3] else "-", rest))
    return out or ["  (empty)"]


def fmt_time(sy, ticks_b, queue_b):
    ticks = struct.unpack("<Q", ticks_b[:8])[0]
    out = ["  TICKS = %d" % ticks]
    n = len(queue_b) // 16
    live = 0
    for i in range(n):
        dl, vt, data = struct.unpack("<QII", queue_b[i * 16:i * 16 + 16])
        if vt:
            live += 1
            out.append("  slot %2d: deadline=%d (%+d ticks) waker vtable=0x%08x %s data=0x%08x %s"
                       % (i, dl, dl - ticks, vt, note(sy, vt) or "", data, note(sy, data) or ""))
    out.append("  %d/%d slots in use%s" % (live, n, "" if len(queue_b) % 16 == 0 else
              "; trailing %d bytes: %s" % (len(queue_b) % 16, queue_b[n * 16:].hex())))
    return out


def fmt_ring(sy, b):
    w = words(b)
    out = ["  head=%d len=%d (%d bytes total)" % (w[0], w[1], len(b))]
    return out + raw_lines(sy, b[:32])[:2]


def fmt_stack(sy, b, base, limit):
    out = []
    for i, w in enumerate(words(b)):
        s = sy.code(w)
        if s:
            out.append("  0x%08x: %08x %s" % (base + i * 4, w, s))
            if len(out) >= limit:
                out.append("  ... (limit %d, raise with --depth)" % limit)
                break
    return out or ["  (no code addresses)"]


# ---- dump ------------------------------------------------------------------
def dump(sy, tgt, depth=48):
    out = []

    def section(title, names, fn, size=None):
        out.append("== %s" % title)
        for name in names:
            f = sy.find(name)
            if not f:
                out.append("%s: not in this ELF" % name)
                continue
            a, s = f
            try:
                out.append("%s @0x%08x size %d" % (name, a, s))
                out.extend(fn(a, s or size or 4))
            except (IOError, RspError, struct.error) as e:
                out.append("  read failed: %s" % e)

    regs = tgt.regs()
    out.append("== registers")
    if regs:
        out.append("  PC   0x%08x %s" % (regs[PC_REG], sy.code(regs[PC_REG]) or ""))
        out.append("  RETS 0x%08x %s" % (regs[RETS_REG], sy.code(regs[RETS_REG]) or ""))
        out.append("  RETI 0x%08x %s" % (regs[RETI_REG], sy.code(regs[RETI_REG]) or ""))
        out.append("  SP   0x%08x" % regs[SP_REG])
    else:
        out.append("  (offline: no register file)")

    section("executors", ["cdj_mods_gui::boot_init::EXEC%d" % i for i in (2, 3, 4, 6, 8)],
            lambda a, s: fmt_executor(sy, tgt.read(a, s)))
    pools = ["cdj_mods_gui::task_runtime::%s::POOL" % n
             for n in ("ui", "worker", "ticker", "transport", "artwork")]
    section("task pools", pools + ["cdj_mods_gui::update_runtime::link::POOL"],
            lambda a, s: fmt_pool(sy, tgt.read(a, s)))
    section("kernel", ["cdj_gui_rt::kernel::K"], lambda a, s: raw_lines(sy, tgt.read(a, s)))

    out.append("== timers")
    tk = sy.find("cdj_gui_rt::time::TICKS")
    if not tk:
        out.append("cdj_gui_rt::time::TICKS: not in this ELF")
    else:
        for q in ("TICK_QUEUE", "QUEUE"):
            f = sy.find("cdj_gui_rt::time::" + q)
            if not f:
                out.append("%s: not in this ELF" % q)
                continue
            try:
                out.append("%s @0x%08x" % (q, f[0]))
                out.extend(fmt_time(sy, tgt.read(tk[0], 8), tgt.read(f[0], f[1])))
            except (IOError, RspError) as e:
                out.append("  read failed: %s" % e)

    section("message rings", ["cdj_mods_gui::messages::" + n for n in ("LINK", "WORKER", "SCREEN")],
            lambda a, s: fmt_ring(sy, tgt.read(a, s)))

    out.append("== watchdog")
    for n in ("cdj_rt_long_polls", "cdj_rt_poll_max_cycles", "cdj_rt_long_poll_task",
              "cdj_rt_long_poll_executor"):
        f = sy.find(n)
        if not f:
            out.append("%s: not in this ELF" % n)
            continue
        try:
            v = struct.unpack("<I", tgt.read(f[0], 4))[0]
            out.append("%s = %d (0x%x) %s" % (n, v, v, (note(sy, v) or "") if "poll_" in n and
                                              n.endswith(("task", "executor")) else ""))
        except (IOError, RspError) as e:
            out.append("%s: read failed: %s" % (n, e))

    out.append("== stacks")
    stacks = [(n, a, sy.find(n + "_end")) for n, a, s, t in sy.by_name
              if re.fullmatch(r"_\w*task_stack", n)]
    stacks = [(n, a, e[0]) for n, a, e in stacks if e]
    if not stacks:
        out.append("no *_task_stack/_end pairs in this ELF")
    sp = regs[SP_REG] if regs else None
    for n, lo, hi in stacks:
        if sp is not None and not lo <= sp < hi:
            out.append("%s [0x%08x,0x%08x): SP not inside" % (n, lo, hi))
            continue
        start = sp if sp is not None else lo
        out.append("%s [0x%08x,0x%08x) scanning from %s0x%08x, %d bytes"
                   % (n, lo, hi, "SP " if sp is not None else "base (SP unknown) ", start, hi - start))
        try:
            out.extend(fmt_stack(sy, tgt.read(start, hi - start), start, depth))
        except (IOError, RspError) as e:
            out.append("  read failed: %s" % e)
    return out


def stamp():
    t = time.time()
    return time.strftime("%H:%M:%S", time.localtime(t)) + ".%03d" % (t % 1 * 1000)


def emit(lines, header):
    print("#### %s" % header)
    print("\n".join(lines))
    sys.stdout.flush()


# ---- modes -----------------------------------------------------------------
def run_live(a, sy):
    t = LiveTarget(a.gdb)
    if a.watch:
        return watch(a, sy, t)
    stop = t.interrupt()
    emit(dump(sy, t, a.depth), "%s stop=%s" % (stamp(), stop))
    if not a.stay:
        t.cont()
    t.s.close()


def watch(a, sy, t):
    pts = []
    for name in a.watch.split(","):
        f = sy.find(name.strip())
        if not f:
            sys.exit("%s: not in this ELF" % name)
        pts.append((name, f[0], f[1] or 4))
    t.interrupt()
    for name, addr, n in pts:
        r = t.watch(addr, n)
        print("Z2 %s @0x%08x len %d -> %s" % (name, addr, n, r))
        if r != "OK":
            sys.exit("watchpoint refused")
    hits = 0
    try:
        t.cont()
        while hits < a.max or not a.max:
            r = t.wait_stop(1.0)
            if r is None:
                continue
            hits += 1
            emit(dump(sy, t, a.depth), "%s hit %d stop=%s" % (stamp(), hits, r))
            if a.max and hits >= a.max:
                break
            t.cont()
    except KeyboardInterrupt:
        pass
    finally:
        if t.running:
            print("interrupt stop=%s" % t.interrupt())
        for name, addr, n in pts:
            t.watch(addr, n, False)
        t.detach()
    print("%d hit(s)" % hits)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("elf")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--gdb", help="unix socket path, host:port or port")
    g.add_argument("--offline", help="CDJ_BF531_DUMP directory")
    p.add_argument("--watch", help="SYMBOL[,SYMBOL]: dump on every write")
    p.add_argument("--max", type=int, default=0, help="stop after N hits (0 = until ^C)")
    p.add_argument("--stay", action="store_true", help="leave the target halted")
    p.add_argument("--depth", type=int, default=48, help="max stack return addresses")
    p.add_argument("--nm", help="bfin-elf-nm (default: $BFIN_NM or PATH)")
    a = p.parse_args(argv)
    sy = Symbols.from_elf(a.elf, a.nm)
    if a.offline:
        emit(dump(sy, OfflineTarget(a.offline), a.depth), "offline " + a.offline)
    else:
        run_live(a, sy)


if __name__ == "__main__":
    main()
