# SPDX-License-Identifier: GPL-2.0-or-later
"""CDJ-850 DSP record SPI-bitstream re-decode (Task 8i, offline, NOT-SPI).

Reconstructs the BITSTREAM from the existing `$CDJ850_DSP_RECORD` file
(Task 8c rig: packed 20 B {pc u32, ms u64, off u16, size u8, val u32} LE)
instead of the (index,data)-pair decode, then tests the DSP56371 UM
Appendix A framing (3 B count + 3 B address + 3N 24-bit words, MSB-first
per UM section 7.4) on the resulting stream in both byte orders with the
exact-fit search from `c850_shi_search` (reused here, not duplicated).

Clock/data assignment (from the unpacked MAIN image
`extract/cdj850/main_flat.bin` -> SDRAM 0x0C000000, SH-2A big-endian;
record PCs are QEMU `env.pc` at write time, i.e. +2 past an executing
write in a delay slot -- see Task 8h off-by-two note):

- Port-E base: `movi20 #932354` sign-extends to 0xFFFE3A02 (PEDRL data)
  and `movi20 #932358` to 0xFFFE3A06 (PEIORL direction); e.g.
  0x0C0FC736 / 0x0C0FCA7E / 0x0C0FCBB0 / 0x0C0FCD84 / 0x0C0FCE56.
- PE6 gate (read): 0x0C0FC73E `mov.w @(r0,r12),r0` + 0x0C0FC740
  `and #64,r0` + `shad #-6` (siblings 0x0C0FCA86 / 0x0C0FCBB0 /
  0x0C0FCD8C / 0x0C0FCE5E, same mask). PE6 is bit 6 (0x0040).
- PE8 gate (read): 0x0C0FC752 `mov.w @(r0,r12),r2` + 0x0C0FC754
  `and r4,r2` where r4 = 0x0100 built at 0x0C0FC74C `shll8 r4`
  (r4 = 1 -> 0x0100) + `shad #-8` at 0x0C0FC756. PE8 is bit 8.
- Sampler high nibble (read): phase 1 at 0x0C0FC922
  `mov.w @(r0,r11),r5` + 0x0C0FC92E `and r2,r5` (r2 = 0xF000 via
  `movi20 #61440` at 0x0C0FC924) + 0x0C0FC934 `shad #-12,r5`;
  phase 2 at 0x0C0FC95C + 0x0C0FC968 (same 0xF000 mask).
  Byte = (ph1 << 4) | ph2. No isolated PE9/PE12/PE14 mask exists.
- Exhaustive scan of 0x0C0FC600-0x0C0FD400: the only Port-E word
  masks are #64 (PE6), 0x0100 (PE8, via shll8), and 0xF000 (nibble);
  `bset`/`bclr` touch only bit 6; `and #240` / `and #15` are
  reply-byte/nibble merges, not Port-E isolates. The hypothesis's
  "samples PE9/PE12/PE14" does not match the code.
- Writes: byte-send 0x0C0FCC68 emits 5 PEDRL halfwords at record PCs
  0x0C0FCC86/B2/C4/D6/F8 (w0/w3 high nibbles = hi/lo, w1/w4 = same +
  PE8 strobe bit); strobe subroutine 0x0C0FCD0E writes record PCs
  0x0C0FCD2C (setup, e.g. 0x5080) / 0x0C0FCD54 (pulse, +0x0100);
  sampler setup record PCs 0x0C0FC918/0x0C0FC948 (setup) with
  pulse PCs 0x0C0FC922/0x0C0FC95C (record attribution +2 from the
  executing delay-slot write); PEIORL direction at record
  0x0C0FCDEA = 0xF1C0 (PE12-15 outputs, `or 0xF000`) /
  0x0C0FCDF0 = 0x01C0 (inputs, `and 0x0FFF`).
- Software delays are real but nibble-paced, not bit-paced: delay
  subroutine at 0x0C107932 (`add #-8,r15; mov #0,r6; extu.w r4,r5;
  bra 0x0C1079D8`, loop at 0x0C1079C8 `movi20 #8192,r0; shll16 r0`
  = 0x20000000 poll + counter, `bf`, `rts` at 0x0C1079E0), called
  with r4 = count (e.g. 0x0C0FC91C `jsr @r12` r4 = 1;
  0x0C0FCA30 `jsr @r10` r4 = 1; 0x0C0FCA0A `bsr 0x0C0FCD0E`
  r4 = 80). Guards at 0x0C0C688A (save) / 0x0C0C6894 (restore)
  bracket Port-E accesses (e.g. 0x0C0FCA10 / 0x0C0FCA20).

Reconstruction method (this module):

- Order PEDRL halfword writes by file order (verified ms-sorted for
  `/tmp/8c-record20.bin`: 91834 entries, ms 6057-24653, 75465 PEDRL +
  16369 PEIORL). Upload window = through the last byte-send-site
  write (index-based, boot-independent): 5174 PEDRL.
- SCK candidates: PE8 (bit 8, the only per-nibble 0->1 strobe) and
  PE6 (bit 6, per-transfer gate) for completeness. Rising edges
  0->1 in file order are the clocks. Full record: 25030 PE8 rises
  (7997 CD2C->CD54 strobe + 7994 C948->C95C + 7993 C918->C922 +
  519 CC86->CCB2 hi + 519 CCD6->CCF8 lo, all with stable high
  nibble); upload window: 1600 PE8 rises (558 strobe/sampler hi=5 +
  1038 byte-send data), 375 PE6 rises.
- Data: each of PE12/PE13/PE14/PE15 alone (serial, 1 bit/clock,
  SPI-shaped) plus nibble-parallel 4 bits/clock in both MSB orders
  ([15,14,13,12] and [12,13,14,15]) to cover the "sample data lines"
  reading. Bits assembled MSB-first per UM section 7.4 (first
  sampled bit = bit 7 of byte 0); trailing partial byte dropped.
- Framing: `framing_at` offset-0 + `exact_fit_offsets` (count ==
  words-remaining, both orders) + `census` (NOP/RTS/valid-REP/
  BRA-short/JMP-family per DSP56300FM ch. 13) + `vector_ok`,
  all imported from `c850_shi_search`.

Result (2026-10-13, see Task 8i report): every reconstruction
misfits everywhere. Nibble-parallel byte-only [15..12] reproduces
the pinned 519-B image byte-identically (SHA
7132eb444b686275a6b2e4e595e6fd7e7351bea14e43709ce5998adfc3972064,
173/173 distinct, zero markers) -- i.e. the "bitstream" collapses
to the already-rejected control image. All serial streams (200/129/
46 B) have no exact fit at any offset in either order and only
0-4 isolated marker singletons (noise). The full-record stream is
control-patterned: 93.8% nibble-1 rises, 93.6% 0x11 bytes
(endless reply-spin idle 0x1080/0x1180/0x11C0), head = strobe 0x55
+ data burst, tail = all 0x11. Verdict NOT-SPI: no program words
emerge, nothing carved.
"""
import hashlib
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from c850_dsp_decode import PEDRL_OFF, SITES
from c850_shi_search import census, exact_fit_offsets, framing_at, words24

REC_STRUCT = struct.Struct("<IQHBBI")
PEIORL_OFF = 0x206
PE6 = 0x0040
PE8 = 0x0100
PE12 = 0x1000
PE13 = 0x2000
PE14 = 0x4000
PE15 = 0x8000
DATA_BITS = (12, 13, 14, 15)
BYTE_RISE_PAIRS = frozenset((
    (0x0C0FCC86, 0x0C0FCCB2),
    (0x0C0FCCD6, 0x0C0FCCF8),
))
NIBBLE_MSB_ORDERS = {
    "15-12": (15, 14, 13, 12),
    "12-15": (12, 13, 14, 15),
}


def read_pedrl(path):
    """PEDRL halfword writes in file order as [(pc, ms, val)]."""
    data = open(path, "rb").read()
    if len(data) % REC_STRUCT.size != 0:
        raise ValueError("truncated record file %s" % path)
    out = []
    for i in range(0, len(data), REC_STRUCT.size):
        pc, ms, off, size, _reserved, val = REC_STRUCT.unpack_from(data, i)
        if off == PEDRL_OFF and size == 2:
            out.append((pc, ms, val & 0xFFFF))
    return out


def upload_window(ped):
    """PEDRL prefix through the last byte-send-site write (inclusive)."""
    sites = set(SITES)
    last = max(i for i, (pc, _ms, _v) in enumerate(ped) if pc in sites)
    return ped[:last + 1]


def rise_bits(ped, sck_bit, data_bit, byte_only=False):
    """Data bits sampled at SCK 0->1 rising edges (file order)."""
    bits = []
    for i in range(1, len(ped)):
        prev_pc, _pms, prev_v = ped[i - 1]
        cur_pc, _cms, cur_v = ped[i]
        if ((prev_v >> sck_bit) & 1) == 0 and ((cur_v >> sck_bit) & 1) == 1:
            if byte_only and (prev_pc, cur_pc) not in BYTE_RISE_PAIRS:
                continue
            bits.append((cur_v >> data_bit) & 1)
    return bits


def nibble_bits(ped, order, byte_only=False):
    """4 data bits per SCK (PE8) rise in the given MSB-first order."""
    bits = []
    for i in range(1, len(ped)):
        prev_pc, _pms, prev_v = ped[i - 1]
        cur_pc, _cms, cur_v = ped[i]
        if ((prev_v >> 8) & 1) == 0 and ((cur_v >> 8) & 1) == 1:
            if byte_only and (prev_pc, cur_pc) not in BYTE_RISE_PAIRS:
                continue
            for b in order:
                bits.append((cur_v >> b) & 1)
    return bits


def bits_to_bytes_msb(bits):
    """Pack bits MSB-first (first bit = bit 7); drop partial tail."""
    out = bytearray()
    for i in range(0, len(bits) // 8 * 8, 8):
        b = 0
        for j in range(8):
            b = (b << 1) | (bits[i + j] & 1)
        out.append(b)
    return bytes(out)


def analyze_stream(bts):
    """Framing + census summary for a reconstructed byte stream."""
    info = {"len": len(bts), "head": bts[:8].hex(),
            "sha": hashlib.sha256(bts).hexdigest() if bts else None}
    if len(bts) >= 6:
        for order in ("BE", "LE"):
            count, addr = framing_at(bts, 0, order)
            info["offset0_%s" % order] = (count, addr)
    else:
        info["offset0_BE"] = info["offset0_LE"] = (None, None)
    info["fits"] = exact_fit_offsets(bts) if len(bts) >= 6 else []
    for order in ("BE", "LE"):
        ws = words24(bts, order) if len(bts) >= 3 else []
        info["census_%s" % order] = census(ws)
        info["distinct_%s" % order] = len(set(ws))
    return info


def fmt_census(marks):
    return ("nop=%d rts=%d rep=%d bra=%d jmp=%d"
            % (marks.get("nop", 0), marks.get("rts", 0),
               marks.get("rep", 0), marks.get("bra_short", 0),
               marks.get("jmp", 0)))


def main(argv):
    if len(argv) != 2:
        print("usage: c850_spi_decode.py <record>", file=sys.stderr)
        return 2
    ped = read_pedrl(argv[1])
    ms0 = min(ms for _pc, ms, _v in ped)
    ms1 = max(ms for _pc, ms, _v in ped)
    print("PEDRL writes: %d (ms %d-%d, file order is ms-sorted: %s)"
          % (len(ped), ms0, ms1,
             all(ped[i][1] <= ped[i + 1][1] for i in range(len(ped) - 1))))
    up = upload_window(ped)
    print("upload window: %d PEDRL through last byte-send write" % len(up))
    n_full = sum(1 for i in range(1, len(ped))
                 if ((ped[i - 1][2] >> 8) & 1) == 0
                 and ((ped[i][2] >> 8) & 1) == 1)
    n_up = sum(1 for i in range(1, len(up))
               if ((up[i - 1][2] >> 8) & 1) == 0
               and ((up[i][2] >> 8) & 1) == 1)
    print("PE8 rises: full=%d upload=%d" % (n_full, n_up))
    streams = []
    for sck_bit, sck_name in ((8, "PE8"), (6, "PE6")):
        for data_bit in DATA_BITS:
            for byte_only in (False, True):
                pool = up if sck_bit == 8 else up
                bits = rise_bits(pool, sck_bit, data_bit,
                                 byte_only=byte_only)
                bts = bits_to_bytes_msb(bits)
                streams.append(("serial SCK=%s DATA=PE%d %s"
                                % (sck_name, data_bit,
                                   "byte-only" if byte_only else "all-rises"),
                                bts))
    for order_name, order in NIBBLE_MSB_ORDERS.items():
        for byte_only in (False, True):
            bits = nibble_bits(up, order, byte_only=byte_only)
            streams.append(("nibble %s %s" % (
                order_name, "byte-only" if byte_only else "all-rises"),
                bits_to_bytes_msb(bits)))
    # Full-record nibble stream for the control-pattern characterization.
    full_nib = bits_to_bytes_msb(nibble_bits(ped, NIBBLE_MSB_ORDERS["15-12"]))
    print("full-record nibble 15-12: %d bytes head=%s tail=%s"
          % (len(full_nib), full_nib[:16].hex(), full_nib[-16:].hex()))
    bad = 0
    for name, bts in streams:
        info = analyze_stream(bts)
        be_c, be_a = info["offset0_BE"]
        le_c, le_a = info["offset0_LE"]
        print("%s: %d bytes sha=%s head=%s offset0 BE(c=0x%x a=0x%x) "
              "LE(c=0x%x a=0x%x) fits=%r distinctBE=%d/%d %s / %s"
              % (name, info["len"], info["sha"] or "-",
                 info["head"],
                 be_c if be_c is not None else -1,
                 be_a if be_a is not None else -1,
                 le_c if le_c is not None else -1,
                 le_a if le_a is not None else -1,
                 info["fits"][:2],
                 info["distinct_BE"], info["len"] // 3,
                 fmt_census(info["census_BE"]),
                 fmt_census(info["census_LE"])))
        if info["fits"]:
            bad += 1
    print("verdict: NOT-SPI -- no exact fit at any offset in either order, "
          "census holds at 0-4 isolated singletons (noise); nibble "
          "byte-only 15-12 reproduces the pinned 519-B control image, "
          "full-record stream is 93%%+ idle (nibble-1 0x11) -> nothing carved")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
