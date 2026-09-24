#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract the GUI board's artwork archive (the bitmaps, not the glyphs).

0x0E517992, image_resource_lookup(index), indexes a table of 1435 44-byte
records and resolves each record's pixel pointer as

    record[+0x0C] = 0x0CD00000 + record[+0x20]

Nothing in the boot load table fills the 0x0CD00000 window, so the board model
injects it from this archive (CDJ_GUI_ARTBLOB), as it does the font window at
0x0DF00000 (CDJ_GUI_FONTBLOB).

The archive sits at file offset 0x150000 of gui_unpacked.bin, framed like the
font archive: a 4-byte big-endian compressed length, then the boot loader's
LZSS. The decoded directory checks out: record[+0x1C] == index * 0x2C, data
offsets chain by w*h*2 (16bpp), and every record's +0x24 holds 0x00FAF850, the
decoded size. Record 0 is 688x198 with 0xF81F (magenta) as the chroma key.

usage:
    gui_artwork.py [out.bin]      write the archive (default extract/artblob.bin)
    gui_artwork.py --list N       print the first N resource records
"""
import struct
import sys

import lzss_decode as L

REGION = 0x150000        # file offset of the artwork archive
RECSIZE = 0x2C
# 0x0DF00000 - 0x0CD00000: the artwork window cannot reach the glyph window.
MAXSPAN = 0x0DF00000 - 0x0CD00000


def archive():
    """Decode the archive, with the input sliced to the declared length.

    Unbounded, the decoder runs through the 0xFF erase padding and on into the
    font archive at 0x440000. Bounded, it stops at 0x00FAF850, the size every
    directory record declares.
    """
    img = open('extract/gui_unpacked.bin', 'rb').read()
    clen = struct.unpack_from('>I', img, REGION)[0]
    end = REGION + 4 + clen
    print('region 0x%X  compressed 0x%X  ends 0x%X  (zeros after: %s)'
          % (REGION, clen, end, not any(img[end:end + 4096])), file=sys.stderr)
    d = bytes(L.unpack(img[:end], REGION + 4))
    want = struct.unpack_from('>I', d, 0x18 + 12)[0] if len(d) > 0x28 else 0
    print('decoded 0x%X bytes; records declare 0x%X  (agree: %s)'
          % (len(d), want, len(d) == want), file=sys.stderr)
    return d[:MAXSPAN]


def records(d, n):
    """Print the directory.

    The directory starts at +0x18, and the dimensions are packed as
    (width << 16) | height at record offset +0x18.
    """
    for i in range(n):
        o = 0x18 + i * RECSIZE
        if o + RECSIZE > len(d):
            break
        off = struct.unpack_from('>I', d, o + 0x08)[0]
        wh = struct.unpack_from('>I', d, o + 0x18)[0]
        total = struct.unpack_from('>I', d, o + 0x0C)[0]
        w, h = wh >> 16, wh & 0xFFFF
        print('  rec%-5d %4dx%-4d data=+0x%08X  archive-size=0x%X'
              % (i, w, h, off, total))


if __name__ == '__main__':
    if '--list' in sys.argv:
        n = int(sys.argv[sys.argv.index('--list') + 1])
        records(archive(), n)
    else:
        out = sys.argv[1] if len(sys.argv) > 1 else 'extract/artblob.bin'
        d = archive()
        open(out, 'wb').write(d)
        print('%s  %d bytes' % (out, len(d)))
