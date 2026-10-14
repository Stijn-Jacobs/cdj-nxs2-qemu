# SPDX-License-Identifier: GPL-2.0-or-later
"""Decode the recorded CDJ-850 AUDIO-DSP init image from a CDJ850_DSP_RECORD file.

Source: MAIN's Port-E bit-bang upload driver, observed live through the
`hw/cdj/boards/cdj850/dsp56371.c` record buffer (`$CDJ850_DSP_RECORD`);
see `notes/cdj850-layout.md` ("DSP init image") and the Task 8c report.
Every decoded byte was driven by MAIN — nothing is invented here.

Record entry: 20-byte packed little-endian
`{pc u32, ms u64, off u16, size u8, reserved u8, val u32}`
(must match the `Cdj850DspRec` struct in `dsp56371.c`).

Decode rule: byte-send `0x0C0FCC68` emits 5 PEDRL halfword writes per call
at sites `0x0C0FCC86/0xB2/0xC4/0xD6/0xF8`; byte =
`((w0>>12)&0xF)<<4 | ((w3>>12)&0xF` (strict interleave over all calls).
PEDRL writes from the `0x0C0FCD0E` strobe family (fixed `0x50`-nibble, e.g.
`0x0C0FCD2C=0x5080` / `0x0C0FCD54=0x5180`), the gate/poll sites, and all
other PCs are strobes — not data — and are excluded by the site filter.

Reproduces 519 bytes, SHA-256
`7132eb444b686275a6b2e4e595e6fd7e7351bea14e43709ce5998adfc3972064`.
"""
import hashlib
import struct
import sys

REC_STRUCT = struct.Struct("<IQHBBI")
REC_SIZE = REC_STRUCT.size
PEDRL_OFF = 0x202
PEIORL_OFF = 0x206
# Byte-send PEDRL write sites, in emission order (w0..w4).
SITES = (0x0C0FCC86, 0x0C0FCCB2, 0x0C0FCCC4, 0x0C0FCCD6, 0x0C0FCCF8)
IMAGE_LEN = 519
IMAGE_SHA256 = "7132eb444b686275a6b2e4e595e6fd7e7351bea14e43709ce5998adfc3972064"


def read_record(path):
    """Parse a CDJ850_DSP_RECORD file into [(pc, ms, off, size, val)]."""
    data = open(path, "rb").read()
    if len(data) % REC_SIZE != 0:
        raise ValueError("truncated record file %s (%d bytes)" % (path, len(data)))
    out = []
    for i in range(0, len(data), REC_SIZE):
        pc, ms, off, size, _reserved, val = REC_STRUCT.unpack_from(data, i)
        out.append((pc, ms, off, size, val))
    return out


def decode_image(records):
    """Carve the DSP init image out of parsed record entries.

    Keeps only PEDRL halfword writes from the 5 byte-send sites, requires
    them in strict emission order, and interleaves w0/w3 high nibbles.
    """
    words = [(pc, val & 0xFFFF) for (pc, _ms, off, size, val) in records
             if off == PEDRL_OFF and size == 2 and pc in SITES]
    if len(words) % len(SITES) != 0:
        raise ValueError("byte-send word count %d not a multiple of 5" % len(words))
    for i, (pc, _val) in enumerate(words):
        if pc != SITES[i % len(SITES)]:
            raise ValueError("byte-send order break at word %d (pc 0x%08x)" % (i, pc))
    out = bytearray()
    for i in range(0, len(words), len(SITES)):
        w0 = words[i][1]
        w3 = words[i + 3][1]
        out.append((((w0 >> 12) & 0xF) << 4) | ((w3 >> 12) & 0xF))
    return bytes(out)


def decode_record_file(path):
    """Decode the DSP init image directly from a record file path."""
    return decode_image(read_record(path))


def main(argv):
    if len(argv) not in (2, 3):
        print("usage: c850_dsp_decode.py <record> [out.bin]", file=sys.stderr)
        return 2
    image = decode_record_file(argv[1])
    digest = hashlib.sha256(image).hexdigest()
    print("%d bytes, sha256 %s" % (len(image), digest))
    if len(argv) == 3:
        open(argv[2], "wb").write(image)
    if len(image) != IMAGE_LEN or digest != IMAGE_SHA256:
        print("MISMATCH: expected %d bytes sha256 %s" % (IMAGE_LEN, IMAGE_SHA256),
              file=sys.stderr)
        return 1
    print("match: pinned 519-B DSP init image")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
