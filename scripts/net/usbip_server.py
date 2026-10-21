#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Export the emulated deck's rear USB-B port to this computer over USB/IP.

The machine's built-in host (CDJ_USBF_HOST=<port>) enumerates the deck and
relays its endpoints as UDP datagrams to localhost, see usb_midi_port.py. This
server takes the other end of that socket and speaks the USB/IP protocol on
TCP, so a USB/IP client (usbip-win2 on Windows) attaches the deck as a real USB
device and the desktop's own drivers and applications talk to it.

  usage: python3 scripts/net/usbip_server.py [--port 27020] [--listen 127.0.0.1:3240]
                                             [--log LOGFILE [--payloads]]
  client: usbip.exe attach -r 127.0.0.1 -b 1-1

What goes where:

  control transfers  GET_DESCRIPTOR and every class or vendor request (HID
                     report descriptor, SET_IDLE, string descriptors) are
                     run on the deck: datagram 0x80 + setup (8) + OUT data, and
                     the reply 0x80 + virtual time (8) + status (1) + IN data.
                     SET_CONFIGURATION, SET_INTERFACE, CLEAR_FEATURE and the
                     status requests are answered here; the built-in host has
                     already configured the deck and the client must not do it
                     twice.
  bulk and interrupt IN data that arrives on an endpoint waits (at most
                     QUEUE_DEPTH packets) for the client's URB; OUT data goes
                     out at once as one datagram.
  isochronous        not modelled by the machine. IN URBs complete with silence
                     and OUT URBs are discarded, paced at one frame per
                     packet, so the audio driver sees a live device.

Every host -> deck packet is logged with --log (endpoint, report type, length;
with --payloads also its bytes), which is how a run counts the report types a
program such as Serato sends and reads what they carry.
"""
import argparse
import heapq
import socket
import struct
import sys
import threading
import time
from collections import deque

DEFAULT_PORT = 27020
DEFAULT_LISTEN = ('127.0.0.1', 3240)
BUSID = b'1-1'
BUSNUM, DEVNUM = 1, 2
SPEED_FULL = 2
QUEUE_DEPTH = 256
CONTROL_TIMEOUT = 8.0
CONTROL_ENDPOINT = 0x80

USBIP_VERSION = 0x0111
OP_REQ_IMPORT, OP_REP_IMPORT = 0x8003, 0x0003
OP_REQ_DEVLIST, OP_REP_DEVLIST = 0x8005, 0x0005
CMD_SUBMIT, CMD_UNLINK, RET_SUBMIT, RET_UNLINK = 1, 2, 3, 4
ECONNRESET, EPIPE, ETIMEDOUT = 104, 32, 110

OP_HEADER = struct.Struct('>HHI')
URB_HEADER = struct.Struct('>5I')
CMD_SUBMIT_BODY = struct.Struct('>5I8s')
ISO_DESCRIPTOR = struct.Struct('>4i')
DEVICE_FIELDS = struct.Struct('>256s32s3I3H6B')

REQ_GET_STATUS, REQ_CLEAR_FEATURE, REQ_SET_FEATURE = 0, 1, 3
REQ_GET_CONFIGURATION, REQ_SET_CONFIGURATION = 8, 9
REQ_GET_INTERFACE, REQ_SET_INTERFACE = 10, 11
DT_DEVICE, DT_CONFIG, DT_INTERFACE = 1, 2, 4
# bmAttributes bit 6 of the deck's configuration descriptor: self powered
SELF_POWERED = 1


class ProtocolError(Exception):
    pass


def read_exact(sock, n):
    chunks = []
    while n:
        chunk = sock.recv(n)
        if not chunk:
            raise ConnectionError('peer closed')
        chunks.append(chunk)
        n -= len(chunk)
    return b''.join(chunks)


# -- descriptors ---------------------------------------------------------------

def parse_descriptors(device, config):
    """The fields USB/IP reports about a device: (device info, interfaces).

    The device info is a dict, the interfaces a list of (class, subclass,
    protocol) in descriptor order, alternate settings other than 0 left out.
    """
    if len(device) < 18 or device[1] != DT_DEVICE:
        raise ProtocolError('not a device descriptor')
    info = {
        'vid': int.from_bytes(device[8:10], 'little'),
        'pid': int.from_bytes(device[10:12], 'little'),
        'bcd': int.from_bytes(device[12:14], 'little'),
        'class': device[4], 'subclass': device[5], 'protocol': device[6],
        'configs': device[17], 'config_value': config[5],
    }
    interfaces = []
    off = 0
    while off + 2 <= len(config) and config[off]:
        if config[off + 1] == DT_INTERFACE and config[off + 3] == 0:
            interfaces.append(tuple(config[off + 5:off + 8]))
        off += config[off]
    info['interfaces'] = interfaces
    return info


def config_total_length(head):
    return int.from_bytes(head[2:4], 'little')


# -- USB/IP encode and decode --------------------------------------------------

def op_header(code, status=0):
    return OP_HEADER.pack(USBIP_VERSION, code, status)


def device_record(info, with_interfaces):
    path = b'/emulated/cdj-2000nxs2'
    out = DEVICE_FIELDS.pack(path, BUSID, BUSNUM, DEVNUM, SPEED_FULL,
                             info['vid'], info['pid'], info['bcd'],
                             info['class'], info['subclass'], info['protocol'],
                             info['config_value'], info['configs'],
                             len(info['interfaces']))
    if with_interfaces:
        for cls, sub, proto in info['interfaces']:
            out += bytes([cls, sub, proto, 0])
    return out


def rep_devlist(info):
    if info is None:
        return op_header(OP_REP_DEVLIST) + struct.pack('>I', 0)
    return (op_header(OP_REP_DEVLIST) + struct.pack('>I', 1)
            + device_record(info, True))


def rep_import(info, busid):
    if info is None or busid.rstrip(b'\0') != BUSID:
        return op_header(OP_REP_IMPORT, 1)
    return op_header(OP_REP_IMPORT) + device_record(info, False)


def parse_op_request(sock):
    """(code, busid or None) of the next operation request."""
    version, code, _status = OP_HEADER.unpack(read_exact(sock, OP_HEADER.size))
    if version != USBIP_VERSION:
        raise ProtocolError('USB/IP version 0x%04x' % version)
    if code == OP_REQ_DEVLIST:
        return code, None
    if code == OP_REQ_IMPORT:
        return code, read_exact(sock, 32)
    raise ProtocolError('operation 0x%04x' % code)


class Submit:
    """One USBIP_CMD_SUBMIT."""

    def __init__(self, seqnum, devid, direction, ep, flags, length,
                 start_frame, packets, interval, setup, data, iso):
        self.seqnum, self.devid, self.direction, self.ep = seqnum, devid, direction, ep
        self.flags, self.length, self.start_frame = flags, length, start_frame
        self.packets, self.interval, self.setup = packets, interval, setup
        self.data, self.iso = data, iso

    @property
    def is_control(self):
        return self.ep == 0

    @property
    def is_in(self):
        # a control transfer's direction is the request's own
        return bool(self.setup[0] & 0x80) if self.is_control else self.direction == 1

    @property
    def is_iso(self):
        return self.packets not in (0, 0xFFFFFFFF)


def parse_urb(sock):
    """A Submit, or ('unlink', seqnum, unlink_seqnum), from the URB stream."""
    command, seqnum, devid, direction, ep = URB_HEADER.unpack(
        read_exact(sock, URB_HEADER.size))
    body = read_exact(sock, 28)
    if command == CMD_UNLINK:
        return ('unlink', seqnum, struct.unpack('>I', body[:4])[0])
    if command != CMD_SUBMIT:
        raise ProtocolError('URB command 0x%08x' % command)
    flags, length, start, packets, interval, setup = CMD_SUBMIT_BODY.unpack(body)
    data = read_exact(sock, length) if direction == 0 else b''
    iso = []
    if packets not in (0, 0xFFFFFFFF):
        raw = read_exact(sock, ISO_DESCRIPTOR.size * packets)
        iso = [ISO_DESCRIPTOR.unpack_from(raw, i * ISO_DESCRIPTOR.size)
               for i in range(packets)]
    return Submit(seqnum, devid, direction, ep, flags, length, start, packets,
                  interval, setup, data, iso)


def ret_submit(seqnum, status, data=b'', actual=None, iso=None, start_frame=0):
    """A USBIP_RET_SUBMIT; `iso` is [(offset, length, actual_length, status)]."""
    iso = iso or []
    if actual is None:
        actual = len(data)
    packets = len(iso) if iso else 0xFFFFFFFF
    errors = sum(1 for d in iso if d[3])
    out = URB_HEADER.pack(RET_SUBMIT, seqnum, 0, 0, 0)
    out += struct.pack('>iiiIi8x', status, actual, start_frame, packets, errors)
    out += data
    for d in iso:
        out += ISO_DESCRIPTOR.pack(*d)
    return out


def ret_unlink(seqnum, status):
    return URB_HEADER.pack(RET_UNLINK, seqnum, 0, 0, 0) + struct.pack('>i24x', status)


# -- the deck, over the UDP seam -----------------------------------------------

class UdpDeck:
    """The deck as the built-in host presents it.

    control() runs one control transfer on the deck. IN packets are queued per
    endpoint; send() writes an OUT packet. `on_in` is called when a packet is
    queued, `on_status` when the "configured" or "detached" line changes.
    """

    def __init__(self, sock, log=None):
        self.sock = sock
        self.log = log
        self.addr = None
        self.status_text = None
        self.fields = {}
        self.lock = threading.Lock()
        self.queues = {}
        self.on_in = lambda ep: None
        self.on_status = lambda: None
        self.control_reply = None
        self.control_ready = threading.Condition(self.lock)

    @property
    def configured(self):
        return self.status_text is not None and self.status_text.startswith('configured')

    def datagram(self, data, addr):
        if not data:
            return
        self.addr = addr
        ep = data[0]
        if ep == 0:
            self.status(data[1:].decode('ascii', 'replace'))
        elif ep == CONTROL_ENDPOINT:
            with self.lock:
                self.control_reply = (data[9] == 0, data[10:]) if len(data) > 9 else (False, b'')
                self.control_ready.notify_all()
        elif ep & 0x80 and len(data) > 9:
            with self.lock:
                q = self.queues.setdefault(ep, deque(maxlen=QUEUE_DEPTH))
                q.append(data[9:])
            if self.log:
                self.log(ep, data[9:])
            self.on_in(ep)

    def status(self, text):
        if text.startswith('hid_desc='):
            return
        if text == self.status_text:
            return
        self.status_text = text
        self.fields = dict(f.split('=', 1) for f in text.split() if '=' in f)
        print('deck: ' + text, flush=True)
        self.on_status()

    def pop_in(self, ep):
        with self.lock:
            q = self.queues.get(ep)
            return q.popleft() if q else None

    def send(self, ep, payload):
        if self.addr is None:
            return False
        self.sock.sendto(bytes([ep]) + payload, self.addr)
        if self.log:
            self.log(ep, payload)
        return True

    def control(self, setup, data=b''):
        """(ok, IN data); ok is False when the deck stalls or does not answer."""
        if self.addr is None:
            return False, b''
        with self.lock:
            self.control_reply = None
        if self.log:
            self.log(CONTROL_ENDPOINT, bytes(setup) + data)
        self.sock.sendto(bytes([CONTROL_ENDPOINT]) + bytes(setup) + data, self.addr)
        deadline = time.monotonic() + CONTROL_TIMEOUT
        with self.lock:
            while self.control_reply is None:
                left = deadline - time.monotonic()
                if left <= 0 or not self.control_ready.wait(left):
                    return False, b''
            return self.control_reply


# -- one attached client -------------------------------------------------------

class Session:
    """The URB stream of one attached client."""

    def __init__(self, conn, deck, info):
        self.conn, self.deck, self.info = conn, deck, info
        self.write_lock = threading.Lock()
        self.pending = {}               # seqnum -> Submit, IN URBs waiting for data
        self.state = threading.Lock()
        self.alt = {}
        self.configuration = info['config_value']
        self.control_queue = deque()
        self.control_wake = threading.Condition()
        self.timers = []
        self.timer_wake = threading.Condition()
        self.closed = False
        deck.on_in = self.deck_in

    def send(self, data):
        with self.write_lock:
            self.conn.sendall(data)

    # standard requests the machine has already settled
    def local_control(self, req):
        setup = req.setup
        kind, request = setup[0], setup[1]
        if kind & 0x60 or (kind & 0x1F) > 2:
            return None
        value = int.from_bytes(setup[2:4], 'little')
        index = int.from_bytes(setup[4:6], 'little')
        if request == REQ_SET_CONFIGURATION:
            self.configuration = value & 0xFF
            return b''
        if request == REQ_SET_INTERFACE:
            self.alt[index] = value
            return b''
        if request in (REQ_CLEAR_FEATURE, REQ_SET_FEATURE):
            return b''
        if request == REQ_GET_CONFIGURATION:
            return bytes([self.configuration])
        if request == REQ_GET_INTERFACE:
            return bytes([self.alt.get(index, 0)])
        if request == REQ_GET_STATUS:
            return bytes([SELF_POWERED if kind & 0x1F == 0 else 0, 0])
        return None

    def run_control(self, req):
        local = self.local_control(req)
        if local is not None:
            return self.finish(req, 0, local[:req.length] if req.is_in else b'')
        ok, data = self.deck.control(req.setup, req.data)
        if not ok:
            return self.finish(req, -EPIPE, b'')
        return self.finish(req, 0, data[:req.length] if req.is_in else b'',
                           actual=len(data) if req.is_in else len(req.data))

    def finish(self, req, status, data, actual=None):
        self.send(ret_submit(req.seqnum, status, data if req.is_in else b'',
                             actual=actual))

    def control_worker(self):
        while not self.closed:
            with self.control_wake:
                while not self.control_queue and not self.closed:
                    self.control_wake.wait(0.5)
                if not self.control_queue:
                    continue
                req = self.control_queue.popleft()
            try:
                self.run_control(req)
            except OSError:
                return

    def timer_worker(self):
        while not self.closed:
            with self.timer_wake:
                while not self.timers and not self.closed:
                    self.timer_wake.wait(0.5)
                if not self.timers:
                    continue
                due, _n, fn = self.timers[0]
                wait = due - time.monotonic()
                if wait > 0:
                    self.timer_wake.wait(wait)
                    continue
                heapq.heappop(self.timers)
            try:
                fn()
            except OSError:
                return

    def at(self, delay, fn):
        with self.timer_wake:
            heapq.heappush(self.timers, (time.monotonic() + delay, id(fn), fn))
            self.timer_wake.notify()

    def deck_in(self, ep):
        with self.state:
            for seqnum, req in list(self.pending.items()):
                if req.ep | 0x80 == ep:
                    data = self.deck.pop_in(ep)
                    if data is None:
                        return
                    del self.pending[seqnum]
                    break
            else:
                return
        try:
            self.send(ret_submit(seqnum, 0, data[:req.length]))
        except OSError:
            pass

    def submit_endpoint(self, req):
        ep_addr = req.ep | (0x80 if req.is_in else 0)
        if req.is_iso:
            return self.submit_iso(req)
        if not req.is_in:
            self.deck.send(ep_addr, req.data)
            return self.send(ret_submit(req.seqnum, 0, actual=len(req.data)))
        data = self.deck.pop_in(ep_addr)
        if data is not None:
            return self.send(ret_submit(req.seqnum, 0, data[:req.length]))
        with self.state:
            self.pending[req.seqnum] = req
        # a packet may have been queued between the pop and the registration
        self.deck_in(ep_addr)

    def submit_iso(self, req):
        iso, data = [], b''
        for offset, length, _actual, _status in req.iso:
            if req.is_in:
                data += bytes(length)
            iso.append((offset, length, length, 0))
        reply = ret_submit(req.seqnum, 0, data,
                           actual=len(data) if req.is_in else req.length,
                           iso=iso, start_frame=req.start_frame)
        # one full-speed frame is a millisecond
        self.at(len(iso) * 0.001, lambda: self.send(reply))

    def unlink(self, seqnum, target):
        with self.state:
            found = self.pending.pop(target, None)
        self.send(ret_unlink(seqnum, -ECONNRESET if found else 0))

    def serve(self):
        for fn in (self.control_worker, self.timer_worker):
            threading.Thread(target=fn, daemon=True).start()
        try:
            while True:
                req = parse_urb(self.conn)
                if isinstance(req, tuple):
                    self.unlink(req[1], req[2])
                elif req.is_control:
                    with self.control_wake:
                        self.control_queue.append(req)
                        self.control_wake.notify()
                else:
                    self.submit_endpoint(req)
        except (ConnectionError, OSError):
            pass
        finally:
            self.closed = True
            self.deck.on_in = lambda ep: None
            with self.control_wake:
                self.control_wake.notify_all()
            with self.timer_wake:
                self.timer_wake.notify_all()


# -- server --------------------------------------------------------------------

class Server:
    def __init__(self, deck):
        self.deck = deck
        self.info = None

    def read_descriptors(self):
        """Device and configuration descriptor, from the deck."""
        ok, device = self.deck.control(bytes([0x80, 6, 0, DT_DEVICE, 0, 0, 18, 0]))
        if not ok:
            return None
        ok, head = self.deck.control(bytes([0x80, 6, 0, DT_CONFIG, 0, 0, 9, 0]))
        if not ok:
            return None
        total = config_total_length(head)
        ok, config = self.deck.control(
            bytes([0x80, 6, 0, DT_CONFIG, 0, 0, total & 0xFF, total >> 8]))
        return parse_descriptors(device, config) if ok else None

    def device_info(self):
        if not self.deck.configured:
            self.info = None
        elif self.info is None:
            self.info = self.read_descriptors()
        return self.info

    def handle(self, conn):
        try:
            code, busid = parse_op_request(conn)
            info = self.device_info()
            if code == OP_REQ_DEVLIST:
                conn.sendall(rep_devlist(info))
                return
            reply = rep_import(info, busid)
            conn.sendall(reply)
            if len(reply) > OP_HEADER.size:
                print('client attached to %s' % BUSID.decode(), flush=True)
                Session(conn, self.deck, info).serve()
                print('client detached', flush=True)
        except (ConnectionError, ProtocolError, OSError) as e:
            print('client: %s' % e, flush=True)
        finally:
            conn.close()

    def listen(self, address, stop=lambda: False):
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(address)
        srv.listen(2)
        srv.settimeout(0.5)
        print('usbip: listening on %s:%d, bus id %s' % (*address, BUSID.decode()),
              flush=True)
        while not stop():
            try:
                conn, _ = srv.accept()
            except socket.timeout:
                continue
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            threading.Thread(target=self.handle, args=(conn,), daemon=True).start()


def make_logger(path, payloads=False):
    if not path:
        return None
    out = open(path, 'a', buffering=1)
    start = time.time()

    def log(ep, payload):
        data = ' ' + payload.hex() if payloads else ''
        if ep == CONTROL_ENDPOINT:
            request, value, index, length = struct.unpack('<xBHHH', payload[:8])
            out.write('%.3f control %02x request %02x value %04x index %04x length %d data %d%s\n' % (
                time.time() - start, payload[0], request, value, index, length,
                len(payload) - 8, data))
            return
        # byte 1 of a vendor HID report is its type (byte 0 is the deck); the
        # authentication reports (0xf0) carry their stage in byte 6
        hid = ep & 0x7f in (6, 7) and len(payload) > 1
        kind = '%02x' % payload[1] if hid else '--'
        stage = ' stage %02x' % payload[6] if hid and payload[1] == 0xF0 and len(payload) > 6 else ''
        out.write('%.3f ep%02x type %s len %d%s%s\n' % (time.time() - start, ep, kind,
                                                        len(payload), stage, data))
    return log


def receive_loop(sock, deck, stop=lambda: False):
    sock.settimeout(0.2)
    while not stop():
        try:
            deck.datagram(*sock.recvfrom(65535))
        except socket.timeout:
            pass


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=DEFAULT_PORT,
                    help='UDP port the machine sends to (default %d)' % DEFAULT_PORT)
    ap.add_argument('--listen', default='%s:%d' % DEFAULT_LISTEN,
                    help='USB/IP address (default %s:%d)' % DEFAULT_LISTEN)
    ap.add_argument('--log', help='append one line per host -> deck packet')
    ap.add_argument('--payloads', action='store_true',
                    help='with --log, add each packet\'s bytes in hex')
    args = ap.parse_args(argv[1:])

    host, _, port = args.listen.rpartition(':')
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1', args.port))
    deck = UdpDeck(sock, make_logger(args.log, args.payloads))
    threading.Thread(target=receive_loop, args=(sock, deck), daemon=True).start()
    try:
        Server(deck).listen((host or '127.0.0.1', int(port)))
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main(sys.argv)
