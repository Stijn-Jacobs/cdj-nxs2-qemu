#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Minimal USB/IP host for checking the deck without usbip-win2.

Talks to usbip_server.py the way a desktop does on attach: reads the device and
configuration descriptors, selects configuration 1, then sends one 64-byte
vendor HID OUT report on endpoint 6 and waits for whatever the deck answers on
the HID IN endpoint 7.

  usage: python3 scripts/net/usbip_fakehost.py [--server 127.0.0.1:3240] [--type 0x21]
"""
import argparse
import socket
import struct
import sys

import usbip_server as us

HID_OUT_EP = 6
HID_IN_EP = 7
REPORT_LEN = 64


def submit(conn, seq, ep, direction, length, setup=bytes(8), data=b''):
    header = us.URB_HEADER.pack(us.CMD_SUBMIT, seq, 0x00010002, direction, ep)
    body = us.CMD_SUBMIT_BODY.pack(0, length, 0, 0xFFFFFFFF, 0, setup)
    conn.sendall(header + body + data)
    command, rseq, *_ = us.URB_HEADER.unpack(us.read_exact(conn, 20))
    ret = us.read_exact(conn, 28)
    status, actual = struct.unpack('>ii', ret[:8])
    data_in = us.read_exact(conn, actual) if direction and actual > 0 else b''
    assert (command, rseq) == (us.RET_SUBMIT, seq), 'unexpected reply'
    return status, data_in


def attach(address):
    conn = socket.create_connection(address)
    conn.sendall(us.op_header(us.OP_REQ_IMPORT) + us.BUSID.ljust(32, b'\0'))
    _, _, status = us.OP_HEADER.unpack(us.read_exact(conn, 8))
    if status:
        raise SystemExit('import refused: the deck is not configured yet')
    us.read_exact(conn, us.DEVICE_FIELDS.size)
    return conn


def run(address, report_type):
    conn = attach(address)
    status, device = submit(conn, 1, 0, 1, 18, bytes.fromhex('80 06 00 01 00 00 12 00'))
    print('device descriptor status %d, %d bytes' % (status, len(device)))
    status, config = submit(conn, 2, 0, 1, 255, bytes.fromhex('80 06 00 02 00 00 ff 00'))
    print('configuration descriptor status %d, %d bytes' % (status, len(config)))
    status, _ = submit(conn, 3, 0, 0, 0, bytes.fromhex('00 09 01 00 00 00 00 00'))
    print('set configuration status %d' % status)
    report = bytes([1, report_type]) + bytes(REPORT_LEN - 2)
    status, _ = submit(conn, 4, HID_OUT_EP, 0, REPORT_LEN, data=report)
    print('hid out report 0x%02x status %d' % (report_type, status))
    conn.settimeout(3)
    try:
        status, answer = submit(conn, 5, HID_IN_EP, 1, REPORT_LEN)
        print('hid in status %d: %s' % (status, answer.hex()))
    except socket.timeout:
        print('hid in: no answer within 3 s')


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--server', default='%s:%d' % us.DEFAULT_LISTEN)
    ap.add_argument('--type', type=lambda text: int(text, 0), default=0x21,
                    help='second byte of the HID OUT report (default 0x21)')
    args = ap.parse_args(argv)
    host, port = args.server.rsplit(':', 1)
    run((host, int(port)), args.type)


if __name__ == '__main__':
    main(sys.argv[1:])
