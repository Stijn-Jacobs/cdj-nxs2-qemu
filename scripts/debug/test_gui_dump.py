#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Host tests for gui_dump.py: python3 scripts/debug/test_gui_dump.py
Builds hw/cdj/bfin/tests/gdb_server into a temp dir (needs cc) and assembles a
tiny ELF with bfin-elf-as/ld (BFIN_BIN or PATH).  No emulator, no firmware."""
import contextlib
import io
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import gui_dump as gd  # noqa: E402

BFIN = os.environ.get("BFIN_BIN", os.path.expanduser("~/.local/share/cdj-toolchain/binutils-bfin/bin"))
ROOT = os.path.join(HERE, "..", "..")
TMP = tempfile.mkdtemp(prefix="gui_dump_test")


def tool(n):
    p = os.path.join(BFIN, n)
    return p if os.path.exists(p) else shutil.which(n)


# The asm mangles v0-mangled, bfin nm strips one leading underscore Rust-style names so nm -C demangles them to the real paths.
ASM = """
.text
.global __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt6kernel3foo
.type __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt6kernel3foo,@function
__RNvNtCs9NR7uvsr2yH_10cdj_gui_rt6kernel3foo:
NOP; NOP; NOP; NOP;
.size __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt6kernel3foo, 8
.global __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt6kernel3bar
.type __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt6kernel3bar,@function
__RNvNtCs9NR7uvsr2yH_10cdj_gui_rt6kernel3bar:
NOP; NOP;
.size __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt6kernel3bar, 4
.data
.global __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt4time5TICKS
.type __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt4time5TICKS,@object
__RNvNtCs9NR7uvsr2yH_10cdj_gui_rt4time5TICKS: .space 8
.size __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt4time5TICKS, 8
.global __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt4time5QUEUE
__RNvNtCs9NR7uvsr2yH_10cdj_gui_rt4time5QUEUE: .space 32
.size __RNvNtCs9NR7uvsr2yH_10cdj_gui_rt4time5QUEUE, 32
.global __ui_task_stack
__ui_task_stack: .space 64
.global __ui_task_stack_end
__ui_task_stack_end:
"""


def build_elf():
    s = os.path.join(TMP, "t.s")
    with open(s, "w") as f:
        f.write(ASM)
    subprocess.check_call([tool("bfin-elf-as"), "-o", s + ".o", s])
    elf = os.path.join(TMP, "t.elf")
    subprocess.check_call([tool("bfin-elf-ld"), "-e", "0", "-Ttext=0x100", "-Tdata=0x2000", "-o", elf, s + ".o"])
    return elf


class Server:
    def __init__(self):
        out = os.path.join(TMP, "bin")
        subprocess.check_call(["make", "-s", "-C", os.path.join(ROOT, "hw/cdj/bfin"),
                               "O=" + out, out + "/gdb_server"], stdout=subprocess.DEVNULL)

    def start(self):
        out = os.path.join(TMP, "bin")
        with socket.socket() as s:
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        self.p = subprocess.Popen([out + "/gdb_server", str(self.port)])
        time.sleep(0.3)
        return "127.0.0.1:%d" % self.port

    def stop(self):
        self.p.kill()
        self.p.wait()


@unittest.skipUnless(tool("bfin-elf-nm") and tool("bfin-elf-ld"), "bfin toolchain missing")
class Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.elf = build_elf()
        cls.srv = Server()
        cls.sy = gd.Symbols.from_elf(cls.elf, tool("bfin-elf-nm"))

    def test_symbols(self):
        sy = self.sy
        self.assertEqual(sy.find("cdj_gui_rt::time::TICKS"), (0x2000, 8))
        self.assertEqual(sy.find("time::QUEUE")[1], 32)
        self.assertIsNone(sy.find("cdj_mods_gui::boot_init::EXEC2"))
        foo = sy.find("cdj_gui_rt::kernel::foo")[0]
        self.assertEqual(sy.code(foo + 2), "cdj_gui_rt::kernel::foo+0x2")
        self.assertEqual(sy.code(foo + 0x8 + 2), "cdj_gui_rt::kernel::bar+0x2")
        self.assertIsNone(sy.code(foo + 3))           # odd
        self.assertIsNone(sy.code(0x2000))            # data, not code
        self.assertEqual(sy.where(0x2004), "cdj_gui_rt::time::TICKS+0x4")

    def test_printers(self):
        sy = self.sy
        foo = sy.find("cdj_gui_rt::kernel::foo")[0]
        pool = struct.pack("<8I", 3, 0, 0x2000, foo + 2, 1, 2, 3, 4)
        pool += struct.pack("<8I", 0, 0, 0, 0, 0, 0, 0, 0)
        t = "\n".join(gd.fmt_pool(sy, pool))
        self.assertIn("state=0x3 [SPAWNED|RUN_QUEUED]", t)
        self.assertIn("cdj_gui_rt::kernel::foo+0x2", t)
        self.assertIn("state=0x0 [idle]", t)
        q = struct.pack("<QII", 1100, 0x2000, 7) + b"\0" * 16
        t = "\n".join(gd.fmt_time(sy, struct.pack("<Q", 1000), q))
        self.assertIn("(+100 ticks)", t)
        self.assertIn("1/2 slots in use", t)
        self.assertIn("run-queue head: empty", gd.fmt_executor(sy, b"\0" * 12)[0])
        self.assertIn("TICKS", gd.fmt_executor(sy, struct.pack("<3I", 0x2000, 0, 0))[0])
        self.assertIn("head=2 len=3", gd.fmt_ring(sy, struct.pack("<8I", 2, 3, *[0] * 6))[0])
        st = gd.fmt_stack(sy, struct.pack("<4I", 0, foo + 2, 0x2000, foo + 5), 0x1000, 10)
        self.assertEqual(len(st), 1)
        self.assertIn("0x00001004", st[0])

    def test_offline_dump(self):
        d = os.path.join(TMP, "off")
        os.makedirs(d, exist_ok=True)
        ram = bytearray(0x10000)
        struct.pack_into("<Q", ram, 0x2000, 1000)
        struct.pack_into("<QII", ram, 0x2008, 1050, 0x2000, 0)
        with open(os.path.join(d, "sdram.bin"), "wb") as f:
            f.write(ram)
        out = "\n".join(gd.dump(self.sy, gd.OfflineTarget(d)))
        self.assertIn("cdj_mods_gui::boot_init::EXEC2: not in this ELF", out)
        self.assertIn("(+50 ticks)", out)
        self.assertIn("SP unknown", out)

    def test_rsp_roundtrip(self):
        t = gd.LiveTarget(self.srv.start())
        try:
            self.assertEqual(t.interrupt(), "T02")      # halted: still answers
            code = t.read(0, 4)
            self.assertEqual(len(code), 4)
            self.assertEqual(len(t.regs()), gd.NUM_REGS)
            self.assertEqual(t.watch(0x2000, 4), "OK")
            t.cont()
            self.assertEqual(t.wait_stop(5), "T05watch:2000;")
            self.assertEqual(struct.unpack("<I", t.read(0x2000, 4))[0], 12)
            t.cont()                                    # the bundle stores again
            self.assertEqual(t.wait_stop(5), "T05watch:2000;")
            t.cont()                                    # into the spin loop
            time.sleep(0.2)
            self.assertEqual(t.interrupt(), "T02")
            self.assertEqual(t.watch(0x2000, 4, False), "OK")
        finally:
            t.detach()
            self.srv.stop()

    def test_cli_watch(self):
        addr = self.srv.start()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            try:
                gd.main([self.elf, "--nm", tool("bfin-elf-nm"), "--gdb", addr,
                         "--watch", "cdj_gui_rt::time::TICKS", "--max", "1"])
            finally:
                self.srv.stop()
        out = buf.getvalue()
        self.assertIn("Z2 cdj_gui_rt::time::TICKS @0x00002000 len 8 -> OK", out)
        self.assertIn("hit 1 stop=T05watch:2000;", out)
        self.assertIn("TICKS = 12", out)
        self.assertIn("1 hit(s)", out)


if __name__ == "__main__":
    unittest.main()
