#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build a real FAT16 disk image of a USB medium directory.

vvfat (fat:rw:<dir>) re-checks and commits the whole volume on EVERY guest
write (vvfat_write -> try_commit), in QEMU's main loop. A raw image takes the
same writes as plain file I/O, so a run on it separates "the firmware's own
work" from "vvfat's write cost".

The layout copies what vvfat presents: an MBR with one FAT16 partition at
sector 63, long file names kept.  Needs pyfatfs (pip install pyfatfs).
  usage: python scripts/media/make_usb_image.py <media_dir> <out.img> [size_mb=256]
"""
import os
import struct
import sys

from pyfatfs.PyFat import PyFat
from pyfatfs.PyFatFS import PyFatFS

SECTOR = 512
PART_LBA = 63

src, out = sys.argv[1], sys.argv[2]
size = int(sys.argv[3] if len(sys.argv) > 3 else 256) * 1024 * 1024
part_sectors = size // SECTOR - PART_LBA
part = out + '.part'

with open(part, 'wb') as f:
    f.truncate(part_sectors * SECTOR)
pf = PyFat()
pf.mkfs(part, PyFat.FAT_TYPE_FAT16, size=part_sectors * SECTOR, label='CDJ')
pf.close()

fs = PyFatFS(part, preserve_case=True)
n = 0
for root, dirs, files in os.walk(src):
    rel = os.path.relpath(root, src).replace(os.sep, '/')
    base = '/' if rel == '.' else '/' + rel
    for d in sorted(dirs):
        fs.makedir(base.rstrip('/') + '/' + d)
    for name in sorted(files):
        with open(os.path.join(root, name), 'rb') as fin:
            fs.writebytes(base.rstrip('/') + '/' + name, fin.read())
        n += 1
fs.close()

# The partition's boot sector must say where it starts, like any partitioned disk.
with open(part, 'r+b') as f:
    f.seek(0x1C)
    f.write(struct.pack('<I', PART_LBA))

mbr = bytearray(SECTOR)
entry = struct.pack('<B3sB3sII', 0x80, b'\x01\x01\x00', 0x06, b'\xfe\xff\xff',
                    PART_LBA, part_sectors)
mbr[0x1BE:0x1BE + 16] = entry
mbr[0x1FE:0x200] = b'\x55\xaa'
with open(out, 'wb') as fo:
    fo.write(mbr)
    fo.write(bytes(SECTOR * (PART_LBA - 1)))
    with open(part, 'rb') as fp:
        while True:
            chunk = fp.read(1 << 20)
            if not chunk:
                break
            fo.write(chunk)
os.remove(part)
print('%s: %d files, %d MB, FAT16 at LBA %d' % (out, n, size >> 20, PART_LBA))
