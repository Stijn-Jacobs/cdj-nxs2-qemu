# SPDX-License-Identifier: GPL-2.0-or-later
"""SH-2A 32-bit instruction smoke test for the sh4 translator (Task 4b).

Hand-assembles (big-endian) one sequence per 32-bit group Task 4 found the
CDJ-850 firmware (big-endian SH-2A) to use and QEMU 9.1.0's sh4 translator
to mistranslate as illegal (notes/cdj850-sh-insns.md, SH2A_ONLY_USED):

- movi20   0000nnnniiii0000  (imm20 sign-extended)
- movi20s  0000nnnniiii0001  (imm20 sign-extended, << 8)
- mov32    0011nnnnmmmm0001  second word sub: 0 MOV.B  1 MOV.W  2 MOV.L
  (stores) / 4 MOV.B  5 MOV.W  6 MOV.L  8 MOVU.B  9 MOVU.W (loads) /
  3 FMOV.S/D (store, disp*4 / disp*8 by FPSCR.SZ) /
  7 FMOV.S/D (load)
- bitop32  0011nnnn0iii1001  second word sub: 0 BCLR.B  1 BSET.B  2 BST.B
  3 BLD.B  4 BAND.B  5 BOR.B  6 BXOR.B  0xB BLDNOT.B  0xC BANDNOT.B
  0xD BORNOT.B

Encodings per GNU binutils opcodes/sh-opc.h (arch_op32 entries), sizes and
first-half masks per Linux arch/sh/kernel/cpu/sh2a/opcode_helper.c.

Vehicle: the built qemu-system-sh4eb (display binary, same translator
sources as qemu-system-sh4) with -M none, a 16 MB RAM (which -M none maps
at phys 0), the blob loaded by the generic loader at phys 0 (the loader
takes a physical address). The blob runs from the SH reset PC 0xA0000000
(P2, uncached direct alias of phys 0), so all its addresses are P2-virtual
and PC-relative literal math stays consistent. The blob is self-recording:
every result word is stored to a cursor at phys 0x1000, then the CPU parks
on bra-self. The test reads the words back over QMP (pmemsave of physical
RAM, parsed locally) and compares against the predetermined values.

Pre-patch the first movi20 raises an illegal-instruction exception, so no
result word (not even the sentinel) is ever written; the blob's handler
records the fault PC for the failure message: RED. Post-patch every word
matches: GREEN. Skipped cleanly when the emulator binary is absent.

Task 6d adds two more blobs in the same vehicle:

- nott (0x0068, SH-2A-only per binutils opcodes/sh-opc.h `{"nott", ...}`,
  Renesas SH-2A/SH2A-FPU Software Manual section 6.3.28, and
  sim/sh/gencode.c `SET_SR_T (T == 0)`): the first-task fault word from
  Task 6c (0x0C08EFCA, EXPEVT=0x180). RED pre-patch faults at the first
  nott exactly like the firmware; GREEN post-patch records the toggles.
- no-RB-banking: the SH-2A SR has no RB bit (SH-2A Software Manual
  section 2.2.2), so an ldc of an RB-set SR must not switch the R0-R7
  bank (the Task 6c pool-2 root cause). RED pre-patch the bank switches
  and the recorded words mismatch; GREEN post-patch everything stays on
  bank 0 and SR reads back without RB.
"""
import json
import os
import socket
import struct
import subprocess
import time

import pytest

BASE = 0xA0000000   # P2 reset PC; direct alias of phys 0, MMU-off safe
DATA = BASE + 0x2000
RES = BASE + 0x1000
RES_PHYS = 0x1000
STACK = DATA + 0x400   # room for the exception pushes below the test areas
FAULT_SLOT = DATA + 0x100  # handler records fault PCs here (RED path)
FAULT_PHYS = 0x2100        # slot[0] = SPC, slot[1] = stack top (see below)
HB = BASE + 0x800          # VBR value: this tree dispatches exceptions
HANDLER = HB + 0x100       # through a vector TABLE, entry = VBR + vec*4
FIRST_MOVI20_OFF = 20      # 10-word prologue; movi20 sits at BASE + 20


class Blob:
    """A minimal big-endian assembler: 16-bit words plus a 32-bit PC-relative
    literal pool laid out after the code."""

    def __init__(self):
        self.words = []
        self.lits = []
        self.refs = []  # (word_index, lit_index)

    def w16(self, w):
        self.words.append(w & 0xFFFF)

    def w32(self, hi, lo):
        self.w16(hi)
        self.w16(lo)

    def movl_pc(self, n, value):
        """mov.l @(disp,PC),Rn loading a 32-bit literal (disp patched later)."""
        self.refs.append((len(self.words), len(self.lits)))
        self.lits.append(value & 0xFFFFFFFF)
        self.w16(0xD000 | (n << 8))

    def seal(self):
        """Lay out the pending literals right after the current code and
        patch their mov.l disps. Called before pad_to() so far-away code
        (the handler) cannot push the pool out of disp range."""
        base = BASE + 2 * len(self.words)
        if len(self.words) & 1:
            self.words.append(0x0009)  # nop pad keeps literals 4-aligned
            base += 2
        for word_index, lit_index in self.refs:
            addr = base + 4 * lit_index
            pc = BASE + 2 * word_index
            disp = (addr - ((pc & ~3) + 4)) // 4
            assert 0 <= disp <= 0xFF, (hex(pc), hex(addr))
            self.words[word_index] |= disp
        for lit in self.lits:
            self.words.append((lit >> 16) & 0xFFFF)
            self.words.append(lit & 0xFFFF)
        self.lits = []
        self.refs = []

    def pad_to(self, nbytes):
        """nop-fill code to an absolute byte offset from the blob base."""
        while 2 * len(self.words) < nbytes:
            self.w16(0x0009)  # nop

    def mov(self, n, imm):
        self.w16(0xE000 | (n << 8) | (imm & 0xFF))

    def add(self, n, imm):
        self.w16(0x7000 | (n << 8) | (imm & 0xFF))

    def store_cursor(self, rval, rcursor):
        """mov.l Rval,@Rcursor; add #4,Rcursor."""
        self.w16(0x2000 | (rcursor << 8) | (rval << 4) | 2)
        self.add(rcursor, 4)

    def bytes(self):
        code = list(self.words)
        if len(code) & 1:
            code.append(0x0009)  # nop pad keeps literals 4-aligned
        lit_base = BASE + 2 * len(code)
        out = list(code)
        for word_index, lit_index in self.refs:
            addr = lit_base + 4 * lit_index
            pc = BASE + 2 * word_index
            disp = (addr - ((pc & ~3) + 4)) // 4
            assert 0 <= disp <= 0xFF, (hex(pc), hex(addr))
            out[word_index] |= disp
        for lit in self.lits:
            out.append((lit >> 16) & 0xFFFF)
            out.append(lit & 0xFFFF)
        return struct.pack(">%dH" % len(out), *out)


def emit_handler(b):
    """Illegal-insn handler at HANDLER (RED path). Which state holds the
    fault PC depends on the CPU: plain SH-4 models (sh7785) save it in
    SPC/SSR, while SH_FEATURE_SH2A entry (sh2a, and the boards that set the
    bit) pushes SR/PC on the R15 stack instead and leaves SPC alone. Record
    both; the test accepts either."""
    b.seal()
    b.pad_to(0x900)
    assert HANDLER == BASE + 0x900
    b.w16(0x6000 | (1 << 8) | (15 << 4) | 2)  # mov.l @R15,R1 (stack top)
    b.w16(0x0042)           # stc SPC,R0
    for _ in range(4):
        b.w16(0x7000 | (10 << 8) | 0x40)  # R10 -> DATA + 0x100
    b.w16(0x2000 | (10 << 8) | (0 << 4) | 2)  # mov.l R0,@R10 (slot[0] = SPC)
    b.w16(0x1000 | (10 << 8) | (1 << 4) | 1)  # mov.l R1,@(1,R10) (slot[1])
    b.w16(0xAFFE)           # bra-self


def prologue(b):
    """Shared setup (see above): VBR + BL clear, stack, vector-table entry 4.
    Ten words; the word under test follows at BASE + 20."""
    b.movl_pc(10, DATA)   # R10 = data base
    b.movl_pc(11, RES)    # R11 = result cursor
    b.movl_pc(0, HB)      # R0 = vector base
    b.w16(0x402E)         # ldc R0,VBR (privileged; SH-2A always is)
    b.movl_pc(0, 0x600000F0)
    b.w16(0x400E)         # ldc R0,SR (clear BL so faults vector)
    b.movl_pc(15, STACK)  # R15 = stack for the exception pushes
    b.movl_pc(0, HANDLER)  # vector-table entry 4 <- handler address
    b.movl_pc(1, HB + 4 * 4)
    b.w16(0x2000 | (1 << 8) | (0 << 4) | 2)  # mov.l R0,@R1


def build_blob():
    """Returns (blob_bytes, expected_words). expected_words is the exact RAM
    content the blob records at RES, in order."""
    b = Blob()
    want = []
    R = 11  # result cursor

    def rec(v):
        want.append(v & 0xFFFFFFFF)

    def rec_reg(r):
        b.store_cursor(r, R)

    # -- setup -----------------------------------------------------------
    prologue(b)

    # -- movi20 ----------------------------------------------------------
    b.w32(0x01A0, 0xBCDE)   # movi20 #0xABCDE,R1
    rec(0xFFFABCDE)
    rec_reg(1)
    b.w32(0x0210, 0x2345)   # movi20 #0x12345,R2
    rec(0x00012345)
    rec_reg(2)

    # -- movi20s ---------------------------------------------------------
    b.w32(0x0311, 0x2345)   # movi20s #0x12345,R3 -> 0x01234500
    rec(0x01234500)
    rec_reg(3)
    b.w32(0x0481, 0xABCD)   # movi20s #0x8ABCD,R4 -> 0xF8ABCD00
    rec(0xF8ABCD00)
    rec_reg(4)

    # -- FMOV.D disp12 (needs FPSCR.SZ=1) --------------------------------
    b.w16(0xF3FD)           # fschg (SZ 0 -> 1)
    b.movl_pc(0, 0x400921FB)  # R0 = DR2 high
    b.w16(0x405A | (0 << 8))  # lds R0,FPUL
    b.w16(0xF00D | (2 << 8))  # fsts FPUL,FR2
    b.movl_pc(0, 0x54442D18)  # R0 = DR2 low
    b.w16(0x405A | (0 << 8))
    b.w16(0xF00D | (3 << 8))  # fsts FPUL,FR3
    b.w32(0x3A21, 0x3008)   # fmov.d DR2,@(0x40,R10)
    b.w32(0x34A1, 0x7008)   # fmov.d @(0x40,R10),DR4
    b.w16(0xF000 | (11 << 8) | (4 << 4) | 0xA)  # fmov DR4,@R11
    b.add(R, 8)
    rec(0x400921FB)
    rec(0x54442D18)
    b.w16(0xF3FD)           # fschg (SZ 1 -> 0)

    # -- FMOV.S disp12 ---------------------------------------------------
    b.movl_pc(0, 0x40490FDB)
    b.w16(0x405A | (0 << 8))
    b.w16(0xF00D | (3 << 8))  # FR3 = 0x40490FDB
    b.w32(0x3A31, 0x3010)   # fmov.s FR3,@(0x40,R10)
    b.w32(0x35A1, 0x7010)   # fmov.s @(0x40,R10),FR5
    b.w16(0xF000 | (11 << 8) | (5 << 4) | 0xA)  # fmov.s FR5,@R11
    b.add(R, 4)
    rec(0x40490FDB)

    # -- MOV.L disp12 ----------------------------------------------------
    b.movl_pc(5, 0xDEADBEEF)
    rec_reg(5)
    rec(0xDEADBEEF)
    b.w32(0x3A51, 0x2004)   # mov.l R5,@(0x10,R10)
    b.w32(0x36A1, 0x6004)   # mov.l @(0x10,R10),R6
    rec(0xDEADBEEF)
    rec_reg(6)

    # -- MOV.W disp12 ----------------------------------------------------
    b.movl_pc(5, 0x0000ABCD)
    rec_reg(5)
    rec(0x0000ABCD)
    b.w32(0x3A51, 0x1021)   # mov.w R5,@(0x42,R10)
    b.w32(0x36A1, 0x5021)   # mov.w @(0x42,R10),R6 (sign-extending)
    rec(0xFFFFABCD)
    rec_reg(6)

    # -- MOV.B disp12 ----------------------------------------------------
    b.movl_pc(5, 0x00000080)
    rec_reg(5)
    rec(0x00000080)
    b.w32(0x3A51, 0x0044)   # mov.b R5,@(0x44,R10)
    b.w32(0x36A1, 0x4044)   # mov.b @(0x44,R10),R6 (sign-extending)
    rec(0xFFFFFF80)
    rec_reg(6)

    # -- MOVU.B / MOVU.W disp12 (zero-extending) --------------------------
    b.w32(0x36A1, 0x8044)   # movu.b @(0x44,R10),R6
    rec(0x00000080)
    rec_reg(6)
    b.w32(0x36A1, 0x9021)   # movu.w @(0x42,R10),R6
    rec(0x0000ABCD)
    rec_reg(6)

    # -- bitop32 ---------------------------------------------------------
    # R8 = test byte address; each op resets the byte, then records the
    # memory byte (via extu.b for a clean 0-255 value) and, where the op
    # produces T, the T bit via movt.
    b.movl_pc(8, DATA + 0x80)  # R8 = bitop byte

    def bitop(bit, sub, init, t_setup, want_byte, want_t):
        # All init bytes are mov-able (0x00-0x7F, or 0xFF/0xFE as #-1/#-2).
        assert 0 <= init <= 0x7F or init in (0xFF, 0xFE), hex(init)
        b.mov(9, init & 0xFF)
        b.w16(0x2000 | (8 << 8) | (9 << 4) | 0)  # mov.b R9,@R8
        if t_setup == 1:
            b.w16(0x0018)  # sett
        elif t_setup == 0:
            b.w16(0x0008)  # clrt
        b.w32(0x3800 | (8 << 8) | (bit << 4) | 9, (sub << 12))
        b.w16(0x6000 | (6 << 8) | (8 << 4) | 0)  # mov.b @R8,R6
        b.w16(0x6000 | (6 << 8) | (6 << 4) | 0xC)  # extu.b R6,R6
        rec(want_byte)
        rec_reg(6)
        if want_t is not None:
            b.w16(0x0029 | (6 << 8))  # movt R6
            rec(want_t)
            rec_reg(6)

    bitop(0, 0x0, 0xFF, None, 0xFE, None)  # bclr.b #0
    bitop(1, 0x1, 0x00, None, 0x02, None)  # bset.b #1
    bitop(2, 0x2, 0x00, 1, 0x04, None)     # bst.b #2 (T=1)
    bitop(2, 0x3, 0x04, None, 0x04, 1)     # bld.b #2
    bitop(0, 0x4, 0xFE, 1, 0xFE, 0)        # band.b #0 (T=1, bit=0)
    bitop(0, 0xC, 0xFE, 1, 0xFE, 1)        # bandnot.b #0
    bitop(2, 0xB, 0x00, None, 0x00, 1)     # bldnot.b #2
    bitop(3, 0x5, 0x08, 0, 0x08, 1)        # bor.b #3 (T=0, bit=1)
    bitop(3, 0xD, 0x00, 0, 0x00, 1)        # bornot.b #3
    bitop(0, 0x6, 0x01, 1, 0x01, 0)        # bxor.b #0 (T=1, bit=1)

    # -- sentinel + park -------------------------------------------------
    b.movl_pc(0, 0x51454D55)
    rec(0x51454D55)
    rec_reg(0)
    b.w16(0xAFFE)           # bra-self

    emit_handler(b)
    return b.bytes(), want


def build_nott_blob():
    """Returns (blob_bytes, expected_words, first_nott_off). T-bit toggles
    through nott (0x0068): 0->1, 1->0, and a double toggle from 1 back to 1,
    each observed via movt. Encoding/semantics per binutils opcodes/sh-opc.h
    (`0000000001101000 nott`, arch_sh2a_nofpu_up) and the Renesas SH-2A
    manual section 6.3.28 (`~T -> T`)."""
    b = Blob()
    want = []
    R = 11

    def rec(v):
        want.append(v & 0xFFFFFFFF)

    def rec_reg(r):
        b.store_cursor(r, R)

    prologue(b)

    b.w16(0x0008)             # clrt (T=0)
    first_off = 2 * len(b.words)
    b.w16(0x0068)             # nott -> T=1
    b.w16(0x0029 | (6 << 8))  # movt R6
    rec(1)
    rec_reg(6)

    b.w16(0x0068)             # nott -> T=0
    b.w16(0x0029 | (6 << 8))  # movt R6
    rec(0)
    rec_reg(6)

    b.w16(0x0018)             # sett (T=1)
    b.w16(0x0068)             # nott -> T=0
    b.w16(0x0068)             # nott -> T=1
    b.w16(0x0029 | (6 << 8))  # movt R6
    rec(1)
    rec_reg(6)

    b.movl_pc(0, 0x51454D55)
    rec(0x51454D55)
    rec_reg(0)
    b.w16(0xAFFE)             # bra-self

    emit_handler(b)
    return b.bytes(), want, first_off


def build_norb_blob():
    """Returns (blob_bytes, expected_words). The SH-2A has no SR.RB bit, so
    ldc of an RB-set value must leave the R0-R7 bank alone and SR must read
    back without RB. Pre-patch (SH-4 banking applied to -cpu sh2a) the bank
    switches and every word below mismatches."""
    b = Blob()
    want = []
    R = 11

    def rec(v):
        want.append(v & 0xFFFFFFFF)

    def rec_reg(r):
        b.store_cursor(r, R)

    def ldc_imm(v):
        b.movl_pc(0, v)
        b.w16(0x400E)         # ldc R0,SR

    prologue(b)
    # Prologue's ldc loaded 0x600000F0 (RB=1): post-fix RB is dropped by the
    # translator, so the whole blob runs on bank 0.
    b.mov(1, 0x11)
    ldc_imm(0x400000E0)       # RB=0: bank must not change either way
    b.w16(0x6000 | (6 << 8) | (1 << 4) | 3)  # mov R1,R6
    rec(0x11)
    rec_reg(6)

    # Attempt to set RB=1, write, then clear: post-fix the write is visible.
    ldc_imm(0x600000F0)       # RB=1 attempt: must be ignored
    b.mov(1, 0x22)
    ldc_imm(0x400000E0)
    b.w16(0x6000 | (6 << 8) | (1 << 4) | 3)  # mov R1,R6
    rec(0x22)
    rec_reg(6)

    # SR reads back without the RB bit (gen_read_sr ORs into Rd: zero first).
    ldc_imm(0x600000F0)       # RB=1 attempt again
    b.mov(6, 0)
    b.w16(0x0002 | (6 << 8))  # stc SR,R6
    rec(0x400000F0)           # MD|IMASK=15, RB clear
    rec_reg(6)

    b.movl_pc(0, 0x51454D55)
    rec(0x51454D55)
    rec_reg(0)
    b.w16(0xAFFE)             # bra-self

    emit_handler(b)
    return b.bytes(), want


def qemu_binary():
    if "GUI_QEMU" in os.environ:
        cand = os.environ["GUI_QEMU"]
    else:
        cand = os.path.expanduser("~/qemu-build-eb/qemu-system-sh4eb")
    if not (cand and os.path.isfile(cand)):
        pytest.skip("qemu-system-sh4eb not built (looked at %s)" % cand)
    return cand


def pick_cpu(qemu):
    r = subprocess.run([qemu, "-cpu", "?"], capture_output=True, text=True,
                       timeout=60)
    models = r.stdout + r.stderr
    for line in models.splitlines():
        name = line.strip().split()[0] if line.strip() else ""
        if name == "sh2a":
            return "sh2a"
    return "sh7785"  # pre-patch fallback: 32-bit groups SIGILL here


class QMP:
    def __init__(self, path):
        self.io = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        deadline = time.time() + 30
        while True:
            try:
                self.io.connect(path)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.1)
        self.io.settimeout(30)
        self.f = self.io.makefile("rwb")
        self._read()  # greeting
        self.cmd("qmp_capabilities")

    def _read(self):
        out = b""
        while not out.endswith(b"\r\n"):
            chunk = self.f.readline()
            if not chunk:
                raise EOFError("QMP closed")
            out += chunk
        return json.loads(out.decode())

    def cmd(self, name, args=None):
        msg = json.dumps({"execute": name,
                          "arguments": args or {}}).encode() + b"\r\n"
        self.f.write(msg)
        self.f.flush()
        for _ in range(100000):
            rsp = self._read()
            if "event" in rsp:
                continue
            if rsp.get("error"):
                raise AssertionError("QMP %s failed: %s" % (name, rsp))
            return rsp.get("return")
        raise AssertionError("QMP %s: no reply within event budget" % name)

    def close(self):
        try:
            self.f.close()
        finally:
            self.io.close()


DUMP_SIZE = 0x3000


def run_guest(qemu, cpu, blob_bytes, tmp_path, tag):
    """Boot blob_bytes at phys 0 under -M none, run briefly, and return the
    first DUMP_SIZE bytes of physical RAM plus a log-tail callable."""
    blob_path = tmp_path / ("%s.bin" % tag)
    blob_path.write_bytes(blob_bytes)
    # NOTE: not tmp_path — macOS TMPDIR is too long for a unix socket path.
    sock = "/tmp/sh2a-%d-%s-qmp" % (os.getpid(), tag)
    try:
        os.unlink(sock)
    except OSError:
        pass
    log_path = tmp_path / ("%s.log" % tag)
    log_fh = open(log_path, "wb")
    # NOTE: stdout/stderr go to a file, not a pipe: pre-patch the guest
    # faults in a reset loop whose log would fill a 64 KB pipe and wedge
    # QEMU's main loop before we read results back.
    cmd = [qemu, "-M", "none", "-cpu", cpu, "-m", "16",
           "-display", "none", "-serial", "none", "-monitor", "none",
           # NOTE: no -no-reboot: pre-patch the blob's first movi20 raises an
           # illegal-instruction exception the bare machine cannot handle;
           # the guest must stay up until we read back (the absence of)
           # results.
           "-S",
           "-qmp", "unix:%s,server=on,wait=off" % sock,
           # No cpu-num: execution starts at the reset PC 0xA0000000, the P2
           # alias of phys 0 where the loader put the blob. (cpu-num would
           # park the PC at phys 0 = P0, which faults without the MMU.)
           "-device", "loader,file=%s,addr=0x00000000" % blob_path]
    proc = subprocess.Popen(cmd, stdout=log_fh, stderr=subprocess.STDOUT)

    def log_tail():
        try:
            data = log_path.read_bytes()[-500:]
            return data.decode("latin1")
        except OSError:
            return ""
    try:
        qmp = QMP(sock)
        try:
            qmp.cmd("cont")
            time.sleep(2.0)
            qmp.cmd("stop")
            # Physical read: with the MMU off, the debug virtual path only
            # serves P0, so pmemsave RAM instead.
            dump_path = tmp_path / ("%s.ram" % tag)
            qmp.cmd("human-monitor-command",
                    {"command-line": 'pmemsave %#x %#x "%s"'
                     % (0, DUMP_SIZE, dump_path)})
            dump = dump_path.read_bytes()
        finally:
            try:
                qmp.cmd("quit")
            except (AssertionError, EOFError, OSError):
                pass
            qmp.close()
        proc.communicate(timeout=30)
        return dump, log_tail
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.communicate()
        log_fh.close()
        try:
            os.unlink(sock)
        except OSError:
            pass


def u32s(data, off, n):
    return [v & 0xFFFFFFFF
            for v in struct.unpack(">%di" % n, data[off:off + 4 * n])]


def build_reject_blob(first, second):
    """Minimal blob: prologue (VBR + BL clear) then one undefined 32-bit
    form, which must fault. Returns (bytes, byte offset of the bad word)."""
    b = Blob()
    prologue(b)
    bad_off = 2 * len(b.words)
    b.w32(first, second)
    b.w16(0xAFFE)         # unreachable: the word above must fault
    emit_handler(b)
    return b.bytes(), bad_off


def test_sh2a_32bit_groups(tmp_path):
    qemu = qemu_binary()
    blob, want = build_blob()
    cpu = pick_cpu(qemu)
    dump, log_tail = run_guest(qemu, cpu, blob, tmp_path, "smoke")
    assert len(dump) == DUMP_SIZE, \
        "short pmemsave (%d bytes, cpu=%s, log tail %r)" \
        % (len(dump), cpu, log_tail())
    got = u32s(dump, RES_PHYS, len(want))
    fault_pcs = u32s(dump, FAULT_PHYS, 2)  # [SPC, stack top]
    if got[len(want) - 1] == 0x51454D55:
        # GREEN: the blob ran to the end; every word must match.
        bad = [(i, hex(a), hex(b)) for i, (a, b) in
               enumerate(zip(want, got)) if a != b]
        assert not bad, \
            "cpu=%s %d/%d mismatch, first: %s" % (cpu, len(bad),
                                                 len(want), bad[:8])
        return
    if any((pc & 0xFFFFFF) == FIRST_MOVI20_OFF for pc in fault_pcs):
        pytest.fail("RED (expected pre-patch): illegal instruction at "
                    "the first movi20 (fault PCs %s, cpu=%s); no result "
                    "words were recorded"
                    % ([hex(pc) for pc in fault_pcs], cpu))
    pytest.fail("neither GREEN nor clean RED: sentinel %#x, fault PCs %s, "
                "cpu=%s, log tail %r"
                % (got[len(want) - 1],
                   [hex(pc) for pc in fault_pcs], cpu, log_tail()))


def test_sh2a_nott(tmp_path):
    """nott (0x0068) toggles T. Pre-patch it faults like the Task 6c
    firmware site (0x0C08EFCA, EXPEVT=0x180): RED. Post-patch GREEN."""
    qemu = qemu_binary()
    blob, want, first_off = build_nott_blob()
    cpu = pick_cpu(qemu)
    dump, log_tail = run_guest(qemu, cpu, blob, tmp_path, "nott")
    assert len(dump) == DUMP_SIZE, \
        "short pmemsave (%d bytes, cpu=%s, log tail %r)" \
        % (len(dump), cpu, log_tail())
    got = u32s(dump, RES_PHYS, len(want))
    fault_pcs = u32s(dump, FAULT_PHYS, 2)  # [SPC, stack top]
    if got[len(want) - 1] == 0x51454D55:
        # GREEN: the blob ran to the end; every word must match.
        bad = [(i, hex(a), hex(b)) for i, (a, b) in
               enumerate(zip(want, got)) if a != b]
        assert not bad, \
            "cpu=%s %d/%d mismatch, first: %s" % (cpu, len(bad),
                                                 len(want), bad[:8])
        return
    if any(pc == BASE + first_off for pc in fault_pcs):
        pytest.fail("RED (expected pre-patch): illegal instruction at "
                    "the first nott (fault PCs %s, cpu=%s); no result "
                    "words were recorded"
                    % ([hex(pc) for pc in fault_pcs], cpu))
    pytest.fail("neither GREEN nor clean RED: sentinel %#x, fault PCs %s, "
                "cpu=%s, log tail %r"
                % (got[len(want) - 1],
                   [hex(pc) for pc in fault_pcs], cpu, log_tail()))


def test_sh2a_no_rb_banking(tmp_path):
    """An ldc of an RB-set SR must not bank R0-R7 on -cpu sh2a, and SR must
    read back without RB. Pre-patch the bank switches (Task 6c pool-2 root
    cause) and the recorded words mismatch: RED. Post-patch GREEN."""
    qemu = qemu_binary()
    blob, want = build_norb_blob()
    cpu = pick_cpu(qemu)
    dump, log_tail = run_guest(qemu, cpu, blob, tmp_path, "norb")
    assert len(dump) == DUMP_SIZE, \
        "short pmemsave (%d bytes, cpu=%s, log tail %r)" \
        % (len(dump), cpu, log_tail())
    got = u32s(dump, RES_PHYS, len(want))
    fault_pcs = u32s(dump, FAULT_PHYS, 2)  # [SPC, stack top]
    if got[len(want) - 1] == 0x51454D55:
        bad = [(i, hex(a), hex(b)) for i, (a, b) in
               enumerate(zip(want, got)) if a != b]
        assert not bad, \
            "cpu=%s RED (RB banking still active) %d/%d mismatch, first: %s" \
            % (cpu, len(bad), len(want), bad[:8])
        return
    pytest.fail("blob did not reach the sentinel: sentinel %#x, fault PCs %s, "
                "cpu=%s, log tail %r"
                % (got[len(want) - 1],
                   [hex(pc) for pc in fault_pcs], cpu, log_tail()))


def test_sh2a_32bit_undefined_faults(tmp_path):
    """Undefined 32-bit forms must raise illegal instructions, not silently
    advance as nops: bit 7 set on a 0x3n09 first half (Linux masks 0xf08f),
    an undefined 0x3nm1 second word (sub 0xA), an undefined 0x3n09 second
    word (sub 0x7). Passes pre- and post-patch; it pins the fault behavior
    the new return-false paths rely on."""
    qemu = qemu_binary()
    cpu = pick_cpu(qemu)
    cases = [
        (0x3899, 0x0000),  # bitop first half with bit 7 set
        (0x3AA1, 0xA014),  # mov32 second word sub 0xA (undefined)
        (0x3809, 0x7000),  # bitop second word sub 0x7 (undefined)
    ]
    for i, (first, second) in enumerate(cases):
        blob, bad_off = build_reject_blob(first, second)
        dump, log_tail = run_guest(qemu, cpu, blob, tmp_path,
                                   "reject%d" % i)
        fault_pcs = u32s(dump, FAULT_PHYS, 2)  # [SPC, stack top]
        assert any(pc == BASE + bad_off for pc in fault_pcs), \
            "cpu=%s case %#06x/%#06x: fault PCs %s, want %#x (log tail %r)" \
            % (cpu, first, second,
               [hex(pc) for pc in fault_pcs], BASE + bad_off, log_tail())
