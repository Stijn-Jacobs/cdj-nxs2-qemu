# CDJ-850 SH-2A opcode coverage vs QEMU 9.1.0 sh4 target

Firmware source (read-only, never committed):
`/Users/julius/Documents/cdj850-emu/C850MAIN.UPD`
Scanned region: `main` (flat 0x060000-0x200000) as big-endian 16-bit words
aligned from post-label code start 0x60028, via
`scripts/firmware/c850_sh_scan.py` (`sh2a_only_counts` /
`sh2a_first_offsets`). The scanner is a counter, not a disassembler: every
word is tested against the masks below, so data words that alias a pattern
are counted too. Counts over-approximate executed instructions (safe
direction for a coverage survey).

## QEMU source checked (read-only, no clone, no build)

No local QEMU tree exists in this checkout (`qemu-src/` absent;
`setup.sh`/`build.sh` never run here — a full build is a 10-40 min job,
explicitly out of scope for this task). Checked the upstream v9.1.0 tag
over plain raw-file fetch on 2026-10-12:

- `target/sh4/translate.c` —
  `https://raw.githubusercontent.com/qemu/qemu/v9.1.0/target/sh4/translate.c`
- `target/sh4/cpu.h` —
  `https://raw.githubusercontent.com/qemu/qemu/v9.1.0/target/sh4/cpu.h`
- `target/sh4/cpu.c` —
  `https://raw.githubusercontent.com/qemu/qemu/v9.1.0/target/sh4/cpu.c`

Reproduce: fetch those three URLs (tag `v9.1.0`) and grep as below.

## Result: 4 gaps — SH2A_ONLY_USED all uncovered

QEMU's sh4 translator fetches one 16-bit word per instruction
(`ctx->opcode = translator_lduw(...)`, `pc + i * 2` loop) and has no 32-bit
`instruction_size` path (contrast Linux
`arch/sh/kernel/cpu/sh2a/opcode_helper.c`). `target/sh4/cpu.h` defines only
`SH_FEATURE_SH4A` / `SH_FEATURE_BCR3_AND_BCR4` — there is no SH-2A feature
bit. `target/sh4/cpu.c` offers only `-cpu sh7750r|sh7751r|sh7785`
(`any` = sh7750r); only sh7785 sets `SH_FEATURE_SH4A`. Unmatched opcodes
fall through to `do_illegal` → `raise_illegal_instruction`.

Encoding ranges per the SH-2A/SH2A-FPU Software Manual (Renesas
REJ09B0051-0300, section 6.3) and Linux's `opcode_helper.c`; grep evidence
against `translate.c` (no `case 0x0000 / 0x0001 / 0x3001 / 0x3x09` exists —
the only `0x0000…` hit in the file is a comment):

| # | group (first half of 32-bit insn) | mask | count | first file offset (word) | QEMU 9.1.0 sh4 |
|---|-----------------------------------|------|-------|--------------------------|----------------|
| 1 | movi20 `0000nnnniiii0000` | `(w&0xF00F)==0x0000` | 12748 | 0x060106 (0000) | MISS → illegal |
| 2 | movi20s `0000nnnniiii0001` | `(w&0xF00F)==0x0001` | 6005 | 0x060160 (0441) | MISS → illegal |
| 3 | mov32-disp12 `0011nnnnmmmm0001` (MOV.B/W/L, MOVU.B/W, FMOV.S/D) | `(w&0xF00F)==0x3001` | 5910 | 0x0603e0 (3851) | MISS → illegal |
| 4 | bitop32 `0011nnnn0iii1001` (BCLR/BLD/BSET/BST/BAND/…/BXOR.B) | `(w&0xF08F)==0x3009` | 1011 | 0x0624d6 (3049) | MISS → illegal |

`SH2A_ONLY_USED = [movi20, movi20s, mov32-disp12, bitop32]` — none covered
by the sh4 target.

Caveats:

- Group 1's first hit is a `0000` word (probably zero padding, not a real
  MOVI20); the 12748 count includes padding/data aliasing. The other three
  groups' first hits (`0441`, `3851`, `3049`) are non-trivial words.
- Overlapping masks: group 4 words also satisfy group 3's mask when their
  low nibble is 1 (`(w&0xF00F)==0x3001` vs `(w&0xF08F)==0x3009`); the
  scanner classifies in movi20 → movi20s → mov32 → bitop32 order, so group
  4 counts only first-halves not already claimed by group 3. True executed
  counts need control-flow disassembly (future work).

## -cpu recommendation for Task 5

No existing `-cpu` covers SH-2A 32-bit instructions. Use the big-endian
system binary (`qemu-system-sh4eb`, same source as `qemu-system-sh4`) with
`-cpu sh7785`: it is the only model with the `SH_FEATURE_SH4A` feature bit
and hence the most complete translator. Expect `illegal instruction`
traps on all four groups above until the translator is patched.

## Follow-up work items (NOT done here — do not patch QEMU in this task)

1. Add a 32-bit fetch path to the sh4 translator: detect first-halves per
   the four masks above (cf. Linux `instruction_size()`), consume the
   trailing half-word, advance PC by 4.
2. Implement semantics: MOVI20/MOVI20S (sign-extended imm20 load, +8 shift
   for S), disp12 MOV/MOVU/FMOV loads/stores, and the ten B*.B bitops.
3. Gate behind a new `SH_FEATURE_SH2A` + `-cpu` model (or enable for the
   CDJ-850 board's CPU), keeping SH-4 behaviour unchanged.
4. Re-run `scripts/firmware/c850_sh_scan.py` plus a control-flow
   disassembly pass to confirm executed (not just aliasing) coverage.
