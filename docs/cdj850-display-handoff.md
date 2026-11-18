# CDJ-850 display handoff

Prepared 2026-11-18. Worktree branch: `codex/display-integration`.
Plan: [display integration plan](cdj850-display-integration-plan.md).

## Where to resume

```sh
cd /Users/julius/Documents/cdj850-emu/worktrees/display-integration
source ./env.sh
```

`env.sh` is a local ignored convenience file. The tracked integration plan
contains the complete environment commands if it is missing. Check `CDJ_ROOT`,
`QEMU_EB_BUILD`, `QEMU_BUILD`, `C66X_JIT_LIBDIR` and `GUI_QEMU` before any build
or emulator test. They must resolve to this worktree, not the active DSP checkout.
The private QEMU v11.1.2 build, prepared firmware, and verified SCIF1
captures are ready in this worktree. Step 4 has integrated the track
number, time readout, REMAIN indication, BPM, pitch and range, and MASTER TEMPO.

## What is complete

- Existing adapter/renderer foundation is already in parent commit `d3ee8fc`;
  this branch was created from `c5e4b47` after the README update.
- Latest display changes copied here: `vfd_render.h`, generated
  `vfd_legends.h`, `cdj850_trace_legends.py`, browser/playing font fixtures,
  and renderer regression tests. The uncommitted DSP change was excluded.
- Pure 1280x480 rasterizer; 100x27 matrix; proportional browser text; pitch
  geometry; now-playing layout; explicit unknown data stays absent.
- Fixed legends now use photo-derived coverage rather than the incorrect
  generic stick alphabet. The user specifically identified REMAIN and INFO as
  the remaining font problem. Letter widths alone had not solved it.
- 24 focused display/panel tests pass. Standalone previews compile with strict
  warnings, and generated legend spans have resource-bounds checks.
- The isolated build passed all 35 CDJ-850 tests, including both live USB runs.
  QMP captures show NO DISC, USB root, three browser rows, selection movement
  and the loaded `BETA.wav` title at 1280x480. Build logs, test results and
  PNG captures are under `runs/display-baseline/`.
- Set `CDJ850_SCIF1_CAPTURE` to a new file to record the private QEMU's MAIN TX
  windows. The standalone replay tool shares the live decoder. The 41-test
  suite passed, and replay matched all 18 playback and all 22 control-run
  display entries. Usage and field evidence are in plan step 3. Captures and
  provenance are under `runs/scif1-replay/`.
- The firmware's binary track number now drives the two-digit readout. The
  43-test suite passed. QMP and replay produced identical frames at idle,
  tracks 10 and 11, USB stop, and reset.
- The six M/S/F digits and REMAIN follow the firmware's SCIF1 status pages.
  Live QMP and replay matched at all six time-test milestones, including both
  TIME toggles, USB stop, and warm reset. Evidence is under `runs/scif1-time/`.
- MAIN's whole BPM word drives the three-digit readout. Live QMP and replay
  matched at idle, 128 BPM, 96 BPM, and USB stop. All 47 CDJ-850 tests passed.
  The synthetic export and captures are under `runs/scif1-bpm/`.
- MAIN's paired pitch status pages drive the signed percentage, decimal point,
  and ±6, ±10, ±16, or WIDE lamp. The panel script supplies the fader position
  and center over SCIF1. The tests and captures are under `runs/scif1-pitch/`.
- The red MASTER TEMPO legend follows bit 7 of MAIN status page `0x02`
  byte 11. The tests and captures are under `runs/scif1-master-tempo/`.

## What is still incomplete

Live adapter input reaches track number, time, REMAIN, BPM, pitch percentage
and range, MASTER TEMPO, mode, text rows, and selection tags. Most other lamps, artist
metadata, waveform, and strip masks remain unmapped. The loaded preview's waveform is an offline fixture. INFO is currently inferred from loaded
mode. See the plan for explicit known-state APIs and reset work.
The photographed font is not a complete arbitrary charset. Some glyphs and the
final p stem are approximate because of glare/obscured dots. Optics remain
approximate. The user has no deck at hand and will supply video later.

## Important files

- `hw/cdj/boards/cdj850/xc/vfd_render.h`: pure drawing, state and field primitives.
- `hw/cdj/boards/cdj850/xc/vfd_legends.h`: generated monochrome fixed-word masks.
- `scripts/debug/cdj850_trace_legends.py`: deterministic authoring from source
  photo PNG; requires Pillow only for regeneration.
- `scripts/debug/cdj850_display_preview.py`: standalone C compilation/PNG export;
  no QEMU, firmware or extra Python packages required for normal preview use.
- `tests/fixtures/cdj850-browser-font.json` and `cdj850-playing-font.json`:
  reviewed masks/positions, including uncertainty notes.
- `tests/fixtures/cdj850-now-playing.json`: approximate photo study for the
  `loaded --photo-readouts` fixture; never wire its values into live state.
- `vfd.c`/`vfd.h`: graphic-console adapter and legacy diagnostic accessor.
- `panel_peer.c`: SCIF1 transaction handling and opt-in MAIN TX capture.
- `panel_display.c` and `panel_display.h`: shared text/header decoder.
- `scripts/debug/cdj850_scif1_replay.c`: offline reader for captured transactions.

SSU0 is the audio DSP link. `vfd_note_tx()` is retained diagnostic compatibility,
not a source from which to decode display pixels. Do not revisit superseded
bring-up assumptions that called SSU0 the panel link.

## Reference data and recent artifacts

Original user photos remain outside Git under `/Users/julius/Downloads/`:
2295 = all-on service mode, 2292 = browser, 2293 = loaded, 2291 = NO USB,
2290 = NO DISC. Locally converted PNGs and scratch comparisons are under
`/tmp/cdj850-display-reference/`; they can disappear across reboots.
The legend tracer expects the original 4032x1610 all-on PNG; obtain it again
from IMG_2295 if the temporary copy is gone. No photo is committed.

Latest INFO preview and label comparison (local artifacts, not required to run):

- `/Users/julius/.codex/visualizations/2026/11/17/01a1eb2a-22a2-71a1-a31f-8e42186f435f/cdj850-info-preview.png`
- `/Users/julius/.codex/visualizations/2026/11/17/01a1eb2a-22a2-71a1-a31f-8e42186f435f/cdj850-fixed-legends-comparison.png`

The ignored `notes/cdj850-display-roadmap.md` in the original checkout contains
historical discussion. This tracked handoff and plan summarize the current
state so resuming does not depend on that ignored file.

## Resume checklist

1. Verify that `CDJ_ROOT`, the QEMU trees and `GUI_QEMU` still point to this
   worktree. Keep the firmware and USB working copies private.
2. Use the control capture and MAIN's builders to select a candidate field
   for step 4. Keep raw captures in the ignored run directory with firmware
   and binary provenance. Track and time captures and framebuffer checks
   are under `runs/scif1-track/` and `runs/scif1-time/`.
3. Reproduce each candidate with isolated input transitions before mapping it
   to the adapter.
4. Before transferring display work, reconcile with the user's finished DSP
   commit. Leave their original working changes in place.
