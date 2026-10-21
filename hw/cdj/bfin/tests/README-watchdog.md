# BF531 sleep model qualification

`clock_test.c` and `watchdog_test.c` exercise the real standalone core and SoC
model using a tiny IDLE/NOP/branch program, register accesses and snapshots.
They do not qualify integrated GUI workflows or physical hardware timing.

Run from the emulator checkout:

```sh
make -C hw/cdj/bfin O=/tmp/bf531-build check
```

The model preserves `bfin_cycles()` as elapsed simulation time for runner
budgets and peripheral events. Architectural CYCLES follows SYSCFG.CCEN,
low-word reads latch the upper word, and CYCLES/core timer stop during IDLE.
Watchdog ticks use elapsed time and the current PLL_DIV CSEL/SSEL ratio;
SIC_IWR drives a level wake signal independently of SIC_IMASK. No extra
interrupt is raised to wake foreground execution.

Watchdog semantics follow the ADSP-BF533 Hardware Reference Rev. 3.6,
chapter 15 (the shared BF531/BF532/BF533 peripheral): exact register widths,
WDEN disable key, disabled CNT preload, enabled STAT reload, stopped countdown
at zero and sticky WDRO clearing after disable. Countdown/fractional phase,
wake state and architectural clocks participate in snapshots.

Supported expiry modes are general-purpose SIC interrupt and disabled event.
Reset and NMI expiry deliberately terminate the model with BFIN_STOP_UNDEF
and a watchdog-specific log diagnostic. They are not simulated resets/NMIs
and cannot qualify workflows requiring those modes. Hardware settling and
expiry-to-resume latency are not modeled; physical timing acceptance remains
separate. Changing PLL_DIV synchronizes the old ratio and starts a new
fractional phase; sub-SCLK behavior during PLL reconfiguration is not qualified.

Rebuild the normal QEMU executable from this checkout before updating any
pinned debug-hooks executable. Record the emulator source commit and executable
SHA256 in endpoint evidence; a previously pinned executable does not gain this
model by rebuilding firmware. Snapshot formats are build-specific, as before;
regenerate snapshots after changing the model. Pinning and integrated emulator
qualification are separate integration steps.

## Ownership timing observations (schema 1)

With `CDJ_BF531_DUMP=<directory>`, the board writes `ownership-timing.json`
beside the memory dumps. Optional `CDJ_BF531_TIMING_START_PC=0x...` is the
runtime entry resolved from the exact firmware ELF; malformed/odd addresses
are refused. A missing or unexecuted marker leaves runtime unavailable.
The marker splits cached blocks, so normal execution and single stepping use
one phase boundary, without a firmware instruction or first-unmask heuristic.
Bind the JSON's hash, emulator executable/source identity, firmware bundle and
resolved marker in the run receipt. The JSON itself cannot establish those
external identities. No saved-state run qualifies: `snapshot_loaded` is sticky
on model restoration/reset after observation begins, and runtime is unavailable.

The unit is `elapsed_model_cycles`, scaled by the model's fixed 400000000 Hz
(2.5 ns per cycle), not host wall time or architectural CYCLES. IDLE elapsed
time is included. These observations do not establish physical latency bounds.
The telemetry is host-only and excluded from snapshots. It changes no firmware
RAM, interrupt masks, event delivery, watchdog countdown or instruction costs.
Counter exhaustion saturates only with explicit top-level `overflow`; such a
report cannot qualify timing. No per-instruction resume bound is claimed.

`boot` precedes the marker and `runtime` follows it. Each has independent
`imask` and `ipend_global` records. IMASK means *all* maskable CEC bits5–15
are clear (CLI-equivalent), not every partial per-level mask or ISR priority
blocking. IPENDglobal means bit4 is set, reported separately rather than
silently counted as CLI. Nested CLI/restoring an already masked value does
not restart an interval. Direct IMASK stores use the same STI hook.

Mask `intervals`, `total_cycles`, `total_awake_cycles`, `total_idle_cycles`
and `max_cycles`, `max_awake_cycles`, `max_idle_cycles` cover completed spans
only. The three maxima are independent observations. A phase cut preserves
its prefix in `censored_intervals/censored_cycles/censored_idle_cycles`,
without inventing a completed interval; a still-blocked runtime continuation
begins at the marker. At dump, an `open` span is included in those censored
fields in the copied report only. Repeated reads do not consume it. Censored
values are observed lower bounds; an empty completed sample set is not zero
latency acceptance. Foreground CLI intentionally spans wake-only IDLE, so
retain full elapsed duration and its idle/awake components.

Watchdog `arms` counts disabled→enabled and enabled STAT reloads. Phase attribution follows the
exact scheduled deadline: a late-observed deadline before the runtime marker
remains boot-owned; a deadline at the marker belongs to runtime. An unexpired
boot arm entering runtime increments runtime `carried_arms`; later expiry or
cancellation belongs to runtime. An arm already expired in boot stays assigned
to boot even if its first STAT read follows the marker. Thus boot may have
more arms than local outcomes, and runtime's outcome capacity is
`arms + carried_arms`. `cancelled_before_expiry` records an arm closed by
disable/reload before expiry. No active arm is manufactured by dumping.

`expiries` counts the first countdown expiry per arm, including an enabled
STAT reload that retains the hardware WDRO sticky flag. Repeated
synchronization at a zero count does not count another expiry. The scheduled deadline is
computed from the previous elapsed stamp with
`ceil((remaining*SSEL - fraction)/2^CSEL)`; it stays distinct from the later
observation time. The first actual WDOG_STAT read after expiry produces one
`stat_samples` observation. Debugger peeks and dump extraction are not samples.
`max_observation_delay_cycles` bounds scheduled→observed synchronization;
`max_expiry_to_stat_cycles` and `total_expiry_to_stat_cycles` describe
scheduled→first-STAT-read. `last_sample` and `worst_sample` retain exact
`arm/scheduled_cycle/observed_cycle/stat_cycle` witnesses (null with no samples;
worst preserves the first equal maximum). STAT is the endpoint relevant to
stopped-clock-correction undercount; this is not a measured first-instruction
resume latency. Arms not using the watchdog yield no bound, not a pass.

`expired_without_sample` counts completed expired arms with no read plus any
currently expired unsampled arm. `pending_expiry` means exactly that current
expired, unsampled arm; an already sampled active arm is false. Run failure,
missing/unseen marker, snapshot restoration, overflow, and required missing
samples must remain explicit in acceptance. Existing `clock_test` and
`watchdog_test` exercise these production hooks, phase cuts, nested masks,
IDLE/CCEN independence, fractional deadlines, repeated expiry, cancellations,
carried arms, JSON output and unavailable/snapshot cases.

## Accumulator-copy regression

`byteops_test` executes both DSP32ALU accumulator copies (`C408 E03F`,
`A1 = A0`, encountered in JPEG presentation geometry, and `C408 C03F`,
`A0 = A1`). The entire 40-bit AX:AW value transfers without ASTAT changes;
source/data registers are retained. Tests cover extension bytes, both signed
40-bit extrema, the low-word sign bit without extension, reserved X/HL forms,
and the existing three accumulator-clear forms.

Primary semantic reference: GNU's Analog Devices Blackfin simulator,
[`decode_dsp32alu_0`, aopcde8/aop3/x0/HL0](https://gnu.googlesource.com/binutils-gdb/+/d01e823438c7dc264d6885fbbfeace4d8955dcb7/sim/bfin/bfin-sim.c).
It copies the selected AX and AW registers, with no ASTAT writes. This emulator
correction changes neither firmware code nor the GUI/MAIN protocol.


## Bounded endpoint fault injection (v3)

`CDJ_BF531_PPI_DELAY_US=0..50000` adds that many microseconds of virtual
model time to a PPI DMA0 schedule. `CDJ_BF531_PPI_DELAY_COUNT=1..1024`
(default32) limits the number of extended schedules. US defaults0 and leaves
the model unchanged. COUNT without US, malformed decimal values, out-of-range
values and a positive delay with zero count reject startup. Configuration is
one-shot before DMA0 starts. These are deliberately injected tests, excluded
from normal performance comparisons and physical timing claims.

Extension is permitted only after the exact `CDJ_BF531_TIMING_START_PC`
marker has executed in a cold boot. Thus `CDJ_BF531_DUMP` and the exact ELF
marker must be supplied to witness a runtime test. Missing/unseen markers or
snapshot loads cannot activate it. Each eligible schedule consumes one finite
count, even if firmware later cancels that DMA; further schedules use the
original model period. The added value is `US*400` elapsed model cycles. Only
the due time changes: the pixel read/frame callback still occurs at actual
DMA completion, followed by DONE/SIC publication and normal descriptor restart.
SPORT and the original PPI period calculation remain unchanged.

The separate dump `ppi-injection.json` has schema1, units
`elapsed_model_cycles` and400000000 `cycles_per_second`. It records
`delay_us`, `requested_count`, `applied_count`, `runtime_seen`,
`snapshot_loaded`, `start_cycle` (null if unseen), `end_cycle`,
`status` (`applied` or `unavailable`), and `last` (null if no extension;
otherwise `scheduled_cycle`, `original_due_cycle`, `delayed_due_cycle`).
`applied_count` counts actual due-time extensions, not completed/visible
frames. No completion or physical scanout proof may be inferred from it.
`ppi_delay_test` checks the actual production scheduler with exact pre-due,
due and cancellation boundaries, pixels-before-DONE/IRQ ordering, finite
budget exhaustion, runtime/snapshot gating, maximum/rejected configuration
and unaffected SPORT RX. An optional first argument saves an actual serializer
sample; it is a unit fixture, not an emulator qualification run.

`CDJ_BF531_LINK_LOG=1 CDJ_LINK_LOG_FULL=1` expands packet hex logs from the
existing first64 bytes to at most2048 bytes, the actual link buffer size.
It changes neither payloads nor delivery. This permits native192-byte state
CRC validation. Without the full-log flag the64-byte default is preserved.

The existing `CDJ_LINK_RX_GAP_US` minimum gap delays **GUI-to-MAIN answers**,
not MAIN-to-GUI partial DMA. With link logging and a positive configured gap,
each actual deferral decision logs `rx-gap deferred gap_us=... now_ns=...
due_ns=... applied=...`; `now_ns<due_ns` witnesses the virtual-time wait.
The monotonic count describes deferral decisions (rechecks can count again),
not distinct delivered packets. No extra logs occur with the normal automatic
wire pacing or gap0. Partial SPORT burst/cancellation coverage remains in host
model/service tests; this knob does not manufacture such endpoint evidence.
