#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Carve a Pioneer .UPD into its sections.

Container layout: a plaintext manifest of CRLF-separated decimal section
lengths, then the sections concatenated in order. The manifest ends where the
lengths stop parsing; sizes summing exactly to filesize-headerlen confirms it.
"""
import sys, os, math, collections, re

path, outdir = sys.argv[1], sys.argv[2]
data = open(path, 'rb').read()
os.makedirs(outdir, exist_ok=True)

# Parse leading decimal-CRLF lines as section lengths.
sizes, pos = [], 0
while True:
    m = re.match(rb'(\d+)\r\n', data[pos:pos+32])
    if not m:
        break
    sizes.append(int(m.group(1)))
    pos += m.end()

hdr = pos
print(f"manifest: {hdr} bytes, {len(sizes)} sections")
print(f"sections: {sizes}")
print(f"sum+header = {sum(sizes)+hdr:,} vs filesize {len(data):,} -> "
      f"{'MATCH' if sum(sizes)+hdr == len(data) else 'MISMATCH'}\n")

off = hdr
for i, size in enumerate(sizes, 1):
    blob = data[off:off+size]
    name = os.path.join(outdir, f"section{i}.bin")
    open(name, 'wb').write(blob)

    f = collections.Counter(blob)
    ent = -sum((c/len(blob)) * math.log2(c/len(blob)) for c in f.values())
    printable = sum(f.get(b, 0) for b in list(range(32, 127)) + [10, 13]) / len(blob)

    # A leading printable run is usually a per-section label (name + version).
    label = re.match(rb'[\x20-\x7e]{4,64}', blob)
    label = label.group().decode('ascii', 'replace').strip() if label else '(binary)'

    print(f"section{i}: offset 0x{off:08x}  size {size:,}  entropy {ent:.2f}  "
          f"printable {printable*100:.1f}%")
    print(f"           label: {label}")
    print(f"           head: {blob[:32].hex()}")
    off += size
