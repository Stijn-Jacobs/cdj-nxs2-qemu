# SPDX-License-Identifier: GPL-2.0-or-later
"""The three TCNet MAIN mods: found by signature, hooked on the shared
trampolines (the status serialiser call for Opt-IN and Status, the DJcont
output loop for Time and for the listener socket that answers Metrics and
MetaData requests), stackable with osc on the status site, and -- through
the SH-4 interpreter of test_main_abletonlink extended with the halfword
stores these routines add -- broadcasting the packets TCNet V3.5 defines. The
decoder below is written from the spec's offsets, not from the routines."""
import functools
import os
import struct

import pytest

import patch_main
import sigpatch
from test_main_abletonlink import LinkCpu, IP, TICK, TCNT4, DEVICE, CEP, POOL, GETTER, UDP_SND
from test_main_osc import Sh4, WRAPPER

DJCONT_OFF, STATUS_OFF = 0x40, 0x100
DSP_STEP, SERIALISER = 0x300, 0x310
CAVE = (0x400, 0x6000)
FRAME = 0xF000
MASK = 0xFFE7FFFF

RECORDS = 0x0A371D3C
POSITION = 0x09947488
LENGTH = 0x0B5125E8
MASTER = 0x0A35F39C
TITLE, ARTIST = 0x0994DE40, 0x0994E040
VCRE_CEP, RCV_DAT = 0x08233A22, 0x0823415E
LISTENER, E_TMOUT = 5, -50
PLAYER = 2
NODE_ID = IP & 0xFFFF


def record(player=PLAYER):
    return RECORDS + player * 0x124


def build_image():
    """A synthetic MAIN image at address 0: the DJcont loop and status call
    signatures, the literal each span loads (the DSP-output step and the
    serialiser, both stubbed with rts) and an erased cave."""
    d = bytearray(CAVE[1])
    for off, site in ((DJCONT_OFF, patch_main.DJCONT_PASS), (STATUS_OFF, patch_main.STATUS_SEND)):
        for k, op in enumerate(site['sig']):
            struct.pack_into('<H', d, off + 2 * k, op)
    struct.pack_into('<I', d, ((DJCONT_OFF + 10) & ~3) + 4 + 0x8F * 4, DSP_STEP)
    struct.pack_into('<I', d, ((STATUS_OFF + 8) & ~3) + 4 + 0x3E * 4, SERIALISER)
    for stub in (DSP_STEP, SERIALISER):
        struct.pack_into('<HH', d, stub, 0x000B, 0x0009)
    d[CAVE[0]:CAVE[1]] = b'\xff' * (CAVE[1] - CAVE[0])
    profile = sigpatch.ImageProfile(flash_base=0, loadtab_off=0, loadtab_n=0,
                                    ram_base=CAVE[0], ram_end=CAVE[1], byteorder='<')
    return profile, bytes(d)


@functools.lru_cache(maxsize=None)
def patched_image(names):
    profile, data = build_image()
    return sigpatch.patch(profile, patch_main.blob_path, data, patch_main.MODS, list(names))


class TcnetCpu(LinkCpu):
    """The Link interpreter with the deck's status records, position and
    length words mapped, plus the halfword stores the TCNet routines use."""
    WINDOWS = ((TICK, 0x6000, 4), (TCNT4, 0x6010, 8), (DEVICE, 0x6020, 1),
               (CEP, 0x6030, 4), (POOL, 0x6040, 4), (MASTER, 0x6050, 4),
               (POSITION, 0x6060, 4), (LENGTH, 0x6070, 4), (RECORDS, 0x6100, 5 * 0x124),
               (TITLE, 0x6800, 0x200), (ARTIST, 0x6A00, 0x200))

    def __init__(self, names):
        super().__init__(patched_image(tuple(names)))
        self.poke(DEVICE, 1, PLAYER)
        self.cep_result, self.created, self.inbox = LISTENER, [], []

    def call(self, target, resume):
        """udp_vcre_cep and udp_rcv_dat on top of the Link recorders: the
        socket comes back as cep_result, and each receive takes the next
        (payload, address, port) from the inbox or times out."""
        r = self.r
        if target not in (VCRE_CEP, RCV_DAT):
            return super().call(target, resume)
        if target == VCRE_CEP:
            self.created.append(bytes(self.mem[r[4]:r[4] + 16]))
            result = self.cep_result
        else:
            assert r[4] == LISTENER and self.u32(r[15]) == 0
            if self.inbox:
                payload, address, port = self.inbox.pop(0)
                payload = payload[:r[7]]
                self.mem[r[6]:r[6] + len(payload)] = payload
                self.mem[r[5]:r[5] + 8] = struct.pack('<IHxx', address, port)
                result = len(payload)
            else:
                result = E_TMOUT
        for k in range(8):
            r[k] = 0xDEAD0000 + k
        r[0] = result & 0xFFFFFFFF
        return resume

    def step(self, op, pc):
        r = self.r
        n, m = op >> 8 & 15, op >> 4 & 15
        if op & 0xF00F == 0x2001:                               # mov.w Rm,@Rn
            struct.pack_into('<H', self.mem, self.loc(r[n]), r[m] & 0xFFFF)
        elif op & 0xFF00 == 0x8100:                             # mov.w R0,@(disp,Rn)
            struct.pack_into('<H', self.mem, self.loc(r[m] + (op & 15) * 2), r[0] & 0xFFFF)
        elif op & 0xF00F == 0x6005:                             # mov.w @Rm+,Rn
            v = struct.unpack_from('<H', self.mem, self.loc(r[m]))[0]
            r[n] = (v - 0x10000 if v & 0x8000 else v) & 0xFFFFFFFF
            if n != m:
                r[m] += 2
        elif op & 0xF00F == 0x3003:                             # cmp/ge
            signed = [v - (1 << 32) if v & 0x80000000 else v for v in (r[n], r[m])]
            self.t = int(signed[0] >= signed[1])
        elif op & 0xF0FF == 0x4001:                             # shlr
            self.t, r[n] = r[n] & 1, r[n] >> 1
        else:
            return super().step(op, pc)
        return pc + 2

    def deck(self, code=3, in_bar=1, track=0x1234ABCD, position=61234, seconds=146,
             player=PLAYER, tick=5000, counter=10416, flags=0, pitch=0x100000,
             bpm=0x80000000 | 12800, beat=17):
        self.poke(DEVICE, 1, player)
        at = record(player)
        self.poke(at + 0x78, 1, code)
        self.poke(at + 0x89, 1, flags)
        self.poke(at + 0x8C, 4, pitch)
        self.poke(at + 0x90, 4, bpm)
        self.poke(at + 0xA0, 4, beat)
        self.poke(at + 0xA6, 1, in_bar)
        self.poke(at + 0x2C, 4, track)
        self.poke(POSITION, 4, position)
        self.poke(LENGTH, 4, seconds)
        self.poke(TCNT4, 4, counter)
        self.tick_reads = [tick]

    def site(self, at, end, regs):
        saved = [0xA0000000 + k for k in range(16)]
        saved[8:15] = [regs.get(k, saved[k]) for k in range(8, 15)]
        self.r[:] = saved[:15] + [FRAME]
        self.pr = Sh4.RET
        self.sent.clear()
        self.datagrams.clear()
        self.run(at, stop=end)
        assert self.r[15] == FRAME and self.r[8:15] == saved[8:15]
        return list(self.sent)

    def status_pass(self, player=PLAYER):
        at = STATUS_OFF + 2 * patch_main.STATUS_SEND['start']
        end = STATUS_OFF + 2 * patch_main.STATUS_SEND['end']
        return self.site(at, end, {11: record(player), 12: MASK})

    def djcont_pass(self):
        at = DJCONT_OFF + 2 * patch_main.DJCONT_PASS['start']
        end = DJCONT_OFF + 2 * patch_main.DJCONT_PASS['end']
        self.fpscr = 0x00180001
        sent = self.site(at, end, {13: 0x099470CC, 14: MASK})
        assert self.r[6] == 0x00000001 and self.r[5] == 0x00000001
        return sent


# -- the spec's packets, decoded from its offsets ----------------------------------

def header(payload):
    node, major, minor, magic, kind, name, seq, node_type, options, stamp = \
        struct.unpack_from('<HBB3sB8sBBHI', payload, 0)
    return dict(node=node, version=(major, minor), magic=magic, type=kind,
                name=name, seq=seq, node_type=node_type, options=options, stamp=stamp)


def broadcast(send, port, length):
    size, mode, dest, tag, to = struct.unpack('<IIIII', send['desc'])
    assert (size, mode & 0xFF, dest, tag & 0xFF, to) == (length, 1, 0, 0x60, port)
    assert len(send['payload']) == length
    return send['payload']


def check_header(payload, kind, player=PLAYER):
    h = header(payload)
    assert h['node'] == NODE_ID and h['version'] == (3, 5) and h['magic'] == b'TCN'
    assert h['type'] == kind and h['name'] == b'NXS2-%d\0\0' % player
    assert h['node_type'] == 2 and h['options'] == 0
    return h


def opt_in(payload):
    count, port, uptime = struct.unpack_from('<HHH', payload, 24)
    return dict(count=count, port=port, uptime=uptime, vendor=payload[32:48],
                device=payload[48:64], version=tuple(payload[64:67]))


def status(payload):
    return dict(count=struct.unpack_from('<H', payload, 24)[0],
                port=struct.unpack_from('<H', payload, 26)[0],
                source=list(payload[34:42]), state=list(payload[42:50]),
                track=list(struct.unpack_from('<8I', payload, 50)),
                smpte=payload[83], auto_master=payload[84],
                names=[payload[172 + 16 * k:188 + 16 * k] for k in range(8)],
                rest=payload[28:34] + payload[82:83] + payload[85:172])


def time(payload):
    return dict(time=list(struct.unpack_from('<8I', payload, 24)),
                total=list(struct.unpack_from('<8I', payload, 56)),
                beat=list(payload[88:96]), state=list(payload[96:104]),
                smpte=payload[104:154], on_air=list(payload[154:162]))


def only(layer_value, player=PLAYER):
    values = [0] * 8
    values[player - 1] = layer_value
    return values


# -- registration -------------------------------------------------------------------

def test_registered_for_main_as_hooks_on_the_status_call_and_the_djcont_loop():
    for name, site in (('tcnet', patch_main.STATUS_SEND), ('tcnettime', patch_main.DJCONT_PASS),
                       ('tcnetdata', patch_main.DJCONT_PASS)):
        mod = patch_main.MODS[name]
        assert mod.target == 'main' and mod.hook and not mod.literal
        assert dict(sig=mod.sig, start=mod.start, end=mod.end) == site


def test_the_registry_row_enables_all_three_mods_and_defaults_off():
    path = os.path.join(os.path.dirname(patch_main.__file__), 'mods.conf')
    rows = [line.split('|') for line in open(path, encoding='utf-8')
            if line.strip() and not line.startswith('#')]
    (row,) = [r for r in rows if r[0] == 'tcnet']
    assert row[1].split('+') == ['CDJ_MAIN_TCNET', 'CDJ_MAIN_TCNETTIME', 'CDJ_MAIN_TCNETDATA']
    assert row[2:5] == ['1', '0', 'off']


REAL_MAIN = os.path.join(os.path.dirname(__file__), '..', '..', 'extract', 'main_unpacked.bin')


@pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason='extract/main_unpacked.bin not present')
def test_real_main_has_each_signature_exactly_once_where_the_design_puts_it():
    with open(REAL_MAIN, 'rb') as f:
        data = f.read()
    img = sigpatch.Image(patch_main.MAIN_PROFILE, data)
    for name, span in (('tcnet', 0x084B4540), ('tcnettime', 0x082D872C)):
        mod = patch_main.MODS[name]
        assert img.addr_of(sigpatch.find_sig(img, mod) + 2 * mod.start) == span
    assert patch_main.patch(data, ['tcnet', 'tcnettime', 'osc']) == \
        patch_main.patch(data, ['osc', 'tcnettime', 'tcnet'])


def test_the_assembled_blobs_are_current():
    for name in ('tcnet', 'tcnettime', 'tcnetdata'):
        assert os.path.getsize(patch_main.blob_path(name)) % 4 == 0


# -- Opt-IN and Status ------------------------------------------------------------

def test_the_first_status_pass_sends_opt_in_then_status_to_port_60000():
    cpu = TcnetCpu(['tcnet'])
    cpu.deck(code=3, track=0x1234ABCD, tick=7_123_456)
    first, second = cpu.status_pass()
    optin = broadcast(first, 60000, 68)
    h = check_header(optin, 2)
    assert h['stamp'] == 456_000
    assert opt_in(optin) == dict(count=0, port=65023, uptime=7123,
                                 vendor=b'cdj-nxs2-qemu\0\0\0', device=b'CDJ-2000NXS2\0\0\0\0',
                                 version=(1, 87, 0))
    assert optin[30:32] == b'\0\0' and optin[67] == 0
    st = broadcast(second, 60000, 300)
    assert check_header(st, 5)['seq'] == (h['seq'] + 1) & 0xFF
    s = status(st)
    assert (s['count'], s['port'], s['smpte'], s['auto_master']) == (0, 65023, 0, 0)
    assert s['source'] == only(PLAYER) and s['state'] == only(3)
    assert s['track'] == only(0x1234ABCD)
    assert s['names'] == [b'DECK 2' + bytes(10) if k == PLAYER - 1 else bytes(16) for k in range(8)]
    assert s['rest'] == bytes(len(s['rest']))


def test_announcements_repeat_once_per_second_of_deck_clock():
    cpu = TcnetCpu(['tcnet'])
    cpu.deck(tick=10_000)
    assert len(cpu.status_pass()) == 2
    for tick, count in ((10_200, 0), (10_999, 0), (11_000, 2), (11_100, 0), (12_050, 2)):
        cpu.tick_reads = [tick]
        assert len(cpu.status_pass()) == count, tick


def test_the_uptime_wraps_every_twelve_hours():
    cpu = TcnetCpu(['tcnet'])
    for tick in (43_199_000, 43_200_000, 43_201_500, 86_405_000, 0xFFFFFFFF):
        cpu.deck(tick=tick)
        optin = cpu.status_pass()[0]['payload']
        assert opt_in(optin)['uptime'] == tick // 1000 % 43_200, tick


def test_the_timestamp_is_microseconds_into_the_second():
    cpu = TcnetCpu(['tcnet'])
    cpu.deck(tick=123_456, counter=5208)
    h = header(cpu.status_pass()[0]['payload'])
    assert h['stamp'] == 456_000 + (10416 - 5208) * 1000 // 10417


def test_an_underflow_not_yet_counted_moves_the_timestamp_on_a_millisecond():
    cpu = TcnetCpu(['tcnet'])
    cpu.deck(tick=999, counter=10416)
    cpu.poke(TCNT4 + 4, 2, 0x0120)
    assert header(cpu.status_pass()[0]['payload'])['stamp'] == 0


@pytest.mark.parametrize('player', [0, 5, 0xFF])
def test_nothing_is_sent_for_a_player_outside_one_to_four(player):
    cpu = TcnetCpu(['tcnet', 'tcnettime'])
    cpu.deck()
    cpu.poke(DEVICE, 1, player)
    assert cpu.status_pass(player=PLAYER) == [] and cpu.djcont_pass() == []


def test_nothing_is_sent_without_an_address_and_the_next_pass_tries_again():
    cpu = TcnetCpu(['tcnet', 'tcnettime'])
    cpu.deck()
    cpu.getter_result = 1
    assert cpu.status_pass() == [] and cpu.djcont_pass() == []
    cpu.getter_result = 0
    assert len(cpu.status_pass()) == 2


def test_another_player_number_moves_the_layer():
    cpu = TcnetCpu(['tcnet'])
    cpu.deck(player=4, code=5, track=77)
    st = cpu.status_pass(player=4)[1]['payload']
    check_header(st, 5, player=4)
    s = status(st)
    assert s['source'] == only(4, 4) and s['state'] == only(5, 4) and s['track'] == only(77, 4)
    assert s['names'][3] == b'DECK 4' + bytes(10) and s['names'][1] == bytes(16)


def test_stacks_with_osc_on_the_status_site():
    cpu = TcnetCpu(['osc', 'tcnet'])
    cpu.deck()
    ports = [struct.unpack_from('<I', s['desc'], 16)[0] for s in cpu.status_pass()]
    assert ports == [50010, 50010, 60000, 60000]


# -- Time ---------------------------------------------------------------------------

def test_a_time_packet_carries_the_layer_from_the_deck_words():
    cpu = TcnetCpu(['tcnettime'])
    cpu.deck(code=3, in_bar=3, position=61_234, seconds=146, tick=2_000_250)
    (send,) = cpu.djcont_pass()
    payload = broadcast(send, 60001, 162)
    assert check_header(payload, 254)['stamp'] == 250_000
    t = time(payload)
    assert t['time'] == only(61_234) and t['total'] == only(146_000)
    assert t['beat'] == only(3) and t['state'] == only(3)
    assert t['smpte'] == bytes(50) and t['on_air'] == [0] * 8


@pytest.mark.parametrize('code,state', [(0, 0), (1, 0), (2, 0), (3, 3), (4, 4), (5, 5),
                                        (6, 6), (7, 7), (8, 8), (9, 9), (0x0A, 10),
                                        (0x0B, 11), (0x0C, 0), (0x0E, 0), (0x11, 0),
                                        (0x12, 0), (0xFF, 0)])
def test_the_layer_state_is_the_play_state_code_where_tcnet_has_it(code, state):
    cpu = TcnetCpu(['tcnet', 'tcnettime'])
    cpu.deck(code=code)
    assert time(cpu.djcont_pass()[0]['payload'])['state'] == only(state)
    assert status(cpu.status_pass()[1]['payload'])['state'] == only(state)


@pytest.mark.parametrize('in_bar,marker', [(0, 0), (1, 1), (4, 4), (5, 0), (0xFF, 0)])
def test_the_beat_marker_is_the_beat_in_the_bar_or_zero(in_bar, marker):
    cpu = TcnetCpu(['tcnettime'])
    cpu.deck(in_bar=in_bar)
    assert time(cpu.djcont_pass()[0]['payload'])['beat'] == only(marker)


def test_time_packets_go_out_every_20_ms_of_deck_clock():
    cpu = TcnetCpu(['tcnettime'])
    cpu.deck(tick=1000)
    sent = []
    for tick in range(1000, 1100, 3):
        cpu.tick_reads = [tick]
        if cpu.djcont_pass():
            sent.append(tick)
    assert sent == [1000, 1021, 1042, 1063, 1084]


def test_a_state_change_is_sent_at_once():
    cpu = TcnetCpu(['tcnettime'])
    cpu.deck(code=3, tick=1000)
    assert len(cpu.djcont_pass()) == 1
    cpu.deck(code=5, tick=1003)
    (send,) = cpu.djcont_pass()
    assert time(send['payload'])['state'] == only(5)
    cpu.tick_reads = [1010]
    assert cpu.djcont_pass() == []


def test_the_sequence_number_counts_each_time_packet():
    cpu = TcnetCpu(['tcnettime'])
    seqs = []
    for k in range(3):
        cpu.deck(tick=1000 + 20 * k)
        seqs.append(header(cpu.djcont_pass()[0]['payload'])['seq'])
    assert seqs[1] == (seqs[0] + 1) & 0xFF and seqs[2] == (seqs[1] + 1) & 0xFF


def test_a_new_player_number_clears_the_old_layer():
    cpu = TcnetCpu(['tcnettime'])
    cpu.deck(player=1, tick=1000)
    cpu.djcont_pass()
    cpu.deck(player=3, tick=1020)
    t = time(cpu.djcont_pass()[0]['payload'])
    assert t['time'] == only(61_234, 3) and t['state'] == only(3, 3)


# -- the listener: Metrics and MetaData ---------------------------------------------

APP = (0xC0A80105, 65100)       # a TCNet receiver, 192.168.1.5, from its listener port
PAUSED = 5


def tcn(kind, body):
    return struct.pack('<HBB3sB8sBBHI', 0x0105, 3, 5, b'TCN', kind, b'APP\0\0\0\0\0', 0, 4, 0, 0) + body


def request(data_type, layer=PLAYER):
    return tcn(20, bytes([data_type, layer]))


def app_opt_in(port):
    return tcn(2, struct.pack('<HHHH', 1, port, 10, 0) + bytes(36))


def unicast(datagram, length, to=APP):
    assert datagram['cep'] == LISTENER and datagram['timeout'] == 0xFFFFFFFF
    assert datagram['to'] == struct.pack('<IHxx', *to)
    assert len(datagram['payload']) == length
    return datagram['payload']


def metrics(payload):
    def u32(at):
        return struct.unpack_from('<I', payload, at)[0]
    return dict(data_type=payload[24], layer=payload[25], state=payload[27], master=payload[29],
                beat=payload[31], length=u32(32), position=u32(36), speed=u32(40),
                beat_number=u32(57), bpm=u32(112), bend=struct.unpack_from('<H', payload, 116)[0],
                track=u32(118), rest=payload[26:27] + payload[28:29] + payload[30:31]
                + payload[44:57] + payload[61:112])


def metadata(payload):
    return dict(data_type=payload[24], layer=payload[25], artist=payload[29:285],
                title=payload[285:541], key=struct.unpack_from('<H', payload, 541)[0],
                track=struct.unpack_from('<I', payload, 543)[0])


def settled(names=('tcnetdata',), tick=5000, **deck):
    """A deck whose listener is open and whose first pushes have gone out to
    an empty node list."""
    cpu = TcnetCpu(list(names))
    cpu.deck(tick=tick, **deck)
    cpu.djcont_pass()
    assert cpu.created and cpu.datagrams == []
    return cpu


def listen(cpu, *datagrams, tick):
    cpu.inbox = [d if isinstance(d, tuple) else (d,) + APP for d in datagrams]
    cpu.tick_reads = [tick]
    cpu.djcont_pass()
    return list(cpu.datagrams)


def test_the_listener_socket_is_opened_once_on_port_65023():
    cpu = settled()
    cpu.tick_reads = [5003]
    cpu.djcont_pass()
    assert cpu.created == [struct.pack('<IIHHI', 0, 0, 65023, 0, 0)]


def test_a_failed_open_is_retried_once_a_second_and_nothing_is_read_before():
    cpu = TcnetCpu(['tcnetdata'])
    cpu.deck(tick=5000)
    cpu.cep_result = -18
    for tick in (5000, 5500, 5999):
        assert listen(cpu, request(2), tick=tick) == []
    assert len(cpu.created) == 1 and len(cpu.inbox) == 1
    cpu.cep_result = LISTENER
    assert len(listen(cpu, request(2), tick=6000)) > 0
    assert len(cpu.created) == 2


def test_nothing_is_opened_without_an_address():
    cpu = TcnetCpu(['tcnetdata'])
    cpu.deck()
    cpu.getter_result = 1
    cpu.djcont_pass()
    assert cpu.created == []


def test_a_metrics_request_is_answered_to_the_requester_from_the_status_record():
    cpu = settled(code=3, in_bar=3, position=61_234, seconds=146, flags=0x60, beat=17)
    (reply,) = listen(cpu, request(2), tick=5001)
    payload = unicast(reply, 122)
    check_header(payload, 200)
    assert metrics(payload) == dict(data_type=2, layer=PLAYER, state=3, master=1, beat=3,
                                    length=146_000, position=61_234, speed=32768,
                                    beat_number=17, bpm=12800, bend=0, track=0x1234ABCD,
                                    rest=bytes(67))


@pytest.mark.parametrize('pitch,bpm', [(0x100000, 0x80000000 | 12800), (0x114000, 0x80000000 | 12800),
                                       (0x0C0000, 0x80000000 | 17450), (0x200000, 0x8000FFFF),
                                       (0x100000, 12800)])
def test_the_bpm_is_the_track_bpm_times_the_pitch_and_the_speed_is_the_pitch(pitch, bpm):
    cpu = settled(pitch=pitch, bpm=bpm)
    m = metrics(listen(cpu, request(2), tick=5001)[0]['payload'])
    track_bpm = bpm & 0xFFFF if bpm & 0x80000000 else 0
    assert m['bpm'] == track_bpm * pitch // 0x100000
    assert m['speed'] == pitch * 32768 // 0x100000


@pytest.mark.parametrize('flags,master', [(0x20, 1), (0xDF, 0), (0xFF, 1), (0, 0)])
def test_sync_master_is_the_status_record_master_flag(flags, master):
    cpu = settled(flags=flags)
    assert metrics(listen(cpu, request(2), tick=5001)[0]['payload'])['master'] == master


@pytest.mark.parametrize('beat,number', [(0, 0), (286, 286), (-1, 0), (0x7FFFFFFF, 0x7FFFFFFF)])
def test_the_beat_number_is_the_status_record_count_or_zero(beat, number):
    cpu = settled(beat=beat & 0xFFFFFFFF)
    assert metrics(listen(cpu, request(2), tick=5001)[0]['payload'])['beat_number'] == number


def put_text(cpu, at, text):
    cpu.mem[cpu.loc(at):cpu.loc(at) + 0x200] = text.ljust(0x200, b'\0')


def test_a_metadata_request_carries_title_and_artist_in_utf16_and_the_track_id():
    cpu = settled(track=77)
    put_text(cpu, TITLE, b'Clarity (Extended)\0garbage')
    put_text(cpu, ARTIST, b'Zedd')
    (reply,) = listen(cpu, request(4), tick=5001)
    payload = unicast(reply, 548)
    check_header(payload, 200)
    assert metadata(payload) == dict(data_type=4, layer=PLAYER,
                                     artist='Zedd'.encode('utf-16-le').ljust(256, b'\0'),
                                     title='Clarity (Extended)'.encode('utf-16-le').ljust(256, b'\0'),
                                     key=0, track=77)


def test_a_long_title_is_cut_at_127_characters_and_an_empty_artist_stays_empty():
    cpu = settled()
    put_text(cpu, TITLE, b'x' * 0x1FF)
    put_text(cpu, ARTIST, b'')
    m = metadata(listen(cpu, request(4), tick=5001)[0]['payload'])
    assert m['title'] == ('x' * 127).encode('utf-16-le') + b'\0\0' and m['artist'] == bytes(256)


@pytest.mark.parametrize('data_type,layer', [(8, PLAYER), (1, PLAYER), (2, 1), (4, 3), (2, 0)])
def test_any_other_request_gets_an_empty_error(data_type, layer):
    cpu = settled()
    (reply,) = listen(cpu, request(data_type, layer), tick=5001)
    payload = unicast(reply, 30)
    check_header(payload, 13)
    assert (payload[24], payload[25]) == (data_type, layer)
    assert struct.unpack_from('<HH', payload, 26) == (14, 20)


def app_data(port):
    """Resolume Arena's 62-byte application packet: data identifiers 0xFF,
    0xFF, 20 data bytes in one packet, its listener port the second field."""
    return tcn(30, bytes.fromhex('ffff14000000010000000100000000000000bc0a0000')
               + struct.pack('<H', port) + bytes(14))


def test_an_opt_in_adds_the_node_at_its_listener_port_and_it_gets_ours_once_a_second():
    cpu = settled(code=PAUSED, track=77)
    node = (0xC0A80107, 65200)
    meta, metric = listen(cpu, (app_opt_in(65200), 0xC0A80107, 60000), tick=5001)
    assert metadata(unicast(meta, 548, to=node))['track'] == 77
    assert metrics(unicast(metric, 122, to=node))['state'] == PAUSED
    for tick, count in ((5999, 0), (6000, 2), (6500, 0), (7000, 2)):
        sent = listen(cpu, tick=tick)
        assert len(sent) == count, tick
    assert metadata(unicast(sent[1], 548, to=node))['track'] == 77
    payload = unicast(sent[0], 68, to=node)
    check_header(payload, 2)
    assert opt_in(payload) == dict(count=0, port=65023, uptime=7,
                                   vendor=b'cdj-nxs2-qemu\0\0\0', device=b'CDJ-2000NXS2\0\0\0\0',
                                   version=(1, 87, 0))


def test_metrics_are_pushed_to_each_node_every_50_ms_while_playing():
    cpu = settled(code=3)
    listen(cpu, request(2), tick=5001)
    pushed = [tick for tick in range(5010, 5200, 7) if listen(cpu, tick=tick)]
    assert pushed == [5052, 5108, 5164]
    (sent,) = listen(cpu, tick=5214)
    assert metrics(unicast(sent, 122))['state'] == 3


def test_a_paused_deck_pushes_metrics_only_when_state_speed_or_bpm_change():
    cpu = settled(code=PAUSED)
    listen(cpu, request(2), tick=5001)
    assert [t for t in range(5010, 5300, 20) if listen(cpu, tick=t)] == []
    at = record()
    for address, size, value, field, expect in ((at + 0x8C, 4, 0x110000, 'speed', 0x8800),
                                                (at + 0x90, 4, 0x80000000 | 13000, 'bpm', 13812),
                                                (at + 0x78, 1, 6, 'state', 6)):
        cpu.poke(address, size, value)
        (sent,) = listen(cpu, tick=5301)
        assert metrics(unicast(sent, 122))[field] == expect
        assert listen(cpu, tick=5302) == []


def test_metadata_is_pushed_to_each_node_when_the_track_changes():
    cpu = settled(code=PAUSED, track=77)
    listen(cpu, request(2), tick=5001)
    assert listen(cpu, tick=5002) == []
    cpu.poke(record() + 0x2C, 4, 78)
    (sent,) = listen(cpu, tick=5003)
    assert metadata(unicast(sent, 548))['track'] == 78


def test_the_node_list_keeps_four_endpoints_and_replaces_the_oldest():
    cpu = settled(code=PAUSED)
    apps = [(0xC0A80110 + k, 65100 + k) for k in range(5)]
    listen(cpu, *[(request(2),) + a for a in apps[:3]] + [(request(2),) + apps[0]], tick=5001)
    listen(cpu, *[(request(2),) + a for a in apps[3:]], tick=5002)
    sent = listen(cpu, tick=6000)
    assert [d['to'] for d in sent] == [struct.pack('<IHxx', *a) for a in (apps[4], apps[1], apps[2], apps[3])] * 2
    assert [len(d['payload']) for d in sent] == [68] * 4 + [548] * 4


def test_an_arena_application_packet_adds_the_node_at_its_listener_port_and_gets_both_at_once():
    cpu = settled(code=3, track=77)
    arena = (0xC0A83201, 65371)
    meta, metric = listen(cpu, (app_data(65371), 0xC0A83201, 52955), tick=5001)
    assert metadata(unicast(meta, 548, to=arena))['track'] == 77
    assert metrics(unicast(metric, 122, to=arena))['state'] == 3
    assert listen(cpu, (app_data(65371), 0xC0A83201, 52955), tick=5002) == []
    sent = listen(cpu, tick=6000)
    assert [(d['to'], len(d['payload'])) for d in sent] == [(struct.pack('<IHxx', *arena), n)
                                                            for n in (68, 548, 122)]


def test_a_listener_port_replaces_the_request_source_port_of_a_known_node():
    cpu = settled(code=PAUSED)
    (reply,) = listen(cpu, (request(2), 0xC0A83201, 52955), tick=5001)
    unicast(reply, 122, to=(0xC0A83201, 52955))
    meta, metric = listen(cpu, (app_data(65371), 0xC0A83201, 52955), tick=5002)
    assert unicast(metric, 122, to=(0xC0A83201, 65371))
    (reply,) = listen(cpu, (request(2), 0xC0A83201, 52955), tick=5003)
    unicast(reply, 122, to=(0xC0A83201, 52955))
    sent = listen(cpu, tick=6000)
    assert [d['to'] for d in sent] == [struct.pack('<IHxx', 0xC0A83201, 65371)] * 2


def test_application_packets_of_other_applications_are_ignored():
    cpu = settled(code=PAUSED)
    other = bytearray(app_data(65371))
    other[24] = 0
    for datagram in (bytes(other), app_data(65371)[:47]):
        assert listen(cpu, (datagram, 0xC0A83201, 52955), tick=5001) == []
    assert listen(cpu, tick=6000) == []


def test_up_to_four_datagrams_are_read_a_pass():
    cpu = settled(code=PAUSED)
    assert len(listen(cpu, *[request(2)] * 6, tick=5001)) == 4 and len(cpu.inbox) == 2


def test_foreign_and_short_datagrams_are_dropped_unanswered():
    cpu = settled(code=PAUSED)
    for datagram in (b'Qspt1WmJOL\0' + bytes(40), request(2)[:25], app_opt_in(65200)[:27],
                     b'\x05\x01\x03\x05TCM' + request(2)[7:], tcn(30, bytes(10))):
        assert listen(cpu, datagram, tick=5001) == []
    assert listen(cpu, tick=6000) == []


def test_stacks_with_the_time_packets_on_the_djcont_site():
    cpu = settled(('tcnettime', 'tcnetdata'))
    (reply,) = listen(cpu, request(2), tick=5020)
    assert unicast(reply, 122) and len(cpu.sent) == 1
    broadcast(cpu.sent[0], 60001, 162)
