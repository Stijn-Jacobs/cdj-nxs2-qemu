#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A virtual tempo master: a playing Pro DJ Link player in software.

With it SYNC, MASTER and the master's beat on the deck's screen can be tried
with one emulated deck instead of two. It joins the decks' multicast segment
(QEMU's `-netdev socket,mcast=GROUP:PORT`, one raw Ethernet frame per UDP
datagram, see capture_link.py) and acts as a playing player that is tempo
master:

  - announces itself with keep-alive packets (UDP 50000, type 0x06),
  - sends its status (UDP 50002, type 0x0a) to every player it has heard,
    with the play, master and on-air flags, BPM, pitch, beat number and beat
    in bar,
  - sends a beat packet (UDP 50001, type 0x28) on every beat,
  - hands the master role over when a deck asks for it (type 0x26, answered
    with 0x27) and can ask a deck for it the same way,
  - answers ARP for its own address, and the decks' portmapper requests
    (UDP 111) with "not registered": a deck asks every player it sees for
    its media, and without an answer it can end in a LINK access error.

It hands out no addresses. A deck says nothing on the segment before it holds
a DHCP lease; the rig runs dhcp_server.py for that whenever Pro DJ Link is on.
Start the master on the segment the launcher prints ("Pro DJ Link: on
GROUP:PORT", or rig.sh's "Pro DJ Link ON: GROUP:PORT"):

  ./setup.sh --djlink on                                # once: Pro DJ Link on
  ./start.sh                                            # one deck
  python3 scripts/net/tempo_master.py 239.77.77.1:45000 --bpm 126 --control 47000
  python3 scripts/net/tempo_master.py --tell 47000 bpm 130

Without the launcher, run `python3 scripts/net/dhcp_server.py GROUP:PORT`
beside it. The master is player 2 by default; with two emulated decks give it
--number 3. A DJLINK=tap: deck is not on the segment and cannot be reached.

  usage: tempo_master.py GROUP:PORT [--bpm 128] [--pitch 0] [--number 2]
                         [--speed 1|auto] [--offset 0] [--no-lock]
                         [--name NAME] [--ip IP] [--mac MAC]
                         [--control PORT] [--seconds S] [--log FILE]
         tempo_master.py --tell PORT COMMAND

Runtime commands, one per UDP datagram to 127.0.0.1:PORT (--control), which
--tell sends: "bpm 126.5", "pitch -2.5" (percent), "play", "stop", "bar" (the
next beat is beat 1 of a bar), "speed 0.5" or "speed auto", "lock on|off",
"offset 0.25" (beats), "role on|off" (set the master flag), "role request"
(ask the decks for the master role, as their MASTER button does), "handoff
yes|no" (the answer to a deck's master request), "quit".

The master's beat runs on the host clock. A deck that plays slower than real
time (a slow host, or a deck without its DSP module) falls behind it: --speed
scales the master's clock, and --speed auto follows the deck's beat packets,
taking the deck's playback rate from them and, at equal tempo, putting the
master's beat on the deck's beat plus --offset beats. The intervals in the
master's own beat packets stay nominal (milliseconds at its tempo), as a real
player sends them.

How a deck treats the master's packets, as observed on the CDJ-900NXS: with
no master on the network it takes the first player whose status has the
master flag; while it is master itself it hands over only when asked; its
MASTER button sends a request (0x26) to the master, which answers (0x27) and
names the new master in its status (byte 0x9f) until that one shows the
master flag.

Packet layouts: the public Pro DJ Link research by Deep Symmetry, "DJ Link
Ecosystem Analysis" (https://djl-analysis.deepsymmetry.org/). The status
packet is the 0xd4-byte form of the nexus generation. The CDJ-900NXS takes
it; whether the NXS2 and XDJ decks do has not been checked.
"""
import argparse
import collections
import math
import os
import queue
import signal
import socket
import struct
import sys
import time

from link_probe import arp_reply, beat_fields, join_group, split_group, udp_frame
from score_link import MAGIC, parse

ANNOUNCE_PORT, BEAT_PORT, STATUS_PORT = 50000, 50001, 50002
PORTMAP_PORT = 111
BROADCAST_MAC = b'\xff' * 6

KEEP_ALIVE, STATUS, MASTER_REQUEST, MASTER_ANSWER, BEAT = 0x06, 0x0a, 0x26, 0x27, 0x28
KEEP_ALIVE_SIZE, BEAT_SIZE, STATUS_SIZE = 0x36, 0x60, 0xd4

FLAG_PLAY, FLAG_MASTER, FLAG_SYNC, FLAG_ON_AIR = 0x40, 0x20, 0x10, 0x08
PITCH_UNITY = 0x100000              # pitch word for +-0 %
NO_HANDOFF = 0xff
NO_BEAT = 0xffffffff
FIRMWARE = '1.31'                   # the status's firmware version string; informational

KEEP_ALIVE_EVERY, STATUS_EVERY = 1.5, 0.2         # seconds
FIRST_BEAT_AFTER = 1.0                            # seconds after start
FOLLOW_WINDOW = 3                   # deck beat periods --speed auto takes the median of
FOLLOW_RESET = 30.0                 # seconds without a deck beat that start the follow afresh
SAME_TEMPO = 0.005                  # relative tempo difference below which auto snaps the phase

COMMANDS = ('bpm', 'pitch', 'play', 'stop', 'bar', 'speed', 'lock', 'offset', 'role', 'handoff', 'quit')

Beat = collections.namedtuple('Beat', 'player interval pitch bpm in_bar')
Status = collections.namedtuple('Status', 'player flags bpm pitch beat in_bar handoff')


# -- packets ---------------------------------------------------------------------

def name_field(name):
    return name.encode('ascii', 'replace')[:20].ljust(20, b'\0')


def pitch_word(pitch):
    """The pitch field for `pitch` percent: 0x100000 is +-0 %."""
    return int(round(PITCH_UNITY * (1 + pitch / 100.0)))


def keep_alive_packet(name, number, mac, ip, peers):
    """UDP 50000 type 0x06. Byte 0x25 is 1: with 2, the value a deck sends
    itself, the CDJ-900NXS sees a second player in its own role and repeats its
    number claim every few seconds."""
    packet = MAGIC + bytes([KEEP_ALIVE, 0]) + name_field(name) + bytes([1, 2])
    packet += struct.pack('>H', KEEP_ALIVE_SIZE) + bytes([number, 1]) + mac + socket.inet_aton(ip)
    packet += bytes([min(peers + 1, 255), 0, 0, 0, 1, 0])       # players seen, ...; 1 = a player
    return packet


def header(kind, name, number, version=0):
    """The start of a 50001/50002 packet, up to the player number at 0x21."""
    return MAGIC + bytes([kind]) + name_field(name) + bytes([1, version, number])


def beat_packet(name, number, bpm, pitch, beat):
    """UDP 50001 type 0x28 for `beat` (1-based). The intervals are
    milliseconds at the track tempo; the receiver scales them by the pitch."""
    period = int(round(60000.0 / bpm))
    in_bar = (beat - 1) % 4 + 1
    to_bar = (5 - in_bar) * period
    packet = header(BEAT, name, number) + struct.pack('>H', BEAT_SIZE - 0x24)
    packet += struct.pack('>IIIIII', period, 2 * period, to_bar, 4 * period, to_bar + 4 * period, 8 * period)
    packet += b'\xff' * 24
    packet += struct.pack('>I', pitch_word(pitch)) + b'\0\0' + struct.pack('>H', int(round(bpm * 100)))
    packet += bytes([in_bar, 0, 0, number])
    return packet


def master_request_packet(name, number):
    """UDP 50001 type 0x26: `number` asks the master for its role."""
    return header(MASTER_REQUEST, name, number) + struct.pack('>HI', 4, number)


def master_answer_packet(name, number, yes):
    """UDP 50001 type 0x27: the master's answer to a 0x26."""
    return header(MASTER_ANSWER, name, number) + struct.pack('>HII', 8, number, 1 if yes else 0)


def status_packet(name, number, bpm, pitch, beat, playing, master, handoff=None, sequence=0):
    """UDP 50002 type 0x0a, the nexus layout. `beat` is 1-based, 0 for none
    yet; `handoff` the player the master role goes to, or None."""
    p = bytearray(STATUS_SIZE)
    p[0:0x22] = header(STATUS, name, number, version=3)
    p[0x22:0x24] = struct.pack('>H', STATUS_SIZE - 0x24)
    p[0x24] = number
    p[0x27] = 1                                         # active
    p[0x7b] = 3 if playing else 5                       # play mode: playing / paused
    p[0x7c:0x80] = FIRMWARE.encode()
    p[0x89] = 0x80 | FLAG_ON_AIR | (FLAG_PLAY if playing else 0) | (FLAG_MASTER if master else 0)
    p[0x8b] = 0xfa if playing else 0xfe
    word = struct.pack('>I', pitch_word(pitch))
    p[0x8c:0x90] = word
    # The BPM x 100 is the low half of the u32 at 0x90. The high half is 0x8000:
    # with 0 there the CDJ-900NXS's SYNC ignores the master's tempo.
    p[0x90:0x94] = struct.pack('>HH', 0x8000, int(round(bpm * 100)))
    p[0x94:0x98] = struct.pack('>I', 0x7fffffff)        # no second tempo
    p[0x98:0x9c] = word
    p[0x9d] = 0x09 if playing else 0x01                 # 9: playing, beat from the beat packets
    p[0x9e] = 1 if master else 0
    p[0x9f] = NO_HANDOFF if handoff is None else handoff
    p[0xa0:0xa4] = struct.pack('>I', beat if beat else NO_BEAT)
    p[0xa4:0xa6] = struct.pack('>H', 0x1ff)             # no memory cue ahead
    p[0xa6] = (beat - 1) % 4 + 1 if beat else 0
    p[0xc0:0xc4] = word
    p[0xc4:0xc8] = word
    p[0xc8:0xcc] = struct.pack('>I', sequence & 0xffffffff)
    p[0xcc] = 0x0f                                      # a nexus player
    return bytes(p)


def parse_keep_alive(payload):
    """(player number, name) of a keep-alive, or None."""
    if len(payload) < KEEP_ALIVE_SIZE or payload[:10] != MAGIC or payload[10] != KEEP_ALIVE:
        return None
    return payload[0x24], payload[0x0c:0x20].rstrip(b'\0').decode('ascii', 'replace')


def parse_beat(payload):
    """The fields of a beat packet, or None."""
    if len(payload) < BEAT_SIZE or payload[:10] != MAGIC or payload[10] != BEAT:
        return None
    bpm, pitch, in_bar, player = beat_fields(payload)
    return Beat(player, struct.unpack_from('>I', payload, 0x24)[0], pitch, bpm / 100.0, in_bar)


def parse_status(payload):
    """The fields of a player status packet the master acts on, or None."""
    if len(payload) <= 0xa6 or payload[:10] != MAGIC or payload[10] != STATUS:
        return None
    return Status(payload[0x21], payload[0x89], struct.unpack_from('>H', payload, 0x92)[0] / 100.0,
                  struct.unpack_from('>I', payload, 0x8c)[0], struct.unpack_from('>I', payload, 0xa0)[0],
                  payload[0xa6], payload[0x9f])


def portmap_reply(payload):
    """The reply to an ONC RPC call (RFC 5531) on the portmapper port, and the
    call's (program, version, procedure); None for anything else. Every call
    gets port 0, "not registered": the master serves no media."""
    if len(payload) < 40:
        return None
    xid, kind, _, program, version, procedure = struct.unpack_from('>IIIIII', payload)
    if kind != 0:
        return None
    # reply, accepted, null verifier, success, port 0
    return struct.pack('>IIIIIII', xid, 1, 0, 0, 0, 0, 0), (program, version, procedure)


def broadcast_of(ip):
    """The /24 broadcast address, the decks' subnet (dhcp_server.py hands out a /24)."""
    return ip.rpartition('.')[0] + '.255'


# -- commands --------------------------------------------------------------------

def parse_command(line):
    """(command, value) of one runtime command; ValueError if it is not one."""
    words = line.split()
    if not words or words[0] not in COMMANDS:
        raise ValueError('unknown command %r' % line)
    cmd, args = words[0], words[1:]
    if cmd in ('play', 'stop', 'bar', 'quit'):
        if args:
            raise ValueError('%s takes no value' % cmd)
        return cmd, None
    if len(args) != 1:
        raise ValueError('%s takes one value' % cmd)
    arg = args[0]
    choices = {'lock': {'on': True, 'off': False}, 'handoff': {'yes': True, 'no': False},
               'role': {'on': True, 'off': False, 'request': 'request'}}
    if cmd in choices:
        if arg not in choices[cmd]:
            raise ValueError('%s takes %s' % (cmd, '|'.join(choices[cmd])))
        return cmd, choices[cmd][arg]
    if cmd == 'speed' and arg == 'auto':
        return cmd, 'auto'
    value = float(arg)
    if not math.isfinite(value) or (cmd in ('bpm', 'speed') and value <= 0) or (cmd == 'pitch' and value <= -100):
        raise ValueError('%s %s is out of range' % (cmd, arg))
    return cmd, value


# -- the beat clock --------------------------------------------------------------

class BeatClock:
    """The master's beat position over host time. Position 0.0 is beat 1 of
    bar 1; it advances by the effective tempo times `speed`, and stands still
    while stopped. Every change goes through reanchor(), so the beat number
    and phase carry over."""

    def __init__(self, bpm, pitch=0.0, speed=1.0, now=0.0):
        self.bpm, self.pitch, self.speed, self.playing = bpm, pitch, speed, True
        self.anchor_t = now
        self.anchor_beats = -FIRST_BEAT_AFTER * self.rate()
        self.lock, self.offset = True, 0.0
        self.deck_beats = []

    def effective_bpm(self):
        return self.bpm * (1 + self.pitch / 100.0)

    def rate(self):
        """Beats per host second."""
        return self.effective_bpm() / 60.0 * self.speed if self.playing else 0.0

    def position(self, now):
        return self.anchor_beats + (now - self.anchor_t) * self.rate()

    def beat(self, now):
        """The beat number, 1-based; 0 before the first beat."""
        pos = self.position(now)
        return int(math.floor(pos)) + 1 if pos >= 0 else 0

    def reanchor(self, now):
        self.anchor_beats, self.anchor_t = self.position(now), now

    def change(self, now, **values):
        """Set bpm, pitch, speed or playing from `now` on."""
        self.reanchor(now)
        for key, value in values.items():
            setattr(self, key, value)

    def next_bar(self, now):
        """Make the next beat the first of a bar."""
        self.reanchor(now)
        whole = math.floor(self.anchor_beats)
        self.anchor_beats += whole - whole % 4 + 3 - whole

    def set_offset(self, now, offset):
        self.reanchor(now)
        self.anchor_beats += offset - self.offset
        self.offset = offset

    def follow(self, now, beat):
        """--speed auto: one beat packet of the deck, received at `now`.

        The speed becomes the deck's track beat period over the median of its
        last few beat periods in host time; a rate estimate alone drifts when
        the deck's speed swings. At equal tempo, with the lock on, the phase
        snaps to the deck's beat plus `offset` beats, keeping the bar. Returns
        the phase error before the snap in beats, or None when nothing changed."""
        if beat.interval in (0, NO_BEAT) or beat.pitch == 0 or not 1 <= beat.in_bar <= 4:
            return None
        if self.deck_beats and now - self.deck_beats[-1] > FOLLOW_RESET:
            self.deck_beats = []                       # stopped or loaded meanwhile
        self.deck_beats = (self.deck_beats + [now])[-FOLLOW_WINDOW - 1:]
        if len(self.deck_beats) < 2:
            return None
        periods = sorted(b - a for a, b in zip(self.deck_beats, self.deck_beats[1:]))
        wall = periods[len(periods) // 2]
        if wall <= 0:
            return None
        track = beat.interval / 1000.0 * PITCH_UNITY / beat.pitch     # the deck's beat period at its tempo
        self.change(now, speed=track / wall)
        before = self.anchor_beats
        if self.lock and abs(self.effective_bpm() - 60.0 / track) < SAME_TEMPO * 60.0 / track:
            target = beat.in_bar - 1 + self.offset
            self.anchor_beats = target + 4 * round((before - target) / 4.0)
        return before - self.anchor_beats


# -- the player ------------------------------------------------------------------

class Master:
    """The player's state. receive(), tick() and command() take the host time
    and return the Ethernet frames to put on the segment; no I/O here."""

    def __init__(self, number=2, name='CDJ-2000nexus', ip=None, mac=None, bpm=128.0, pitch=0.0, speed='1',
                 lock=True, offset=0.0, accept_handoff=True, send=('alive', 'status', 'beat'),
                 dump=False, verbose=False, now=0.0, say=print):
        self.number, self.name = number, name
        self.ip = ip or default_ip(number)
        self.mac = mac or default_mac(number)
        self.broadcast = broadcast_of(self.ip)
        self.auto = speed == 'auto'
        self.clock = BeatClock(bpm, pitch, 1.0 if self.auto else float(speed), now)
        self.clock.lock, self.clock.offset = lock, offset
        self.master, self.handoff, self.want_master = True, None, False
        self.accept_handoff = accept_handoff
        self.send_kinds, self.dump, self.verbose = set(send), dump, verbose
        self.say = say
        self.peers = {}                     # player number -> (ip, mac, name)
        self.following = None              # --speed auto: the player whose beats set the clock
        self.last_status = {}
        self.seen = set()
        self.next_alive = self.next_status = now
        self.last_beat = 0
        self.sequence = 0
        self.sent = collections.Counter()

    def frame(self, dst_mac, dst_ip, sport, dport, payload):
        return udp_frame(self.mac, dst_mac, self.ip, dst_ip, sport, dport, payload)

    def to_peers(self, port, payload):
        return [self.frame(mac, ip, port, port, payload) for ip, mac, _ in self.peers.values()]

    # -- sending ----------------------------------------------------------------

    def tick(self, now):
        out = []
        clock = self.clock
        if now >= self.next_alive and 'alive' in self.send_kinds:
            out.append(self.frame(BROADCAST_MAC, self.broadcast, ANNOUNCE_PORT, ANNOUNCE_PORT,
                                  keep_alive_packet(self.name, self.number, self.mac, self.ip, len(self.peers))))
            self.next_alive = now + KEEP_ALIVE_EVERY
            self.sent['keep-alive'] += 1
        if now >= self.next_status and 'status' in self.send_kinds:
            self.sequence += 1
            frames = self.to_peers(STATUS_PORT, status_packet(
                self.name, self.number, clock.bpm, clock.pitch, clock.beat(now), clock.playing, self.master,
                self.handoff, self.sequence))
            out += frames
            self.next_status = now + STATUS_EVERY
            self.sent['status'] += len(frames)
        beat = clock.beat(now)
        if clock.playing and beat and beat != self.last_beat and 'beat' in self.send_kinds:
            out.append(self.frame(BROADCAST_MAC, self.broadcast, BEAT_PORT, BEAT_PORT,
                                  beat_packet(self.name, self.number, clock.bpm, clock.pitch, beat)))
            self.sent['beat'] += 1
            if self.verbose:
                self.say('beat %d (%d in bar)' % (beat, (beat - 1) % 4 + 1))
        self.last_beat = beat
        return out

    # -- receiving --------------------------------------------------------------

    def receive(self, frame, now):
        if frame[6:12] == self.mac:
            return []                                   # our own, looped back
        reply = arp_reply(frame, self.ip, self.mac)
        if reply:
            return [reply]
        got = parse(frame)
        if not got or got['kind'] != 'udp':
            self.other(got)
            return []
        mac, sip, dip = got['src'], socket.inet_ntoa(got['sip']), socket.inet_ntoa(got['dip'])
        sport, dport, payload = got['sport'], got['dport'], got['payload']
        if dport == PORTMAP_PORT and dip == self.ip:
            answer = portmap_reply(payload)
            if not answer:
                return []
            if answer[1] not in self.seen:
                self.seen.add(answer[1])
                self.say('portmapper call from %s: program %d version %d procedure %d -> port 0' % ((sip,) + answer[1]))
            return [self.frame(mac, sip, PORTMAP_PORT, sport, answer[0])]
        if dport not in (ANNOUNCE_PORT, BEAT_PORT, STATUS_PORT) or payload[:10] != MAGIC or len(payload) < 0x22:
            self.other(got)
            return []
        kind = payload[10]
        if dport == ANNOUNCE_PORT and kind == KEEP_ALIVE:
            return self.on_keep_alive(payload, sip, mac)
        if dport == BEAT_PORT and kind == MASTER_REQUEST and len(payload) >= 0x28:
            return self.on_master_request(payload, sip, mac)
        if dport == BEAT_PORT and kind == MASTER_ANSWER and len(payload) >= 0x2c:
            yes = struct.unpack_from('>I', payload, 0x28)[0] == 1
            self.say('player %d answers our master request: %s' % (payload[0x21], 'yes' if yes else 'no'))
            return []
        if dport == BEAT_PORT and kind == BEAT:
            beat = parse_beat(payload)
            if beat and self.auto and self.following in (None, beat.player):
                self.following = beat.player
                error = self.clock.follow(now, beat)
                if error is not None and self.verbose:
                    self.say('deck beat %d: speed %.3f, phase error before the snap %+.2f beats'
                             % (beat.in_bar, self.clock.speed, error))
            return []
        if dport == STATUS_PORT and kind == STATUS:
            self.on_status(payload)
            return []
        self.other(got, 'Pro DJ Link type 0x%02x on UDP %d' % (kind, dport))
        return []

    def on_keep_alive(self, payload, sip, mac):
        got = parse_keep_alive(payload)
        if not got:
            return []
        number, name = got
        if number == self.number:
            if ('conflict', sip) not in self.seen:
                self.seen.add(('conflict', sip))
                self.say('player %d %r at %s has OUR number; start the master with another --number'
                         % (number, name, sip))
            return []
        if self.peers.get(number, (None,))[0] != sip:
            self.say('player %d %r at %s' % (number, name, sip))
        self.peers[number] = (sip, mac, name)
        return []

    def on_master_request(self, payload, sip, mac):
        asker = struct.unpack_from('>I', payload, 0x24)[0]
        yes = self.master and self.accept_handoff
        if yes:
            self.handoff = asker
        self.say('player %d asks for the master role -> %s' % (asker, 'yes' if yes else 'no'))
        return [self.frame(mac, sip, BEAT_PORT, BEAT_PORT, master_answer_packet(self.name, self.number, yes))]

    def on_status(self, payload):
        status = parse_status(payload)
        if not status or status.player == self.number:
            return
        flagged = status.flags & FLAG_MASTER
        if self.handoff == status.player and flagged:
            self.master, self.handoff = False, None
            self.say('player %d is master now' % status.player)
        elif self.master and flagged and not self.want_master and status.handoff in (0, NO_HANDOFF):
            self.master, self.handoff = False, None
            self.say('player %d took the master role without asking; we are not master any more' % status.player)
        if self.want_master and status.handoff == self.number:
            self.master, self.want_master = True, False
            self.say('player %d hands the master role to us' % status.player)
        key = (status.flags, status.bpm, status.pitch, status.handoff)
        if self.last_status.get(status.player) != key or self.dump:
            self.last_status[status.player] = key
            self.say('status of player %d: %d bytes, flags %02x, %.2f BPM, pitch %06x, beat %d (%d in bar), '
                     'handoff %02x' % (status.player, len(payload), status.flags, status.bpm, status.pitch,
                                       status.beat, status.in_bar, status.handoff))
            if self.dump:
                self.say('  ' + payload.hex())

    def other(self, got, what=None):
        """Log the first frame of every other kind the decks send."""
        if not got:
            return
        if what is None and got['kind'] == 'udp':
            what = 'UDP to port %d' % got['dport']
        key = what or got['kind']
        if key not in self.seen:
            self.seen.add(key)
            self.say('other traffic: %s from %s' % (key, socket.inet_ntoa(got['sip']) if 'sip' in got
                                                    else got['src'].hex(':')))

    # -- commands ---------------------------------------------------------------

    def command(self, cmd, value, now):
        """Apply one parsed command (see parse_command); 'quit' is the caller's."""
        clock = self.clock
        out = []
        if cmd in ('bpm', 'pitch'):
            clock.change(now, **{cmd: value})
        elif cmd in ('play', 'stop'):
            clock.change(now, playing=cmd == 'play')
        elif cmd == 'bar':
            clock.next_bar(now)
        elif cmd == 'speed':
            self.auto = value == 'auto'
            if not self.auto:
                clock.change(now, speed=value)
            clock.deck_beats, self.following = [], None
        elif cmd == 'lock':
            clock.lock = value
        elif cmd == 'offset':
            clock.set_offset(now, value)
        elif cmd == 'handoff':
            self.accept_handoff = value
        elif cmd == 'role' and value == 'request':
            self.want_master = True
            out = self.to_peers(BEAT_PORT, master_request_packet(self.name, self.number))
        elif cmd == 'role':
            self.master, self.handoff, self.want_master = value, None, False
        return out


def default_ip(number):
    """An address in dhcp_server.py's /24 (DHCP_NET) above the decks' leases."""
    return '%s.%d' % (os.environ.get('DHCP_NET', '192.168.50'), 20 + number)


def default_mac(number):
    """Locally administered; outside the decks' 02:00:00:00:00:0N and the DHCP
    server's address. 4d:53:54:52 is "MSTR"."""
    return bytes.fromhex('024d535452%02x' % number)


# -- I/O -------------------------------------------------------------------------

running = True


def stop(_signum, _frame):
    global running
    running = False


class Segment:
    """The decks' multicast segment: one Ethernet frame per datagram."""

    def __init__(self, group):
        self.dest = split_group(group)
        self.sock = join_group(*self.dest)

    def recv(self, timeout):
        self.sock.settimeout(timeout)
        try:
            return self.sock.recvfrom(65535)[0]
        except socket.timeout:
            return None

    def send(self, frame):
        self.sock.sendto(frame, self.dest)


def control_socket(port):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', port))
    sock.setblocking(False)
    return sock


def run(master, link, commands=None, seconds=0.0, clock=time.monotonic):
    """Serve until `seconds` have passed (0 = until stopped) or a quit command.
    `commands` is a queue.Queue or a non-blocking socket of command lines."""
    end = clock() + seconds if seconds else None
    while running and (end is None or clock() < end):
        frame = link.recv(0.01)
        if frame is not None:
            for out in master.receive(frame, clock()):
                link.send(out)
        for line in pending(commands):
            try:
                cmd, value = parse_command(line)
            except ValueError as error:
                master.say('command %r: %s' % (line, error))
                continue
            master.say('command: %s' % line)
            if cmd == 'quit':
                return
            for out in master.command(cmd, value, clock()):
                link.send(out)
        for out in master.tick(clock()):
            link.send(out)


def pending(commands):
    if commands is None:
        return []
    lines = []
    while True:
        try:
            if isinstance(commands, queue.Queue):
                lines.append(commands.get_nowait())
            else:
                lines.append(commands.recv(512).decode('utf-8', 'replace').strip())
        except (queue.Empty, BlockingIOError):
            return lines


def tell(port, words):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.sendto(' '.join(words).encode(), ('127.0.0.1', port))


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('segment', nargs='?', metavar='GROUP:PORT', help='the decks\' multicast segment')
    parser.add_argument('--tell', type=int, metavar='PORT',
                        help='send the rest of the command line to a running master\'s --control PORT and exit')
    parser.add_argument('--bpm', type=float, default=128.0, help='track tempo (default 128)')
    parser.add_argument('--pitch', type=float, default=0.0, help='tempo fader, percent (default 0)')
    parser.add_argument('--number', type=int, default=2, help='player number 1-4 (default 2)')
    parser.add_argument('--name', default='CDJ-2000nexus', help='device name in the packets (default CDJ-2000nexus)')
    parser.add_argument('--ip', help='own address (default DHCP_NET.2N, DHCP_NET=192.168.50 as dhcp_server.py)')
    parser.add_argument('--mac', help='own MAC (default 02:4d:53:54:52:0N)')
    parser.add_argument('--speed', default='1',
                        help='beat clock per host second: a factor, or auto to follow the deck (default 1)')
    parser.add_argument('--no-lock', dest='lock', action='store_false',
                        help='with --speed auto: follow the deck\'s rate only, not its beat')
    parser.add_argument('--offset', type=float, default=0.0,
                        help='with --speed auto: the master\'s beat this many beats after the deck\'s (default 0)')
    parser.add_argument('--refuse-handoff', action='store_true', help='answer the decks\' master requests with no')
    parser.add_argument('--send', default='alive,status,beat',
                        help='packet kinds to send, comma-separated (default alive,status,beat)')
    parser.add_argument('--control', type=int, default=0, metavar='PORT',
                        help='take runtime commands as UDP datagrams on 127.0.0.1:PORT')
    parser.add_argument('--seconds', type=float, default=0, help='stop after this long (default 0, never)')
    parser.add_argument('--log', help='append the log to this file instead of stdout')
    parser.add_argument('--dump', action='store_true', help='hex-dump every status packet received')
    parser.add_argument('--verbose', action='store_true', help='log every beat sent and every deck beat followed')
    args, rest = parser.parse_known_args(argv)
    if args.tell is not None:
        words = ([args.segment] if args.segment else []) + rest
        if not words:
            parser.error('--tell needs a command')
        args.command = words
        return args
    if rest:
        parser.error('unrecognized arguments: %s' % ' '.join(rest))
    if not args.segment:
        parser.error('the segment GROUP:PORT is required')
    if not 1 <= args.number <= 4:
        parser.error('--number must be 1-4')
    try:
        split_group(args.segment)
        if args.speed != 'auto':
            parse_command('speed ' + args.speed)
        parse_command('bpm %r' % args.bpm)
        parse_command('pitch %r' % args.pitch)
        args.mac = bytes.fromhex(args.mac.replace(':', '').replace('-', '')) if args.mac else None
        if args.mac is not None and len(args.mac) != 6:
            raise ValueError('--mac needs six bytes')
        if args.ip:
            socket.inet_aton(args.ip)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    args.send = {kind for kind in args.send.split(',') if kind}
    if not args.send <= {'alive', 'status', 'beat'}:
        parser.error('--send takes alive, status and beat')
    return args


def main(argv):
    args = parse_args(argv[1:])
    if args.tell is not None:
        tell(args.tell, args.command)
        return 0
    log = open(args.log, 'a') if args.log else sys.stdout
    t0 = time.monotonic()

    def say(text):
        log.write('%8.3f %s\n' % (time.monotonic() - t0, text))
        log.flush()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    master = Master(args.number, args.name, args.ip, args.mac, args.bpm, args.pitch, args.speed, args.lock,
                    args.offset, not args.refuse_handoff, args.send, args.dump, args.verbose, t0, say)
    link = Segment(args.segment)
    try:
        commands = control_socket(args.control) if args.control else None
    except OSError as error:
        # Windows reserves whole port ranges (netsh int ipv4 show excludedportrange
        # protocol=udp). --tell must name the same port, so no fallback to a free one.
        sys.exit('tempo_master: cannot take commands on 127.0.0.1:%d (%s); the port is reserved or in use, '
                 'pick another for --control and --tell' % (args.control, error.strerror or error))
    say('tempo_master: player %d %r at %s (%s), %.2f BPM, pitch %+.2f %%, speed %s, on %s%s'
        % (master.number, master.name, master.ip, master.mac.hex(':'), args.bpm, args.pitch, args.speed,
           args.segment, ', commands on 127.0.0.1:%d' % args.control if args.control else ''))
    try:
        run(master, link, commands, args.seconds)
    except KeyboardInterrupt:
        pass
    say('tempo_master: stopped; sent %s; players heard: %s' % (
        ', '.join('%d %s' % (n, kind) for kind, n in sorted(master.sent.items())) or 'nothing',
        ', '.join('%d %r at %s' % (n, peer[2], peer[0]) for n, peer in sorted(master.peers.items())) or 'none'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
