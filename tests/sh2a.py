# SPDX-License-Identifier: GPL-2.0-or-later
"""A big-endian SH-2A interpreter for the display mods: the integer, branch
and load/store instructions the routines in mods/*/gui_*.s use and the stretches
of display firmware they replace. Anything else raises, so a routine that grows
an instruction this does not model fails loudly instead of running wrong."""
import struct

MASK = 0xFFFFFFFF


def signed(v, bits=32):
    v &= (1 << bits) - 1
    return v - (1 << bits) if v >> (bits - 1) else v


class Memory:
    """A few byte regions at fixed addresses."""

    def __init__(self):
        self.regions = []

    def add(self, base, data):
        self.regions.append((base, bytearray(data)))

    def find(self, addr, size):
        for base, buf in self.regions:
            if base <= addr and addr + size <= base + len(buf):
                return buf, addr - base
        raise AssertionError('access to unmapped %#x' % addr)

    def read(self, addr, size):
        buf, off = self.find(addr, size)
        return bytes(buf[off:off + size])

    def write(self, addr, data):
        buf, off = self.find(addr, len(data))
        buf[off:off + len(data)] = data

    def u16(self, addr):
        return struct.unpack('>H', self.read(addr, 2))[0]

    def u32(self, addr):
        return struct.unpack('>I', self.read(addr, 4))[0]

    def put32(self, addr, value):
        self.write(addr, struct.pack('>I', value & MASK))


class Sh2a:
    RET = 0xFFFF0000

    def __init__(self, mem):
        self.mem = mem
        self.r = [0] * 16
        self.t = 0
        self.pr = self.RET
        self.steps = 0

    def load(self, addr, size):
        fmt = {1: '>b', 2: '>h', 4: '>i'}[size]
        return struct.unpack(fmt, self.mem.read(addr, size))[0] & MASK

    def store(self, addr, value, size):
        fmt = {1: '>B', 2: '>H', 4: '>I'}[size]
        self.mem.write(addr, struct.pack(fmt, value & ((1 << 8 * size) - 1)))

    def run(self, pc, stop, limit=200000):
        while pc != stop:
            self.steps += 1
            assert self.steps < limit, 'no stop at %#x after %d instructions' % (stop, limit)
            pc = self.step(pc)
        return pc

    def word(self, pc):
        return self.mem.u16(pc)

    def step(self, pc):
        """Execute the instruction at pc and return the next pc."""
        op = self.word(pc)
        r = self.r
        n, m = op >> 8 & 15, op >> 4 & 15
        imm = op & 0xFF
        simm = signed(imm, 8)
        nxt = pc + 2
        top = op >> 12

        if op & 0xF00F == 0x0000:                               # movi20 #imm20,Rn
            r[n] = signed((op >> 4 & 15) << 16 | self.word(pc + 2), 20) & MASK
            return pc + 4
        if op & 0xF00F == 0x3001:                               # movu.b/w @(disp12,Rm),Rn
            ext = self.word(pc + 2)
            kind, disp = ext >> 12, ext & 0xFFF
            if kind == 0x8:
                r[n] = self.load(r[m] + disp, 1) & 0xFF
            elif kind == 0x9:
                r[n] = self.load(r[m] + disp * 2, 2) & 0xFFFF
            else:
                raise AssertionError('32-bit op %#06x %#06x at %#x' % (op, ext, pc))
            return pc + 4

        if top == 0xE:
            r[n] = simm & MASK
        elif top == 0x7:
            r[n] = (r[n] + simm) & MASK
        elif top == 0xD:                                        # mov.l @(disp,pc),Rn
            r[n] = self.mem.u32((pc & ~3) + 4 + imm * 4)
        elif top == 0x9:                                        # mov.w @(disp,pc),Rn
            r[n] = self.load(pc + 4 + imm * 2, 2)
        elif top == 0x5:                                        # mov.l @(disp,Rm),Rn
            r[n] = self.load(r[m] + (op & 15) * 4, 4)
        elif top == 0x1:                                        # mov.l Rm,@(disp,Rn)
            self.store(r[n] + (op & 15) * 4, r[m], 4)
        elif top == 0xA:                                        # bra
            return self.branch(pc, pc + 4 + signed(op & 0xFFF, 12) * 2)
        elif op & 0xFF00 == 0xC700:                             # mova @(disp,pc),R0
            r[0] = (pc & ~3) + 4 + imm * 4
        elif op & 0xFF00 in (0x8900, 0x8B00, 0x8D00, 0x8F00):   # bt, bf, bt/s, bf/s
            taken = self.t == (1 if op & 0xFF00 in (0x8900, 0x8D00) else 0)
            target = pc + 4 + simm * 2
            if op & 0xFF00 in (0x8D00, 0x8F00):
                return self.branch(pc, target) if taken else self.delay(pc)
            return target if taken else nxt
        elif op & 0xFF00 == 0x8800:
            self.t = int(r[0] == simm & MASK)                   # cmp/eq #imm,R0
        elif op & 0xFF00 == 0xC800:
            self.t = int(r[0] & imm == 0)                       # tst #imm,R0
        elif op & 0xFF00 == 0xC900:
            r[0] &= imm                                         # and #imm,R0
        elif op & 0xFF00 == 0xCB00:
            r[0] |= imm                                         # or #imm,R0
        elif op & 0xFF00 == 0x8400:                             # mov.b @(disp,Rm),R0
            r[0] = self.load(r[m] + (op & 15), 1)
        elif op & 0xFF00 == 0x8500:                             # mov.w @(disp,Rm),R0
            r[0] = self.load(r[m] + (op & 15) * 2, 2)
        elif op == 0x0009:
            pass
        elif op == 0x000B:                                      # rts
            return self.branch(pc, self.pr)
        elif op & 0xF0FF == 0x400B:                             # jsr @Rn
            self.pr = pc + 4
            return self.branch(pc, r[n])
        else:
            self.alu(op, n, m)
        return nxt

    def branch(self, pc, target):
        self.step(pc + 2)
        return target

    def delay(self, pc):
        self.step(pc + 2)
        return pc + 4

    def alu(self, op, n, m):
        r = self.r
        lo = op & 15
        group = op >> 12
        if group == 0x2:
            if lo in (0, 1, 2):                                 # mov.b/w/l Rm,@Rn
                self.store(r[n], r[m], 1 << lo)
            elif lo == 6:                                       # mov.l Rm,@-Rn
                r[n] = (r[n] - 4) & MASK
                self.store(r[n], r[m], 4)
            elif lo == 8:
                self.t = int(r[n] & r[m] == 0)                  # tst
            elif lo == 9:
                r[n] &= r[m]
            elif lo == 0xB:
                r[n] |= r[m]
            else:
                raise AssertionError('op %#06x' % op)
        elif group == 0x6:
            if lo in (0, 1, 2):                                 # mov.b/w/l @Rm,Rn
                r[n] = self.load(r[m], 1 << lo)
            elif lo == 3:
                r[n] = r[m]
            elif lo in (5, 6):                                  # mov.w/l @Rm+,Rn
                size = 1 << (lo - 4)
                r[n] = self.load(r[m], size)
                r[m] = (r[m] + size) & MASK
            elif lo == 0xB:
                r[n] = -r[m] & MASK                             # neg
            elif lo == 0xC:
                r[n] = r[m] & 0xFF
            elif lo == 0xD:
                r[n] = r[m] & 0xFFFF
            else:
                raise AssertionError('op %#06x' % op)
        elif group == 0x0:
            if lo in (4, 5, 6):                                 # mov.b/w/l Rm,@(R0,Rn)
                self.store(r[0] + r[n], r[m], 1 << (lo - 4))
            elif lo in (0xC, 0xD, 0xE):                         # mov.b/w/l @(R0,Rm),Rn
                r[n] = self.load(r[0] + r[m], 1 << (lo - 0xC))
            elif op & 0xF0FF == 0x0029:
                r[n] = self.t                                   # movt
            else:
                raise AssertionError('op %#06x' % op)
        elif group == 0x3:
            a, b = r[n], r[m]
            if lo == 0:
                self.t = int(a == b)
            elif lo == 2:
                self.t = int(a >= b)
            elif lo == 3:
                self.t = int(signed(a) >= signed(b))
            elif lo == 6:
                self.t = int(a > b)
            elif lo == 7:
                self.t = int(signed(a) > signed(b))
            elif lo == 8:
                r[n] = (a - b) & MASK
            elif lo == 0xC:
                r[n] = (a + b) & MASK
            else:
                raise AssertionError('op %#06x' % op)
        elif group == 0x4:
            self.shift(op, n, m)
        else:
            raise AssertionError('op %#06x' % op)

    def shift(self, op, n, m):
        r = self.r
        low = op & 0xFF
        v = r[n]
        if low == 0x00:
            r[n] = v << 1 & MASK                                # shll
        elif low == 0x01:
            r[n] = v >> 1                                       # shlr
        elif low == 0x08:
            r[n] = v << 2 & MASK
        elif low == 0x09:
            r[n] = v >> 2
        elif low == 0x18:
            r[n] = v << 8 & MASK
        elif low == 0x19:
            r[n] = v >> 8
        elif low == 0x21:
            r[n] = signed(v) >> 1 & MASK                        # shar
        elif low == 0x24:                                       # rotcl
            r[n] = (v << 1 | self.t) & MASK
            self.t = v >> 31
        elif low == 0x10:                                       # dt
            r[n] = (v - 1) & MASK
            self.t = int(r[n] == 0)
        elif low == 0x15:
            self.t = int(signed(v) > 0)                         # cmp/pl
        elif low == 0x80 and op & 0xF00F == 0x4000:             # mulr R0,Rn
            r[n] = v * r[0] & MASK
        elif op & 0xF00F == 0x400C:                             # shad Rm,Rn
            amount = r[m]
            if signed(amount) >= 0:
                r[n] = v << (amount & 31) & MASK
            elif amount & 31:
                r[n] = signed(v) >> (32 - (amount & 31)) & MASK
            else:
                r[n] = MASK if v >> 31 else 0
        elif op & 0xF00F == 0x400D:                             # shld Rm,Rn
            amount = r[m]
            if signed(amount) >= 0:
                r[n] = v << (amount & 31) & MASK
            else:
                r[n] = v >> (32 - (amount & 31)) if amount & 31 else 0
        else:
            raise AssertionError('op %#06x' % op)
