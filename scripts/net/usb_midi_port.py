#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn the deck's rear USB-B port into a MIDI port on this computer.

The machine's USB device port enumerates against a small host inside QEMU,
which relays each endpoint's transfers as one UDP datagram to localhost: byte 0
is the endpoint address (bit 7 set = IN, deck -> host), the rest is the raw
payload (deck -> host payloads start with 8 bytes, little-endian: the deck's
virtual time in nanoseconds), and endpoint 0 carries text status lines ("configured vid=... pid=...
midi_in=85 midi_out=04 ...", "detached"). The MIDI endpoints carry USB-MIDI
event packets; this decodes them into MIDI messages and the other way round.

  usage: python3 scripts/net/usb_midi_port.py [--port 27020] [--port-name NAME]
                                              [--in-port-name NAME] [--latency MS]
         python3 scripts/net/usb_midi_port.py --monitor   one line per message
         python3 scripts/net/usb_midi_port.py --count     per-second counts by status byte

Deck 1 sends from port 27020, deck 2 from 27021. Without --monitor/--count the
messages go to a MIDI output port and whatever arrives on the matching input
port goes back to the deck. On macOS and Linux that port is a virtual one named
"CDJ-2000NXS2 MIDI"; Windows cannot create virtual ports, so it opens an
existing one whose name contains --port-name (a loopMIDI port). A loopMIDI port
loops what is written to it back to its own input, so on Windows the return
path stays off unless --in-port-name names a second port.

The emulator runs in bursts against wall-clock time, so the messages arrive
unevenly. Towards a MIDI port each one is held until its virtual time, shifted
by a fixed --latency buffer, so the host sees the deck's own timing and tempo.
"""
import argparse
import socket
import sys
import threading
import time
from collections import deque

DEFAULT_PORT = 27020
DEFAULT_PORT_NAME = 'CDJ-2000NXS2 MIDI'

# USB MIDI 1.0 section 4: bytes carried by each code index number
CIN_LENGTH = {0x2: 2, 0x3: 3, 0x4: 3, 0x5: 1, 0x6: 2, 0x7: 3, 0x8: 3, 0x9: 3,
              0xA: 3, 0xB: 3, 0xC: 2, 0xD: 2, 0xE: 3, 0xF: 1}
SYSEX_START, SYSEX_END = 0xF0, 0xF7
# code index of a system common message, by status byte
COMMON_CIN = {0xF1: 0x2, 0xF2: 0x3, 0xF3: 0x2, 0xF6: 0x5}


def decode_usb_midi(payload, sysex=None):
    """[(cable, message bytes)] from a run of 4-byte event packets.

    A sysex message continues across packets, and across transfers: pass the
    same dict to every call so the unfinished ones are picked up again.
    """
    sysex = {} if sysex is None else sysex
    messages = []
    for i in range(0, len(payload) - 3, 4):
        cable, cin = payload[i] >> 4, payload[i] & 0x0F
        length = CIN_LENGTH.get(cin)
        if length is None:
            continue
        data = payload[i + 1:i + 1 + length]
        if cin == 0x4 or cable in sysex:
            sysex[cable] = sysex.get(cable, b'') + data
            if data[-1] != SYSEX_END or cin == 0x4:
                continue
            data = sysex.pop(cable)
        messages.append((cable, bytes(data)))
    return messages


def encode_usb_midi(message, cable=0):
    """The event packets that carry one MIDI message."""
    if message[0] == SYSEX_START:
        packets = bytearray()
        for i in range(0, len(message), 3):
            chunk = message[i:i + 3]
            last = i + 3 >= len(message)
            cin = {1: 0x5, 2: 0x6, 3: 0x7}[len(chunk)] if last else 0x4
            packets += bytes([cable << 4 | cin]) + chunk.ljust(3, b'\0')
        return bytes(packets)
    status = message[0]
    if status >= 0xF8:
        cin = 0xF
    elif status >= 0xF0:
        cin = COMMON_CIN[status]
    else:
        cin = status >> 4
    return bytes([cable << 4 | cin]) + message.ljust(3, b'\0')


class Counter:
    """Messages seen this second, by status byte."""

    def __init__(self, out=sys.stdout):
        self.out = out
        self.seen = {}
        self.second = int(time.time())

    def add(self, cable, message, vt):
        self.seen[message[0]] = self.seen.get(message[0], 0) + 1

    def tick(self):
        now = int(time.time())
        if now == self.second:
            return
        self.second = now
        if self.seen:
            print(' '.join('%02X x%d' % kv for kv in sorted(self.seen.items())),
                  file=self.out, flush=True)
            self.seen = {}


def print_message(cable, message, vt):
    now = time.time()
    print('%s.%03d vt %.6f cable %d  %s' % (time.strftime('%H:%M:%S', time.localtime(now)),
                                            now % 1 * 1000, vt / 1e9, cable,
                                            message.hex(' ').upper()),
          flush=True)


class Replay:
    """Sends each message at its virtual time, plus a fixed latency.

    The first message, and every Start and Continue, pin virtual time to the wall
    clock. The pin is moved again when a message arrives after its time, or would
    wait more than twice the latency, which is the emulator running slower or
    faster than real time.
    """

    def __init__(self, send, latency=0.05, now=time.perf_counter):
        self.send = send
        self.latency = latency
        self.now = now
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.queue = deque()
        self.pin = None
        self.last_due = 0.0

    def add(self, vt, message):
        with self.lock:
            now = self.now()
            due = self.pin and self.pin[0] + (vt - self.pin[1]) / 1e9
            if (not self.pin or message[0] in (0xFA, 0xFB)
                    or due < now or due > now + 2 * self.latency):
                self.pin = (now + self.latency, vt)
                due = self.pin[0]
            self.last_due = due = max(due, self.last_due)
            self.queue.append((due, message))
        self.wake.set()

    def step(self):
        """Send what is due; return the wait until the next one, None if empty."""
        while True:
            with self.lock:
                if not self.queue:
                    return None
                wait = self.queue[0][0] - self.now()
                if wait > 0:
                    return wait
                message = self.queue.popleft()[1]
            self.send(message)

    def run(self, stop=lambda: False):
        while not stop():
            self.wake.clear()
            wait = self.step()
            if wait is None:
                self.wake.wait(0.01)
            elif wait > 0.002:
                # Windows sleeps overshoot by a few ms, so the last 2 ms spin
                self.wake.wait(wait - 0.002)


class Deck:
    """The datagram side: who the deck is, and which endpoint carries MIDI."""

    def __init__(self, on_message):
        self.on_message = on_message
        self.addr = None
        self.midi_in = 0x85
        self.midi_out = 0x04
        self.sysex = {}
        self.last_status = None

    def datagram(self, data, addr):
        self.addr = addr
        if data[0] == 0:
            self.status(data[1:].decode('ascii', 'replace'))
        elif data[0] == self.midi_in:
            vt = int.from_bytes(data[1:9], 'little')
            for cable, message in decode_usb_midi(data[9:], self.sysex):
                self.on_message(cable, message, vt)

    def status(self, text):
        # the machine repeats it every second
        if text == self.last_status:
            return
        self.last_status = text
        print('deck: ' + text, flush=True)
        fields = dict(f.split('=', 1) for f in text.split() if '=' in f)
        self.midi_in = int(fields.get('midi_in', '85'), 16)
        self.midi_out = int(fields.get('midi_out', '04'), 16)

    def send(self, sock, message):
        if self.addr and self.midi_out:
            sock.sendto(bytes([self.midi_out]) + encode_usb_midi(message), self.addr)


def serve(sock, deck, poll=None, stop=lambda: False):
    """Feed the deck's datagrams to `deck`; call `poll` every 2 ms or so."""
    sock.settimeout(0.002)
    while not stop():
        try:
            deck.datagram(*sock.recvfrom(65535))
        except socket.timeout:
            pass
        if poll:
            poll()


def open_midi(args):
    """(output, input or None) for the MIDI side; exits when there is no port."""
    try:
        import mido
    except ImportError:
        sys.exit('mido is missing:  python -m pip install mido python-rtmidi')

    if sys.platform != 'win32':
        return (mido.open_output(args.port_name, virtual=True),
                mido.open_input(args.port_name, virtual=True))

    def find(names, want):
        return next((n for n in names if want.lower() in n.lower()), None)

    out_name = find(mido.get_output_names(), args.port_name)
    if not out_name:
        sys.exit('no MIDI output port named like %r -- Windows cannot create one: '
                 'make a loopMIDI port called "%s" (tobias-erichsen.de/software/loopmidi.html)'
                 % (args.port_name, args.port_name))
    in_name = args.in_port_name and find(mido.get_input_names(), args.in_port_name)
    if args.in_port_name and not in_name:
        sys.exit('no MIDI input port named like %r' % args.in_port_name)
    return mido.open_output(out_name), (mido.open_input(in_name) if in_name else None)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=DEFAULT_PORT,
                    help='UDP port the machine sends to (default %d; deck 2 is one higher)'
                         % DEFAULT_PORT)
    ap.add_argument('--port-name', default=DEFAULT_PORT_NAME,
                    help='MIDI port to create, or on Windows a substring of an existing one')
    ap.add_argument('--in-port-name', default=None,
                    help='Windows only: a second port whose messages go back to the deck')
    ap.add_argument('--latency', type=float, default=50,
                    help='milliseconds each message is held to even out the '
                         "emulator's bursts (default 50)")
    ap.add_argument('--monitor', action='store_true',
                    help='print every message, send nothing to a MIDI port')
    ap.add_argument('--count', action='store_true',
                    help='print message counts by status byte once a second')
    args = ap.parse_args(argv[1:])

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', args.port))

    if args.count:
        counter = Counter()
        deck, poll = Deck(counter.add), counter.tick
    elif args.monitor:
        deck, poll = Deck(print_message), None
    else:
        import mido
        midi_out, midi_in = open_midi(args)

        replay = Replay(lambda message: midi_out.send(mido.Message.from_bytes(list(message))),
                        args.latency / 1000)
        threading.Thread(target=replay.run, daemon=True).start()
        deck = Deck(lambda cable, message, vt: replay.add(vt, message))

        def poll():
            for msg in midi_in.iter_pending() if midi_in else ():
                deck.send(sock, bytes(msg.bytes()))
    print('usb_midi_port: 127.0.0.1:%d' % args.port, flush=True)
    serve(sock, deck, poll)


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv))
    except KeyboardInterrupt:
        pass
