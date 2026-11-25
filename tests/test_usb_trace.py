# SPDX-License-Identifier: GPL-2.0-or-later
"""Turning a CDJ_USB_TRACE log into file names: the opens a directory medium
reports, and the LBA-to-file mapping an image medium needs."""

import os
import struct
import sys

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if os.path.join(EMU, 'scripts', 'usb') not in sys.path:
    sys.path.insert(0, os.path.join(EMU, 'scripts', 'usb'))

from usb_trace import Fat, attribute_opens, map_image, parse_trace, summarise_opens  # noqa: E402


def _fat16_image(path, files):
    """A minimal FAT16 volume: one sector per cluster, two FATs, a 64-entry root
    directory, and `files` as (8.3 name, bytes) in consecutive clusters."""
    sector = 512
    reserved, num_fats, fat_sectors, root_entries = 1, 2, 16, 64
    root_sectors = root_entries * 32 // sector
    data_start = reserved + num_fats * fat_sectors + root_sectors
    total_sectors = data_start + len(files) + 8
    image = bytearray(total_sectors * sector)

    image[0:3] = b'\xeb\x3c\x90'
    image[11:13] = struct.pack('<H', sector)
    image[13] = 1
    image[14:16] = struct.pack('<H', reserved)
    image[16] = num_fats
    image[17:19] = struct.pack('<H', root_entries)
    image[19:21] = struct.pack('<H', total_sectors)
    image[22:24] = struct.pack('<H', fat_sectors)
    image[510:512] = b'\x55\xaa'

    fat = bytearray(fat_sectors * sector)
    struct.pack_into('<H', fat, 0, 0xFFF8)
    struct.pack_into('<H', fat, 2, 0xFFFF)
    root = data_start - root_sectors
    for i, (name, payload) in enumerate(files):
        cluster = 2 + i
        struct.pack_into('<H', fat, cluster * 2, 0xFFFF)
        off = data_start * sector + i * sector
        image[off:off + len(payload)] = payload
        entry = root * sector + i * 32
        base, _, ext = name.partition('.')
        image[entry:entry + 8] = base.upper().ljust(8).encode()
        image[entry + 8:entry + 11] = ext.upper().ljust(3).encode()
        image[entry + 11] = 0x20
        struct.pack_into('<H', image, entry + 26, cluster)
        struct.pack_into('<I', image, entry + 28, len(payload))
    for copy in range(num_fats):
        off = (reserved + copy * fat_sectors) * sector
        image[off:off + len(fat)] = fat

    with open(path, 'wb') as f:
        f.write(image)
    return data_start


def test_parses_both_kinds_of_line(tmp_path):
    log = tmp_path / 'trace.log'
    log.write_text(
        'cdj-usb read lba=0 bytes=36\n'
        'noise that is not a line\n'
        'cdj-usb open file=/media/PIONEER/rekordbox/exportExt.pdb\n'
        'cdj-usb write lba=4096 bytes=512\n'
        'cdj-usb read lba=2048 bytes=0\n'
    )
    events = parse_trace(str(log))

    assert events[0] == ('read', (0, 36))
    assert events[1] == ('open', '/media/PIONEER/rekordbox/exportExt.pdb')
    # A zero-byte transfer says nothing and is dropped; a write is not a read.
    assert len(events) == 2


def test_reads_between_opens_belong_to_the_file_last_opened(tmp_path):
    log = tmp_path / 'trace.log'
    log.write_text(
        'cdj-usb open file=/m/PIONEER/rekordbox/export.pdb\n'
        'cdj-usb open file=/m/PIONEER/rekordbox/export.pdb\n'
        'cdj-usb open file=/m/PIONEER/rekordbox/exportExt.pdb\n'
        'cdj-usb open file=/m/PIONEER/rekordbox/export.pdb\n'
        'cdj-usb open file=/m/PIONEER/rekordbox/exportExt.pdb\n'
        'cdj-usb open file=/m/PIONEER/rekordbox/exportExt.pdb\n'
    )
    events = parse_trace(str(log))

    counts = summarise_opens(events)
    assert counts == [
        ('/m/PIONEER/rekordbox/export.pdb', 3),
        ('/m/PIONEER/rekordbox/exportExt.pdb', 3),
    ]


def test_image_medium_maps_an_lba_to_the_file_it_lands_in(tmp_path):
    image = tmp_path / 'stick.img'
    data_start = _fat16_image(str(image), [
        ('export.pdb', b'A' * 512),
        ('tags.pdb', b'B' * 512),
    ])
    fat = Fat(str(image))
    files = {path: size for path, _clusters, size in fat.files()}
    assert files == {'EXPORT.PDB': 512, 'TAGS.PDB': 512}

    events = [('read', (data_start, 512)), ('read', (data_start + 1, 512))]
    per_file, lines = map_image(fat, events)

    assert per_file == {'EXPORT.PDB': 512, 'TAGS.PDB': 512}
    assert [path for _lba, _size, path in lines] == ['EXPORT.PDB', 'TAGS.PDB']
