#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unpack the MAIN section (section 3) of a Pioneer .UPD.

Unlike the GUI section, this one is Motorola S-records, not a raw binary
blob: Pioneer's own bootloader at NOR offset 0, an emergency-boot updater
image at 0x10000, then the packed MAIN application at 0x50000, each record's
address a NOR flash offset.

    [32-byte ASCII label]   "CDJ-2000NXS2MAINVer1.87        0"
    [S-record text]         S1/S2/S3 lines, CRLF-terminated
    [S7 record]             execution start address
    [2-byte trailer]        CRC-16/XMODEM (see gui_encode.crc16_xmodem),
                            little-endian -- MAIN is a little-endian SH7724,
                            where the GUI section's own trailer (a big-endian
                            SH7269) is stored big-endian

At NOR offset MAIN_IMG_OFF the reconstructed flat bytes hold:

    [4-byte LE length]      of the LZSS stream that follows
    [LZSS stream]           the same codec as the GUI section (lzss_decode.py),
                            little-endian length because MAIN is little-endian
                            where the GUI's SH7269 is big-endian
    [2-byte LE checksum]    the 16-bit sum of every byte from the length
                            field through the end of the stream -- this is
                            the one Pioneer's own IPL recomputes and checks
                            before it will decompress and run the image

    usage: main_decode.py <section3.bin|*.UPD> [outfile]
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzss_decode import unpack

MAIN_IMG_OFF = 0x50000
MAIN_RING_INIT = 0x20
MAIN_RING_POS = 4078
LABEL_SIZE = 32


def srecords(body):
    """(address, data, raw line) for every S1/S2/S3 line in the section's
    body (the part after its 32-byte label)."""
    for line in body.split(b'\r\n'):
        if line[:1] != b'S' or line[1:2] not in (b'1', b'2', b'3'):
            continue
        rec = bytes.fromhex(line[2:].decode('ascii'))
        if sum(rec) & 0xFF != 0xFF:
            raise ValueError('bad S-record checksum: %r' % line[:24])
        abytes = int(line[1:2]) + 1
        addr = int.from_bytes(rec[1:1 + abytes], 'big')
        yield addr, rec[1 + abytes:-1], line


def decode_image(flat, off=MAIN_IMG_OFF):
    """(declared stream length, decoded image) from a flat NOR/flash buffer
    (section3's own address space, or a real flash.bin) with the packed
    length+LZSS+checksum MAIN application at file offset `off`."""
    declared = struct.unpack_from('<I', flat, off)[0]
    stream_off = off + 4
    image = unpack(bytes(flat[:stream_off + declared]), stream_off,
                   MAIN_RING_INIT, MAIN_RING_POS)
    return declared, image


def main_section_from_upd(data):
    """Carve the MAIN section out of a .UPD (manifest of CRLF decimal
    lengths); section 3 of the CDJ-2000NXS2's container."""
    sizes, pos = [], 0
    while True:
        m = re.match(rb'(\d+)\r\n', data[pos:pos + 32])
        if not m:
            break
        sizes.append(int(m.group(1)))
        pos += m.end()
    if len(sizes) < 3:
        return None
    off = pos + sum(sizes[:2])
    return data[off:off + sizes[2]]


def decode(section):
    """(label, declared length, available bytes, image) -- the image is the
    same flat, 0x08000000-based bytes patch_main.py works on."""
    label = section[:LABEL_SIZE].rstrip(b'\x00 ').decode('latin1')
    body = section[LABEL_SIZE:]
    chunks = list(srecords(body))
    if not any(addr == MAIN_IMG_OFF for addr, _, _ in chunks):
        raise SystemExit('main_decode: no S-record at 0x%x (the image length field)' % MAIN_IMG_OFF)
    hi = max(addr + len(data) for addr, data, _ in chunks if addr >= MAIN_IMG_OFF)
    flat = bytearray([0xFF]) * (hi - MAIN_IMG_OFF)
    for addr, data, _ in chunks:
        if addr >= MAIN_IMG_OFF:
            flat[addr - MAIN_IMG_OFF:addr - MAIN_IMG_OFF + len(data)] = data
    declared, image = decode_image(flat, 0)
    avail = len(flat) - 4
    return label, declared, avail, image


def main():
    path = sys.argv[1]
    data = open(path, 'rb').read()
    section = main_section_from_upd(data) if data[:1].isdigit() else data
    if section is None:
        sys.exit('not a .UPD and not a section blob')

    label, declared, avail, image = decode(section)
    print('label        : %s' % label)
    print('LE32 length  : %d  (payload available %d)' % (declared, avail))
    print('decoded      : %d bytes (0x%x)' % (len(image), len(image)))

    outfile = sys.argv[2] if len(sys.argv) > 2 else 'extract/main_unpacked.bin'
    open(outfile, 'wb').write(image)
    print('wrote %s' % outfile)


if __name__ == '__main__':
    main()
