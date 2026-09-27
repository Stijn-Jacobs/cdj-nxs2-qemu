#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Re-encode the GUI (display) firmware section for a Pioneer .UPD: the
inverse of gui_decode.py.

    [32-byte ASCII header]  unchanged -- a code patch does not change what
                            section or sub-version this is
    [BE32 payload length]   recomputed from the new LZSS stream
    [LZSS stream]           lzss_encode.pack() of the (possibly patched) image
    [2-byte trailer]        recomputed, see below

The trailer is CRC-16/XMODEM (poly 0x1021, init 0, no reflect, no xorout)
over everything before it -- header, length field and stream together.
Confirmed against the real C2KNXS2.UPD v1.87 GUI section: its last two bytes
are exactly that CRC of the rest of the section.

    usage: gui_encode.py <original section1.bin> <gui_unpacked.bin> <out section1.bin>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzss_encode import pack

STREAM_OFF = 0x24

_CRC_TABLE = []
for _b in range(256):
    _crc = _b << 8
    for _ in range(8):
        _crc = ((_crc << 1) ^ 0x1021) & 0xFFFF if _crc & 0x8000 else (_crc << 1) & 0xFFFF
    _CRC_TABLE.append(_crc)


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC_TABLE[((crc >> 8) ^ b) & 0xFF]
    return crc


def encode(header32, decoded):
    """header32: the original section's first 32 bytes, unchanged. decoded:
    the (possibly patched) raw image bytes. Returns the new section, ready to
    take the original's place in a .UPD."""
    stream = pack(decoded)
    body = header32 + struct.pack('>I', len(stream)) + stream
    return body + struct.pack('>H', crc16_xmodem(body))


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    orig = open(sys.argv[1], 'rb').read()
    decoded = open(sys.argv[2], 'rb').read()
    out = encode(orig[:32], decoded)
    with open(sys.argv[3], 'wb') as f:
        f.write(out)
    print('gui_encode: %d bytes (was %d), stream %d bytes'
          % (len(out), len(orig), len(out) - STREAM_OFF - 2))


if __name__ == '__main__':
    main()
