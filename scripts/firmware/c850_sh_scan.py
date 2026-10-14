# SPDX-License-Identifier: GPL-2.0-or-later
"""Count candidate SH-2A-only 32-bit first-halves in the CDJ-850 MAIN region.

Reads the flat C850MAIN.UPD image read-only via ``c850_decode.read_upd``
(size + SHA-256 pinned there), slices the ``main`` region
(flat 0x060000-0x200000) from post-label code start 0x60028, and interprets
it as big-endian 16-bit words.  This is a counter, not a disassembler: every
word is tested against the SH-2A-only 32-bit first-half patterns below, so
data words that happen to match are counted too (notably 0x0000 zero padding
matching the movi20 mask).  Counts therefore over-approximate real executed
32-bit instructions, which is the safe direction for a coverage survey.

SH-2A-only encodings counted (first half of a 32-bit instruction; the second
half is a second 16-bit word holding the remaining immediate/displacement
bits).  Ranges per the SH-2A/SH2A-FPU Software Manual (Renesas
REJ09B0051-0300, section 6.3 "New Instructions": MOVI20/MOVI20S, 32-bit
MOV.B/W/L + MOVU.B/W + FMOV.S/D with disp12, and the 32-bit B*.B bit-operand
group) and per Linux's decoder (arch/sh/kernel/cpu/sh2a/opcode_helper.c,
instruction_size()):

- movi20:  0000nnnniiii0000, i.e. (w & 0xF00F) == 0x0000
           (20-bit immediate load; second half = low 16 bits of imm)
- movi20s: 0000nnnniiii0001, i.e. (w & 0xF00F) == 0x0001
           (20-bit immediate load, shifted up 8; second half = low 16 bits)
- mov32:   0011nnnnmmmm0001, i.e. (w & 0xF00F) == 0x3001
           (32-bit MOV.B/W/L, MOVU.B/W, FMOV.S/D with 12-bit displacement;
           the size/register field lives in the second half)
- bitop32: 0011nnnn0iii1001, i.e. (w & 0xF08F) == 0x3009
           (32-bit BCLR.B/BLD.B/BSET.B/BST.B/BAND.B/BANDNOT.B/BLDNOT.B/
           BOR.B/BORNOT.B/BXOR.B; bit number + opcode live in second half)

None of these first-halves is a legal 16-bit SH-1/SH-2/SH-4 opcode, so each
count is a lower bound on code QEMU's 16-bit-only sh4 translator (QEMU
v9.1.0 target/sh4/translate.c, which fetches one 16-bit word per insn via
translator_lduw and has no 32-bit path) mistranslates as illegal.
"""
import collections
import sys

CODE_START = 0x60028
CODE_END = 0x200000

PATTERNS = ("movi20", "movi20s", "mov32-disp12", "bitop32")


def _classify(w):
    if (w & 0xF00F) == 0x0000:
        return "movi20"
    if (w & 0xF00F) == 0x0001:
        return "movi20s"
    if (w & 0xF00F) == 0x3001:
        return "mov32-disp12"
    if (w & 0xF08F) == 0x3009:
        return "bitop32"
    return None


def _words(data):
    code = data[CODE_START:CODE_END]
    return [int.from_bytes(code[i:i + 2], "big") for i in range(0, len(code), 2)]


def sh2a_only_counts(path):
    """Return {name: count} of SH-2A-only 32-bit first-halves (zero counts omitted)."""
    from c850_decode import read_upd
    words = _words(read_upd(path))
    counts = collections.Counter()
    for w in words:
        name = _classify(w)
        if name is not None:
            counts[name] += 1
    return dict(counts)


def sh2a_first_offsets(path):
    """Return {name: (file_offset, word)} of the first hit per pattern."""
    from c850_decode import read_upd
    words = _words(read_upd(path))
    first = {}
    for i, w in enumerate(words):
        name = _classify(w)
        if name is not None and name not in first:
            first[name] = (CODE_START + 2 * i, w)
            if len(first) == len(PATTERNS):
                break
    return first


def main(argv):
    from c850_decode import read_upd
    path = argv[1] if len(argv) > 1 else "/Users/julius/Documents/cdj850-emu/C850MAIN.UPD"
    words = _words(read_upd(path))
    counts = sh2a_only_counts(path)
    first = sh2a_first_offsets(path)
    print("SH-2A-only 32-bit first-half counts (BE words from %#x):" % CODE_START)
    for name in PATTERNS:
        if name in counts:
            off, w = first[name]
            print("  %-12s %6d  first %#08x (word %04x)" % (name, counts[name], off, w))
    print("top-20 16-bit patterns:")
    for w, n in collections.Counter(words).most_common(20):
        print("  %04x %d" % (w, n))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
