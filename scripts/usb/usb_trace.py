#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn a CDJ_USB_TRACE log into the files the player actually touched.

The emulators log two kinds of line, depending on how the medium is attached:

    cdj-usb read lba=2048 bytes=512                  (MEDIA_MODE=img: an image)
    cdj-usb open file=/path/PIONEER/rekordbox/exportExt.pdb
                                                     (MEDIA_MODE=rw: a directory)

A directory medium has no image, so its LBA means nothing on its own; the vvfat
driver knows which host file it opens, and logs that instead. An image medium is
the other way round: the driver never sees a name, but the log's LBA can be
mapped through the filesystem on the image.

    usage: usb_trace.py <trace-log> [<medium.img>] [--lines]

Without an image the opens are reported as they come. With one, each read is
resolved to the file whose clusters it lands in.
"""

import argparse
import struct
import sys

SECTOR = 512
DIR_ENTRY = 32
ATTR_LFN = 0x0F
ATTR_DIR = 0x10
ATTR_VOLUME = 0x08


def parse_trace(path):
    """[(kind, payload)] where kind is 'open' or 'read'.

    'open' payloads are the host path, 'read' payloads are (lba, bytes).
    """
    events = []
    with open(path) as f:
        for line in f:
            if not line.startswith('cdj-usb '):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            if parts[1] == 'open':
                for token in parts[2:]:
                    if token.startswith('file='):
                        events.append(('open', token[5:]))
                        break
                continue
            if parts[1] != 'read':
                continue
            try:
                lba = int(parts[2].split('=')[1])
                size = int(parts[3].split('=')[1])
            except (IndexError, ValueError):
                continue
            if size > 0:
                events.append(('read', (lba, size)))
    return events


def attribute_opens(events):
    """[(path, bytes)] in the order the deck opened them, with the bytes read
    while that file was the one the driver was serving."""
    rows = []
    current = None
    for kind, payload in events:
        if kind == 'open':
            if current and current[1] == 0:
                current[1] = 0
            current = [payload, 0]
            rows.append(current)
        elif current is not None:
            current[1] += payload[1]
    merged = {}
    order = []
    for path, size in rows:
        if path not in merged:
            merged[path] = 0
            order.append(path)
        merged[path] += size
    return [(path, merged[path]) for path in order]


def summarise_opens(events):
    """How many times each file was opened, and whether that is the whole story."""
    counts = {}
    order = []
    for kind, payload in events:
        if kind != 'open':
            continue
        if payload not in counts:
            counts[payload] = 0
            order.append(payload)
        counts[payload] += 1
    return [(path, counts[path]) for path in order]


class Fat:
    """The parts of a FAT16/FAT32 volume this needs: where the clusters of every
    file are, so a read's LBA can be named."""

    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        start = self._partition_start()
        bpb = self.data[start:start + SECTOR]
        self.sector_size = struct.unpack_from('<H', bpb, 11)[0]
        self.sectors_per_cluster = bpb[13]
        self.reserved = struct.unpack_from('<H', bpb, 14)[0]
        self.num_fats = bpb[16]
        self.root_entries = struct.unpack_from('<H', bpb, 17)[0]
        total16 = struct.unpack_from('<H', bpb, 19)[0]
        fat16 = struct.unpack_from('<H', bpb, 22)[0]
        total32 = struct.unpack_from('<I', bpb, 32)[0]
        fat32 = struct.unpack_from('<I', bpb, 36)[0]
        self.total_sectors = total16 or total32
        self.fat_sectors = fat16 or fat32
        self.root_dir_sectors = (self.root_entries * DIR_ENTRY + self.sector_size - 1) // self.sector_size
        self.fat_base = start + self.reserved * self.sector_size
        self.root_base = self.fat_base + self.num_fats * self.fat_sectors * self.sector_size
        self.data_base = self.root_base + self.root_dir_sectors * self.sector_size
        clusters = (self.total_sectors - self.reserved - self.num_fats * self.fat_sectors
                    - self.root_dir_sectors) // self.sectors_per_cluster
        self.fat32 = self.root_entries == 0 or clusters >= 65525
        self.entry_bytes = 4 if self.fat32 else 2
        self.root_cluster = struct.unpack_from('<I', bpb, 44)[0]

    def _partition_start(self):
        if len(self.data) < 512:
            raise SystemExit('image shorter than one sector')
        if self.data[510:512] != b'\x55\xaa':
            return 0
        for i in range(4):
            off = 446 + i * 16
            if self.data[off + 4] in (0x01, 0x04, 0x06, 0x0B, 0x0C, 0x0E):
                return struct.unpack_from('<I', self.data, off + 8)[0] * SECTOR
        return 0

    def cluster_bytes(self):
        return self.sectors_per_cluster * self.sector_size

    def cluster_lba(self, cluster):
        return self.data_base // self.sector_size + (cluster - 2) * self.sectors_per_cluster

    def next_cluster(self, cluster):
        off = self.fat_base + cluster * self.entry_bytes
        if self.fat32:
            value = struct.unpack_from('<I', self.data, off)[0] & 0x0FFFFFFF
            return None if value >= 0x0FFFFFF8 else value
        value = struct.unpack_from('<H', self.data, off)[0]
        return None if value >= 0xFFF8 else value

    def chain(self, first, size):
        clusters = []
        cluster = first
        want = (size + self.cluster_bytes() - 1) // self.cluster_bytes()
        while cluster and cluster >= 2 and len(clusters) <= want:
            clusters.append(cluster)
            cluster = self.next_cluster(cluster)
        return clusters

    def files(self):
        out = []
        if self.fat32:
            first = self.root_cluster
            for cluster in self.chain(first, 1 << 30):
                self._walk(self.data_base + (cluster - 2) * self.cluster_bytes(), '', out, 0)
        else:
            self._walk(self.root_base, '', out, 0)
        return out

    def _walk(self, offset, prefix, out, depth):
        if depth > 12:
            return
        pos = offset
        while True:
            entry = self.data[pos:pos + DIR_ENTRY]
            if len(entry) < DIR_ENTRY or entry[0] == 0x00:
                break
            pos += DIR_ENTRY
            attr = entry[11]
            if attr == ATTR_LFN or entry[0] == 0xE5 or attr & ATTR_VOLUME:
                continue
            name = entry[0:8].decode('latin1').rstrip()
            ext = entry[8:11].decode('latin1').rstrip()
            full = name + ('.' + ext if ext else '')
            if full in ('.', '..'):
                continue
            first = struct.unpack_from('<H', entry, 26)[0]
            if self.fat32:
                first |= struct.unpack_from('<H', entry, 20)[0] << 16
            size = struct.unpack_from('<I', entry, 28)[0]
            if attr & ATTR_DIR:
                if first:
                    self._walk(self.data_base + (first - 2) * self.cluster_bytes(),
                               prefix + full + '/', out, depth + 1)
                continue
            out.append((prefix + full, self.chain(first, size), size))


def map_image(fat, events):
    """{path: bytes} for the reads in a log from an image medium."""
    per_cluster = {}
    for path, clusters, size in fat.files():
        left = size
        for cluster in clusters:
            take = min(left, fat.cluster_bytes())
            if take <= 0:
                break
            per_cluster[fat.cluster_lba(cluster)] = path
            left -= take

    per_file = {}
    lines = []
    span = fat.sectors_per_cluster
    for kind, payload in events:
        if kind != 'read':
            continue
        lba, size = payload
        last = lba + (size + fat.sector_size - 1) // fat.sector_size - 1
        hit = None
        for cluster_lba, path in per_cluster.items():
            if cluster_lba <= last and lba <= cluster_lba + span - 1:
                hit = path
                per_file[path] = per_file.get(path, 0) + size
                break
        lines.append((lba, size, hit))
    return per_file, lines


def main():
    ap = argparse.ArgumentParser(description='Map a CDJ_USB_TRACE log to the files on the medium')
    ap.add_argument('trace')
    ap.add_argument('image', nargs='?')
    ap.add_argument('--lines', action='store_true', help='print every transfer too')
    args = ap.parse_args()

    events = parse_trace(args.trace)
    if not events:
        print('no cdj-usb lines in %s: was CDJ_USB_TRACE set for the deck?' % args.trace)
        return 1

    if any(kind == 'open' for kind, _ in events):
        rows = attribute_opens(events)
        print('%d opens, %d reads; files in the order the deck opened them:'
              % (len(summarise_opens(events)), sum(1 for k, _ in events if k == 'read')))
        for path, size in rows:
            print('  %9d  %s' % (size, path))
        if args.lines:
            for path, count in summarise_opens(events):
                print('  opened %dx  %s' % (count, path))
        return 0

    if not args.image:
        print('%d reads and no opens: this log came from an image medium, pass the image '
              'to map its LBAs to files' % sum(1 for k, _ in events if k == 'read'))
        return 1

    fat = Fat(args.image)
    per_file, lines = map_image(fat, events)
    total = sum(size for kind, (_lba, size) in events if kind == 'read')
    print('%d transfers, %d bytes, %d files touched' % (len(lines), total, len(per_file)))
    for path, size in sorted(per_file.items(), key=lambda kv: -kv[1]):
        print('  %9d  %s' % (size, path))
    outside = total - sum(per_file.values())
    if outside:
        print('  %9d  (filesystem metadata or raw area)' % outside)
    if args.lines:
        for lba, size, path in lines:
            print('lba=%-9d bytes=%-6d %s' % (lba, size, path or '-'))
    return 0


if __name__ == '__main__':
    sys.exit(main())
