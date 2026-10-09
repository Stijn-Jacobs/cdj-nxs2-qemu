#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Flatten an S-record section into a raw image that starts at its lowest
address, gaps filled with 0xFF (the erased state of the flash it came from).

usage: srec_flat.py section.bin out.flat
"""
import sys

ADDR_BYTES = {b'1': 2, b'2': 3, b'3': 4}

records = []
for line in open(sys.argv[1], 'rb').read().split(b'\n'):
    line = line.strip()
    alen = ADDR_BYTES.get(line[1:2]) if line[:1] == b'S' else None
    if alen is None:
        continue
    try:
        raw = bytes.fromhex(line[2:].decode())
    except ValueError:
        continue
    records.append((int.from_bytes(raw[1:1 + alen], 'big'), raw[1 + alen:-1]))

base = min(a for a, _ in records)
image = bytearray(b'\xff' * (max(a + len(b) for a, b in records) - base))
for a, b in records:
    image[a - base:a - base + len(b)] = b
open(sys.argv[2], 'wb').write(image)
print('wrote %s, base 0x%08x, %d bytes' % (sys.argv[2], base, len(image)))
