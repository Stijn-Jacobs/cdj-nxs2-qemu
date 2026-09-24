#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Report true S-record coverage, so analysis never runs on gap-fill padding.

srec.py pre-fills the image with 0xFF; any region the S-records do not write
stays 0xFF and will be mistaken for data. This rebuilds the image alongside a
coverage bitmap and reports only the regions that actually came from the file.
"""
import sys, os, re, collections, math

SREC_ADDR_BYTES = {'0': 2, '1': 2, '2': 3, '3': 4, '5': 2, '7': 4, '8': 3, '9': 2}

src = sys.argv[1]
raw = open(src, 'rb').read()
text = raw[raw.find(b'S'):].decode('ascii', 'replace')

chunks = []
for line in text.splitlines():
    line = line.strip()
    if len(line) < 4 or line[0] != 'S':
        continue
    rt = line[1]
    if rt not in '123':
        continue
    nb = SREC_ADDR_BYTES[rt]
    try:
        cnt = int(line[2:4], 16)
        addr = int(line[4:4 + nb*2], 16)
        payload = bytes.fromhex(line[4 + nb*2: 2 + 2*(cnt+1)])
    except ValueError:
        continue
    chunks.append((addr, payload))

lo = min(a for a, _ in chunks)
hi = max(a + len(p) for a, p in chunks)
size = hi - lo
img = bytearray(b'\x00' * size)
cov = bytearray(size)
for a, p in chunks:
    img[a-lo:a-lo+len(p)] = p
    for i in range(a-lo, a-lo+len(p)):
        cov[i] = 1

covered = sum(cov)
print(f"=== {os.path.basename(src)} ===")
print(f"  address range : 0x{lo:08X} - 0x{hi:08X}  ({size:,} bytes)")
print(f"  covered bytes : {covered:,} ({100*covered/size:.2f}%)")
print(f"  gap bytes     : {size-covered:,} ({100*(size-covered)/size:.2f}%)\n")

# Contiguous gaps.
gaps, start = [], None
for i, c in enumerate(cov):
    if not c and start is None:
        start = i
    elif c and start is not None:
        gaps.append((start, i))
        start = None
if start is not None:
    gaps.append((start, size))
gaps.sort(key=lambda g: g[1]-g[0], reverse=True)
print(f"  {len(gaps)} gap regions; largest:")
for a, b in gaps[:10]:
    print(f"    0x{lo+a:08X} - 0x{lo+b:08X}  ({b-a:,} bytes)")

# Entropy of covered data only.
data = bytes(img[i] for i in range(size) if cov[i])
f = collections.Counter(data)
ent = -sum((c/len(data))*math.log2(c/len(data)) for c in f.values())
print(f"\n  entropy of COVERED data only: {ent:.3f} bits/byte")
top = f.most_common(6)
print("  top bytes: " + ', '.join(f"0x{b:02x}={100*c/len(data):.2f}%" for b, c in top))

out = os.path.join(os.path.dirname(src), os.path.basename(src).replace('.bin', '.covered.bin'))
open(out, 'wb').write(data)
print(f"\n  wrote covered-only stream: {out} ({len(data):,} bytes)")

# Also write the image with a distinguishable fill so future scans can tell.
out2 = os.path.join(os.path.dirname(src), os.path.basename(src).replace('.bin', '.sparse.bin'))
open(out2, 'wb').write(bytes(img))
print(f"  wrote zero-filled image:    {out2} ({size:,} bytes)")
