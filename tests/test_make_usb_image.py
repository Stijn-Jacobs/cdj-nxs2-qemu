# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/media/make_usb_image.py: build a small image from a temp folder and
read it back with an independent FAT16 reader (not pyfatfs, which wrote it)."""
import struct

import pytest

from helpers import run_script

pytest.importorskip("pyfatfs")

SECTOR = 512


class Fat16:
    """Just enough FAT16 to list directories and read files, long names included."""

    def __init__(self, part):
        self.p = part
        bps, self.spc, reserved, nfats, self.root_entries, _, _, spf = \
            struct.unpack_from("<HBHBHHBH", part, 0x0B)
        assert bps == SECTOR
        self.fat = reserved * SECTOR
        self.root = (reserved + nfats * spf) * SECTOR
        self.data = self.root + self.root_entries * 32

    def chain(self, cluster):
        while 2 <= cluster < 0xFFF8:
            yield cluster
            cluster = struct.unpack_from("<H", self.p, self.fat + cluster * 2)[0]

    def read_chain(self, cluster):
        size = self.spc * SECTOR
        return b"".join(self.p[self.data + (c - 2) * size:self.data + (c - 1) * size]
                        for c in self.chain(cluster))

    def entries(self, raw):
        lfn = []
        for o in range(0, len(raw), 32):
            e = raw[o:o + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5:
                lfn = []
                continue
            if e[11] == 0x0F:
                part = e[1:11] + e[14:26] + e[28:32]
                lfn.insert(0, part.decode("utf-16-le").split("\0")[0].rstrip("￿"))
                continue
            short = e[0:8].decode("ascii").rstrip() + (
                "." + e[8:11].decode("ascii").rstrip() if e[8:11].strip() else "")
            name = "".join(lfn) or short
            lfn = []
            if name in (".", "..") or e[11] & 0x08:
                continue
            yield name, e[11], struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]

    def listdir(self, cluster=None):
        raw = (self.p[self.root:self.data] if cluster is None else self.read_chain(cluster))
        return {name: (attr, clus, size) for name, attr, clus, size in self.entries(raw)}

    def read(self, path):
        parts = path.strip("/").split("/")
        cluster = None
        for i, name in enumerate(parts):
            attr, clus, size = self.listdir(cluster)[name]
            if i == len(parts) - 1:
                return self.read_chain(clus)[:size]
            assert attr & 0x10
            cluster = clus


@pytest.fixture(scope="module")
def image(tmp_path_factory):
    src = tmp_path_factory.mktemp("media")
    (src / "PIONEER").mkdir()
    (src / "PIONEER" / "rekordbox").mkdir()
    (src / "PIONEER" / "rekordbox" / "export.pdb").write_bytes(b"\x00\x01" * 3000)
    (src / "Contents").mkdir()
    (src / "Contents" / "A Long Track Name.txt").write_bytes(b"x" * 70000)
    (src / "README.TXT").write_bytes(b"hello")
    out = tmp_path_factory.mktemp("img") / "usb.img"
    r = run_script("scripts/media/make_usb_image.py", src, out, 32)
    assert r.returncode == 0, r.stderr
    assert "3 files" in r.stdout
    return out.read_bytes()


def test_image_is_the_requested_size_with_no_leftovers(image, tmp_path):
    assert len(image) == 32 * 1024 * 1024


def test_mbr_has_one_fat16_partition_at_lba_63(image):
    assert image[0x1FE:0x200] == b"\x55\xaa"
    boot, _, ptype, _, lba, count = struct.unpack_from("<B3sB3sII", image, 0x1BE)
    assert (boot, ptype, lba) == (0x80, 0x06, 63)
    assert count == 32 * 1024 * 1024 // SECTOR - 63
    assert image[0x1CE:0x1FE] == bytes(48)


def test_partition_boot_sector_says_fat16_and_where_it_starts(image):
    part = image[63 * SECTOR:]
    assert part[0x1FE:0x200] == b"\x55\xaa"
    assert struct.unpack_from("<I", part, 0x1C)[0] == 63
    assert part[0x36:0x3E] == b"FAT16   "
    assert part[0x2B:0x36].rstrip() == b"CDJ"


def test_files_and_long_names_read_back(image):
    fs = Fat16(image[63 * SECTOR:])
    root = fs.listdir()
    assert {"PIONEER", "Contents", "README.TXT"} <= set(root)
    assert fs.read("README.TXT") == b"hello"
    assert fs.read("PIONEER/rekordbox/export.pdb") == b"\x00\x01" * 3000
    assert fs.read("Contents/A Long Track Name.txt") == b"x" * 70000
