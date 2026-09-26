# C66x core — decisions and how to check them

Build and test: `make -C hw/cdj/c6x O=~/build/c6x all core-test` (the tests
need a `tic6x-elf` binutils; `m1` also needs a disassembly cache of the DSP
image, which is built from your own firmware).

## Decode
- `c66x_decode.c` is a structured port of binutils 2.42 `print_insn_tic6x`,
  driven by the vendored tables in `binutils/`. `make m1` diffs it against the
  objdump cache `/tmp/dsp-disasm.json`: **158,660 entries, 0 differences**
  outside what objdump could not decode, plus one deliberate departure below.
- **RS header bit (binutils bug).**
  - SPRU732J 3.9.2.2 says RS moves only the 3-bit register fields to
    A16-A23/B16-B23.
  - The 5-bit register of the compact moves (LSDmvto `srcms:src2`, LSDmvfr
    `dstms:dst`, G-1/G-2) is absolute. binutils adds 16 to it anyway.
  - The effect: 0x80072576 decoded as `mv b22,a19` instead of `mv b6,a19`. The
    loop then zeroed the MP3 bitrate-table pointer at 0x80102BE8, so every frame
    header was rejected.
  - 202 compact `mv` change and no other form does. `m1` counts them separately.
  - Checked against the doc and left as they were:
    - Dpp and Sx1b ignore RS (4-bit fields).
    - L2c, Lx3c and Lx1c use a fixed A0/A1 dst.
    - M3 and the compact DW forms take RS on their shifted 2-bit field.
    - The Dind/Doff4 ptr is A4-A7.
  - `tests/core_rs.s` checks this with hand-encoded words, because gas cannot be
    the oracle here.
- MP3 header-search instructions are tested in
  `tests/core_mp3.s` and `core_resv.s`:
  - the ring store with its same-packet `xor 3` index, including wrap;
  - the reservoir backward copy with its self-branch and `mpysu` mask;
  - `sub 1,b5,b5`;
  - `extu` 21,21 / 30,27 / 25,30 / 28,28 / 21,23.
- binutils 2.42 lacks the C66x additions. `gen_ext.py` builds
  `c66x_ext_table.h` from TI SPRUGH7's opcode figures (153 entries). It adds two
  inferred entries, opfields 0x74/0x76 = fast `.S` addsp/subsp; see the comment
  in `gen_ext.py`. Across the image: 1,696 decodes
  in code context, with fsubsp/faddsp/dadd/dmpysp/dmv/fmpydp/qmpysp on top.
- **SPRUGH7 4.93 is DINTSP, not DINTHSP.** The manual prints it under the
  heading DINTHSP, but its title and Execution block convert the two 32-bit
  words of a register pair. Decoded as DINTHSP (the 16-bit halves of one
  register), the MASTER TEMPO phase wrap (`dspint` then `dintsp`, 4 sites
  from 0x8003E930) left every odd-lane phase unwrapped and the time-stretch
  output ran at full scale. `gen_ext.py` renames it and reads src2 as a pair;
  `tests/ext_test.c` runs the firmware's own word.

## Timing — the pipeline is architectural
- **Result latency is visible to software**; SPRU732 Table 3-3 is literal.
  - Measured over the whole image: the distance from a
    non-PROT load to the first read of its destination. It peaks at exactly
    **5 cycles** (6,243 of 19,343).
  - In PROT fetch packets (4 NOP cycles added after each load) the peak is at
    1 packet.
  - 16x16 multiplies peak at 2 cycles; plain `add` peaks at 1.
- **Register writes** land at the cycle binutils' operand table gives: E1 = 1,
  loads 5, mpysp 4, adddp 6/7, mpyid 9/10. A write becomes readable one cycle
  later. For C66x extensions the write cycle is the page's "Delay Slots" + 1.
- **Control registers:** mvc to ILC/RILC takes 4 cycles (SPRU732 7.4.3), and
  ICR/ISR take 2.
- **A delay slot is one E1 advance:** an execute packet or a NOP cycle.
  - NOP n, BNOP n, CALLP (5), ADDKPC n and PROT loads (4) all add NOP cycles
    that count as slots; stalls and interrupt entry do not.
  - A branch in cycle C lands its target in C+6, and `bnop t,n` runs 5−n packets.
- Cross-path stalls (SPRU732 3.7.4) are modelled; about 45k occur during boot.
  - Only a result written at E1 (and a load's base-register update) stalls a
    1X/2X read in the next cycle.
  - A load's data does not stall, and neither does a multi-cycle result read as
    it lands.
  - The MP3 decoder at 0x8006CE34 depends on this: it reads an E4 `mpysp`
    result through 1X exactly as it lands, in the same packet that reads an E4
    square before an E5 load replaces it. Stalling there produces garbage
    floats.
  - `tests/core_xstall.s` and `tests/core_wlat.s` cover this.
  - The image has these timing-sensitive packets: 754 write
    one register at two cycles, and 198 read a multi-cycle result through a
    cross path as it lands.
- **Memory timing.** Loads and stores both touch memory at E3 (SPRU732J 4.2.3),
  so the core models both at E1.
  - A store is applied at the end of its cycle, after every instruction of that
    cycle has executed. So in one execute packet, or across overlapping SPLOOP
    iterations, a load reads the old value whatever the instruction order.
  - A store takes its source register at E1, as the stage table says.
  - Tests: `tests/core_memord.s` and `core_splmem.s`.
  - The image has 332 packets with a store before a load, 8 of
    them to the same base and offset.
- **A cross-path stall freezes the whole pipeline.** Writes still in their
  stages land one cycle later, so a load stays readable five execute packets
  after it was issued.
  - The long-block scale builder at 0x8006D040 stalls every iteration and
    depends on this: without it, 406 integer results there were wrong and the
    gains became 2^119, which gave the NaNs.
  - Test: `tests/core_stall.s`.
- **Float gate.** Every SP/DP instruction in a replay trace is re-evaluated
  against IEEE-754.
  - A boot replay checked 387,250 ops with 0 mismatches.
  - Loop-buffer instructions are not judged, because iterations after the
    first are not traced.

## SPLOOP (SPRU732 ch. 7)
- The loop is modelled as iteration overlay. Iteration k runs body offset c at
  t0 + k·ii + c.
- **ILC tests at stage boundaries:** SPLOOPD/W force continue for the first 3
  cycles.
- **SPLOOPW exit:** the loop exits abruptly when the predicate, sampled 3 cycles
  before the boundary, is false.
- **Epilog:** drains in-flight iterations; program fetch resumes after
  fstg·ii + fcyc cycles.
- **Masking:** SPMASK masks buffer units and keeps masked program-memory
  instructions out of the buffer.
- **Unsupported:** reload (SPKERNELR/SPMASKR) traps; the image has none.
  Interrupts are held while a loop is active, rather than interrupt-draining.
- **Tests:** `tests/core_pipeline.s` runs SPRU732 Example 7-4 exactly, and 7-13
  with one fix (the printed example copies only the last byte).

## Branches in delay slots
- Up to 6 branches can be in flight (one per E1 cycle), each landing 5 slots
  after its own E1.
- Stage 1's divide (0x00800280) relies on this. It seeds five branches, so its
  one-packet loop has a landing every cycle.
- A 4-entry queue silently dropped two of them. The queue now holds 8 and traps
  on overflow.
- Tested by `tests/core_brchain.s`.

## SPLOOP exit and interrupts
- **Exit timing.** When the kernel had disabled program fetch, post-loop code
  reaches E1 at drain start + min(fstg·ii+fcyc, epilog) + 6.
  - The 6 comes from the image. Over all 273
    SPLOOP(D) loops it scores register conflicts (a post-loop write visible
    while the last iteration still reads) and functional-unit conflicts with
    the epilog, for offsets 0..10.
  - h=6 is the smallest clean offset; h=8 is also clean. 6 matches the six
    pipeline stages from fetch to E1.
  - With h=0 the frame copier 0x0080209C wrote its trailer as 0x04020001
    (`tests/core_splexit.s`).
  - SPLOOPW abrupt exits get the same refill; nothing in the image
    discriminates that case.
- **Interrupt draining (7.13).** At a stage boundary, an enabled pending
  interrupt drains the loop instead of starting an iteration, but only when
  the loop is not loading, is past the first 3 cycles, and ILC ≥ ceil(dynlen/ii).
  - The interrupt is then taken with IRP = the SPLOOP packet and ITSR.SPLX = 1.
  - On return, that packet's other instructions and SPMASKed program-memory
    instructions act as NOPs.
  - Tested by `tests/core_sploop_irq.s` (INT4 pulsed across the loop).
  - Approximations: while draining, SPLOOPW termination is not re-evaluated,
    and BNOP-in-body resume is not modelled.

## Busy-wait skipping
- The app has no IDLE instruction. Its main loop at 0x80076F00 polls the state
  machine 0x80076DF0; with no work it runs the poll list 0x800739D8, including
  a status republish (0x0080E380 → 0x008C8950/54/58, DSP indices 0x150/154/158).
- `c66x_set_idle_loop()` returns C66X_STOP_IDLE at the declared head once a
  whole iteration was a fixed point. A fixed point means:
  - no store outside the stack window, ignoring same-value stores;
  - no bus access;
  - no TSC read and no interrupt;
  - identical registers and no writes in flight.
- Offline boot: 39,564 of 40,019 passes were fixed points (98.9%, 9,493 cycles
  each).
- Callers should jump to their next interrupt-raising event and call
  `c66x_skip_cycles()` with the same amount.

## Performance
400M-cycle offline boot (stage 1 + 37 uPP windows + app), 16-core host:

| build | M insns/s | M cycles/s | host load |
|---|---|---|---|
| first cut | 32.3 | 56 | ~0 |
| optimised, `-O3`, runner trace off | 53–56 | 91–97 | ≤0.6 |
| optimised, `-O3`, runner trace on | 44–50 | 77–87 | 0.6–1.7 |

What changed:
- sub-kinds precomputed at decode (no name compares on the hot path);
- a per-packet shape cache with a PC-indexed front;
- a read-operand mask;
- a fast path for multi-cycle NOP cycles (~half of all cycles);
- a two-level decode page map with a code-page flag, so data stores skip
  invalidation;
- `-O3`.

Host writers into guest RAM (DMA) must call `c66x_invalidate()`.

### Fast path
Workload: the offline audio replay, 800M cycles, pinned to one CPU, best of 3.

| build | q=2300 M cycles/s | q=1M M cycles/s | host load |
|---|---|---|---|
| optimised (reference) | 72.2 | 73.9 | ~2 |
| fast path | 108.9 | 106.8 | ~2 |

Every step was checked bit-exact against the reference, on two hashes: the full
state after every step for the first 50M cycles, and the final
regs/crs/L2/DDR plus the WAV. What changed:
- `fast_cycles()` runs the common cycle (no SPLOOP, interrupt, idle, trace or
  hook) straight from the packet-cache entry. It hands anything else to the
  general loop.
- Delay-0 writes go to a side buffer that is applied after the ring slot, so
  order is kept. The ring holds only delayed writes, and `wmask` replaces the
  per-register stall scan.
- NOP counts, branch flags and constant operands are precomputed per
  instruction. Runs of multi-cycle NOPs are batched up to the next queued write
  or branch landing.
- Hot fields come first in the core struct.

Quantum size costs nothing measurable from 1M down to 300 cycles. QEMU's
hardening flags cost ≤2%. An in-machine rate far below these numbers is host
contention, not the core.

## Known approximations
- RCPSP/RSQRSP return exact values, not the 8-bit seed.
- FAUCR sticky flags are not maintained.
- AMR circular addressing is not implemented.
- Exceptions (EFR/IERR) stop the step loop instead of vectoring.
