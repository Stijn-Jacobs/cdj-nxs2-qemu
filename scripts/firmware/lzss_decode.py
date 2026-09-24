#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Standalone Pioneer LZSS decoder (no side effects on import).

Parameters confirmed live from the emulator at the decompressor loop head:
  ring mask 0x0FFF (4096), initial ring index 0x0FEE (4078),
  stream starts at payload+4 (the first 4 bytes are the compressed length),
  offset = b1 | ((b2 & 0xF0) << 4)  -- absolute ring position
  length = (b2 & 0x0F) + 3
"""
import sys, collections, math, re

RING = 4096
MASK = RING - 1


def unpack(data, start, ring_init=0x20, ring_pos=4078, limit=None):
    ring = bytearray([ring_init]) * RING
    r = ring_pos
    out = bytearray()
    i = start
    f = 0
    n = len(data)
    while i < n:
        f >>= 1
        if not (f & 0x100):
            if i >= n:
                break
            f = data[i] | 0xFF00
            i += 1
        if f & 1:
            if i >= n:
                break
            b = data[i]; i += 1
            out.append(b); ring[r] = b; r = (r + 1) & MASK
        else:
            if i + 1 >= n:
                break
            b1, b2 = data[i], data[i+1]; i += 2
            off = b1 | ((b2 & 0xF0) << 4)
            ln = (b2 & 0x0F) + 3
            for k in range(ln):
                b = ring[(off + k) & MASK]
                out.append(b); ring[r] = b; r = (r + 1) & MASK
        if limit and len(out) >= limit:
            break
    return bytes(out)


def report(out, label):
    f = collections.Counter(out)
    ent = -sum((c/len(out))*math.log2(c/len(out)) for c in f.values())
    strs = re.findall(rb'[\x20-\x7e]{8,}', out)
    real = [s for s in strs if not all(c == 0x20 for c in s)]
    print(f"{label}: {len(out):,} bytes  entropy {ent:.3f}  strings>=8 {len(real):,}")
    return ent, real


if __name__ == '__main__':
    path = sys.argv[1]
    start = int(sys.argv[2], 0)
    d = open(path, 'rb').read()

    out = unpack(d, start, 0x20, 4078, limit=300000)
    ent, strs = report(out, "emulator params (init=0x20 pos=4078)")

    print("\n--- first 256 bytes ---")
    for o in range(0, 256, 16):
        c = out[o:o+16]
        print("%08x  %-47s |%s|" % (o, ' '.join('%02x' % b for b in c),
              ''.join(chr(b) if 32 <= b < 127 else '.' for b in c)))

    print("\n--- first real strings ---")
    for s in strs[:20]:
        print("   ", s.decode('ascii', 'replace')[:70])

    outp = path.rsplit('/', 1)[0] + '/main_unpacked.bin'
    full = unpack(d, start, 0x20, 4078)
    open(outp, 'wb').write(full)
    report(full, "\nFULL")
    print(f"wrote {outp}")
