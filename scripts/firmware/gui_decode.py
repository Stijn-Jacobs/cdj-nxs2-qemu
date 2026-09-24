#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Unpack the GUI section (section 1) of a Pioneer .UPD.

The container was long recorded here as "unknown". It is not: it is the *same*
LZSS codec as MAIN, with the same parameters. What made it look unknown is that
the decoded image is still ~85% high-entropy, because most of it is
already-compressed assets -- so every earlier judgement of "wrong parameters,
output is garbage" was reading the payload, not a decode failure. The readable
part is also UTF-16LE, so ASCII string scans found nothing.

Only the length field differs from MAIN: it is big-endian here (the GUI is a
big-endian SH7269, MAIN is a little-endian SH7724).

    [32-byte ASCII header]  "CDJ-2000NXS2GUI Ver1.81        0"
    [BE32 payload length]   @ 0x20
    [LZSS stream]           @ 0x24
    [2-byte trailer]        last 2 bytes

Evidence the decode is right, in increasing order of strength:

  1. The framing parse -- one flag byte per eight items, literal 1 byte, match
     2 bytes -- consumes the payload to the exact final byte, overshoot 0.
  2. 0xFF is 11.03% of the payload and 65% of consecutive 0xFF bytes are exactly
     9 apart (7x enrichment over baseline): that is an all-literals control byte
     in a stream of largely incompressible content.
  3. Of the candidate offset/length splits, only MAIN's scores ~96% valid on
     matches taken before the 4096-byte ring fills; the others sit at chance.
  4. Two independently compressed images, GUI v1.80 and v1.81, both decode to
     0x7C0000 bytes and share a 4,538,991-byte contiguous identical run. An
     LZSS decode error propagates, so that cannot happen unless both are right.
  5. The output contains 7,902 UTF-16LE strings: "PUSH MEMORY",
     "rekordbox Database not found!", "E-8309: LINK ACCESS ERROR", and the
     German/Italian/Spanish/Czech localisations.

    usage: gui_decode.py <section1.bin|*.UPD> [outfile]
"""
import sys, os, re, struct, collections, math

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lzss_decode import unpack

GUI_RING_INIT = 0x20
GUI_RING_POS = 4078
GUI_STREAM_OFF = 0x24


def gui_section_from_upd(data):
    """Carve section 1 out of a .UPD (manifest of CRLF decimal lengths)."""
    sizes, pos = [], 0
    while True:
        m = re.match(rb'(\d+)\r\n', data[pos:pos + 32])
        if not m:
            break
        sizes.append(int(m.group(1)))
        pos += m.end()
    if not sizes:
        return None
    return data[pos:pos + sizes[0]]


def decode(section):
    hdr = section[:32].rstrip(b'\x00 ').decode('latin1')
    declared = struct.unpack_from('>I', section, 0x20)[0]
    avail = len(section) - GUI_STREAM_OFF
    out = unpack(section, GUI_STREAM_OFF, GUI_RING_INIT, GUI_RING_POS)
    return hdr, declared, avail, out


def main():
    path = sys.argv[1]
    data = open(path, 'rb').read()
    section = gui_section_from_upd(data) if data[:1].isdigit() else data
    if section is None:
        sys.exit('not a .UPD and not a section blob')

    hdr, declared, avail, out = decode(section)
    print(f'header       : {hdr}')
    print(f'BE32 length  : {declared:,}  (payload available {avail:,})')
    print(f'decoded      : {len(out):,} bytes (0x{len(out):x})')

    freq = collections.Counter(out)
    ent = -sum((c / len(out)) * math.log2(c / len(out)) for c in freq.values())
    print(f'entropy      : {ent:.3f} bits/byte')

    u16 = re.findall(rb'(?:[\x20-\x7e]\x00){5,}', out)
    print(f'UTF-16LE strings >=5 : {len(u16):,}')
    for s in u16[:12]:
        print('   ', s.decode('utf-16-le', 'replace')[:70])

    outfile = sys.argv[2] if len(sys.argv) > 2 else 'extract/gui_unpacked.bin'
    open(outfile, 'wb').write(out)
    print(f'wrote {outfile}')


if __name__ == '__main__':
    main()
