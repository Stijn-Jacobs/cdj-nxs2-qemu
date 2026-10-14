# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-850 packed-MAIN -> SDRAM decoder map (Task 8d).

The C850MAIN.UPD container holds ONE packed segment for MAIN: a 4-byte
big-endian LZSS stream length at UPD 0x60020 followed by the stream at
UPD 0x60024 (same Okumura/Yoshizaki codec as `lzss_decode.py`, ring 4096,
init 0x20, pos 4078). The bootloader stages it to high SDRAM and LZSS-
decodes it to 0x0C000000; the output matches a live-boot
32 MB SDRAM dump over code/rodata (spot-verified at 0/0x100/0x1000/
0x10000/0x100000/0x18BF2C; BSS/heap diverge at runtime from 0x1C14DC
as expected; verified 2026-10-13).

Map (all values pinned, see notes/cdj850-layout.md):
  packed field  main_flat 0x20-0x19E70A (len BE32 0x0019E6E6 + stream;
                = UPD 0x060020-0x1FE70A)
  unpacked      SDRAM 0x0C000000-0x0C39231E (3744542 B)
  unpack SHA-256 26c8bfe933afc1e08a05dc65513303f1343a570d1e87f2734d60316e87c01591
  remainder     main_flat 0x19E70A-0x1A0000 (6390 B checksum + zero pad,
                consumed by the decoder overrun exactly as the bootloader
                consumes it — unpack the whole main_flat, not the stream
                alone, to reproduce the SDRAM code image)

No DSP program blob lives in this image as a contiguous range: the
recorded 519-B init image and the 284-B 0x0CE4B6F4 candidate are both
absent from the packed AND unpacked bytes (see report). Nothing here is
a carve — this module only names ranges so later hunts can cite them.
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzss_decode import unpack

FLAT_LEN_OFF = 0x20
FLAT_STREAM_OFF = 0x24
UPD_PACKED_OFF = 0x60020
UPD_STREAM_OFF = 0x60024
UPD_PACKED_END = 0x1FE70A
STREAM_LEN = 0x19E6E6
SDRAM_BASE = 0x0C000000
UNPACK_LEN = 0x39231E
UNPACK_SHA256 = "26c8bfe933afc1e08a05dc65513303f1343a570d1e87f2734d60316e87c01591"
RING_INIT = 0x20
RING_POS = 4078


def decode_main(flat_path):
    """(packed_len, unpacked_bytes) from a main_flat.bin path."""
    data = open(flat_path, "rb").read()
    (declared,) = struct.unpack_from(">I", data, FLAT_LEN_OFF)
    if declared != STREAM_LEN:
        raise ValueError("packed length %s, expected 0x%x"
                         % (hex(declared), STREAM_LEN))
    image = unpack(data, FLAT_STREAM_OFF, RING_INIT, RING_POS)
    return declared, image


def main(argv):
    if len(argv) not in (2, 3):
        print("usage: c850_unpack.py <main_flat.bin> [out.bin]", file=sys.stderr)
        return 2
    declared, image = decode_main(argv[1])
    digest = hashlib.sha256(image).hexdigest()
    print("stream 0x%x bytes -> SDRAM 0x%08X-0x%08X (%d bytes) sha256 %s"
          % (declared, SDRAM_BASE, SDRAM_BASE + len(image),
             len(image), digest))
    if len(argv) == 3:
        open(argv[2], "wb").write(image)
    if len(image) != UNPACK_LEN or digest != UNPACK_SHA256:
        print("MISMATCH: expected %d bytes sha256 %s"
              % (UNPACK_LEN, UNPACK_SHA256), file=sys.stderr)
        return 1
    print("match: pinned unpacked MAIN")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
