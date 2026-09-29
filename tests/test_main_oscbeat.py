# SPDX-License-Identifier: GPL-2.0-or-later
"""The oscbeat MAIN mod: found by signature, hooked on the beat send's
trampoline so the stock send is kept, stackable with the other MAIN mods,
and -- through a small SH-4 interpreter covering only the instructions the
trampoline and the routine use -- sending the stock packet unchanged
followed by one OSC datagram built from the packet's own fields."""
import struct

import pytest

import patch_main
import sigpatch

WRAPPER = 0x085113EC
PORT = 50010
BASE = 0x08000000
SIG_OFF = 0x100                 # oscbeat's site
SPAN = SIG_OFF + 4
CAVE = (0x400, 0x1800)


def build_image(base=BASE):
    """A synthetic MAIN image: both real signatures, the literal the oscbeat
    span loads (the send wrapper's address) where its pc-relative load reads
    it, and an erased cave big enough for every registered routine."""
    d = bytearray(0x2000)
    for k, op in enumerate(patch_main.MODS['oscbeat'].sig):
        struct.pack_into('<H', d, SIG_OFF + 2 * k, op)
    # 0xDC2E at SPAN reads (pc & ~3) + 4 + 46 * 4
    struct.pack_into('<I', d, (SPAN & ~3) + 4 + 46 * 4, WRAPPER)
    d[CAVE[0]:CAVE[1]] = b'\xff' * (CAVE[1] - CAVE[0])
    profile = sigpatch.ImageProfile(flash_base=base, loadtab_off=0, loadtab_n=0,
                                    ram_base=base + CAVE[0], ram_end=base + CAVE[1],
                                    byteorder='<')
    return profile, bytes(d)


def blob():
    with open(patch_main.blob_path('oscbeat'), 'rb') as f:
        return f.read()


def test_registered_for_main_as_a_hook_on_the_beat_send():
    mod = patch_main.MODS['oscbeat']
    assert mod.target == 'main' and mod.hook and not mod.literal
    assert dict(sig=mod.sig, start=mod.start, end=mod.end) == patch_main.BEAT_SEND


def test_site_found_once_and_jump_to_the_trampoline_written():
    profile, data = build_image()
    mods = {'oscbeat': patch_main.MODS['oscbeat']}
    out = sigpatch.patch(profile, patch_main.blob_path, data, mods, ['oscbeat'])

    tramp = struct.unpack_from('<I', out, SPAN + 8)[0]
    assert BASE + CAVE[0] <= tramp < BASE + CAVE[1]
    want = sigpatch.call_site(mods['oscbeat'], BASE + SPAN, BASE + SPAN + 18, tramp,
                              byteorder='<', jump=True)
    assert out[SPAN:SPAN + 18] == want
    assert out[SIG_OFF:SPAN] == data[SIG_OFF:SPAN]              # context untouched
    assert out[SPAN + 18:SIG_OFF + 26] == data[SPAN + 18:SIG_OFF + 26]


# -- a just-big-enough SH-4 interpreter ------------------------------------------

class Sh4:
    RET = 0xFFFF0000

    def __init__(self, code, code_at):
        self.mem = bytearray(0x10000)
        self.mem[code_at:code_at + len(code)] = code
        self.r = [0] * 16
        self.pr = self.RET
        self.fpscr = 0
        self.t = 0
        self.sends = []

    def u32(self, a):
        return struct.unpack_from('<I', self.mem, a)[0]

    def s8(self, a):
        return struct.unpack_from('<b', self.mem, a)[0] & 0xFFFFFFFF

    def put32(self, a, v):
        struct.pack_into('<I', self.mem, a, v & 0xFFFFFFFF)

    def run(self, pc, stop=RET):
        while pc != stop:
            op = struct.unpack_from('<H', self.mem, pc)[0]
            pc = self.step(op, pc)

    def delay(self, pc):
        self.step(struct.unpack_from('<H', self.mem, pc + 2)[0], pc + 2)

    def step(self, op, pc):
        r = self.r
        n, m, d = op >> 8 & 15, op >> 4 & 15, op & 15
        imm = op & 0xFF
        simm = imm - 256 if imm & 0x80 else imm
        hi = op >> 12
        if hi == 0xD:                                           # mov.l @(disp,pc),Rn
            r[n] = self.u32((pc & ~3) + 4 + imm * 4)
        elif hi == 0x9:                                         # mov.w @(disp,pc),Rn
            v = struct.unpack_from('<H', self.mem, pc + 4 + imm * 2)[0]
            r[n] = (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF
        elif op & 0xF00F == 0x000E:                             # mov.l @(R0,Rm),Rn
            r[n] = self.u32(r[0] + r[m])
        elif op & 0xF00F == 0x6002:                             # mov.l @Rm,Rn
            r[n] = self.u32(r[m])
        elif hi == 0xE:                                         # mov #imm,Rn
            r[n] = simm & 0xFFFFFFFF
        elif hi == 0x7:                                         # add #imm,Rn
            r[n] = (r[n] + simm) & 0xFFFFFFFF
        elif op & 0xF00F == 0x0006:                             # mov.l Rm,@(R0,Rn)
            self.put32(r[0] + r[n], r[m])
        elif op & 0xF0FF == 0x4008:                             # shll2
            r[n] = r[n] << 2 & 0xFFFFFFFF
        elif op & 0xF00F == 0x300C:                             # add Rm,Rn
            r[n] = (r[n] + r[m]) & 0xFFFFFFFF
        elif op == 0x4F22:                                      # sts.l pr,@-r15
            r[15] -= 4
            self.put32(r[15], self.pr)
        elif op & 0xF00F == 0x2006:                             # mov.l Rm,@-Rn
            r[n] -= 4
            self.put32(r[n], r[m])
        elif op & 0xF0FF == 0x006A:                             # sts fpscr,Rn
            r[n] = self.fpscr
        elif op & 0xF0FF == 0x406A:                             # lds Rm,fpscr
            self.fpscr = r[n]
        elif op & 0xF00F == 0x2009:                             # and Rm,Rn
            r[n] &= r[m]
        elif op & 0xFF00 == 0xC700:                             # mova @(disp,pc),r0
            r[0] = (pc & ~3) + 4 + imm * 4
        elif op & 0xF00F == 0x6003:                             # mov Rm,Rn
            r[n] = r[m]
        elif op & 0xF00F == 0x6006:                             # mov.l @Rm+,Rn
            r[n] = self.u32(r[m])
            r[m] += 4
        elif op & 0xF00F == 0x2002:                             # mov.l Rm,@Rn
            self.put32(r[n], r[m])
        elif op & 0xF0FF == 0x4010:                             # dt Rn
            r[n] -= 1
            self.t = int(r[n] == 0)
        elif op & 0xFF00 == 0x8B00:                             # bf
            if not self.t:
                return pc + 4 + simm * 2
        elif hi == 0x1:                                         # mov.l Rm,@(disp,Rn)
            self.put32(r[n] + d * 4, r[m])
        elif hi == 0x5:                                         # mov.l @(disp,Rm),Rn
            r[n] = self.u32(r[m] + d * 4)
        elif op & 0xF00F == 0x000C:                             # mov.b @(R0,Rm),Rn
            r[n] = self.s8(r[0] + r[m])
        elif op & 0xFF00 == 0x8000:                             # mov.b R0,@(disp,Rn)
            self.mem[r[m] + d] = r[0] & 0xFF
        elif op & 0xF00F == 0x6004:                             # mov.b @Rm+,Rn
            r[n] = self.s8(r[m])
            r[m] += 1
        elif op & 0xF0FF == 0x400B:                             # jsr @Rn
            target = r[n]
            self.delay(pc)
            self.pr = pc + 4
            return self.call(target, pc + 4)
        elif op & 0xF0FF == 0x402B:                             # jmp @Rn
            target = r[n]
            self.delay(pc)
            return target
        elif op & 0xF0FF == 0x4026:                             # lds.l @Rn+,pr
            self.pr = self.u32(r[n])
            r[n] += 4
        elif op == 0x000B:                                      # rts
            back = self.pr
            self.delay(pc)
            return back
        elif op == 0x0009:
            pass
        else:
            raise AssertionError('unmodelled opcode %#06x at %#x' % (op, pc))
        return pc + 2

    def call(self, target, resume):
        """The send wrapper is not in the image: record what it was given,
        and clobber r0-r7 as a C function may. Any other target is code in
        the image, run as a subroutine."""
        if target != WRAPPER:
            self.pr = resume
            return target
        r = self.r
        length = self.u32(r[5])
        self.sends.append(dict(desc=bytes(self.mem[r[5]:r[5] + 20]),
                               payload=bytes(self.mem[r[4]:r[4] + length])))
        for k in range(8):
            r[k] = 0xDEAD0000 + k
        return resume


def run_site():
    """Patch a synthetic image mapped at address 0 and run it from the start
    of the hooked span to the instruction after it, with a beat packet of
    known field values and the registers the beat loop has there: r4 = the
    packet, r5 = r15, r0 = 0x160, r2 = the port."""
    profile, data = build_image(base=0)
    image = sigpatch.patch(profile, patch_main.blob_path, data,
                           {'oscbeat': patch_main.MODS['oscbeat']}, ['oscbeat'])
    cpu = Sh4(image, 0)
    packet, frame = 0x4000, 0xF000
    fields = {0x54: bytes([0x00, 0x10, 0x20, 0x00]), 0x5A: bytes([0x1E, 0xDC]),
              0x5C: bytes([3]), 0x5F: bytes([2])}
    for off, val in fields.items():
        cpu.mem[packet + off:packet + off + len(val)] = val
    cpu.put32(frame + 0x218, packet)
    desc = frame + 0x150
    cpu.mem[desc:desc + 20] = struct.pack('<IIIII', 0x60, 1, 0, 0x60, 0)
    saved = [0xA0000000 + k for k in range(16)]
    saved[10] = 0xFFE7FFFF
    cpu.r[8:15] = saved[8:15]
    cpu.r[0], cpu.r[2], cpu.r[4], cpu.r[5], cpu.r[15] = 0x160, 50001, packet, frame, frame
    cpu.run(SPAN, stop=SPAN + 18)
    return cpu, saved, frame, desc


def test_stock_send_is_kept_exactly():
    cpu, _, _, _ = run_site()
    stock = cpu.sends[0]
    assert struct.unpack('<IIIII', stock['desc']) == (0x60, 1, 0, 0x60, 50001)
    assert len(stock['payload']) == 0x60


def test_extra_datagram_is_an_osc_message_built_from_the_packet_fields():
    cpu, _, _, _ = run_site()
    assert len(cpu.sends) == 2
    extra = cpu.sends[1]
    want = (b'/cdj/beat\0\0\0' + b',iiii\0\0\0'
            + struct.pack('>iiii', 2, 3, 0x1EDC, 0x00102000))
    assert extra['payload'] == want
    length, mode, ip, tag, port = struct.unpack('<IIIII', extra['desc'])
    assert (length, mode, ip, tag, port) == (len(want), 1, 0, 0x60, PORT)


def test_the_site_resumes_as_after_the_stock_span():
    cpu, saved, frame, desc = run_site()
    assert cpu.r[15] == frame
    assert cpu.r[8:12] == saved[8:12] and cpu.r[13:15] == saved[13:15]
    assert cpu.r[12] == WRAPPER
    assert struct.unpack_from('<IIIII', cpu.mem, desc) == (0x60, 1, 0, 0x60, 50001)
