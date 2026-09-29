#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Re-encode the MAIN section (section 3) for a Pioneer .UPD: the inverse of
main_decode.py.

The bootloader/emergency-updater S-records (every address below
MAIN_IMG_OFF) are carried over unchanged; only the packed application at
MAIN_IMG_OFF is replaced, at whatever length the new (possibly patched)
image compresses to -- the write-target window the real updater erases
before writing is far larger than either image, so a few hundred bytes'
difference from re-encoding changes nothing else in it. See main_decode.py
for the section layout and the two checksums involved.

    usage: main_encode.py <original section3.bin> <main_unpacked.bin> <out section3.bin>
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzss_encode import pack
from gui_encode import crc16_xmodem
from main_decode import srecords, MAIN_IMG_OFF, LABEL_SIZE

RECORD_LEN = 32          # data bytes per emitted S-record line


def encode_image(image):
    """[4-byte LE length][LZSS stream][2-byte LE checksum], ready to place at
    MAIN_IMG_OFF in a flat NOR/flash buffer."""
    stream = pack(image)
    packed = struct.pack('<I', len(stream)) + stream
    return packed + struct.pack('<H', sum(packed) & 0xFFFF)


def _srecord(addr, data):
    body = bytes([len(data) + 4]) + addr.to_bytes(3, 'big') + data
    checksum = (~sum(body)) & 0xFF
    return ('S2' + body.hex().upper() + '%02X' % checksum).encode('ascii')


def encode(section, image):
    """section: the original MAIN section this update carried (its prefix
    S-records and S7 terminator are reused verbatim). image: the
    (possibly patched) flat bytes patch_main.py works on. Returns the new
    section, ready to take the original's place in a .UPD."""
    label = section[:LABEL_SIZE]
    body = section[LABEL_SIZE:]
    prefix = [line for addr, _, line in srecords(body) if addr < MAIN_IMG_OFF]
    term = next((line for line in body.split(b'\r\n') if line[:2] == b'S7'), None)
    if term is None:
        raise SystemExit('main_encode: no S7 termination record in the section')

    packed = encode_image(image)
    lines = list(prefix)
    for off in range(0, len(packed), RECORD_LEN):
        chunk = packed[off:off + RECORD_LEN]
        if len(chunk) < RECORD_LEN:
            chunk += b'\xff' * (RECORD_LEN - len(chunk))
        lines.append(_srecord(MAIN_IMG_OFF + off, chunk))
    lines.append(term)

    body_out = b'\r\n'.join(lines) + b'\r\n'
    out = label + body_out
    return out + struct.pack('<H', crc16_xmodem(out))


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    orig = open(sys.argv[1], 'rb').read()
    image = open(sys.argv[2], 'rb').read()
    out = encode(orig, image)
    with open(sys.argv[3], 'wb') as f:
        f.write(out)
    print('main_encode: %d bytes (was %d)' % (len(out), len(orig)))


if __name__ == '__main__':
    main()
