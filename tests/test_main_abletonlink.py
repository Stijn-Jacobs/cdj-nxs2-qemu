# SPDX-License-Identifier: GPL-2.0-or-later
"""The two Ableton Link MAIN mods: found by signature, hooked on the shared
trampolines (the beat send for the ALIVE, the Pro DJ Link receive for the
PONG), stackable with the other MAIN mods, and -- through the SH-4 interpreter
of test_main_oscbeat extended with the float and branch instructions these
routines add -- producing the datagrams Link's discovery v1 and measurement
v1 wire formats define (the encoders below are written from the Link headers,
not from the routines)."""
import functools
import os
import struct

import pytest

import patch_main
import sigpatch
from test_main_oscbeat import Sh4, WRAPPER

BASE = 0
BEAT_OFF = 0x300
RECV_OFF = 0x100
CAVE = (0x400, 0x3000)
FRAME = 0xF000
MASK = 0xFFE7FFFF

TICK = 0x0B0C5258
TCNT4 = 0xFFD90018
DEVICE = 0x0AB84CFA
CEP = 0x0AB84CE8
POOL = 0x0AB84CDC
GETTER = 0x08221D9E
GET_BLOCK, REL_BLOCK, UDP_SND = 0x08518C70, 0x08518DA8, 0x08233CF6

IP = 0xC0A80114                 # the deck, 192.168.1.20
PLAYER = 2
NODE_ID = struct.pack('>I', IP) + b'NXS' + bytes([PLAYER])
CEP_ID, POOL_ID, BLOCK = 31, 7, 0xE000
SENDER = (0xC0A80105, 54321)    # a Link app, 192.168.1.5
GHOST_OFFSET = 1 << 40


def build_image():
    """A synthetic MAIN image at address 0: the beat-send signature (with the
    wrapper literal its span loads), the Link receive signature (with the two
    literals its span loads) and an erased cave for every registered routine."""
    d = bytearray(0x3000)
    for off, site in ((BEAT_OFF, patch_main.BEAT_SEND), (RECV_OFF, patch_main.LINK_RECEIVE)):
        for k, op in enumerate(site['sig']):
            struct.pack_into('<H', d, off + 2 * k, op)
    struct.pack_into('<I', d, ((BEAT_OFF + 4) & ~3) + 4 + 46 * 4, WRAPPER)
    for k, value in ((3, POOL), (5, GET_BLOCK)):
        pc = RECV_OFF + 2 * k
        struct.pack_into('<I', d, (pc & ~3) + 4 + 0x5C * 4, value)
    d[CAVE[0]:CAVE[1]] = b'\xff' * (CAVE[1] - CAVE[0])
    profile = sigpatch.ImageProfile(flash_base=BASE, loadtab_off=0, loadtab_n=0,
                                    ram_base=BASE + CAVE[0], ram_end=BASE + CAVE[1],
                                    byteorder='<')
    return profile, bytes(d)


ALL = ('abletonlink', 'abletonlinkpong', 'oscbeat')       # the mods whose sites the image holds


@functools.lru_cache(maxsize=None)
def patched_image(names):
    profile, data = build_image()
    return sigpatch.patch(profile, patch_main.blob_path, data, patch_main.MODS, list(names))


def patched(names):
    return patched_image(tuple(names))


def test_registered_for_main_as_hooks_on_the_beat_send_and_the_link_receive():
    alive, pong = patch_main.MODS['abletonlink'], patch_main.MODS['abletonlinkpong']
    for mod, site in ((alive, patch_main.BEAT_SEND), (pong, patch_main.LINK_RECEIVE)):
        assert mod.target == 'main' and mod.hook and not mod.literal
        assert dict(sig=mod.sig, start=mod.start, end=mod.end) == site
    assert pong.end - pong.start == 7


def test_the_registry_row_enables_both_mods_and_defaults_off():
    path = os.path.join(os.path.dirname(patch_main.__file__), 'mods.conf')
    rows = [line.split('|') for line in open(path, encoding='utf-8')
            if line.strip() and not line.startswith('#')]
    (row,) = [r for r in rows if r[0] == 'ableton_link']
    assert row[1].split('+') == ['CDJ_MAIN_ABLETONLINK', 'CDJ_MAIN_ABLETONLINKPONG']
    assert row[2:5] == ['1', '0', 'off']


def test_jumps_to_the_trampolines_are_written_at_both_sites():
    profile, data = build_image()
    out = patched(['abletonlink', 'abletonlinkpong'])
    for off, name in ((BEAT_OFF + 4, 'abletonlink'), (RECV_OFF + 6, 'abletonlinkpong')):
        tramp = struct.unpack_from('<I', out, (off + 9) & ~3)[0]
        assert BASE + CAVE[0] <= tramp < BASE + CAVE[1]
        mod = patch_main.MODS[name]
        want = sigpatch.call_site(mod, off, off + 2 * (mod.end - mod.start), tramp,
                                  byteorder='<', jump=True)
        assert out[off:off + len(want)] == want
    assert out[:BEAT_OFF] != data[:BEAT_OFF]


def test_the_assembled_blobs_are_current():
    for name in ('abletonlink', 'abletonlinkpong'):
        assert os.path.getsize(patch_main.blob_path(name)) % 4 == 0


REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')


@pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
def test_real_main_has_each_signature_exactly_once_where_the_design_puts_it():
    with open(REAL_MAIN, 'rb') as f:
        img = sigpatch.Image(patch_main.MAIN_PROFILE, f.read())
    for name, span in (('abletonlink', 0x082DB248), ('abletonlinkpong', 0x083E1B8A)):
        mod = patch_main.MODS[name]
        assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == span


# -- the wire formats, from the Link headers -------------------------------------

def entry(key, value):
    return key + struct.pack('>I', len(value)) + value


def alive_message(tempo_us, beat_origin, time_origin, ttl=10):
    """discovery/v1 Messages.hpp + PeerState.hpp toPayload: header, tmln,
    sess, stst and mep4, each entry a key, a u32 size and the value."""
    header = b'_asdp_v' + bytes([1]) + bytes([1, ttl]) + struct.pack('>H', 0) + NODE_ID
    return (header
            + entry(b'tmln', struct.pack('>qqq', tempo_us, beat_origin, time_origin))
            + entry(b'sess', NODE_ID)
            + entry(b'stst', struct.pack('>?qq', False, 0, 0))
            + entry(b'mep4', struct.pack('>IH', IP, 50000)))


def ping(host_time=123456789, ghost_time=None):
    """link/v1 Measurement.hpp: header, type 1, __ht and, after the first
    exchange, _pgt."""
    message = b'_link_v' + bytes([1]) + bytes([1]) + entry(b'__ht', struct.pack('>q', host_time))
    if ghost_time is not None:
        message += entry(b'_pgt', struct.pack('>q', ghost_time))
    return message


def pong(ping_message, ghost_time):
    """PingResponder.hpp reply(): header, type 2, sess, __gt, then the PING's
    payload unchanged."""
    return (b'_link_v' + bytes([1]) + bytes([2]) + entry(b'sess', NODE_ID)
            + entry(b'__gt', struct.pack('>q', ghost_time)) + ping_message[9:])


# -- a just-big-enough SH-4 interpreter ------------------------------------------

def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


class ScratchCpu(Sh4):
    """The oscbeat interpreter plus the integer and branch instructions the
    routines add, with the firmware's fixed addresses mapped onto a small
    scratch memory."""
    WINDOWS = ()

    def loc(self, a):
        for real, scratch, size in self.WINDOWS:
            if real <= a < real + size:
                return scratch + a - real
        return a

    def u32(self, a):
        return super().u32(self.loc(a))

    def put32(self, a, v):
        super().put32(self.loc(a), v)

    def s8(self, a):
        return super().s8(self.loc(a))

    def poke(self, real, size, value):
        at = self.loc(real)
        self.mem[at:at + size] = value.to_bytes(size, 'little')

    def branch(self, cond, pc, simm, delayed):
        if delayed:
            taken = cond
            self.delay(pc)
            return pc + 4 + simm * 2 if taken else pc + 4
        return pc + 4 + simm * 2 if cond else pc + 2

    def step(self, op, pc):
        r = self.r
        n, m = op >> 8 & 15, op >> 4 & 15
        imm = op & 0xFF
        simm = imm - 256 if imm & 0x80 else imm
        if op & 0xFF00 in (0x8900, 0x8B00, 0x8D00, 0x8F00):
            cond = self.t if op & 0xFF00 in (0x8900, 0x8D00) else not self.t
            return self.branch(cond, pc, simm, op & 0xFF00 in (0x8D00, 0x8F00))
        if op >> 12 == 0xA:                                     # bra
            disp = op & 0xFFF
            disp = disp - 0x1000 if disp & 0x800 else disp
            self.delay(pc)
            return pc + 4 + disp * 2
        if op >> 12 == 0x9:                                     # mov.w @(disp,pc),Rn
            v = struct.unpack_from('<H', self.mem, pc + 4 + imm * 2)[0]
            r[n] = v - 0x10000 if v & 0x8000 else v
        elif op & 0xFF00 == 0x8500:                             # mov.w @(disp,Rm),r0
            r[0] = struct.unpack_from('<H', self.mem, self.loc(r[m] + (op & 15) * 2))[0]
        elif op & 0xF00F == 0x6000:                             # mov.b @Rm,Rn
            r[n] = self.s8(r[m])
        elif op & 0xF00F == 0x6001:                             # mov.w @Rm,Rn
            r[n] = struct.unpack_from('<H', self.mem, self.loc(r[m]))[0]
        elif op & 0xF00F == 0x6002:                             # mov.l @Rm,Rn
            r[n] = self.u32(r[m])
        elif op & 0xF00F == 0x600C:                             # extu.b
            r[n] = r[m] & 0xFF
        elif op & 0xF00F == 0x600D:                             # extu.w
            r[n] = r[m] & 0xFFFF
        elif op & 0xF00F == 0x2000:                             # mov.b Rm,@Rn
            self.mem[self.loc(r[n])] = r[m] & 0xFF
        elif op & 0xF00F == 0x0006:                             # mov.l Rm,@(R0,Rn)
            self.put32(r[0] + r[n], r[m])
        elif op & 0xF00F == 0x2008:                             # tst Rm,Rn
            self.t = int(r[n] & r[m] == 0)
        elif op & 0xF00F == 0x200A:                             # xor Rm,Rn
            r[n] ^= r[m]
        elif op & 0xF00F == 0x3000:                             # cmp/eq
            self.t = int(r[n] == r[m])
        elif op & 0xF00F == 0x3002:                             # cmp/hs
            self.t = int(r[n] >= r[m])
        elif op & 0xF00F == 0x3006:                             # cmp/hi
            self.t = int(r[n] > r[m])
        elif op & 0xF00F == 0x3008:                             # sub
            r[n] = (r[n] - r[m]) & 0xFFFFFFFF
        elif op & 0xF00F == 0x000E:                             # mov.l @(R0,Rm),Rn
            r[n] = self.u32(r[0] + r[m])
        elif op & 0xF00F == 0x0007:                             # mul.l
            self.macl = r[n] * r[m] & 0xFFFFFFFF
        elif op & 0xF00F == 0x3005:                             # dmulu.l
            product = r[n] * r[m]
            self.macl, self.mach = product & 0xFFFFFFFF, product >> 32
        elif op & 0xF0FF == 0x001A:                             # sts macl,Rn
            r[n] = self.macl
        elif op & 0xF0FF == 0x000A:                             # sts mach,Rn
            r[n] = self.mach
        elif op & 0xF00F == 0x200B:                             # or
            r[n] |= r[m]
        elif op & 0xF0FF == 0x0029:                             # movt
            r[n] = self.t
        elif op & 0xF0FF == 0x4011:                             # cmp/pz
            self.t = int(r[n] < 0x80000000)
        elif op & 0xF0FF == 0x4000:                             # shll
            r[n] = r[n] << 1 & 0xFFFFFFFF
        elif op & 0xF0FF == 0x4009:                             # shlr2
            r[n] = r[n] >> 2
        elif op & 0xF0FF == 0x4018:                             # shll8
            r[n] = r[n] << 8 & 0xFFFFFFFF
        elif op & 0xF0FF == 0x4029:                             # shlr16
            r[n] = r[n] >> 16
        elif op & 0xFF00 == 0xC800:                             # tst #imm,r0
            self.t = int(r[0] & imm == 0)
        else:
            return super().step(op, pc)
        return pc + 2


class LinkCpu(ScratchCpu):
    """The scratch-memory interpreter plus what the Link routines add, with
    the firmware functions they call replaced by recorders."""
    WINDOWS = ((TICK, 0x3800, 4), (TCNT4, 0x3810, 8), (DEVICE, 0x3820, 1),
               (CEP, 0x3830, 4), (POOL, 0x3840, 4))

    def __init__(self, image):
        Sh4.__init__(self, image, 0)
        self.macl = self.mach = 0
        self.fpul, self.fr = 0, [0.0] * 16
        self.tick_reads = [1000]
        self.ip, self.block_result, self.getter_result = IP, 0, 0
        self.sent, self.datagrams, self.freed = [], [], []
        self.poke(DEVICE, 1, PLAYER)
        self.poke(CEP, 4, CEP_ID)
        self.poke(POOL, 4, POOL_ID)
        self.poke(TCNT4, 4, 10416)

    def u32(self, a):
        if a == TICK:
            value = self.tick_reads[0]
            if len(self.tick_reads) > 1:
                self.tick_reads.pop(0)
            return value
        return super().u32(a)

    def step(self, op, pc):
        r, fr = self.r, self.fr
        n, m = op >> 8 & 15, op >> 4 & 15
        imm = op & 0xFF
        if op >> 12 == 0xB:                                     # bsr
            disp = op & 0xFFF
            disp = disp - 0x1000 if disp & 0x800 else disp
            self.delay(pc)
            self.pr = pc + 4
            return pc + 4 + disp * 2
        if op >> 12 == 0xF:
            self.float_op(op, n, m)
        elif op & 0xF00F == 0x6008:                             # swap.b
            r[n] = r[m] & 0xFFFF0000 | (r[m] & 0xFF) << 8 | (r[m] >> 8 & 0xFF)
        elif op & 0xF00F == 0x6009:                             # swap.w
            r[n] = (r[m] << 16 | r[m] >> 16) & 0xFFFFFFFF
        elif op & 0xF0FF == 0x4019:                             # shlr8
            r[n] >>= 8
        elif op & 0xF00F == 0x2004:                             # mov.b Rm,@-Rn
            r[n] -= 1
            self.mem[self.loc(r[n])] = r[m] & 0xFF
        elif op & 0xFF00 == 0x8400:                             # mov.b @(disp,Rm),r0
            r[0] = self.s8(r[m] + (op & 15))
        elif op & 0xF00F == 0x000D:                             # mov.w @(R0,Rm),Rn
            v = struct.unpack_from('<H', self.mem, self.loc(r[0] + r[m]))[0]
            r[n] = (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF
        elif op & 0xFF00 == 0x8800:                             # cmp/eq #imm,r0
            self.t = int(r[0] == (imm - 256 if imm & 0x80 else imm) & 0xFFFFFFFF)
        elif op & 0xF0FF == 0x4015:                             # cmp/pl
            self.t = int(0 < r[n] < 0x80000000)
        elif op & 0xFF00 == 0xC900:                             # and #imm,r0
            r[0] &= imm
        elif op == 0x0008:                                      # clrt
            self.t = 0
        elif op & 0xF00F == 0x300E:                             # addc
            total = r[n] + r[m] + self.t
            r[n], self.t = total & 0xFFFFFFFF, total >> 32
        elif op & 0xF0FF == 0x405A:                             # lds Rm,fpul
            self.fpul = r[n]
        elif op & 0xF0FF == 0x005A:                             # sts fpul,Rn
            r[n] = self.fpul & 0xFFFFFFFF
        else:
            return super().step(op, pc)
        return pc + 2

    def float_op(self, op, n, m):
        fr = self.fr
        if op & 0xF0FF == 0xF02D:                               # float FPUL,FRn
            signed = self.fpul - (1 << 32) if self.fpul & 0x80000000 else self.fpul
            fr[n] = f32(signed)
        elif op & 0xF0FF == 0xF00D:                             # fsts FPUL,FRn
            fr[n] = struct.unpack('<f', struct.pack('<I', self.fpul))[0]
        elif op & 0xF0FF == 0xF03D:                             # ftrc FRm,FPUL
            self.fpul = int(fr[n]) & 0xFFFFFFFF
        elif op & 0xF00F == 0xF002:                             # fmul
            fr[n] = f32(fr[n] * fr[m])
        elif op & 0xF00F == 0xF003:                             # fdiv
            fr[n] = f32(fr[n] / fr[m])
        elif op & 0xF00F == 0xF000:                             # fadd
            fr[n] = f32(fr[n] + fr[m])
        else:
            raise AssertionError('unmodelled float opcode %#06x' % op)

    def call(self, target, resume):
        r = self.r
        if target not in (WRAPPER, GETTER, GET_BLOCK, REL_BLOCK, UDP_SND):
            self.pr = resume
            return target
        result = 0
        if target == WRAPPER:
            length = self.u32(r[5])
            self.sent.append(dict(desc=bytes(self.mem[r[5]:r[5] + 20]),
                                  payload=bytes(self.mem[r[4]:r[4] + length])))
        elif target == GETTER:
            assert r[4] == 0
            self.put32(r[5] + 4, self.ip)
            self.put32(r[5] + 8, 0xFFFFFF00)
            result = self.getter_result
        elif target == GET_BLOCK:
            assert r[4] == POOL_ID and r[6] == 0xFFFFFFFF
            self.put32(r[5], BLOCK)
            result = self.block_result
        elif target == REL_BLOCK:
            self.freed.append((r[4], r[5]))
        else:
            self.datagrams.append(dict(cep=r[4], to=bytes(self.mem[r[5]:r[5] + 8]),
                                       payload=bytes(self.mem[r[6]:r[6] + r[7]]),
                                       timeout=self.u32(r[15])))
        for k in range(8):
            r[k] = 0xDEAD0000 + k
        r[0] = result & 0xFFFFFFFF
        return resume


def ghost(ticks, counter=10416, underflow=0):
    """Ghost microseconds for a tick word and a TCNT4 value, from the timer's
    own definition: a tick is 10417 counts of 1 ms."""
    return (ticks + underflow) * 1000 + (10416 - counter) * 1000 // 10417 + GHOST_OFFSET


def deck_registers(cpu):
    saved = [0xA0000000 + k for k in range(16)]
    saved[10], saved[12] = 0xFFE7FFFF, 0xFFE7FFFF
    cpu.r[8:15] = saved[8:15]
    return saved


# -- the ALIVE, at the beat send -------------------------------------------------

BEAT_SPAN = BEAT_OFF + 4


def beat(names=('abletonlink',), bpm=12800, pitch=0x100000, in_bar=3, cpu=None, **deck):
    """Run the patched beat send, a beat packet of these fields in the
    frame, and return the cpu. Reuse `cpu` for the next beat of one boot."""
    if cpu is None:
        cpu = LinkCpu(patched(list(names)))
        cpu.poke(TCNT4 + 4, 2, 0x20)
    for key, value in deck.items():
        setattr(cpu, key, value)
    packet = 0x4000
    cpu.mem[packet:packet + 0x60] = bytes(0x60)
    fields = {0x54: struct.pack('>I', pitch), 0x5A: struct.pack('>H', bpm),
              0x5C: bytes([in_bar]), 0x5F: bytes([PLAYER])}
    for off, value in fields.items():
        cpu.mem[packet + off:packet + off + len(value)] = value
    cpu.put32(FRAME + 0x218, packet)
    cpu.mem[FRAME + 0x150:FRAME + 0x164] = struct.pack('<IIIII', 0x60, 1, 0, 0x60, 0)
    cpu.sent.clear()
    saved = deck_registers(cpu)
    cpu.r[0], cpu.r[4], cpu.r[5], cpu.r[15] = 0x160, packet, FRAME, FRAME
    cpu.pr = Sh4.RET
    cpu.run(BEAT_SPAN, stop=BEAT_SPAN + 18)
    assert cpu.r[15] == FRAME
    saved[12] = WRAPPER
    assert cpu.r[8:15] == saved[8:15]
    return cpu


def us_per_beat(bpm, pitch):
    return 6e9 * 0x100000 / (bpm * pitch)


def test_the_alive_is_the_link_wire_format_byte_for_byte():
    cpu = beat(bpm=12800, pitch=0x10A3D7, in_bar=3)
    stock, alive = cpu.sent
    assert len(stock['payload']) == 0x60
    tempo = struct.unpack_from('>q', alive['payload'], 28)[0]
    assert abs(tempo - us_per_beat(12800, 0x10A3D7)) <= 1
    want = alive_message(tempo, 2 * 1000000, ghost(1000))
    assert alive['payload'] == want and len(want) == 107
    length, mode, ip, tag, port = struct.unpack('<IIIII', alive['desc'])
    assert (length, mode, ip, tag, port) == (107, 1, 0, 0x60, 20808)


def test_the_tempo_is_the_effective_tempo_to_the_microsecond():
    for bpm, pitch in ((12800, 0x100000), (12800, 0x10A3D7), (7450, 0xF0000),
                       (17400, 0x120000), (2000, 0x133333), (30000, 0x1A0000)):
        tempo = struct.unpack_from('>q', beat(bpm=bpm, pitch=pitch).sent[1]['payload'], 28)[0]
        assert abs(tempo - us_per_beat(bpm, pitch)) <= 1, (bpm, pitch)
    assert struct.unpack_from('>q', beat(bpm=12800).sent[1]['payload'], 28)[0] == 468750


def test_the_beat_origin_grows_and_follows_the_bar_phase():
    cpu = beat(in_bar=3)
    origins = [struct.unpack_from('>q', cpu.sent[1]['payload'], 36)[0]]
    for in_bar in (4, 1, 2, 3, 3, 1):
        cpu = beat(in_bar=in_bar, cpu=cpu)
        origins.append(struct.unpack_from('>q', cpu.sent[1]['payload'], 36)[0])
    assert origins == [n * 1000000 for n in (2, 3, 4, 5, 6, 10, 12)]
    assert all(b > a for a, b in zip(origins, origins[1:]))
    assert all(o // 1000000 % 4 == bar - 1 for o, bar in zip(origins, (3, 4, 1, 2, 3, 3, 1)))


def test_the_ghost_time_is_the_tick_clock_plus_the_founder_offset():
    for ticks, counter, tcr, underflow in ((1000, 10416, 0x20, 0), (51234, 5200, 0x20, 0),
                                           (7, 0, 0x20, 0), (900, 4000, 0x120, 1)):
        cpu = LinkCpu(patched(['abletonlink']))
        cpu.poke(TCNT4, 4, counter)
        cpu.poke(TCNT4 + 4, 2, tcr)
        cpu.tick_reads = [ticks]
        stamp = struct.unpack_from('>q', beat(cpu=cpu).sent[1]['payload'], 44)[0]
        assert abs(stamp - ghost(ticks, counter, underflow)) <= 1
        ideal = ((ticks + underflow) * 1000 + (10416 - counter) * 1000 / 10417) + GHOST_OFFSET
        assert abs(stamp - ideal) <= 1.5


def test_a_tick_that_moves_during_the_read_is_read_again():
    cpu = LinkCpu(patched(['abletonlink']))
    cpu.poke(TCNT4 + 4, 2, 0x20)
    cpu.tick_reads = [1000, 1001]
    stamp = struct.unpack_from('>q', beat(cpu=cpu).sent[1]['payload'], 44)[0]
    assert stamp == ghost(1001)


def test_the_node_and_session_ids_are_the_decks_address_nxs_and_player():
    alive = beat().sent[1]['payload']
    assert alive[12:20] == NODE_ID == alive[60:68]
    assert alive[101:105] == bytes([192, 168, 1, 20]) and alive[105:107] == b'\xC3\x50'


def test_nothing_is_sent_without_a_bpm_a_pitch_or_an_address():
    for deck in (dict(bpm=0), dict(pitch=0), dict(ip=0), dict(getter_result=0xFFFFFFEF)):
        cpu = beat(**deck)
        assert len(cpu.sent) == 1, deck
    cpu = beat(bpm=0)
    cpu = beat(cpu=cpu)
    assert struct.unpack_from('>q', cpu.sent[1]['payload'], 36)[0] == 2000000


def test_the_stock_send_and_its_descriptor_are_left_as_they_were():
    cpu = beat()
    stock = cpu.sent[0]
    assert struct.unpack('<IIIII', stock['desc']) == (0x60, 1, 0, 0x60, 0)
    assert struct.unpack_from('<IIIII', cpu.mem, FRAME + 0x150) == (0x60, 1, 0, 0x60, 0)
    assert cpu.r[12] == WRAPPER and cpu.r[10] == 0xFFE7FFFF


def test_stacks_with_osc_beat_on_the_same_site_in_registry_order():
    cpu = beat(names=('abletonlink', 'oscbeat'))
    stock, alive, osc = cpu.sent
    assert len(stock['payload']) == 0x60
    assert alive['payload'][:8] == b'_asdp_v\x01' and len(alive['payload']) == 107
    assert osc['payload'].startswith(b'/cdj/beat')
    assert struct.unpack('<IIIII', osc['desc'])[4] == 50010
    assert struct.unpack('<IIIII', alive['desc'])[4] == 20808


def test_the_site_resumes_with_the_stock_registers_after_every_mod():
    cpu = beat(names=ALL)
    assert [len(s['payload']) for s in cpu.sent][:2] == [0x60, 107]
    assert cpu.r[12] == WRAPPER and cpu.r[15] == FRAME
    assert cpu.r[8:12] == [0xA0000008, 0xA0000009, 0xFFE7FFFF, 0xA000000B]
    assert cpu.r[13:15] == [0xA000000D, 0xA000000E]


# -- the PONG, at the Pro DJ Link receive ----------------------------------------

RECV_SPAN = RECV_OFF + 6
BUFFER = FRAME + 20


def receive(datagram, names=('abletonlinkpong',), cpu=None, ticks=2000, **deck):
    """Run the patched receive site for a datagram that udp_rcv_dat has just
    put in the task's buffer, and return the cpu."""
    if cpu is None:
        cpu = LinkCpu(patched(list(names)))
        cpu.poke(TCNT4 + 4, 2, 0x20)
    for key, value in deck.items():
        setattr(cpu, key, value)
    cpu.tick_reads = [ticks]
    cpu.mem[BUFFER:BUFFER + 128] = bytes(128)
    cpu.mem[BUFFER:BUFFER + len(datagram)] = datagram
    cpu.mem[FRAME + 12:FRAME + 20] = struct.pack('<IHxx', *SENDER)
    cpu.datagrams.clear()
    cpu.freed.clear()
    saved = deck_registers(cpu)
    cpu.r[10], cpu.r[12], cpu.r[14] = BUFFER, MASK, len(datagram)
    saved[10], saved[12], saved[14] = BUFFER, MASK, len(datagram)
    cpu.r[15] = FRAME
    cpu.pr = Sh4.RET
    cpu.run(RECV_SPAN, stop=RECV_SPAN + 14)
    assert cpu.r[15] == FRAME
    assert cpu.r[8:15] == saved[8:15]
    return cpu


def test_a_ping_is_answered_with_the_link_pong_from_the_pro_dj_link_socket():
    message = ping(123456789, 9876543210)
    assert len(message) == 41
    cpu = receive(message)
    (sent,) = cpu.datagrams
    assert sent['payload'] == pong(message, ghost(2000)) and len(sent['payload']) == 73
    assert sent['cep'] == CEP_ID and sent['timeout'] == 0xFFFFFFFF
    assert sent['to'] == struct.pack('<IHxx', *SENDER)


def test_a_ping_without_the_previous_ghost_time_is_answered_too():
    message = ping(55)
    (sent,) = receive(message).datagrams
    assert sent['payload'] == pong(message, ghost(2000)) and len(sent['payload']) == 57


def test_the_datagram_never_reaches_the_pro_dj_link_layer():
    cpu = receive(ping())
    assert cpu.freed == [(POOL_ID, BLOCK)]
    assert cpu.r[0] == 1


def test_the_answer_uses_a_fresh_ghost_time_each_time():
    cpu = receive(ping(), ticks=2000)
    first = cpu.datagrams[0]['payload']
    second = receive(ping(), cpu=cpu, ticks=2050).datagrams[0]['payload']
    assert struct.unpack_from('>q', second, 33)[0] - struct.unpack_from('>q', first, 33)[0] == 50000


def test_the_longest_ping_that_fits_is_answered_and_a_longer_one_is_dropped():
    longest = ping() + bytes(96 - len(ping()))
    assert len(longest) == 96
    (sent,) = receive(longest).datagrams
    assert sent['payload'] == pong(longest, ghost(2000)) and len(sent['payload']) == 128
    too_long = receive(longest + b'\0')
    assert too_long.datagrams == [] and too_long.freed == [(POOL_ID, BLOCK)]


def test_link_datagrams_that_are_not_pings_are_swallowed_unanswered():
    for message in (b'_link_v\x01\x01', b'_link_v\x01\x02' + entry(b'sess', NODE_ID),
                    b'_link_v\x01\x03' + bytes(20)):
        cpu = receive(message)
        assert cpu.datagrams == [] and cpu.freed == [(POOL_ID, BLOCK)] and cpu.r[0] == 1


def test_pro_dj_link_traffic_passes_through_untouched():
    for datagram in (b'Qspt1WmJOL\x00' + bytes(40), b'_asdp_v\x01\x01' + bytes(30),
                     b'_link_w\x01\x01' + bytes(30)):
        cpu = receive(datagram)
        assert cpu.datagrams == [] and cpu.freed == [] and cpu.r[0] == 0
        assert bytes(cpu.mem[BUFFER:BUFFER + len(datagram)]) == datagram


def test_a_ping_is_answered_even_when_the_block_pool_is_empty():
    cpu = receive(ping(), block_result=0xFFFFFFFF)
    assert len(cpu.datagrams) == 1 and cpu.freed == [] and cpu.r[0] == 0xFFFFFFFF


def test_a_ping_is_swallowed_unanswered_when_the_deck_has_no_address():
    cpu = receive(ping(), ip=0)
    assert cpu.datagrams == [] and cpu.freed == [(POOL_ID, BLOCK)] and cpu.r[0] == 1


def test_both_link_mods_and_the_other_mods_run_on_their_own_sites():
    names = ALL
    assert len(receive(ping(), names=names).datagrams) == 1
    assert len(beat(names=names).sent) == 3
