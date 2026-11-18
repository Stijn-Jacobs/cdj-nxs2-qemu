# CDJ-850 display integration plan

Status: renderer ready for isolated integration; live field decoding incomplete.
Prepared 2026-11-18. Companion: [handoff](cdj850-display-handoff.md).

## Isolation contract

This branch starts at `c5e4b47a222e9389f548a26d4544b20895a4a334` on
`cdj850-bringup`. Its existing renderer/adapter baseline was committed in
`d3ee8fc`. The outstanding display refinements are copied into this worktree;
the original checkout's uncommitted `dsp56300.c` change is excluded.

Local checkout: `/Users/julius/Documents/cdj850-emu/worktrees/display-integration`.
Original DSP checkout: `/Users/julius/Documents/cdj-nxs2`.

A different `QEMU_EB_BUILD` alone is insufficient. `build_display.sh` calls
`install_machine.sh "$CDJ_ROOT/qemu-src"` before compiling. That installer
copies the entire `hw/cdj` board tree, removes stale sources and edits QEMU
build configuration. Keep **both source and build trees private**:

```sh
cd /Users/julius/Documents/cdj850-emu/worktrees/display-integration
export CDJ_ROOT="$(pwd -P)"
export QEMU_EB_BUILD="$CDJ_ROOT/qemu-build-eb"
export QEMU_BUILD="$CDJ_ROOT/qemu-build"
export C66X_JIT_LIBDIR="$CDJ_ROOT/build/c6x"
export GUI_QEMU="$QEMU_EB_BUILD/qemu-system-sh4eb"
```

An ignored `env.sh` with these settings is present locally. The commands above
are the portable source of truth; that local file is not part of the commit.

Do not symlink or hard-link mutable QEMU sources, build output, firmware/media
working copies or run directories to the original checkout. Do not use the
original binary through an inherited `GUI_QEMU`. Use unique monitor sockets,
logs, VNC ports and run tags. Do not kill or attach to the DSP session's process.

No QEMU source installation, emulator compilation or live boot was performed
for this handoff. `qemu-src` and the build directories are absent in the new
worktree. A later build is authorized only within these isolated paths; the
original source/build restrictions remain in force.

## Current behavior and boundaries

| Area | Implemented | Integration gap |
|---|---|---|
| Console | Adapter creates a 1280x480 QEMU graphic console | No isolated emulator boot/capture yet |
| Text | SCIF1 text records feed all three rows; tag bit 5 highlights selection | Most text attributes are retained by panel peer but not forwarded to renderer |
| Browser | 100x27 matrix, proportional glyphs, nine-dot bands, five-dot rail | Font evidence covers photographed characters, not the entire charset |
| Fixed legends | Photo-traced REMAIN, INFO, BROWSE, TAG/LIST, A.CUE, MT, WIDE, MEMORY, CUE, BPM, M/S/F | Shapes available; most active states still unmapped |
| Playing | Title band/note, explicit artist/overview/strip fields | Only title/mode currently reach adapter; artist, overview and strip masks are fixture-only |
| Numeric/pitch | Rendering primitives, split leading 1, decimal apertures, range badges | No live time/BPM/pitch/lamp decoder/state API yet |
| Reference fixtures | Loaded photo values plus approximate sampled waveform | Never treat fixture data as decoded firmware traffic |
| Persistence | Render state is currently static host state | Reset and migration behavior must be defined; VM save/restore is not established |

`vfd_note_tx()` is legacy **SSU0 audio-DSP diagnostics**, not a display data
source. The real panel is **SCIF1**. Older bring-up notes claiming otherwise
are superseded. Do not modify DSP/SSI/interrupt timing to make a display fixture
appear live. INFO activation from mode 0x0a is presently a mode-based assumption,
not a decoded lamp bit.

## Delivery sequence

### 1. Establish the offline baseline (completed for this handoff)

Run the focused tests and generate standalone browser, loaded and all-on PNGs.
They use the production drawing header and no emulator or firmware:

```sh
/usr/local/bin/python3.11 -m pytest -q \
  tests/test_cdj850_vfd_render.py tests/test_cdj850_vfd.py \
  tests/test_cdj850_panel_peer.py tests/test_cdj850_panel_seq.py
python3 scripts/debug/cdj850_display_preview.py all-on /tmp/display-all-on.png
python3 scripts/debug/cdj850_display_preview.py browser /tmp/display-browser.png --photo-readouts
python3 scripts/debug/cdj850_display_preview.py loaded /tmp/display-playing.png --photo-readouts
python3 scripts/debug/cdj850_display_preview.py loaded /tmp/display-unmapped.png
```

Acceptance: 24 tests pass; unknown live fields stay absent; no clipped glyphs
or writes beyond clipped/padded surfaces. Preview compilations use
`-Wall -Wextra -Werror`. Python 3.11 is the currently available pytest runtime;
use another Python with pytest installed on other machines.

### 2. Build and boot an isolated baseline (pending)

First verify all resolved paths are inside this worktree. With the environment
above, initialize a fresh private QEMU v11.1.2 tree using `./build.sh source`,
then run `./build.sh display`. These commands may fetch sources and build; they
have **not** been executed for this handoff. Do not reuse the original
`qemu-src` to save time. A genuinely independent, immutable-source copy can be
used instead of fetching, but must have no shared writable files or worktree
build metadata.

Put an owned copy of prepared firmware in this worktree's ignored
`extract/cdj850/flash_cdj850.bin`. Keep USB data disposable; use `snapshot=on`
for both block devices. No firmware or photos belong in the commit. Do not
assume a VM snapshot is safe merely because a block drive uses snapshot mode.

Run `GUI_QEMU="$QEMU_EB_BUILD/qemu-system-sh4eb"` explicitly for emulator tests.
The existing live scenario is:

```sh
/usr/local/bin/python3.11 -m pytest -q \
  tests/test_cdj850_usb.py::test_live_usb_source_select_browse_load_and_play
```

That test finds flash relative to its own worktree, creates a disposable USB
image, and uses `-icount shift=2,sleep=off` for deterministic panel input. It
checks firmware logs/state, **not visual rendering**. Add a separate visual
capture smoke test through that private process's QMP `screendump`:
boot -> NO DISC -> USB browser -> cursor move -> loaded title. Assert 1280x480,
three visible rows, changed selection and title changes. Review captured pixels
against offline fixtures; don't expect the photographed song's name or waveform
from a different USB test track.

Acceptance: all existing milestones pass and the real console renders decoded
text. A failing DSP milestone is a baseline issue to report or resolve by taking
the user's completed DSP commit, not by editing their live checkout.

### 3. Capture and map real display traffic (pending)

Add optional, lossless SCIF1 TX transaction capture and an offline replay tool.
Preserve framing, checksums, sequence, virtual timestamps, all header bytes,
three text attribute bytes and unknown 0x60/0x70-family messages. Bound recorder
output and leave it disabled by default. Derive fields from MAIN's message
builders and controlled state transitions, not plausible values from screenshots.

Useful transitions: idle/source select, browse selection, load, play/pause/cue,
TIME/REMAIN, tempo/range/MT, and metadata page changes when available. Record
which firmware version and baseline binary produced each capture. Commit only
hand-authored/sanitized test stimuli; retain raw firmware-derived captures locally.

Acceptance: replayed validated text produces the same state as the live panel
peer; bad/truncated messages do not corrupt prior complete state. Document each
field's byte location, meaning, units, evidence and unresolved alternatives.

### 4. Integrate one verified field at a time (pending)

Add a typed adapter API for numeric fields, pitch, indicators and `FlPlaying`.
Use explicit known/valid flags and atomic complete-message updates. Clear stale
metadata/waveform/strip/lamp state on source/track transitions and reset. Keep
photo fixture initialization confined to the preview tool.

Suggested order: time/track number -> BPM/pitch -> fixed indicator states ->
artist/title attributes -> waveform and lower strip messages. Apply actual text
attributes once mapped; do not equate a renderer row with a semantic field
without protocol evidence. Lower strip names currently describe appearance,
not proven hardware semantics. Preserve the two-line diagnostic accessor while
existing tests/scripts depend on it.

Acceptance per field: known-good and malformed-message tests, replay fixture,
live capture comparison, and an explicit test that absent data remains absent.
Do not report full now-playing support while its main data is fixture-only.

### 5. Verify frontend, reset and timing (pending)

Check direct console and VNC/virtual-deck paths for dynamic framebuffer size,
aspect ratio, inactive segments and label tint. The app can use VNC; do not
assume this board writes the other models' RGB565 frame-file format. Check
ordinary text and inverse text with long titles, punctuation and unknown glyphs.

Define reset invalidation and state reconstruction. Either add complete
migration state for the display and its producer, or explicitly keep saved-deck
support unsupported for this board. Optimize redraws on actual state changes
only after correctness is established.

The user will provide a physical-player video later. Until then, scrolling,
blink cadence and waveform animation remain unverified. Do not invent timings
and call them 1:1 behavior.

Acceptance: documented static visual coverage, replay/live evidence, reset
checks and frontend smoke tests. Keep visual tolerances for geometry distinct
from photo bloom, glare and exposure. Broader DSP/audio validation is required
before merging an updated DSP baseline; this display work does not establish it.

## Return path

Keep display changes on `codex/display-integration` until reviewed. Cherry-pick
only the display commit into the user's chosen integration branch after their
DSP work settles; don't merge or reset their dirty checkout automatically.
Use `git show --stat` before transfer to verify no DSP, QEMU-generated tree,
firmware, personal configuration or image artifacts entered this commit.
