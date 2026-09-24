#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""GUI image address <-> file offset, plus raw read helpers.

The GUI boot loader scatters gui_unpacked.bin into nine disjoint load segments
(the same table CdjGuiMap.java uses).  Every static question about the GUI board
starts by translating one of those addresses, so it lives in one place.

usage:
    gui_address.py a2o <addr>            address -> file offset
    gui_address.py o2a <off>             file offset -> address(es)
    gui_address.py hex <addr> [len]      hexdump at an address
    gui_address.py u16 <addr> [maxchars] read a UTF-16BE NUL-terminated string
    gui_address.py words <addr> [n]      n big-endian 32-bit words
"""
import sys

IMG = 'extract/gui_unpacked.bin'

# {load address, length, file offset} -- file 0x006FEC..0x03137E loads TWICE.
SEG = [
    (0x0E500000, 0x0003127E, 0x000100),
    (0x0E531280, 0x0005B430, 0x031380),
    (0x0E58C6B0, 0x0002AA38, 0x08C7B0),
    (0xFFF84000, 0x00000190, 0x0B71E8),
    (0xFFF88000, 0x00030070, 0x006FEC),
    (0x0FFECDE4, 0x000000B8, 0x0B7378),
    (0x1C000000, 0x00010126, 0x0B7430),
    (0x1C010128, 0x00030C08, 0x0C7558),
    (0x1C040D30, 0x000012E8, 0x0F8160),
]


def a2o(addr):
    for base, ln, off in SEG:
        if base <= addr < base + ln:
            return off + (addr - base)
    return None


def o2a(off):
    return [base + (off - o) for base, ln, o in SEG if o <= off < o + ln]


def img():
    return open(IMG, 'rb').read()


def read(addr, n):
    o = a2o(addr)
    if o is None:
        raise SystemExit('address %#x is not in any load segment' % addr)
    return img()[o:o + n]


def u16str(addr, maxchars=256, little=False):
    """UTF-16 string. The flash string tables are stored LITTLE-endian even
    though the CPU is big-endian, so `little=True` is the right call for
    anything read out of the image; RAM buffers hold the byte-swapped copy."""
    b = read(addr, maxchars * 2)
    out = []
    for i in range(0, len(b) - 1, 2):
        c = (b[i] | (b[i + 1] << 8)) if little else ((b[i] << 8) | b[i + 1])
        if c == 0:
            break
        out.append(chr(c))
    return ''.join(out)


def word(addr):
    return int.from_bytes(read(addr, 4), 'big')


def _hexdump(addr, n):
    b = read(addr, n)
    for i in range(0, len(b), 16):
        row = b[i:i + 16]
        txt = ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)
        print('%08X  %-47s  %s' % (addr + i, ' '.join('%02x' % c for c in row), txt))


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'a2o':
        print('%#x' % a2o(int(sys.argv[2], 0)))
    elif cmd == 'o2a':
        for a in o2a(int(sys.argv[2], 0)):
            print('%08X' % a)
    elif cmd == 'hex':
        _hexdump(int(sys.argv[2], 0), int(sys.argv[3], 0) if len(sys.argv) > 3 else 64)
    elif cmd == 'u16':
        print(repr(u16str(int(sys.argv[2], 0),
                          int(sys.argv[3], 0) if len(sys.argv) > 3 else 256,
                          little='--le' in sys.argv)))
    elif cmd == 'words':
        a = int(sys.argv[2], 0)
        n = int(sys.argv[3], 0) if len(sys.argv) > 3 else 8
        for i in range(n):
            print('%08X: %08X' % (a + 4 * i, word(a + 4 * i)))
    else:
        raise SystemExit(__doc__)
