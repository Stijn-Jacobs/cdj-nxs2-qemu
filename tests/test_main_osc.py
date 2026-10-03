# SPDX-License-Identifier: GPL-2.0-or-later
"""The osc mod (main_osc.s on the status serialiser call, main_oscbeat.s on the
beat send): both found by signature, hooked on the shared trampoline, and --
through a small SH-4 interpreter covering only the instructions the trampoline
and the two routines use -- sending the exact OSC datagrams: per-beat messages
built from the beat packet, and the state messages on change and by refresh."""
import os
import struct

import pytest

import patch_main
import sigpatch

WRAPPER = 0x085113EC
PORT = 50010
MASK = 0xFFE7FFFF
STATUS_OFF, BEAT_OFF = 0x100, 0x300
CAVE = (0x400, 0x2E00)
SERIALISER = 0x2F00
FRAME = 0xF000
# MAIN's fixed addresses the routines read, moved into the interpreter's memory
FIXED = {0x0AB84CFA: 0xE000, 0x0A35F39C: 0xE010, 0x0B0C5258: 0xE020}
DEVICE, MASTER, TICK, RECORD = 0xE000, 0xE010, 0xE020, 0xE100
TRACK_ID = RECORD + 0x2C


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


class OscCpu(Sh4):
    """Sh4 plus the instructions the two routines add."""

    def step(self, op, pc):
        r = self.r
        n, m = op >> 8 & 15, op >> 4 & 15
        imm = op & 0xFF
        simm = imm - 256 if imm & 0x80 else imm
        if op & 0xF00F == 0x3006:                               # cmp/hi
            self.t = int(r[n] > r[m])
        elif op & 0xF0FF == 0x4011:                             # cmp/pz
            self.t = int(r[n] < 0x80000000)
        elif op & 0xFF00 == 0x8F00:                             # bf/s
            if not self.t:
                self.delay(pc)
                return pc + 4 + simm * 2
        elif op & 0xF00F == 0x600D:                             # extu.w
            r[n] = r[m] & 0xFFFF
        elif op & 0xF00F == 0x3002:                             # cmp/hs
            self.t = int(r[n] >= r[m])
        elif op & 0xF00F == 0x3000:                             # cmp/eq
            self.t = int(r[n] == r[m])
        elif op & 0xFF00 == 0x8800:                             # cmp/eq #imm,r0
            self.t = int(r[0] == simm & 0xFFFFFFFF)
        elif op & 0xFF00 == 0x8900:                             # bt
            if self.t:
                return pc + 4 + simm * 2
        elif op & 0xF00F == 0x3008:                             # sub
            r[n] = (r[n] - r[m]) & 0xFFFFFFFF
        elif op & 0xF00F == 0x2008:                             # tst
            self.t = int(r[n] & r[m] == 0)
        elif op & 0xF0FF == 0x0029:                             # movt
            r[n] = self.t
        elif op & 0xFF00 == 0xCA00:                             # xor #imm,r0
            r[0] ^= imm
        elif op & 0xF00F == 0x600C:                             # extu.b
            r[n] = r[m] & 0xFF
        elif op & 0xF0FF == 0x4000:                             # shll
            r[n] = r[n] << 1 & 0xFFFFFFFF
        elif op & 0xF0FF == 0x4009:                             # shlr2
            r[n] >>= 2
        elif op & 0xF00F == 0x6008:                             # swap.b
            v = r[m]
            r[n] = v & 0xFFFF0000 | (v & 0xFF) << 8 | v >> 8 & 0xFF
        elif op & 0xF00F == 0x6009:                             # swap.w
            r[n] = (r[m] << 16 | r[m] >> 16) & 0xFFFFFFFF
        elif op & 0xF00F == 0x6000:                             # mov.b @Rm,Rn
            r[n] = self.s8(r[m])
        elif op & 0xF00F == 0x2000:                             # mov.b Rm,@Rn
            self.mem[r[n]] = r[m] & 0xFF
        elif op & 0xF00F == 0x0004:                             # mov.b Rm,@(R0,Rn)
            self.mem[r[0] + r[n]] = r[m] & 0xFF
        elif op & 0xE000 == 0xA000:                             # bra, bsr
            disp = (op & 0xFFF) - (0x1000 if op & 0x800 else 0)
            self.delay(pc)
            if op & 0x1000:
                self.pr = pc + 4
            return pc + 4 + disp * 2
        else:
            return super().step(op, pc)
        return pc + 2

    def call(self, target, resume):
        if target == SERIALISER:
            self.pr = resume
            return target
        return super().call(target, resume)


def build_image():
    """A synthetic MAIN image at address 0: both real signatures, the literals
    their spans load (the serialiser's and the send wrapper's address) and an
    erased cave for every registered routine."""
    d = bytearray(0x3000)
    for off, site in ((STATUS_OFF, patch_main.STATUS_SEND), (BEAT_OFF, patch_main.BEAT_SEND)):
        for k, op in enumerate(site['sig']):
            struct.pack_into('<H', d, off + 2 * k, op)
    struct.pack_into('<I', d, ((STATUS_OFF + 8) & ~3) + 4 + 0x3E * 4, SERIALISER)
    struct.pack_into('<I', d, ((BEAT_OFF + 4) & ~3) + 4 + 46 * 4, WRAPPER)
    struct.pack_into('<HH', d, SERIALISER, 0x000B, 0x0009)
    d[CAVE[0]:CAVE[1]] = b'\xff' * (CAVE[1] - CAVE[0])
    profile = sigpatch.ImageProfile(flash_base=0, loadtab_off=0, loadtab_n=0,
                                    ram_base=CAVE[0], ram_end=CAVE[1], byteorder='<')
    return profile, bytes(d)


def patched(names):
    profile, data = build_image()
    out = bytearray(sigpatch.patch(profile, patch_main.blob_path, data, patch_main.MODS, names))
    for real, low in FIXED.items():
        pat = struct.pack('<I', real)
        at = out.find(pat, CAVE[0])
        while at >= 0:
            out[at:at + 4] = struct.pack('<I', low)
            at = out.find(pat, at + 4)
    return bytes(out)


def test_registered_for_main_as_hooks_on_the_status_call_and_the_beat_send():
    for name, site in (('osc', patch_main.STATUS_SEND), ('oscbeat', patch_main.BEAT_SEND)):
        mod = patch_main.MODS[name]
        assert mod.target == 'main' and mod.hook and not mod.literal
        assert dict(sig=mod.sig, start=mod.start, end=mod.end) == site


def test_the_registry_row_enables_both_mods_and_defaults_off():
    path = os.path.join(os.path.dirname(patch_main.__file__), 'mods.conf')
    rows = [line.split('|') for line in open(path, encoding='utf-8')
            if line.strip() and not line.startswith('#')]
    (row,) = [r for r in rows if r[0] == 'osc']
    assert row[1].split('+') == ['CDJ_MAIN_OSC', 'CDJ_MAIN_OSCBEAT']
    assert row[2:5] == ['1', '0', 'off']
    assert not [r for r in rows if r[0] == 'osc_beat']


REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')


@pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
def test_real_main_has_each_signature_exactly_once_and_patches_in_any_order():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    img = sigpatch.Image(patch_main.MAIN_PROFILE, data)
    for name, span in (('osc', 0x084B4540), ('oscbeat', 0x082DB248)):
        mod = patch_main.MODS[name]
        assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == span
    names = list(patch_main.MODS)
    assert patch_main.patch(data, names) == patch_main.patch(data, names[::-1])


def parse(payload):
    """An independent OSC decoder: (address, type tags, arguments)."""
    def string(at):
        end = payload.index(b'\0', at)
        return payload[at:end].decode(), (end + 4) & ~3
    address, at = string(0)
    tags, at = string(at)
    assert tags[0] == ',' and len(payload) == at + 4 * (len(tags) - 1)
    return address, tags, struct.unpack('>%di' % (len(tags) - 1), payload[at:])


def check_descriptor(send):
    length, mode, ip, tag, port = struct.unpack('<IIIII', send['desc'])
    assert (length, mode, ip, tag, port) == (len(send['payload']), 1, 0, 0x60, PORT)
    assert len(send['payload']) % 4 == 0


# -- the beat half -----------------------------------------------------------------

def run_beat(names=('oscbeat',), in_bar=3, player=2, tick=123456):
    cpu = OscCpu(patched(list(names)), 0)
    packet = 0x4000
    fields = {0x24: struct.pack('>I', 293), 0x2C: struct.pack('>I', 1171),
              0x54: struct.pack('>I', 0x100000), 0x5A: struct.pack('>H', 20486),
              0x5C: bytes([in_bar]), 0x5F: bytes([player])}
    for off, value in fields.items():
        cpu.mem[packet + off:packet + off + len(value)] = value
    cpu.put32(FRAME + 0x218, packet)
    cpu.mem[FRAME + 0x150:FRAME + 0x164] = struct.pack('<IIIII', 0x60, 1, 0, 0x60, 0)
    cpu.put32(TICK, tick)
    saved = [0xA0000000 + k for k in range(16)]
    saved[10], saved[12] = MASK, WRAPPER
    cpu.r[8:15] = saved[8:15]
    cpu.r[0], cpu.r[4], cpu.r[5], cpu.r[15] = 0x160, packet, FRAME, FRAME
    cpu.run(BEAT_OFF + 4, stop=BEAT_OFF + 22)
    assert cpu.r[15] == FRAME and cpu.r[8:15] == saved[8:15]
    assert struct.unpack_from('<IIIII', cpu.mem, FRAME + 0x150) == (0x60, 1, 0, 0x60, 0)
    return cpu


def test_a_beat_sends_the_stock_packet_then_beat_and_timing():
    cpu = run_beat()
    stock, beat, timing = cpu.sends
    assert len(stock['payload']) == 0x60
    assert beat['payload'] == b'/cdj/2/beat\0,i\0\0' + struct.pack('>i', 3)
    assert timing['payload'] == (b'/cdj/2/timing\0\0\0,iiiiii\0'
                                 + struct.pack('>6i', 3, 20486, 0x100000, 293, 1171, 123456))
    assert parse(timing['payload'])[:2] == ('/cdj/2/timing', ',iiiiii')
    for send in (beat, timing):
        check_descriptor(send)


def test_the_bar_message_is_sent_on_beat_one_only():
    cpu = run_beat(in_bar=1)
    assert [parse(s['payload'])[0] for s in cpu.sends[1:]] == ['/cdj/2/beat', '/cdj/2/bar', '/cdj/2/timing']
    assert parse(cpu.sends[2]['payload']) == ('/cdj/2/bar', ',i', (1,))
    check_descriptor(cpu.sends[2])


@pytest.mark.parametrize('player', [0, 5])
def test_nothing_is_sent_for_a_player_outside_one_to_four(player):
    assert len(run_beat(player=player).sends) == 1


# -- the state half ----------------------------------------------------------------

class Deck:
    """The Sentinel task's call site on a patched image, run pass by pass."""

    def __init__(self, player=1):
        self.cpu = OscCpu(patched(['osc']), 0)
        self.cpu.mem[DEVICE] = player
        self.cpu.r[11], self.cpu.r[12] = RECORD, MASK
        self.set(flags=0, code=0, master=0, tick=0, track=0)

    def set(self, flags=None, code=None, master=None, tick=None, track=None):
        for addr, value in ((RECORD + 0x89, flags), (RECORD + 0x78, code), (MASTER, master)):
            if value is not None:
                self.cpu.mem[addr] = value
        if tick is not None:
            self.cpu.put32(TICK, tick)
        if track is not None:
            self.cpu.put32(TRACK_ID, track)

    def sent(self):
        self.cpu.sends.clear()
        saved = [0xA0000000 + k for k in range(16)]
        self.cpu.r[8:11], self.cpu.r[13:15] = saved[8:11], saved[13:15]
        self.cpu.r[0], self.cpu.r[15] = 0x1F8, FRAME
        self.cpu.run(STATUS_OFF + 4, stop=STATUS_OFF + 18)
        assert self.cpu.r[8:11] == saved[8:11] and self.cpu.r[13:15] == saved[13:15]
        assert self.cpu.r[11:13] == [RECORD, MASK] and self.cpu.r[15] == FRAME
        for send in self.cpu.sends:
            check_descriptor(send)
        return [parse(s['payload']) for s in self.cpu.sends]

    def drain(self):
        while self.sent():
            pass


def msg(name, value, player=1):
    return ('/cdj/%d/%s' % (player, name), ',i', (value,))


def test_the_first_passes_send_every_value_two_at_a_time_with_the_exact_bytes():
    deck = Deck(player=3)
    deck.set(flags=0xF8, code=3, master=2, track=4)
    assert deck.sent() == [msg('playing', 1, 3), msg('state', 3, 3)]
    assert deck.cpu.sends[0]['payload'] == b'/cdj/3/playing\0\0,i\0\0' + struct.pack('>i', 1)
    assert deck.cpu.sends[1]['payload'] == b'/cdj/3/state\0\0\0\0,i\0\0' + struct.pack('>i', 3)
    assert deck.sent() == [msg('loaded', 1, 3), msg('end', 0, 3)]
    assert deck.sent() == [msg('master', 1, 3), msg('sync', 1, 3)]
    assert deck.sent() == [msg('masterdeck', 2, 3), msg('track', 4, 3)]
    assert deck.cpu.sends[0]['payload'] == b'/cdj/3/masterdeck\0\0\0,i\0\0' + struct.pack('>i', 2)
    assert deck.sent() == []


def test_one_changed_value_is_sent_once_and_only_that_address():
    deck = Deck()
    deck.drain()
    deck.set(code=0x11)
    assert deck.sent() == [msg('state', 0x11), msg('loaded', 1)]
    assert deck.sent() == [msg('end', 1)]
    assert deck.sent() == []


def test_a_burst_of_changes_drains_two_per_pass_with_the_final_values():
    deck = Deck()
    deck.drain()
    deck.set(flags=0x30, code=3, master=4, track=9)
    first = deck.sent()
    assert len(first) == 2
    got = dict((a, v[0]) for a, _, v in first)
    for _ in range(3):
        got.update((a, v[0]) for a, _, v in deck.sent())
    assert got == {'/cdj/1/state': 3, '/cdj/1/loaded': 1,
                   '/cdj/1/master': 1, '/cdj/1/sync': 1, '/cdj/1/masterdeck': 4,
                   '/cdj/1/track': 9}


def test_refresh_sends_one_value_per_250_ticks_and_covers_every_row_once_per_rotation():
    deck = Deck()
    deck.drain()
    deck.set(tick=100)
    assert deck.sent() == []
    seen = []
    for k in range(8):
        deck.set(tick=250 * (k + 1))
        (one,) = deck.sent()
        seen.append(one[0])
        assert deck.sent() == []
    assert seen == ['/cdj/1/' + n for n in
                    ('playing', 'state', 'loaded', 'end', 'master', 'sync', 'masterdeck', 'track')]
    deck.set(tick=250 * 9)
    assert deck.sent()[0][0] == '/cdj/1/playing'


def test_the_refresh_continues_across_the_tick_wrapping():
    deck = Deck()
    deck.set(tick=0xFFFFFF00)
    deck.drain()
    deck.set(tick=(0xFFFFFF00 + 250) & 0xFFFFFFFF)
    assert len(deck.sent()) == 1


@pytest.mark.parametrize('player', [0, 5])
def test_nothing_is_sent_on_the_status_pass_for_a_player_outside_one_to_four(player):
    assert Deck(player=player).sent() == []


# -- the edge events ---------------------------------------------------------------

def play_deck(player=1):
    """A deck whose first pass has recorded a track-less, stopped record."""
    deck = Deck(player=player)
    deck.set(tick=0)
    deck.drain()
    return deck


def set_bpm(deck, word):
    deck.cpu.put32(RECORD + 0x90, word)


def test_the_first_pass_records_without_sending_an_event():
    deck = Deck()
    deck.set(code=3, flags=0x40)
    assert [a for a, _, _ in deck.sent()] == ['/cdj/1/playing', '/cdj/1/state']


def test_load_sends_the_rekordbox_id_once_with_the_exact_bytes():
    deck = play_deck()
    deck.set(code=2, track=0x01020304)
    first = deck.sent()
    assert first[0] == msg('load', 0x01020304)
    assert deck.cpu.sends[0]['payload'] == b'/cdj/1/load\0,i\0\0' + struct.pack('>i', 0x01020304)
    assert 'load' not in ' '.join(a for a, _, _ in deck.sent())


def test_play_carries_the_bpm_and_stop_zero():
    deck = play_deck(player=3)
    set_bpm(deck, 0x80000000 | 12850)
    deck.set(code=3, flags=0x40)
    assert deck.sent()[0] == msg('play', 12850, 3)
    assert deck.cpu.sends[0]['payload'] == b'/cdj/3/play\0,i\0\0' + struct.pack('>i', 12850)
    deck.drain()
    deck.set(flags=0)
    assert deck.sent()[0] == msg('stop', 0, 3)
    assert deck.cpu.sends[0]['payload'] == b'/cdj/3/stop\0,i\0\0' + struct.pack('>i', 0)


def test_a_bpm_word_without_bit_31_is_sent_as_zero():
    deck = play_deck()
    set_bpm(deck, 0x7FFFFFFF)
    deck.set(flags=0x40)
    assert deck.sent()[0] == msg('play', 0)


@pytest.mark.parametrize('code, sent', [(6, True), (7, True), (5, False), (8, False)])
def test_cue_is_sent_for_the_two_cue_codes_only(code, sent):
    deck = play_deck()
    deck.set(code=code)
    events = [m for m in deck.sent() if m[0] == '/cdj/1/cue']
    assert events == ([msg('cue', code)] if sent else [])


def test_loop_is_on_when_the_code_becomes_4_and_off_when_it_leaves():
    deck = play_deck()
    deck.set(code=3)
    deck.drain()
    deck.set(code=4)
    assert msg('loop', 1) in deck.sent()
    deck.drain()
    deck.set(code=3)
    assert msg('loop', 0) in deck.sent()
    deck.drain()
    assert deck.sent() == []


def test_events_go_out_beside_the_two_state_messages_and_only_once():
    deck = play_deck()
    set_bpm(deck, 0x80000000 | 12800)
    deck.set(code=3, flags=0x40)
    first = deck.sent()
    assert [a for a, _, _ in first] == ['/cdj/1/play', '/cdj/1/playing', '/cdj/1/state']
    assert not [a for a, _, _ in deck.sent() if a == '/cdj/1/play']


@pytest.mark.parametrize('player', [0, 5])
def test_no_event_is_sent_for_a_player_outside_one_to_four(player):
    deck = Deck(player=player)
    deck.set(code=2, flags=0x40)
    assert deck.sent() == [] and deck.sent() == []
