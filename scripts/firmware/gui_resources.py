#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Extract the GUI board's resource archive (fonts and artwork).

The resource table at 0x0E598DC8 declares six (base, size, type=3) blobs
spanning 0x57C099 bytes of SDRAM from 0x0DF00000, and the runtime font slots at
0x0F682210 hold those six bases. Nothing in the load table writes that memory,
so the board model injects it from this archive (CDJ_GUI_FONTBLOB).

The archive is at file offset 0x440000 of gui_unpacked.bin: a 4-byte
big-endian compressed length, then the boot loader's LZSS (lzss_decode.py).
The decoder overruns the end of the stream by 0.3%, so the output is truncated
to the declared span.

usage:
    gui_resources.py [out.bin]      write the archive (default extract/resblob.bin)
    gui_resources.py --render ABC   render those characters as ASCII art
"""
import sys

import gui_address as G
import lzss_decode as L

REGION = 0x440000        # file offset of the archive, inside the unloaded tail
SPAN = 0x57C099          # the declared resource span at 0x0DF00000
# The glyph cell format, as the firmware reads it.
CELL_W, CELL_H, CELL_BYTES = 28, 27, 189
SHADES = ' .:#'


def archive():
    img = G.img()
    return bytes(L.unpack(img, REGION + 4)[:SPAN])


def render(blob, ch):
    """Bank 0 maps character code 0x20+n to blob + n*189, 2bpp, MSB first."""
    n = ord(ch) - 0x20
    cell = blob[n * CELL_BYTES:(n + 1) * CELL_BYTES]
    rows = []
    bit = 0
    for _ in range(CELL_H):
        row = ''
        for _ in range(CELL_W):
            byte = cell[bit >> 3] if (bit >> 3) < len(cell) else 0
            row += SHADES[(byte >> (6 - (bit & 7))) & 3]
            bit += 2
        rows.append(row)
    return rows


if __name__ == '__main__':
    blob = archive()
    if len(sys.argv) > 1 and sys.argv[1] == '--render':
        for ch in sys.argv[2]:
            print('--- %r ---' % ch)
            for r in render(blob, ch):
                print('  |%s|' % r)
    else:
        out = sys.argv[1] if len(sys.argv) > 1 else 'extract/resblob.bin'
        open(out, 'wb').write(blob)
        print('wrote %s  0x%X bytes' % (out, len(blob)))
