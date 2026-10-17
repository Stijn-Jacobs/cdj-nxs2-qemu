# SH7764 TMU underflow accounting and input clock

The fix is in the generated `patches/hw_timer_sh_timer.c.patch`; the live
implementation is `qemu-src/hw/timer/sh_timer.c`.

Two independent problems were reproduced without running QEMU or firmware:

- `sh_timer_tick()` used only the UNF latch. Several expiries before a clear
  became one pending interrupt. Ten callbacks per late acknowledgement gave
  100 simulated handler entries for 1000 actual underflows.
- `sh_timer_write()` stopped, set the frequency and restarted the ptimer on
  every TCR write, including an unchanged-clock UNF acknowledgement.
  `ptimer_get_count()` rounds down, and returns zero when expiry is due.
  `ptimer_stop()` saves that zero; `ptimer_transaction_commit()` then calls
  `ptimer_reload()`, whose legacy zero-count rule invokes the callback again.
  Clearing near expiry reproduced an extra callback and moved the deadline
  by 999850 ns. The experimental payback patch retains this defect.

In the fixed source, the acknowledgement/clock decision is at
`sh_timer.c:111`, retirement at `sh_timer.c:180`, and retention of additional
interrupt-enabled expiries at `sh_timer.c:273`. The underlying immediate
callback rule is `hw/core/ptimer.c:68`. Acknowledgement and UNIE-only writes
now leave the countdown and scheduled deadline alone; genuine prescaler
changes still reconfigure it.

For continuously interrupt-enabled operation, pending expiries equal
`!!int_level + missed`. A callback adds exactly one; an UNF-clear write with
an outstanding expiry removes exactly one. UNF=1 writes preserve it. The
IRQ remains asserted until the final expiry is acknowledged, so a masked
handler can return and accept the next interrupt. The backlog is a migrated
64-bit count, with no one-second cap, and is discarded on stop/reset.
UNIE-clear polling channels retain the existing single-bit UNF latch. There
is no environment switch for the fix; the existing UNIE/UNF separation is
preserved.

Run the host-only test:

```sh
python3 scripts/test/test_sh_tmu.py
```

It compiles the production TMU channel functions, reset function and real
ptimer implementation. Only clock scheduling, IRQ plumbing and diagnostics
are stubbed. Simulated guest handler entries are counted while the IRQ is
asserted, using a TCR read-modify-write acknowledgement. Tests cover:

- 1000 underflows in one guest second with late, bursty callbacks: exactly
  1000 simulated interrupt entries.
- Clears one nanosecond before expiry: exactly 1000 entries, no synthetic
  callbacks and no deadline movement, including repeated idle clears.
- Both orders of callback and acknowledgement, including a callback between
  the guest's read and write, UNF=1 writes, and UNIE masking.
- A 5000-event burst, stop/reset with pending backlog, polling UNF, and starting TSTR before the first TCR write. The reset /4
  frequency is initialized so that the latter ordering still starts counting.

All four scenarios pass. With the pre-fix source, all four fail (the burst
case delivers 100 entries). With `tmu-payback.diff` applied to QEMU 9.1.0 and
`CDJ_TMU_PAYBACK=1`, the ordinary burst passes, but near-expiry,
callback/clear ordering and the 5000-event burst fail. The near-expiry case
delivers two entries for one scheduled expiry. An optional positional
source filename runs the same tests against an alternative `sh_timer.c`.

A bounded static check of the stock CDJ-2000NXS MAIN image (SHA-256
`7b21d1d32ff36fb349c8556c46475cd349c982bdf76bacf339fb309af532677c`, matching
`models/cdj2000nxs.conf`) found one RMW clear in the TMU4 wrapper at
`0x043080EE`: read `TCR4` at `0x043080F6`, AND with `0xFEFF`, write at
`0x043080FA`, then tail-call `0x042F1534`. This is local SH objdump evidence;
REA's firmware scanner was unavailable on macOS. It is not a runtime trace
or an exhaustive proof of every indirect callback's behaviour. The host
proof assumes one acknowledgement per simulated interrupt entry; arbitrary
extra clears while backlog remains would also retire events.

`patches/verify.sh` reports tree/patch agreement. A fresh `qemu-build/`
completed all 1499 build steps using the normal `build_main.sh`
configure/build path with `QEMU_BUILD=$PWD/qemu-build SKIP_PATCH_REGEN=1`.
Its final `-M help` and `-display help` invocations were omitted in a temporary
copy of the script to avoid executing the emulator. No emulator or firmware
ran. Measuring stock NORTi time at `0x04d13690` remains the operator's runtime
validation; host results prove event accounting under the stated conditions.

The reproduced defects explain loss and a mechanism for overdelivery; the
reported 1188/s sample itself was not reproduced under the no-execution rule.
A finite sampling window can also exceed 1000/s while an existing backlog is
being drained. The exactness assertion is for N scheduled expiries through
complete backlog drain, rather than for a transient wall-clock sample.

## Stock NXS clock-mode mismatch (2026-10-13)

The steady 1200.2/s NORTi observation is explained by the board's wrong
FRQCR reset value, independently of underflow accounting. The generic
`qemu-src/hw/sh4/sh7750.c` 60 MHz default is **not used** by this machine.
`hw/cdj/common/sh4_board.c` builds the custom SoC; `cdj_tmu_init()` in
`hw/cdj/common/sh4_periph.c` passes the board descriptor's `periph_hz` to
both TMU blocks (channels 0–2 and 3–5). `CDJ_PERIPH_HZ`, if set, overrides
both. Previously that descriptor supplied 54,000,000 Hz while the CPG
register file at `0xffc80000` returned `0x30000000`.

Static evidence was read through Ghidra MCP with explicit program
`main-normal.bin`. Its input and the user-specified stock `main.bin` both
hash to SHA-256
`f73da28a7886955c879748c63052fb199d966aaa133f4fd5c9d27065b6301d4f`.
This is the decompressed MAIN image, rather than the NOR/container digest
used in the earlier underflow investigation. No emulator or firmware ran.

The stock writes and their effective values are:

| Instruction address | Register / operation | Value |
| --- | --- | --- |
| `0x042c5b7e` (also `0x042c5cde`) | Read FRQCR `0xffc80000` | Mask `0xf0000000`; compare with `0x30000000` |
| `0x042c5b8e–96` (also `0x042c5cee–f6`) | Select period N | Equal: `0x2bf2` = 11250; otherwise `0x34bc` = 13500 |
| `0x042c5ba0` (also `0x042c5d00`) | Save N at `0x04e20bf8` | Selected period |
| `0x042c5ba6/a8/ac/b2` | Free-run TCOR0 / TCNT0 / TCR0 / TSTR0 | `0xffffffff` / `0xffffffff` / 0 / old value OR 1 |
| `0x042c5d52` | Stop TMU0, TSTR0 `0xffd80004` | Old value AND `0xfe` |
| `0x042c5d58/60` | TCOR0 `0xffd80008`, TCNT0 `0xffd8000c` | N−1 (`0x2bf1` before fix, `0x34bb` after) |
| `0x042c5d6a/70` | TCR0 `0xffd80010`, TSTR0 | `0x20` (UNIE, TPSC=0), then OR 1 |
| `0x04308058` | TSTR1 `0xffdc0004` | 0 (stop channels 3–5) |
| `0x04308060/66` | TCOR4 `0xffdc0014`, TCNT4 `0xffdc0018` | `0x34bc` = 13500 |
| `0x04308068/74` | TCOR5 `0xffdc0020`, TCNT5 `0xffdc0024` | `0x20f58` = 135000 |
| `0x04308076/78` | TCR4 `0xffdc001c`, TCR5 `0xffdc0028` | `0x20` (UNIE, TPSC=0) |
| `0x043080a0/a6` | TSTR1 | OR 2, then OR 4 (start TMU4/5) |

The literal pools at `0x042c5db4` and `0x04308120/24/28` confirm the
register bases and TMU5 constant. TMU0 ISR `0x042c5d84` acknowledges TCR0,
calls `periodic_task_dispatch_tick`, then `norti_tick_isr_handler` at
`0x0436996a`. TMU4 ISR `0x043080ee` acknowledges TCR4 and calls Pioneer's
player timer service. TMU4 is not the NORTi scheduler source.

There is no software FRQCR programming in these setup paths: the stock
reads the strapped clock mode. The
[SH7764 hardware manual](https://www.renesas.com/en/document/mah/sh7764-group-users-manual-hardware?r=1055191),
Table 10.2 and §10.4.1 (printed pp. 249–251), specifies mode 2 reset
`0x30320044`, mode 3 reset `0x40320044`, and read-only FRQCR. Pioneer's
RRV4356 service manual, parts list p.154, gives X2 DSS1185 = 26.965 MHz;
p.91 straps MODE2:1:0 = 011 (PLL ×12), and p.12 prints 323.58 MHz core.
The local reference `cdj-2000nxs-re-docs/docs/hardware/clocks.md` records
these pages, including the p.91 oscillator annotation erratum. Thus:

```
Pφ = 26,965,000 × 12 / 6 = 53,930,000 Hz
TPSC = 0 => divider = 4
```

The hardware counts through zero, so the exact hardware period uses
TCOR+1. QEMU's current legacy ptimer uses TCOR directly. Both conventions
are shown to distinguish the large clock-mode defect from that remaining
one-count difference:

| Configuration | NORTi TMU0 rate (/s) | Pioneer TMU4 rate (/s) |
| --- | ---: | ---: |
| Old model: 54 MHz, FRQCR nibble 3 | `54e6/4/11249 = 1200.106676` | `54e6/4/13500 = 1000` |
| Real NXS: 53.930 MHz, mode 3 | `53.93e6/4/13500 = 998.703704` | `53.93e6/4/13501 = 998.629731` |
| Corrected model: 53.930 MHz, FRQCR nibble 4 | `53.93e6/4/13499 = 998.777687` | `53.93e6/4/13500 = 998.703704` |

Nominally, using 54 MHz and hardware periods, mode 3 yields 1000 NORTi
ticks/s. Real crystal frequency makes the tick 1.001298 ms. The reported
1200.2/s is consistent with the old model's 1200.106676/s over a finite
sample; its 20% excess is not the real deck's NORTi clock. The position
observation alone does not establish its clock source.

The fix is owned by `hw/cdj/boards/cdj2000/board.c`: seed the full mode-3
FRQCR reset and give the NXS descriptor its physical 53.930 MHz Pφ.
Board sources are tracked whole files in this repository's patches
workflow, installed into `qemu-src/hw/sh4/cdj/`; no SH7750 patch is needed.
The shared CDJ-2000/CDJ-900 CPG also receives the mode-3 reset (consistent
with its existing nominal 54 MHz); their descriptor frequencies remain
54 MHz because this investigation established the NXS crystal only.

All six NXS TMUs receive the 0.12963% input-frequency reduction. Besides
TMU0's corrected reload selection, this changes TMU3's network free counter,
TMU4/5's Pioneer periodics, TMU5's busy delays, and the owned runtime's
TMU2 free-running clock / TMU1 alarms. The runtime's `time.rs` converts
counts ×2/27 assuming nominal 13.5 MHz, so its µs clock now returns about
998704 µs per virtual second and alarms take 0.1298% longer, matching
the real deck. SCIF, DSP/audio and virtual-clock scheduling do not consume
this descriptor frequency. An explicit `CDJ_PERIPH_HZ=54000000` retains
nominal clock speed with the corrected mode selection.

The host regression now includes a fifth scenario: stock TMU0 and TMU4
setup reproduces 1200 versus 1000 events in one second under the old
configuration, then 998 each with the corrected board constants. It also
checks TMU2 count-to-µs conversion and TMU1 alarm timing using production
TMU and ptimer functions. It does not execute stock instructions. The
hardware TCOR+1 discrepancy is documented above and remains outside this
clock-mode fix.

Validation: `python3 scripts/test/test_sh_tmu.py` passes all five scenarios;
`patches/verify.sh` reports tree/patch/whole-file agreement after
`scripts/build/install_machine.sh`. The changed CDJ-2000 board object also
compiles with `ninja -C qemu-build
libqemu-sh4-softmmu.a.p/hw_sh4_cdj_boards_cdj2000_board.c.o`.
