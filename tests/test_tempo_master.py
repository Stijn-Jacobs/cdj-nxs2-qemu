# SPDX-License-Identifier: GPL-2.0-or-later
"""tempo_master: the Pro DJ Link packets it builds and reads, the beat clock
and the deck follow, the runtime commands, and the player's answers to a
deck's frames, all without a segment or an emulator."""
import queue
import socket
import struct

import pytest

import link_probe
import score_link
import tempo_master as tm

DECK_MAC = bytes.fromhex('020000000001')
DECK_IP = '192.168.50.10'
OWN_IP = '192.168.50.22'


def deck_frame(dst_ip, sport, dport, payload, dst_mac=b'\xff' * 6, src_mac=DECK_MAC):
    return link_probe.udp_frame(src_mac, dst_mac, DECK_IP, dst_ip, sport, dport, payload)


def unwrap(frame):
    got = score_link.parse(frame)
    assert got and got['kind'] == 'udp'
    return got


def deck_keep_alive(number=1, name='CDJ-900nexus'):
    return deck_frame('192.168.50.255', 50000, 50000,
                      tm.keep_alive_packet(name, number, DECK_MAC, DECK_IP, 1))


def deck_status(flags=0x80, handoff=tm.NO_HANDOFF, number=1, bpm=128.0):
    payload = bytearray(tm.status_packet('CDJ-900nexus', number, bpm, 0, 5, True, False))
    payload[0x89], payload[0x9f] = flags, handoff
    return deck_frame(OWN_IP, 50002, 50002, bytes(payload))


def deck_beat(in_bar, bpm=128.0, pitch=0.0, number=1):
    payload = tm.beat_packet('CDJ-900nexus', number, bpm, pitch, in_bar)
    return deck_frame('192.168.50.255', 50001, 50001, payload)


def master(**options):
    log = []
    options.setdefault('ip', OWN_IP)
    return tm.Master(say=log.append, **options), log


def payloads(frames, port=None):
    return [unwrap(f)['payload'] for f in frames if port is None or unwrap(f)['dport'] == port]


# -- packets ---------------------------------------------------------------------

def test_the_keep_alive_carries_number_mac_address_and_player_type():
    mac = bytes.fromhex('024d53545202')
    packet = tm.keep_alive_packet('CDJ-2000nexus', 2, mac, OWN_IP, 1)
    assert len(packet) == tm.KEEP_ALIVE_SIZE == struct.unpack_from('>H', packet, 0x22)[0]
    assert packet[:10] == score_link.MAGIC and packet[10] == 0x06
    assert packet[0x0c:0x20].rstrip(b'\0') == b'CDJ-2000nexus'
    assert (packet[0x24], packet[0x25]) == (2, 1)
    assert packet[0x26:0x2c] == mac and packet[0x2c:0x30] == socket.inet_aton(OWN_IP)
    assert packet[0x30] == 2 and packet[0x34] == 1
    assert tm.parse_keep_alive(packet) == (2, 'CDJ-2000nexus')


def test_a_long_name_is_cut_to_the_twenty_byte_field():
    packet = tm.keep_alive_packet('X' * 30, 3, bytes(6), OWN_IP, 0)
    assert len(packet) == tm.KEEP_ALIVE_SIZE and tm.parse_keep_alive(packet) == (3, 'X' * 20)


@pytest.mark.parametrize('beat, in_bar, to_bar', [(1, 1, 4 * 500), (2, 2, 3 * 500), (7, 3, 2 * 500), (8, 4, 500)])
def test_the_beat_packet_counts_the_bar_in_nominal_milliseconds(beat, in_bar, to_bar):
    packet = tm.beat_packet('CDJ-2000nexus', 2, 120.0, 0.0, beat)
    assert len(packet) == tm.BEAT_SIZE and packet[10] == 0x28 and packet[0x21] == 2
    assert struct.unpack_from('>H', packet, 0x22)[0] == tm.BEAT_SIZE - 0x24
    assert struct.unpack_from('>6I', packet, 0x24) == (500, 1000, to_bar, 2000, to_bar + 2000, 4000)
    assert packet[0x3c:0x54] == b'\xff' * 24
    assert link_probe.beat_fields(packet) == (12000, 0x100000, in_bar, 2)


def test_the_beat_packet_carries_the_pitch_but_keeps_the_intervals_at_track_tempo():
    packet = tm.beat_packet('CDJ-2000nexus', 2, 125.0, 4.0, 1)
    beat = tm.parse_beat(packet)
    assert beat == tm.Beat(2, 480, tm.pitch_word(4.0), 125.0, 1)
    assert tm.pitch_word(4.0) == 0x10A3D7 and tm.pitch_word(-100) == 0 and tm.pitch_word(0) == 0x100000


def test_the_status_packet_of_a_playing_master():
    packet = tm.status_packet('CDJ-2000nexus', 2, 126.5, -2.0, 6, True, True, None, 7)
    assert len(packet) == tm.STATUS_SIZE == 0x24 + struct.unpack_from('>H', packet, 0x22)[0]
    assert packet[:10] == score_link.MAGIC and packet[10] == 0x0a
    assert (packet[0x1f], packet[0x20], packet[0x21], packet[0x24]) == (1, 3, 2, 2)
    assert packet[0x89] == 0x80 | tm.FLAG_PLAY | tm.FLAG_MASTER | tm.FLAG_ON_AIR
    assert struct.unpack_from('>HH', packet, 0x90) == (0x8000, 12650)
    assert struct.unpack_from('>I', packet, 0x8c)[0] == tm.pitch_word(-2.0)
    assert (packet[0x9d], packet[0x9e], packet[0x9f]) == (0x09, 1, 0xff)
    assert struct.unpack_from('>I', packet, 0xc8)[0] == 7
    assert tm.parse_status(packet) == tm.Status(2, packet[0x89], 126.5, tm.pitch_word(-2.0), 6, 2, 0xff)


def test_the_status_packet_before_the_first_beat_and_while_handing_over():
    packet = tm.status_packet('CDJ-2000nexus', 3, 128.0, 0.0, 0, False, False, handoff=1)
    status = tm.parse_status(packet)
    assert status.flags & (tm.FLAG_PLAY | tm.FLAG_MASTER) == 0
    assert (status.beat, status.in_bar, status.handoff) == (tm.NO_BEAT, 0, 1)
    assert packet[0x7b] == 5 and packet[0x9d] == 0x01


def test_the_master_request_and_answer():
    request = tm.master_request_packet('CDJ-2000nexus', 2)
    assert request[10] == 0x26 and request[0x21] == 2 and struct.unpack_from('>HI', request, 0x22) == (4, 2)
    for yes in (True, False):
        answer = tm.master_answer_packet('CDJ-2000nexus', 2, yes)
        assert answer[10] == 0x27 and struct.unpack_from('>HII', answer, 0x22) == (8, 2, int(yes))


def test_foreign_and_short_packets_are_not_parsed():
    beat = tm.beat_packet('CDJ-2000nexus', 2, 120.0, 0.0, 1)
    status = tm.status_packet('CDJ-2000nexus', 2, 120.0, 0.0, 1, True, True)
    assert tm.parse_beat(beat[:-1]) is None and tm.parse_beat(status) is None
    assert tm.parse_status(status[:0xa6]) is None and tm.parse_status(beat) is None
    assert tm.parse_keep_alive(beat) is None and tm.parse_keep_alive(b'hello') is None


def test_a_portmapper_getport_is_answered_with_port_zero():
    call = struct.pack('>IIIIII', 0xC0FFEE, 0, 2, 100000, 2, 3) + bytes(16) + struct.pack('>IIII', 100005, 3, 17, 0)
    reply, (program, version, procedure) = tm.portmap_reply(call)
    assert (program, version, procedure) == (100000, 2, 3)
    assert struct.unpack('>7I', reply) == (0xC0FFEE, 1, 0, 0, 0, 0, 0)
    assert tm.portmap_reply(reply + bytes(20)) is None          # a reply is not a call
    assert tm.portmap_reply(call[:39]) is None


def test_the_broadcast_address_and_the_defaults_stay_out_of_the_decks_range():
    assert tm.broadcast_of('10.0.7.22') == '10.0.7.255'
    assert tm.default_mac(3) == bytes.fromhex('024d53545203')
    assert tm.default_mac(2) not in (DECK_MAC, bytes.fromhex('024448435044'))


# -- commands --------------------------------------------------------------------

@pytest.mark.parametrize('line, parsed', [
    ('bpm 126.5', ('bpm', 126.5)), ('pitch -2.5', ('pitch', -2.5)), ('play', ('play', None)),
    ('stop', ('stop', None)), ('bar', ('bar', None)), ('speed 0.5', ('speed', 0.5)),
    ('speed auto', ('speed', 'auto')), ('lock off', ('lock', False)), ('offset 0.25', ('offset', 0.25)),
    ('role on', ('role', True)), ('role request', ('role', 'request')), ('handoff no', ('handoff', False)),
    ('  quit  ', ('quit', None)),
])
def test_commands_parse(line, parsed):
    assert tm.parse_command(line) == parsed


@pytest.mark.parametrize('line', ['', 'tempo 120', 'bpm', 'bpm 0', 'bpm fast', 'bpm nan', 'speed -1',
                                  'pitch -100', 'play now', 'lock maybe', 'role master', 'offset 1 2'])
def test_bad_commands_are_refused(line):
    with pytest.raises(ValueError):
        tm.parse_command(line)


# -- the beat clock --------------------------------------------------------------

def test_the_first_beat_comes_a_second_after_the_start():
    clock = tm.BeatClock(120.0, now=100.0)
    assert clock.beat(100.0) == 0 and clock.beat(100.99) == 0
    assert clock.beat(101.0) == 1 and clock.beat(101.49) == 1 and clock.beat(101.5) == 2


def test_pitch_and_speed_scale_the_rate():
    assert tm.BeatClock(120.0, pitch=10.0).rate() == pytest.approx(2.2)
    assert tm.BeatClock(120.0, speed=0.5).rate() == pytest.approx(1.0)


def test_a_tempo_change_keeps_beat_and_phase():
    clock = tm.BeatClock(120.0, now=0.0)
    clock.change(5.25, bpm=60.0)                    # 8.5 beats in, at half the tempo from here
    assert clock.position(5.25) == pytest.approx(8.5)
    assert clock.position(6.25) == pytest.approx(9.5)


def test_stop_holds_the_position_and_play_resumes_it():
    clock = tm.BeatClock(120.0, now=0.0)
    clock.change(3.0, playing=False)
    assert clock.rate() == 0 and clock.position(50.0) == pytest.approx(4.0)
    clock.change(50.0, playing=True)
    assert clock.position(50.5) == pytest.approx(5.0)


@pytest.mark.parametrize('position', [-0.5, 0.2, 4.0, 5.3, 6.9, 7.1])
def test_bar_makes_the_next_beat_the_first_of_a_bar(position):
    clock = tm.BeatClock(120.0, now=0.0)
    clock.anchor_beats = position
    clock.next_bar(0.0)
    current = int(clock.position(0.0) // 1) + 1      # the beat now; the next one is a bar's first
    assert current % 4 == 0 and clock.position(0.0) % 1 == pytest.approx(position % 1)


def test_an_offset_moves_the_phase_by_the_difference():
    clock = tm.BeatClock(120.0, now=0.0)
    clock.set_offset(2.0, 0.25)
    clock.set_offset(2.0, 1.0)
    assert clock.position(2.0) == pytest.approx(2.0 + 1.0)


def deck_beat_fields(in_bar, bpm=128.0, pitch=0.0):
    return tm.parse_beat(tm.beat_packet('CDJ-900nexus', 1, bpm, pitch, in_bar))


def test_follow_takes_the_decks_rate_and_snaps_to_its_beat():
    clock = tm.BeatClock(128.0, now=0.0)
    clock.anchor_beats = 1.37                         # out of phase with the deck
    wall = 60 / 128.0 / 0.5                           # the deck plays at half real time
    for k in range(5):
        clock.follow(10.0 + k * wall, deck_beat_fields(k % 4 + 1))
    assert clock.speed == pytest.approx(0.5, rel=1e-3)
    now = 10.0 + 4 * wall
    position = clock.position(now)                    # the deck's beat 1 of a bar: a whole bar
    assert position == pytest.approx(round(position)) and round(position) % 4 == 0
    assert clock.beat(now + wall / 2) % 4 == 1


def test_follow_with_an_offset_and_with_the_lock_off():
    clock = tm.BeatClock(128.0, now=0.0)
    clock.offset = 0.25
    wall = 60 / 128.0
    for k in range(3):
        clock.follow(k * wall, deck_beat_fields(3))
    assert clock.position(2 * wall) % 4 == pytest.approx(2.25)
    loose = tm.BeatClock(128.0, now=0.0)
    loose.lock = False
    loose.anchor_beats = 0.6
    for k in range(3):
        loose.follow(k * wall, deck_beat_fields(3))
    assert loose.speed == pytest.approx(1.0, rel=1e-3) and loose.position(0.0) == pytest.approx(0.6, abs=0.01)


def test_follow_does_not_snap_at_another_tempo_and_reads_the_decks_pitch():
    clock = tm.BeatClock(128.0, now=0.0)
    clock.anchor_beats = 0.6
    wall = 60 / (125.0 * 1.04)                        # the deck plays 125 BPM at +4 % in real time
    for k in range(3):
        clock.follow(k * wall, deck_beat_fields(1, bpm=125.0, pitch=4.0))
    assert clock.speed == pytest.approx(1.0, rel=1e-3)
    assert clock.position(0.0) == pytest.approx(0.6)


def test_follow_ignores_beats_without_timing_and_restarts_after_a_pause():
    clock = tm.BeatClock(128.0, now=0.0)
    assert clock.follow(1.0, tm.Beat(1, tm.NO_BEAT, 0x100000, 128.0, 1)) is None
    assert clock.follow(1.0, tm.Beat(1, 469, 0, 128.0, 1)) is None
    assert clock.follow(1.0, tm.Beat(1, 469, 0x100000, 128.0, 0)) is None
    clock.follow(1.0, deck_beat_fields(1))
    clock.follow(1.0 + 60 / 128.0, deck_beat_fields(2))
    assert clock.follow(100.0, deck_beat_fields(3)) is None and clock.deck_beats == [100.0]


# -- the player ------------------------------------------------------------------

def test_the_master_announces_itself_and_sends_status_only_to_players_it_has_heard():
    m, _ = master(now=0.0)
    out = m.tick(0.0)
    assert [unwrap(f)['dport'] for f in out] == [50000]
    got = unwrap(out[0])
    assert got['dst'] == b'\xff' * 6 and socket.inet_ntoa(got['dip']) == '192.168.50.255'
    assert tm.parse_keep_alive(got['payload']) == (2, 'CDJ-2000nexus')

    assert m.receive(deck_keep_alive(), 0.1) == []
    assert m.peers == {1: (DECK_IP, DECK_MAC, 'CDJ-900nexus')}
    out = m.tick(0.3)
    status = [unwrap(f) for f in out if unwrap(f)['dport'] == 50002]
    assert len(status) == 1 and status[0]['dst'] == DECK_MAC and socket.inet_ntoa(status[0]['dip']) == DECK_IP
    assert tm.parse_status(status[0]['payload']).flags & tm.FLAG_MASTER
    assert m.tick(0.31) == []                                   # nothing due yet


def test_one_beat_packet_per_beat_at_the_tempo():
    m, _ = master(bpm=120.0, send=('beat',), now=0.0)
    beats = []
    for k in range(400):
        now = k / 100
        beats += [(now, tm.parse_beat(p)) for p in payloads(m.tick(now), 50001)]
    assert [round(t, 2) for t, _ in beats] == [1.0, 1.5, 2.0, 2.5, 3.0, 3.5]
    assert [b.in_bar for _, b in beats] == [1, 2, 3, 4, 1, 2]
    m.command('stop', None, 4.0)
    assert m.tick(10.0) == []


def test_arp_for_our_address_is_answered_and_our_own_frames_are_dropped():
    m, _ = master()
    request = (b'\xff' * 6 + DECK_MAC + b'\x08\x06' + struct.pack('>HHBBH', 1, 0x0800, 6, 4, 1) + DECK_MAC
               + socket.inet_aton(DECK_IP) + bytes(6) + socket.inet_aton(OWN_IP))
    reply = m.receive(request, 0.0)
    assert len(reply) == 1 and reply[0][:6] == DECK_MAC and reply[0][22:28] == m.mac
    assert m.receive(m.tick(0.0)[0], 0.0) == []


def test_a_deck_with_our_number_is_reported_and_not_sent_to():
    m, log = master(number=1)
    m.receive(deck_keep_alive(number=1), 0.0)
    assert m.peers == {} and any('OUR number' in line for line in log)


def test_the_portmapper_call_of_a_deck_is_answered():
    m, _ = master()
    call = struct.pack('>IIIIII', 9, 0, 2, 100000, 2, 3) + bytes(16) + struct.pack('>IIII', 100005, 3, 17, 0)
    out = m.receive(deck_frame(OWN_IP, 1023, 111, call, dst_mac=m.mac), 0.0)
    got = unwrap(out[0])
    assert (got['sport'], got['dport'], got['dst']) == (111, 1023, DECK_MAC)
    assert struct.unpack('>7I', got['payload'])[:2] == (9, 1)


def test_a_deck_that_asks_gets_the_master_role():
    m, _ = master()
    m.receive(deck_keep_alive(), 0.0)
    request = deck_frame(OWN_IP, 50001, 50001, tm.master_request_packet('CDJ-900nexus', 1), dst_mac=m.mac)
    answer = payloads(m.receive(request, 0.0))
    assert len(answer) == 1 and struct.unpack_from('>I', answer[0], 0x28)[0] == 1
    status = tm.parse_status(payloads(m.tick(0.0), 50002)[0])
    assert status.flags & tm.FLAG_MASTER and status.handoff == 1          # still master, naming the next one
    m.receive(deck_status(flags=0x80 | tm.FLAG_MASTER | tm.FLAG_PLAY), 0.1)
    status = tm.parse_status(payloads(m.tick(0.3), 50002)[0])
    assert not status.flags & tm.FLAG_MASTER and status.handoff == tm.NO_HANDOFF


def test_a_refused_request_keeps_the_role():
    m, _ = master(accept_handoff=False)
    request = deck_frame(OWN_IP, 50001, 50001, tm.master_request_packet('CDJ-900nexus', 1), dst_mac=m.mac)
    answer = payloads(m.receive(request, 0.0))
    assert struct.unpack_from('>I', answer[0], 0x28)[0] == 0 and m.master and m.handoff is None


def test_a_deck_that_takes_the_role_without_asking_ends_ours():
    m, log = master()
    m.receive(deck_status(flags=0x80), 0.0)
    assert m.master
    m.receive(deck_status(flags=0x80 | tm.FLAG_MASTER), 0.1)
    assert not m.master and any('without asking' in line for line in log)


def test_role_request_asks_every_player_and_takes_the_role_when_named():
    m, _ = master()
    m.command('role', False, 0.0)
    m.receive(deck_keep_alive(), 0.0)
    out = m.command('role', 'request', 0.0)
    assert len(out) == 1 and unwrap(out[0])['dst'] == DECK_MAC
    assert tm.parse_command('role request') == ('role', 'request') and payloads(out)[0][10] == 0x26
    m.receive(deck_status(flags=0x80 | tm.FLAG_MASTER, handoff=2), 0.1)
    assert m.master and not m.want_master


def test_speed_auto_follows_the_first_deck_whose_beats_it_hears():
    m, _ = master(speed='auto', now=0.0)
    wall = 60 / 128.0 / 0.8
    for k in range(4):
        m.receive(deck_beat(k % 4 + 1), k * wall)
        m.receive(deck_beat(1, bpm=90.0, number=3), k * wall + 0.1)    # a second deck is ignored
    assert m.following == 1 and m.clock.speed == pytest.approx(0.8, rel=1e-3)
    m.command('speed', 2.0, 5.0)
    assert not m.auto and m.clock.speed == 2.0 and m.following is None


def test_other_traffic_is_logged_once_per_kind():
    m, log = master()
    nfs = deck_frame(OWN_IP, 800, 2049, b'x' * 40, dst_mac=b'\x02' * 6)
    m.receive(nfs, 0.0)
    m.receive(nfs, 0.1)
    assert sum('other traffic: UDP to port 2049' in line for line in log) == 1


# -- the loop ---------------------------------------------------------------------

class FakeLink:
    def __init__(self, frames):
        self.frames, self.sent = list(frames), []

    def recv(self, _timeout):
        return self.frames.pop(0) if self.frames else None

    def send(self, frame):
        self.sent.append(frame)


def test_the_loop_answers_frames_applies_commands_and_quits():
    m, log = master(now=0.0)
    link = FakeLink([deck_keep_alive()])
    commands = queue.Queue()
    for line in ('bpm 140', 'nonsense', 'quit', 'bpm 90'):
        commands.put(line)
    ticks = iter(range(10 ** 6))
    tm.run(m, link, commands, clock=lambda: next(ticks) * 0.001)
    assert m.peers and m.clock.bpm == 140.0
    assert any("'nonsense'" in line for line in log)
    assert commands.empty() and m.clock.bpm == 140.0             # nothing after quit is applied


def test_the_loop_sends_until_its_time_is_up():
    m, _ = master(now=0.0)
    link = FakeLink([deck_keep_alive()])
    ticks = iter(range(10 ** 6))
    tm.run(m, link, seconds=2.0, clock=lambda: next(ticks) * 0.001)
    ports = [unwrap(f)['dport'] for f in link.sent]
    assert ports.count(50000) == 2 and ports.count(50001) == 3 and ports.count(50002) >= 9


def test_the_command_line(capsys):
    args = tm.parse_args(['239.77.77.1:45000', '--speed', 'auto', '--mac', '02:4d:53:54:52:09', '--send', 'beat'])
    assert args.mac == bytes.fromhex('024d53545209') and args.send == {'beat'} and args.speed == 'auto'
    told = tm.parse_args(['--tell', '47000', 'pitch', '-2.5'])
    assert told.tell == 47000 and told.command == ['pitch', '-2.5']
    for bad in (['239.77.77.1:45000', '--number', '5'], ['239.77.77.1'], ['g:1', '--speed', 'fast'],
                ['g:1', '--mac', '0102'], ['g:1', '--send', 'video'], []):
        with pytest.raises(SystemExit):
            tm.parse_args(bad)
