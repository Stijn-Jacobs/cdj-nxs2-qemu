# CDJ-850 MAIN container layout (C850MAIN.UPD)

Source: `/Users/julius/Documents/cdj850-emu/C850MAIN.UPD` (read-only, never committed).
Container: flat big-endian binary flash image, no manifest, no S-records.
Profile: size 2156300 bytes, SHA-256
`12ed34539d844e4c3d9230eebccbc31f0b6ff2ac4d813455084987c53beac3b7`.
Label: 32-byte field at flat offset 0x60000:
`CDJ-850 MAIN` + `1.13` (cf. `main_decode.LABEL_SIZE`).

Carved with `scripts/firmware/c850_decode.py::carve_regions()`:

| region | flat range          | size (bytes) | size (hex) | SHA-256                                                        |
|--------|---------------------|--------------|------------|----------------------------------------------------------------|
| boot   | 0x000000–0x040000   | 262144       | 0x40000    | eb4f9f9885d6157d7521b13a18b20f685706152fabfe3ef2c3f6c9a5cd51d58c |
| erased | 0x040000–0x060000   | 131072       | 0x20000    | b5a41c3758763bbec72769fab4a2533bf2db0b6312d93d25a695f9e4b9e02260 |
| main   | 0x060000–0x200000   | 1703936      | 0x1A0000   | 51457aadf771fb7027569df8e3def364f0e456c7a2af2ec03a51b43efbd5c2de |
| tail   | 0x200000–0x20E70C   | 59148        | 0xE70C     | d9fca9e35615d60b4f50471d08483ff3af7363aabe41a2f44f14ec36d5fd8080 |

Survey assertions (2026-10-12, re-verified here):

- `erased` region is all `0xFF` (erased flash).
- `tail` region is all `0x00`.
- `main` region starts with the 32-byte label at its offset 0 (flat 0x60000).

`main` SHA-256 above was computed with
`python3 -c "import hashlib; print(hashlib.sha256(open('main.bin','rb').read()).hexdigest())"`
on the carved output in a temp dir (never under `extract/`).

## DSP init image (Task 8c, RECORDED — not carved from flash)

Source: MAIN's Port-E bit-bang upload driver, observed live through the
`dsp56371.c` record buffer (`CDJ850_DSP_RECORD`), three independent boots
(20 s / 100 s / 25 s) — every byte driven by MAIN, none invented.
Decode: byte-send `0x0C0FCC68` emits 5 PEDRL words per call
(`0x0C0FCC86/0xB2/0xC4/0xD6/0xF8` sites); byte =
`((w[0]>>12)&0xF)<<4 | ((w[3]>>12)&0xF`.

- Bytes: 519 (259 `(index,data)` control pairs + trailing `0x00` pad).
  Head pairs `(139,160),(140,128),(144,13),(145,136),…` match the static
  `0x0C0FDA2C` 36-pair sequencer indices (Task 8b §1e) with LIVE data
  (authoritative over the static delay-slot parse where they differ);
  body walks banks `0xF0–0xF8` then `0xD7` with a repeated offset
  sequence — a DSP control-register/memory fill, NOT DSP56371 opcodes
  (24-bit-word census: 173/173 distinct, no vectors/branches/NOP —
  address-structured init data).
- SHA-256: `7132eb444b686275a6b2e4e595e6fd7e7351bea14e43709ce5998adfc3972064`.
- Local copy (untracked, reproducible via the record env):
  `extract/cdj850/dsp_init_image.bin` (519 B).
- Census decision: opcode census N/A (not code) → no
  interpreter-vs-vendor core call from this image; the genuine reply
  needs DSP-side execution semantics (Task 8c report).

## Packed-MAIN → SDRAM decoder map (Task 8d, VERIFIED)

Carved with `scripts/firmware/c850_unpack.py::decode_main()` (same
Okumura/Yoshizaki codec as `lzss_decode.py`: ring 4096, init `0x20`,
pos 4078; unpack the whole `main_flat.bin`, not the stream alone —
the 6390-B pad is consumed by the decoder overrun exactly as the
bootloader consumes it):

- Packed field: `main_flat` `0x20–0x19E70A` (4-B BE32 length
  `0x0019E6E6` + stream) = UPD flat `0x060020–0x1FE70A`.
- Unpacked: SDRAM `0x0C000000–0x0C39231E` (3744542 B, `0x39231E`).
- SHA-256 (unpacked):
  `26c8bfe933afc1e08a05dc65513303f1343a570d1e87f2734d60316e87c01591`.
- Remainder: `main_flat` `0x19E70A–0x1A0000` (6390 B checksum + zero
  pad; head bytes `FB71`, rest zeros).
- Verification (2026-10-13): output matches a live-boot 32 MB SDRAM
  dump over code/rodata (spot-checked at `0/0x100/0x1000/0x10000/
  0x100000/0x18BF2C`; BSS/heap diverge at runtime from `0x1C14DC`
  as expected); driver code (`0x0C0FC9DC`, gates, send, sampler,
  `0x0C0FCD6C–0x0C0FCE52`, callers) disassembles identically from
  the unpack as from live SDRAM dumps.
- Negative search (same date): the 519-B init image (head
  `8BA08C80…`) and the 284-B `0x0CE4B6F4` candidate header
  (`000B000000330057…`) are both ABSENT from the packed AND
  unpacked bytes — no contiguous DSP program blob lives in MAIN.
  (The candidate sits at SDRAM offset `0xE4B6F4`, beyond the
  `0x39231E` image — runtime heap, not image.)

## SHI-framed hunt (Task 8e, VERIFIED NEGATIVE 2026-10-13)

Tool: `scripts/firmware/c850_shi_search.py` (GPL + tests
`test_c850_shi_*`, full suite 665 passed / 38 skipped / 1 xfailed).
UM source: DSP56371UM App. A — SHI slave-SPI bootstrap takes word
count (3 B) + start address (3 B) + N program words (3 B each),
MSB-first, 24-bit (HM=10), HCSR `$A9`, HREQ per-word handshake
(§7.7.1); PRAM 4K `$000000–$000FFF` (Fig. 3-1); opcode markers per
DSP56300FM ch. 13 (NOP `$000000`, RTS `$00000C`, REP `$06xxxx`
+ `$Ax` low nibble, BRA-short `$05`, JMP `$0A/$0C`, BRA-long `$0D`).

- 519-B re-parse: 173×24-bit + 0 rem. Offset-0 count is `0x8BA08C`
  (BE) / `0x8CA08B` (LE), never 171 (`0xAB`); NO offset anywhere
  satisfies count == words-remaining (either order); 16-bit, data-
  only, and index-only framings misfit equally. Payload census both
  orders: 173/173 distinct, 0 NOP, 0 RTS, 0 valid REP, 0 BRA-short;
  the only JMP-family words are 2 isolated LE singletons in head
  control bytes; the 43 `00Fxxx` BE words spell the F-bank control
  walk (`00 f1 b7 … 00 f7 d0`), not opcodes; zero-runs ≤ 2 B.
- Header scan (16≤count≤0x1000, addr PRAM, BE+LE, full-image):
  unpacked → 4816 structural / 253 vector-table survivors; packed →
  88 / 1. Every class explained, none genuine: top fake counts are
  SH opcodes (`0x0900` = SH `nop`+zero, `0x0200/0x0700` = PC-rel
  loads); examined survivors disassemble as valid SH-2A (e.g.
  `0x804c`, `0x2100` payload, `0x15EB0` MAC-loop driver,
  `0x1D1BC` movi20 immediates); BSS-region hits are SH
  length/address tables; no survivor combines ≥4 distinct vector
  targets + RTS + valid REP + nonzero reset jump (pinned in test).
  The packed survivor (`0xE1D36` LE count=1571 addr=`0xA28`) sits
  inside the LZSS stream: 1567/1571 distinct, 0 NOP/RTS/REP — stream
  coincidence, rejected.
- Rigorous negative redo (full-length `bytes.find` to exhaustion):
  519 B absent from packed (1703936 B) and unpacked (3744542 B);
  284-B candidate
  (SHA `d47e1d71…c125d9fa4`, from live SDRAM `0x0CE4B6F4`) absent
  from both as well.
- Unpacker loop PCs (fresh 12 s early-window `-d in_asm` trace
  `/tmp/8e-inasm.main.log`, 7923 TBs; 8d's own log already held
  them — its exact-case grep `0DDEFA78` missed the lowercase
  `0xddefa78`): decoder entry `0x0DDEFA20`, bit-test head
  `0x0DDEFA52`, literal store `0x0DDEFA78`/`0x0DDEFA7A`
  (`bra 0xddefa52`, `and r1,r2` 4096-ring wrap — matches the
  committed LZSS ring), match-copy loop `0x0DDEFAA6`
  (`bra 0xddefaa6`), exit `0x0DDEFAC2`, caller `0x0DDEFBE4`,
  then jump to unpacked entry `0x0C000000`.
- Verdict: SHI framing fits nothing and no header exists — carve
  BLOCKED with new evidence (mode-5-vs-PROM re-examination for the
  next task: the observed 519 B is phase-1 control fill; the program
  upload, if any, is gated behind the never-completing reply
  handshake, or assembled purely from scattered immediates).

## SPI-bitstream re-decode (Task 8i, offline NOT-SPI 2026-10-13)

Tool: `scripts/firmware/c850_spi_decode.py` (GPL + tests
`test_c850_spi_*`; reuses `c850_shi_search` framing/census, not
duplicated). Record: existing `/tmp/8c-record20.bin` (91834 entries,
ms 6057-24653, 75465 PEDRL + 16369 PEIORL; file order is ms-sorted;
no re-record, no boots, no board-code changes).

- Clock/data assignment (unpacked MAIN, SH-2A BE; record PCs are
  QEMU `env.pc`, +2 past delay-slot writes): PE6 gate reads
  (`0x0C0FC73E` + `and #64`; siblings `CA86/BB0/CD8C/CE5E`),
  PE8 gate reads (`0x0C0FC752` + `and r4,r2`, r4 = 0x0100 via
  `shll8` at `0x0C0FC74C`), sampler nibble reads (ph1 `0x0C0FC922`
  + `and 0xF000` at `0x0C0FC92E` + `shad #-12`; ph2 `0x0C0FC95C` /
  `0x0C0FC968`). Exhaustive `0x0C0FC600-0x0C0FD400` scan: only
  `and #64` / 0x0100 / 0xF000 touch Port-E; `bset/bclr` only #6;
  `and #240/#15` are byte/nibble merges; `bld` samples SDRAM flags
  (`0x0C0FCA44` bit 7 of `0x0C7F6D6C`). No PE9/PE12/PE14 isolate
  exists — the hypothesis's sampling claim does not match the code.
  Delays via `0x0C107932` (r4 = count; e.g. `0x0C0FC91C` r4 = 1,
  `0x0C0FCA0A` r4 = 80) are nibble-paced; guards
  `0x0C0C688A/94` bracket Port-E. PEIORL `0x0C0FCDEA=0xF1C0`
  (outputs) / `0x0C0FCDF0=0x01C0` (inputs) toggles the whole
  nibble — parallel GPIO, not fixed-direction SPI.
- Bitstream (SCK = PE8 0->1 rises; data = PE12/13/14/15 serially
  plus nibble-parallel both MSB orders; MSB-first per UM §7.4):
  full = 25030 rises; upload (through last byte-send) = 5174 PEDRL,
  1600 rises (558 strobe/sampler hi=5 + 1038 byte-send), 375 PE6
  rises. Serial upload streams = 200/129/46 B; nibble upload =
  800 B (all) / 519 B (byte-only).
- Framing (both orders, exact-fit at every offset): ALL streams
  `fits == []`; offset-0 counts are megabytes (never 171/199/...);
  census 0-4 isolated singletons (nibble all-rises 7-8) — noise.
  Nibble byte-only 15-12 reproduces the pinned 519-B image
  byte-identically (SHA `7132eb44…3972064`, 173/173 distinct, zero
  markers) — the "bitstream" collapses to the rejected control
  image. Full-record nibble = 12515 B, 93.6% `0x11` (23476/25030
  rises are nibble-1; endless reply-spin `0x1080/0x1180/0x11C0`
  idle); head = strobe `0x55` + data burst, tail = all `0x11`.
- Verdict NOT-SPI: periodic/control-patterned, no program words
  (no vectors/branches/repeats, no PRAM-range count+address),
  nothing carved; core decision stays blocked.
