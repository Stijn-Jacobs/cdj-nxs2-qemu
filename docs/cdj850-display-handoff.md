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
The next task is plan step 2: an isolated QEMU baseline build/boot. No build has
been attempted here, and no QEMU source tree has been copied or linked here.

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
  warnings. Generated legend spans have resource-bounds checks. There has been
  no emulator compilation, private live boot or new live visual verification.

## What is still incomplete

Live adapter input reaches mode, text rows and selection tags. Numeric values,
most lamp states, artist metadata, waveform and strip masks do not yet reach it.
The screenshot-like loaded preview is an **offline fixture**, not a demonstration
of decoded runtime waveform/time/BPM. INFO is currently inferred from loaded
mode. See the plan for explicit known-state APIs, capture/replay and reset work.
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
- `panel_peer.c`: SCIF1 framing/text producer; future capture and field mapping.

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

1. Inspect this branch's commit and clean status; rerun the four focused suites.
2. Source/recreate the isolated environment. Ensure source and build paths are
   absent or independently owned; no links into the DSP checkout.
3. Initialize/build the private QEMU baseline following plan step 2. Prepare
   private firmware/media working copies; set GUI_QEMU explicitly.
4. Capture real console output and only then add SCIF1 capture/replay and one
   verified field at a time. Keep protocol decoding separate from rendering.
5. Before eventual transfer, reconcile with the user's finished DSP commit.
   Do not overwrite or stash their original working changes automatically.

No outstanding user answer is required. Build dependencies/source fetching and
local firmware preparation may be needed for the next phase. Preserve the
original QEMU/DSP development environment throughout.
