#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mint a deck's NOR flash image with its PLAYER No. already set.

Two decks on one Pro DJ Link segment both set to AUTO each claim player 1 and
deadlock. Setting the number in the UTILITY menu works but costs a boot per
deck; this writes the same bytes the firmware would into a copy of the image.

Backup_TASK (entry 0x08354B40) keeps its settings in the top-boot 8 KiB sectors
of the NOR flash. The first boot on a blank store formats them: an "FBOK" stamp
in each sector's last four bytes plus a few default records (FORMAT below).
PLAYER No. lives in the type-2 record in sector 0x7F0000: four halfwords,
appended log-style, the last one wins. The first halfword is 0x0179 | n << 10,
n = 0 for AUTO or 1..4; the other three are 403f 8495 0322.

This relies on the board's top-boot flash geometry (127 x 64 KiB + 8 x 8 KiB,
see the NOR flash model in hw/cdj/common/sh4_board.c).

The firmware provisions serial "PDJ0000001XX" at 0x7FE000 on every image, so
two decks are otherwise identical there. --serial <12 chars> writes a different
one; --serial auto derives it from the player number (PDJ0000002XX for 2).

usage:  player_flash.py <auto|1..4> <out.bin> [base=extract/flash.bin]
                          [--serial <12 chars>|auto]
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BASE = os.path.join(HERE, "..", "..", "extract", "flash.bin")
FLASH_SIZE = 8 * 1024 * 1024

# (offset, bytes) runs of a first boot's format, from a diff of the pristine
# image against a deck that booted once. The player record at 0x7F0008 is left
# out: it is appended by record().
FORMAT = [
    (0x7EFFFC, bytes.fromhex("46424f4b")),
    (0x7F0000, bytes.fromhex("79013f4095842203")),         # type 2, AUTO
    (0x7F1FFC, bytes.fromhex("46424f4b") + b"HISTORY" + bytes(25)),
    (0x7F3FFC, bytes.fromhex("46424f4b1100")),             # type 4 word 0x0011
    (0x7F5FFC, bytes.fromhex("46424f4bffffffff")),         # 0x7F6000 erased
    (0x7F7FFC, bytes.fromhex("46424f4b")),
    (0x7F9FFC, bytes.fromhex("46424f4b")),
    (0x7FBFFC, bytes.fromhex("46424f4b") + bytes(32) + b"ok"),
    (0x7FDFFC, bytes.fromhex("46424f4b") + b"PDJ0000001XX"),
    (0x7FFFFC, bytes.fromhex("46424f4b")),
]
PLAYER_SECTOR = 0x7F0000
RECORD_TAIL = bytes.fromhex("3f4095842203")
SERIAL_OFF = 0x7FE000
SERIAL_LEN = 12


def player_halfword(n):
    return 0x0179 | (n << 10)


def mint(base, n, serial=None):
    img = bytearray(base)
    if len(img) != FLASH_SIZE:
        raise SystemExit("base image is %d bytes, expected %d" % (len(img), FLASH_SIZE))
    for off, data in FORMAT:
        img[off:off + len(data)] = data
    if n:
        # The firmware appends; the next free slot after the AUTO record.
        img[PLAYER_SECTOR + 8:PLAYER_SECTOR + 16] = struct.pack("<H", player_halfword(n)) + RECORD_TAIL
    if serial:
        s = serial.encode("ascii")
        if len(s) != SERIAL_LEN:
            raise SystemExit("serial must be %d characters" % SERIAL_LEN)
        img[SERIAL_OFF:SERIAL_OFF + SERIAL_LEN] = s
    return bytes(img)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    argv = list(sys.argv[1:])
    serial = None
    if "--serial" in argv:
        i = argv.index("--serial")
        serial = argv[i + 1]
        del argv[i:i + 2]
    arg = argv[0].lower()
    n = 0 if arg == "auto" else int(arg)
    if not 0 <= n <= 4:
        raise SystemExit("PLAYER No. must be auto or 1..4")
    if serial == "auto":
        serial = "PDJ000000%dXX" % (n or 1)
    base = open(argv[2] if len(argv) > 2 else DEFAULT_BASE, "rb").read()
    out = mint(base, n, serial)
    with open(argv[1], "wb") as f:
        f.write(out)
    print("%s: PLAYER No. %s (record halfword 0x%04x)%s"
          % (argv[1], "AUTO" if n == 0 else n, player_halfword(n),
             ", serial %s" % serial if serial else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
