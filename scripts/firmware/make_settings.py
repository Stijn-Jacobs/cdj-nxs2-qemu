#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthesise the CDJ-2000NXS2 flash settings sector.

Layout recovered from the firmware:

  sector table  @0x080B0978 : entry[0x208/4] = 0x007F6000
  size table    @0x080B0B94 : entry[0x208/4] = 0x00002000
  bounds check  @0x0035152E : 0xA07F6000 .. 0xA07F7FFF   (P2 view)

  record format @0x00351916..0x00351942:
      write16(off,     key)
      write16(off + 2, value)
  i.e. packed [key:u16][value:u16] little-endian, appended sequentially.
  A reader scans 16-bit words for 0xFFFF to find the first free slot, which is
  the erased-NOR terminator.

usage:  make_settings.py <out.bin> [key=value ...]      keys/values in hex or dec
"""
import sys, struct

REGION_SIZE = 0x2000          # 8 KB parameter sector
ERASED = 0xFF

# Key observed being queried during init; main() bails out when its lookup
# returns the same value the (zeroed) DRAM cache already holds, so any non-zero
# value breaks the 0 == 0 tie that causes the reboot loop.
DEFAULT_SETTINGS = [
    (0x7101, 0x0001),
]


def build(settings):
    buf = bytearray([ERASED]) * REGION_SIZE
    off = 0
    for key, val in settings:
        struct.pack_into('<HH', buf, off, key, val)
        off += 4
    # everything past the last record stays 0xFF, so the first u16 the scanner
    # meets after the records is 0xFFFF -- the terminator it looks for.
    return bytes(buf)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    out = sys.argv[1]
    settings = list(DEFAULT_SETTINGS)
    for arg in sys.argv[2:]:
        k, _, v = arg.partition('=')
        settings.append((int(k, 0), int(v, 0)))

    blob = build(settings)
    with open(out, 'wb') as f:
        f.write(blob)

    print(f"wrote {out}: {len(blob)} bytes for flash 0x7F6000-0x7F7FFF")
    for k, v in settings:
        print(f"  key 0x{k:04X} = 0x{v:04X}")
    print(f"  terminator 0xFFFF at offset 0x{len(settings)*4:X}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
