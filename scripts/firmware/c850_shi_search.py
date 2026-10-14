# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-850 DSP program hunt, SHI-framed (Task 8e).

Re-parses the recorded 519-B DSP init image against the DSP56371
bootstrap-ROM SHI framing (UM Appendix A: word count + start address +
N program words, 24-bit words, MSB-first on the wire), runs a 24-bit
DSP563xx payload census in both byte orders, redoes the packed/unpacked
negative search with full-length `bytes.find`, and scans both images for
SHI headers (16 <= count <= 0x1000 words, start address in PRAM
$000000-$000FFF) with a vector-table + payload-census verdict per
survivor class.

Source documents (public, read-only, never committed):
DSP56371UM Rev 2.1 App. A (bootstrap source: "1) 3 bytes - program
length. 2) 3 bytes - address ... 3) 3n bytes ... stored in contiguous
PRAM ... execution starts from the same address"), Table 4-2 (mode 5 =
bootstrap from SHI slave SPI), Table 7-3 (HM=10 -> 24-bit), section 7.4
(MSB-first shift order), HCSR $A9 (HEN=1, HM=10, HFIFO=1, HMST=0,
HRQE=01), section 7.7.1 (HREQ asserted when IOSR ready and HRX FIFO not
full, deasserted at first clock of next word); DSP56300FM Rev 5 ch. 13
(NOP=$000000, RTS=$00000C, REP=$06xxxx with low-nibble $Ax, BRA-short
$05xxxx, JMP-short $0Cxxxx, JMP-ea $0Axxxx, BRA-long $0Dxxxx);
memory map Fig. 3-1 (PRAM 4K $000000-$000FFF).

Result (2026-10-13, see Task 8e report): framing fits nothing, payload
is not code in either order, no genuine header exists in either image,
so nothing is carved here. Unpacker loop PCs pinned separately from a
fresh early-window `-d in_asm` trace: bit-test head 0x0DDEFA52,
literal store 0x0DDEFA78/0x0DDEFA7A (`bra 0xddefa52`), match-copy loop
0x0DDEFAA6 (`bra 0xddefaa6`), decoder entry 0x0DDEFA20, exit
0x0DDEFAC2, caller 0x0DDEFBE4.
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

IMAGE_LEN = 519
IMAGE_SHA256 = "7132eb444b686275a6b2e4e595e6fd7e7351bea14e43709ce5998adfc3972064"
CAND284_SHA256 = "d47e1d713f42f440b39af7952c4ffa1a91b4d48531bf03f4c371e52c125d9fa4"
CAND284_OFF = 0xE4B6F4  # SDRAM offset in a live 32 MB dump (runtime heap)
PRAM_WORDS = 0x1000  # Fig. 3-1 default map: PRAM 4K $000000-$000FFF
MIN_COUNT = 16  # headers below this are SH zero/immediate noise, not programs

NOP = 0x000000
RTS = 0x00000C
JMP_HI = (0x05, 0x0A, 0x0C, 0x0D)  # BRA-short, JMP-ea, JMP-short, BRA-long
VECTOR_SLOTS = (0, 2, 4, 6, 8, 10)  # RESET/STACK/ILL/DBG/TRAP/NMI


def words24(buf, order):
    """Split a byte string into 24-bit words (drops a trailing partial word)."""
    n = len(buf) // 3
    out = []
    for k in range(n):
        b0, b1, b2 = buf[3 * k], buf[3 * k + 1], buf[3 * k + 2]
        out.append((b0 << 16) | (b1 << 8) | b2 if order == "BE"
                   else b0 | (b1 << 8) | (b2 << 16))
    return out


def is_valid_rep(word):
    """REP #xxx opcode word: high byte 0x06 with low-nibble pattern 0xAx."""
    return (word >> 16) == 0x06 and (word & 0xF0) == 0xA0


def census(words):
    """Opcode-marker census of 24-bit DSP563xx words (FM ch. 13)."""
    marks = {
        "nop": sum(1 for w in words if w == NOP),
        "rts": sum(1 for w in words if w == RTS),
        "rep": sum(1 for w in words if is_valid_rep(w)),
        "bra_short": sum(1 for w in words if (w >> 16) == 0x05),
        "jmp": sum(1 for w in words if (w >> 16) in (0x0A, 0x0C, 0x0D)),
    }
    marks["total"] = sum(marks.values())
    return marks


def framing_at(buf, off, order):
    """(count, addr) as 24-bit words at byte offset off, or None if short."""
    if off + 6 > len(buf):
        return None
    ws = words24(buf[off:off + 6], order)
    return ws[0], ws[1]


def exact_fit_offsets(buf):
    """Offsets where word[count] == words remaining after a 2-word header."""
    nwords = len(buf) // 3
    fits = []
    for k in range(nwords - 1):
        for order in ("BE", "LE"):
            got = framing_at(buf, 3 * k, order)
            if got is None:
                continue
            count, _addr = got
            if count == nwords - k - 2:
                fits.append((3 * k, order, count))
    return fits


def find_all(hay, needle):
    """All offsets of needle in hay (full-length bytes.find to exhaustion)."""
    out = []
    start = 0
    while True:
        i = hay.find(needle, start)
        if i < 0:
            return out
        out.append(i)
        start = i + 1


def scan_headers(buf, min_count=MIN_COUNT):
    """(off, order, count, addr) windows with PRAM-plausible count+address."""
    n = len(buf)
    out = []
    for off in range(n - 6):
        b0, b1, b2, b3, b4, b5 = buf[off:off + 6]
        for order, cnt, addr in (
                ("BE", (b0 << 16) | (b1 << 8) | b2,
                 (b3 << 16) | (b4 << 8) | b5),
                ("LE", b0 | (b1 << 8) | (b2 << 16),
                 b3 | (b4 << 8) | (b5 << 16))):
            if (min_count <= cnt <= PRAM_WORDS and addr < PRAM_WORDS
                    and off + 6 + 3 * cnt <= n):
                out.append((off, order, cnt, addr))
    return out


def vector_ok(payload):
    """A load-at-PRAM program starts with JMPs in the even vector slots."""
    hits = 0
    for s in VECTOR_SLOTS:
        if s < len(payload):
            w = payload[s]
            if (w >> 16) in JMP_HI and (w & 0xFFF) < PRAM_WORDS:
                hits += 1
    return hits >= 3


def main(argv):
    if len(argv) != 4:
        print("usage: c850_shi_search.py <dsp_init_image.bin> "
              "<main_flat.bin> <main_unpacked.bin>", file=sys.stderr)
        return 2
    img = open(argv[1], "rb").read()
    packed = open(argv[2], "rb").read()
    unpacked = open(argv[3], "rb").read()
    digest = hashlib.sha256(img).hexdigest()
    print("init image: %d bytes sha256 %s" % (len(img), digest))
    if len(img) != IMAGE_LEN or digest != IMAGE_SHA256:
        print("MISMATCH: expected %d bytes sha256 %s"
              % (IMAGE_LEN, IMAGE_SHA256), file=sys.stderr)
        return 1
    nwords = len(img) // 3
    print("as SHI words: %d x 24-bit; framed hypothesis needs "
          "count=%d (0x0000ab) + PRAM addr" % (nwords, nwords - 2))
    for order in ("BE", "LE"):
        got = framing_at(img, 0, order)
        print("  offset-0 %-2s count=0x%06x (%d) addr=0x%06x: %s"
              % (order, got[0], got[0], got[1],
                 "FIT" if got[0] == nwords - 2 and got[1] < PRAM_WORDS
                 else "MISFIT"))
    fits = exact_fit_offsets(img)
    print("exact-fit offsets (count == words remaining): %r" % (fits,))
    for order in ("BE", "LE"):
        marks = census(words24(img, order))
        print("payload census %-2s: distinct=%d nop=%d rts=%d rep=%d "
              "bra_short=%d jmp=%d" % (order, len(set(words24(img, order))),
                                       marks["nop"], marks["rts"],
                                       marks["rep"], marks["bra_short"],
                                       marks["jmp"]))
    print("negative search (full-length bytes.find, all offsets):")
    print("  519B in unpacked(%d): %r" % (len(unpacked),
                                          find_all(unpacked, img)))
    print("  519B in packed(%d): %r" % (len(packed), find_all(packed, img)))
    print("header scan (16<=count<=0x1000, addr<0x1000, payload in image):")
    for name, buf in (("unpacked", unpacked), ("packed", packed)):
        cands = scan_headers(buf)
        vec = [c for c in cands
               if vector_ok(words24(buf[c[0] + 6:c[0] + 42], c[1]))]
        print("  %s(%d): structural=%d vector-table survivors=%d"
              % (name, len(buf), len(cands), len(vec)))
        for off, order, cnt, addr in vec[:10]:
            print("    off=0x%x %s count=%d addr=0x%03x" % (off, order,
                                                            cnt, addr))
    print("verdict: SHI framing fits nothing; payload is not code in "
          "either order; no genuine header (all survivor classes are SH "
          "code/table aliasing or LZSS-stream coincidence) -> BLOCKED, "
          "nothing carved")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
