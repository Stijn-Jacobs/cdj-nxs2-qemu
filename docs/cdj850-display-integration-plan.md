# CDJ-850 display integration plan

The isolated QEMU baseline and SCIF1 replay are verified for text, track number, time, BPM, pitch, MASTER TEMPO, A.CUE, the navigation lamps, and now-playing metadata and overview.
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

The baseline build uses this worktree's `qemu-src` and `qemu-build-eb`.
The original source and build paths remain outside the integration worktree.

## Current behavior and boundaries

| Area | Implemented | Integration gap |
|---|---|---|
| Console | Private QEMU boot, 1280x480 VNC RFB, QMP screendumps, and desktop face/jog display verified | Physical-player video checks for panel-side scroll/blink/waveform timing pending |
| Text | SCIF1 text records feed all three rows; tag bit 5 highlights selection | Most text attributes are retained by panel peer but not forwarded to renderer |
| Browser | 100x27 matrix, proportional glyphs, nine-dot bands, five-dot rail | Font evidence covers photographed characters, not the entire charset |
| Fixed legends | Photo-traced REMAIN, INFO, BROWSE, TAG/LIST, A.CUE, MT, WIDE, MEMORY, CUE, BPM, M/S/F | REMAIN, MT, A.CUE, navigation lamps, and pitch range follow live status. Other active states need mapping |
| Playing | Title band/note, explicit artist/overview/strip fields | Title, artist, overview, progress, MEMORY and CUE reach the adapter |
| Numeric/pitch | Live track number, M/S/F time, REMAIN, whole BPM, and signed pitch with range | Other lamp decoding pending |
| Reference fixtures | Loaded photo values plus approximate sampled waveform | Never treat fixture data as decoded firmware traffic |
| Persistence | Display/VFD/peripheral warm `system_reset` invalidation + clean reboot and VMState save/restore (`test_cdj850_snapshot.py`) verified | Physical-player comparison across live power/standby flows pending |

`vfd_note_tx()` is legacy **SSU0 audio-DSP diagnostics**, not a display data
source. The real panel is **SCIF1**. Older bring-up notes claiming otherwise
are superseded. Do not modify DSP/SSI/interrupt timing to make a display fixture
appear live. INFO follows the status bit described in step 4.

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

### 2. Build and boot an isolated baseline (completed)

First verify all resolved paths are inside this worktree. With the environment
above, initialize a fresh private QEMU v11.1.2 tree using `./build.sh source`,
then run `./build.sh display`. A separate clone of the committed upstream tag
can seed `qemu-src` when fetching is unavailable. Use independent Git objects,
the wrap-pinned subproject revisions, and a fresh build directory.

On 2026-11-18 GitLab DNS failed. The private baseline used a `git clone
--no-local` of the locally available upstream v11.1.2 tag (`4fc49f46dc95`).
The four Meson subprojects were cloned independently at their wrap revisions.
The display build completed on macOS with Python 3.11. The local
`runs/display-baseline/bin/python3` shim keeps Homebrew's older
`gdbus-codegen` on Python 3.11. For a repeat build on this host, prepend that
directory and `/usr/local/bin` to `PATH` and set
`QEMU_PYTHON=/usr/local/bin/python3.11`. The build log is under
`runs/display-baseline/`.

Put an owned copy of prepared firmware in this worktree's ignored
`extract/cdj850/flash_cdj850.bin`. Keep USB data disposable; use `snapshot=on`
for both block devices. No firmware or photos belong in the commit. Do not
assume a VM snapshot is safe merely because a block drive uses snapshot mode.

Run `GUI_QEMU="$QEMU_EB_BUILD/qemu-system-sh4eb"` explicitly for emulator tests.
Run the live USB scenario and its separate framebuffer smoke test:

```sh
GUI_QEMU="$QEMU_EB_BUILD/qemu-system-sh4eb" /usr/local/bin/python3.11 -m pytest -q \
  tests/test_cdj850_usb.py::test_live_usb_source_select_browse_load_and_play \
  tests/test_cdj850_usb.py::test_live_usb_display_screendump
```

Both tests find flash relative to the worktree, create disposable USB images,
and use `-icount shift=2,sleep=off` for deterministic panel input. The USB scenario
checks firmware logs and player state. The framebuffer test captures NO DISC,
the USB root, the three-row file list, the moved selection, and the loaded
title using QMP `screendump`. It checks 1280x480 output, the three visible rows,
selection movement, and the correspondence between the chosen row and the
loaded title. The test's temporary directory contains the PPM captures.

On 2026-11-18, 33 non-live tests and both firmware boots passed. The firmware
auto-played `BETA.wav`, braked to a stop, and restarted. The NO DISC, file
list, moved selection, and loaded-title captures matched the standalone
renderer pixel for pixel with the same input text and selection. The USB root
was captured and checked as a separate state. The PNGs and test log are under
`runs/display-baseline/`.

### 3. Capture and replay real display traffic (completed)

Set `CDJ850_SCIF1_CAPTURE` to the path of a new file in an ignored run
directory when starting the private CDJ-850 QEMU. Recording is off by
default. The recorder refuses to overwrite an existing file and caps output
at 8 MiB. Use a fresh firmware boot and keep the binary version and SHA-256
of the binary and prepared flash alongside each recording.

The ASCII format starts with `CDJ850-SCIF1 1`. Each `T` record holds the
transaction sequence, start and end virtual nanoseconds, frame count, and a
`timestamp:hex` pair for every 16-byte MAIN TX frame in wire order. The file
includes closed zero- or one-frame windows and all received bytes, including
bad checksums. An `R <virtual ns>` record invalidates the track, time, BPM,
pitch, MASTER TEMPO, A.CUE, navigation lamps, and the now-playing overview on a warm reset. Earlier captures without
reset records can still be replayed.
A final `LIMIT` line marks a capture stopped at the size cap. An in-flight
window at exit is outside the capture.

From the integration worktree, build the reader and pass it a capture file.

```sh
xc=hw/cdj/boards/cdj850/xc
mkdir -p runs/scif1-replay
cc -std=c11 -Wall -Wextra -Werror -O2 -I "$xc" \
  scripts/debug/cdj850_scif1_replay.c "$xc/panel_display.c" \
  -o runs/scif1-replay/cdj850-scif1-replay
runs/scif1-replay/cdj850-scif1-replay path/to/capture.scif1 --ppm runs/scif1-replay/last-frame.ppm
```

The reader emits the same change-only display diagnostics as the live panel
peer, using the same decoder and the recorded transaction end time. The optional
`--ppm` argument renders the final state to a new file. For a
repeatable end-to-end comparison, run the live USB playback test from step 2.
It enables capture in its own temporary directory and compares the entire
replayed display log with the live log, including timestamps, modes, row
tags, all three attributes per row, and visible text. Synthetic tests cover
malformed capture records, bad checksums, partial and aborted window replay,
and resumption at the next valid display message. Keep raw firmware-derived
captures in the ignored run directory.

| Field | Location and units | Evidence and limits |
|---|---|---|
| Frame checksum | Byte 15, end-around carry over bytes 0–14 | PyGhidra confirmed MAIN `0x0C0F9EE8`. The shared decoder checks MAIN text and status frames |
| Header and mode | `0x80` at byte 0, sequence at byte 1, and mode at byte 2 | Firmware v1.13 capture observed `0x01` USB browse, `0x08` NO DISC, and `0x0a` loaded. The meanings of bytes 3–14 remain unresolved |
| Text chunks | `0x81` onward at byte 0, 14 bytes of payload per frame | PyGhidra confirmed the counter and chunk layout in MAIN `0x0C066046` and `0x0C0660D4` |
| Row record | Tag at relative byte 0, attributes at 1–3, byte length at 4, and text from 5 onward | MAIN `0x0C0660D4` writes all five fields then copies the counted text. The decoder stages all three rows before publishing |
| Row and selection | Tag low two bits are row IDs 1–3. Bit 5 marks the selected row in browse | The capture changed from `25/02/03` to `01/26/03` on one encoder step while text stayed identical. Other tag bits and attribute meanings remain unresolved |
| Other TX families | Full 16-byte records, including observed `0x60` and `0x70` | Both occur in the boot-to-load capture. Their display roles and units remain unresolved |

A solitary valid header can update the mode with the current rows. The
decoder uses the existing two-transaction wait to distinguish it from the
start of a text burst. A received continuation, even with a bad checksum,
cancels that header-only path. Until all three row records validate, the last
published display state stays intact.

#### Verification on CDJ-850 MAIN v1.13

- The original live decoder, with capture enabled, produced 17 display entries
  from 1,054 transactions. The standalone decoder reproduced every entry.
- The shared decoder's live USB playback run captured 1,900 transactions.
  Replay reproduced all 18 live entries byte for byte.
- A separate control run captured 2,117 transactions and 6,189 frames through
  TIME, tempo range, Master Tempo, INFO, PLAY, CUE, and DISC inputs. All 22
  live entries matched replay, including the three-row DISC confirmation.
  Frames `0x70`–`0x77` were present. Header byte 11 alternated between
  `0x20` and `0x00` around INFO. The later isolated probe also captured
  that header transition in BROWSE and identified the lamp in status page `0x02`.
- All 41 CDJ-850 tests passed, including the independent framebuffer smoke test.
  The local logs, recordings, input timeline, and firmware and binary hashes
  are under `runs/scif1-replay/`.

### 4. Integrate one verified field at a time

#### Track number

- MAIN `0x0C065DB4` reads the display index at `0x0C9D0ADB` and
  `0x0C065DB6` copies it into zero-based byte 4 of TX page `0x01`. PyGhidra
  shows the producer at `0x0C0644D4` assigning a track ordinal and applying
  a 99-track display limit. The same MAIN builder emits `0xBB` for overflow
  and `0xFF` when unavailable. Their hardware glyphs remain unverified.
- CDJ-850 MAIN v1.13 captures show `0x02` with `BETA.wav`, `0x0a` with
  `TRACK10.wav`, `0x0b` after selecting `TRACK11.wav`, and `0x00` after
  USB stop. Values are binary, so `0x0a` renders as decimal `10`.
- The first frame in a closed window must be page `0x01` with a valid
  checksum. Values 0–99 render as two decimal digits, including `00`. An
  out-of-range value blanks the readout. The alternating `0x02` page,
  corrupt checksums, and aborted windows leave the last valid state in
  place. The track display starts blank and clears on reset pending a
  valid post-reset status page.

The live test uses independent seven-segment masks for `00`, `10`, `11`,
and blank. For each sampled state it compares the live QMP track-number
pixels with the frame rendered from the same capture prefix. The test covers
idle, load, track advance, USB stop, and reset. Synthetic replay tests cover
checksum failure, unavailable values, and a status transaction that straddles
a reset. All 43 CDJ-850 tests passed. A separate byte comparison matched the
full 1280x480 framebuffer at all five sampled states. Logs, captures and build
provenance are under `runs/scif1-track/`.

#### Time readout

| Field | Location and units | Evidence |
|---|---|---|
| Minutes, seconds, frames | MAIN TX `0x01` bytes 5, 6, and 7, zero-based and binary. One second is 75 frames. | PyGhidra shows `0x0C065DB8` through `0x0C065DC2` copying the bytes at `0x0C9D0ADC` through `0x0C9D0ADE`. MAIN `0x0C0644D4` passes the remaining duration to `0x0C003E56`, which divides by 75 and 60 to obtain M/S/F. |
| REMAIN | MAIN TX `0x02` byte 13, bit 0. | MAIN `0x0C0D9194` toggles the mode at `0x0CE6D0DD`. `0x0C063D36` copies its truth value to bit 0 at `0x0C9D0AF2`, and `0x0C065E56` puts the flags byte on the wire. |

A v1.13 capture of a synthetic 66-second WAV showed `01:06:00` followed by
`01:05:74`. The first TIME press changed the digits to elapsed
(`00:00:21` in the initial sample) and the flags byte from `0x2f` to
`0x2e`. The second TIME press restored remaining time and `0x2f`.
Each page is applied when its checksum validates in a closed transaction.
The digits and lamp can therefore update one page apart during a toggle.

The renderer shows the firmware's tuple as two digits per component.
The six digits and unit markers stay dark until there is a value in range.
An unavailable or unsupported tuple, including `ff ff ff`, also blanks
the digits. REMAIN follows the most recent valid page `0x02` independently.
A warm reset invalidates both pages until fresh data arrives. QEMU and replay
use the same decoder and consume the same reset record.

Six full 1280x480 QMP frames matched replay byte for byte at idle, initial
remaining time, elapsed time, restored remaining time, USB stop, and warm
reset. The test independently checks all six digit masks against the
captured bytes and samples the REMAIN legend. The USB playback regression
also matched all 79 live display log entries. The 44 other CDJ-850 tests
passed on the same build. The time regression passed on rerun with one
wall-clock deadline per milestone so slow virtual playback could reach
USB stop. Captures, PNGs, logs, and build provenance are under
`runs/scif1-time/`.

#### BPM

PyGhidra shows MAIN instructions `0x0C065DC4` through `0x0C065DD0`
copying the halfword at `0x0C9D0AE0` to zero-based bytes 8–9 of TX page
`0x01`, high byte first. The local producer at `0x0C064118` initializes
the field to `0xffff` and selects a BPM value when available. The writer
at `0x0C0726EC` rounds the tempo it stores at `0x0C9D23F0` to whole BPM
before the page builder copies it. The renderer uses the wire value directly.

MAIN v1.13 mounted an analyzed synthetic rekordbox export and emitted
`0x0080` for `GRID128`, `0x0060` for `GRID96`, and `0xffff`
while idle, between tracks, and after USB stop. Earlier plain-WAV captures
also carried `0xffff` where the firmware had no BPM reading.

The numeric field accepts 1–999 from a complete, checksummed page `0x01`.
The renderer right-aligns shorter values and leaves unused leading digits
dark. `0xffff`, zero, and other out-of-range values clear the digits.
A warm reset invalidates BPM until a fresh status page arrives. The BPM
legend and outline use the existing glass artwork.

Run the live BPM regression with the private export image.

```sh
source ./env.sh
CDJ850_BPM_TEST_IMAGE="$CDJ_ROOT/runs/scif1-bpm/probe/export.img" \
  /usr/local/bin/python3.11 -m pytest -q \
  tests/test_cdj850_usb.py::test_live_scif1_bpm_matches_replay
```

The fixture in `runs/scif1-bpm/probe/` contains only synthetic WAVs and
their generated analysis. The live test copies the image into its run directory
and checks the BPM word, the three segment masks, and the full-frame replay.
The standalone test covers bad checksums, aborted windows, the high byte,
blank values, and reset.

All 47 CDJ-850 tests passed. The live QMP and replay framebuffers matched
byte for byte at idle, 128 BPM, 96 BPM, and USB stop. The captures, rendered
PNGs, image hash, and build provenance are under `runs/scif1-bpm/`.

#### Pitch percentage and range

PyGhidra traced firmware routine `0x0C063FD6` and the SCIF1 page builder at
`0x0C065DD4`. The display uses the absolute magnitude and the sign from
adjacent, complete status windows.

| MAIN TX page | Zero-based bytes | Field |
| --- | --- | --- |
| `0x01` | 10–11 | Big-endian unsigned magnitude in hundredths, or tenths for WIDE |
| `0x02` | 11 | Low two bits are 0 for zero, 2 for minus, and 3 for plus |
| `0x02` | 12 | Bits 4–5 are `0x10` for hundredths or `0x20` for tenths |
| `0x02` | 14 | Bit 1 enables the range indication. Bits 7, 6, 5, and 2 select ±6, ±10, ±16, and WIDE respectively |

MAIN copies the panel's fader position and center reference from incoming
page `0x01` bytes 5–8 into `0x0CE6CA48` and `0x0CE6CA4A` at
`0x0C0ECF12`–`0x0C0ECF1A`. The old peer sent zero for both words,
which kept MAIN's numeric pitch output inactive. The peer now starts both
words at `0x8000`. `<ms>:fader=<0-1023>` changes the position word
and MAIN calculates the percentage and writes both outgoing status pages.

With a known range and no supported numeric format, the range lamp stays
visible and the digits stay dark. A damaged or incomplete transaction breaks
status pairing. Reset clears both the display field and any pending flags.
WIDE values below 10.0 use the two rightmost full digits.

Run the live framebuffer and standalone replay checks from the worktree.

```sh
source ./env.sh
/usr/local/bin/python3.11 -m pytest -q \
  tests/test_cdj850_usb.py::test_live_scif1_pitch_matches_replay \
  tests/test_cdj850_usb.py::test_live_scif1_wide_pitch_matches_replay \
  tests/test_cdj850_scif1_replay.py::test_replay_pitch_status_pairing_and_reset
```

The first live test moves the fader and cycles ±10, ±16, and ±6 with a
synthetic WAV loaded. The second selects WIDE from DISC mode and checks
center and both endpoints. Each compares the raw wire fields, lit apertures,
and full QMP screendump against replay. All 50 CDJ-850 tests passed. Live
QEMU and replay framebuffers matched byte for byte at all ten pitch
checkpoints. The raw captures, framebuffers, match hashes, and binary
provenance are under `runs/scif1-pitch/verified/`.

#### MASTER TEMPO indicator

PyGhidra traced the MASTER TEMPO button handler in `0x0C0D8D78` to the
latch at `0x0CE6D0E0`. The writer at `0x0C06405A` sets bit 7 of staging
byte `0x0C9D0AF0` when the latch is on. The page builder at
`0x0C065E42`–`0x0C065E4A` copies that byte to zero-based byte 11 of
MAIN TX page `0x02`. The earlier controls capture has `0x00` in that byte
before the press at 18002 ms and `0x80` in the status frame ending at
18017.650 ms.

A complete, checksummed page `0x02` sets or clears the existing red
`MT` legend and border. The indicator starts dark and clears on warm reset.
The replay test checks that corrupt or aborted frames and unrelated page
and text traffic preserve the last validated state.

```sh
source ./env.sh
/usr/local/bin/python3.11 -m pytest -q \
  tests/test_cdj850_usb.py::test_live_scif1_master_tempo_matches_replay \
  tests/test_cdj850_scif1_replay.py::test_replay_master_tempo_status_and_reset
```

Live MAIN emitted byte values `0x04`, `0x84`, and `0x04` at the three
checkpoints. The full QEMU and replay framebuffers matched byte for byte.
The captures, framebuffer hashes, and build provenance are under
`runs/scif1-master-tempo/`.

#### A.CUE indicator

The service manual digest for RRV4128 lists AUTO CUE as a flash-backed key
status with a factory default of OFF (§8.2). The panel uses the shared
TIME MODE/AUTO CUE key (§10.8/§10.9).

PyGhidra traced the held-key path in MAIN to the AUTO CUE latch at
`0x0CE6D0DF`, written at `0x0C0D91E4`. The display writer sets bit 0
of staging byte `0x0C9D0AF1` at `0x0C0642C6`. The page builder at
`0x0C065E46`–`0x0C065E4E` copies it to zero-based byte 12 of MAIN TX
page `0x02`. The short-press path updates REMAIN in byte 13.

The raw probe for MAIN 1.13 held the existing `time` input for 1600 ms
twice on the DISC screen. Byte 12 went from `0x50` to `0x51` to `0x50`.
A 200 ms press changed the REMAIN bit while A.CUE stayed dark. The display
uses the A.CUE bit from complete, checksummed status windows. It clears on
warm reset until fresh status arrives.

```sh
source ./env.sh
/usr/local/bin/python3.11 -m pytest -q \
  tests/test_cdj850_usb.py::test_live_scif1_auto_cue_matches_replay \
  tests/test_cdj850_scif1_replay.py::test_replay_auto_cue_status_and_reset
```

Live QEMU and replay framebuffers matched byte for byte at all four
checkpoints. Both holds toggled A.CUE while REMAIN stayed off. The
captures, framebuffer hashes, and build provenance are under
`runs/scif1-auto-cue/`.

#### INFO indicator

The service manual digest for RRV4128 places INFO on the Panel CPU's KEY4
ladder (§10.8/§10.9). The service button test also lists its LED separately
from the diagnostic text (§6, printed page 37).

PyGhidra traced the navigation selection at `0x0CE6D0FD`. MAIN's writer
in `0x0C062FFC` sets staging byte `0x0C9D0AEB` bit 6 at `0x0C063450`
when the selector is 0. The writer uses bit 4 for selector 1 and bit 5
for selector 3. The copy at `0x0C065E1A`–`0x0C065E26` puts those bits
in zero-based byte 7 of MAIN TX page `0x02`.

In the USB probe, byte 7 was `0x4f` on the loaded INFO page, `0x1f` in
BROWSE, and `0x2f` in TAG LIST. INFO restored `0x4f` from both views.
The DISC key brought up a confirmation dialog in text mode `0x07`, and
the INFO bit stayed set. Text-header byte 11 also toggled `0x20` within
both BROWSE and INFO, so the renderer uses the status bit for the blue
INFO legend and border. INFO starts dark and clears on warm reset
until fresh status arrives.

```sh
source ./env.sh
/usr/local/bin/python3.11 -m pytest -q \
  tests/test_cdj850_usb.py::test_live_scif1_navigation_lamps_match_replay \
  tests/test_cdj850_scif1_replay.py::test_replay_navigation_status_and_reset
```

The full live and replay framebuffers matched byte for byte at all seven
checkpoints, including the DISC confirmation dialog. The captures,
framebuffer hashes, and build provenance are under `runs/scif1-info/`.

#### BROWSE indicator

The service manual digest for RRV4128 places the BROWSE key on the Panel
CPU's KEY4 ladder (§10.8/§10.9). The service button test lists its LED
and the BROWSE diagnostic text (§6, printed page 37).

PyGhidra traced the navigation selector at `0x0CE6D0FD`. When selector 1
is active in the supported source states, MAIN sets bit 4 of staging byte
`0x0C9D0AEB` at `0x0C063444`. The copy at `0x0C065E1A`–`0x0C065E26`
places it in zero-based byte 7 of MAIN TX page `0x02`.

SCIF1 captures show byte 7 at `0x1f` for NO DISC, the USB root, and the
BROWSE file list. It is `0x4f` in INFO and `0x2f` in the empty TAG LIST.
NO DISC and the empty TAG LIST both have text mode `0x08`. The blue
BROWSE legend and border now follow the validated status bit. Reset clears
the lamp until fresh status arrives.

The navigation tests above cover both BROWSE and INFO. The BROWSE run
includes the USB root and checks the BROWSE border and glyphs. The full
live and replay framebuffers matched byte for byte at all eight checkpoints.
Captures, hashes, and build provenance are under `runs/scif1-browse/`.

#### MENU and TAG LIST

The service manual digest lists MENU and TAG LIST in KEY4 and identifies
their LEDs in the service button test. The DFLB schematic calls the separate
MENU button lamp `UTILITY_LED` (§10.8, printed page 81). The all-on DATA FL
photo shows the TAG LIST legend. The renderer represents the MENU button
lamp as a blue badge in the lower-right margin.

On MAIN v1.13, holding MENU opens UTILITY and changes status page `0x02`
byte 7 from `0x1f` to `0x8f`. The lamp stays on after release and goes
dark when a subsequent press closes UTILITY. PyGhidra shows the navigation
writer setting bit 7 of staging byte `0x0C9D0AEB` at `0x0C063480`. Selector
3 sets TAG LIST bit 5 at `0x0C06345C`. The shared page builder copies both
bits to byte 7 of page `0x02`.

TAG LIST lit in the empty list with byte 7 at `0x2f`, then cleared when INFO
took over. Live and replay framebuffers matched at four MENU checkpoints
and eight navigation checkpoints. The captures, hashes, and build evidence
are under `runs/scif1-menu/`.

#### Now-playing overview, artist, and cue strips

The analyzed USB fixture contains two synthetic tracks with the artist
`SCIF ARTIST`. GRID128 carries three saved memory cues. GRID96 has an
independent waveform and an empty memory list.

| Field | MAIN source |
| --- | --- |
| Artist | Mode `0x0a`, text row 2 with attribute `0x07`. The row builder at `0x0C0660D4` serializes the attribute and text. |
| Waveform | `0x0C0662AE` packs 100 samples from `0x0C9D1199` into frames `0x70`–`0x77`. Samples start at byte 3 on `0x70`, run through bytes 1–14 on `0x71`–`0x76`, and end with bytes 1–4 on `0x77`. |
| MEMORY | `0x0C06664E` packs the normalized starts of saved cues and loops MSB first, serialized by `0x0C0664A8` into bytes 2–14 of frame `0x60`. Status page `0x02` byte 11 bit 4 drives the caption. |
| Playing address | `0x0C0637CC` emits the elapsed column count (0–100) in page `0x02` byte 4. Byte 9 bits 1 and 0 indicate the slow and fast warnings, and bit 2 carries the lit phase. Byte 13 bit 0 selects REMAIN. |
| CUE | `0x0C0642CC` emits one column for the current cue or loop-in in page `0x02` byte 10. The value is one-based. Zero leaves the cue marker and caption dark. |

The renderer groups the firmware's 15 waveform levels into five dot heights
with three levels per height. The decoder publishes an overview after all
eight checksummed parts arrive in order. Track number changes and warm reset
clear cached data. A valid empty waveform or memory bitmap clears that field.
The next track's title can arrive before its overview, so its waveform stays
dark until the new burst is complete.

The green playing-address bar fills from the left in elapsed mode and
extinguishes columns from the left in REMAIN. The decoder follows the phase
sent by MAIN for the warnings below 30 and 15 seconds. Replay preserves both
phases using the recorded status pages.

The red MEMORY and blue CUE strips and legends follow SCIF1 independently of
the matrix mode. The firmware retains both strips through the `MEMORY` and
`DELETED` messages. The red legend flashes using its status bit while the
saved marks follow the bitmap, including when a saved point is removed.

```sh
source ./env.sh
CDJ850_PLAYING_TEST_IMAGE="$CDJ_ROOT/runs/scif1-playing/probe/export.img" \
  /usr/local/bin/python3.11 -m pytest -q \
    tests/test_cdj850_usb.py::test_live_scif1_playing_fields_match_replay \
    tests/test_cdj850_scif1_replay.py::test_replay_overview_ignores_partial_and_pre_reset_packets
```

Full QMP and replay framebuffers matched for GRID128, its current CUE point,
GRID96, and USB unload. GRID128 displayed saved memory columns 12, 36, and
66. The fixture generator, input timeline, captures, hashes, and build
evidence are under `runs/scif1-playing/`. All 59 CDJ-850 tests passed
across the initial run and the targeted reruns.

#### Panel-side behavior (evidence limit)

Keep the typed `Cdj850Playing` state backed by capture evidence.
Use explicit known/valid flags and atomic complete-message updates. Clear stale
metadata/waveform/strip/lamp state on source/track transitions and reset. Keep
photo fixture initialization confined to the preview tool.

The long-name probe held a 62-character title and a 45-character artist
on the deck for more than 18 seconds and pressed INFO again. Each mode
`0x0a` update carried the same full strings. Header byte 11 changed,
but the capture contained no successive text slices. The service manual
identifies IC1007 (PEG799A8) as the processor that drives the DATA FL.
The available firmware image contains MAIN, and the captures end at the
MAIN-to-panel link. Exact scrolling cadence, remaining animation timing,
and the original glyph rendering need the panel firmware or a synchronized
capture of its glass output. The probe is under `runs/scif1-panel-boundary/`. Preserve the two-line diagnostic accessor while
existing tests/scripts depend on it.

Acceptance per field: known-good and malformed-message tests, replay fixture,
live capture comparison, and an explicit test that absent data remains absent.

### 5. Verify frontend, reset and timing (completed; hardware video items deferred)

Verified across direct QEMU console, `1280x480` VNC RFB + QMP screendumps, and
the desktop virtual-deck app face (`app/faces/cdj850.json`,
`app/cdj850_panel.py`, `tests/test_app_faces.py`): dynamic `FlCanvas`
padded-stride bounds (`640x240`, `1320x500`, `test_cdj850_vfd_render.py`),
inactive segment and red/blue label tinting, and long (>60 char), inverse
(`0x25`), punctuation, high-byte (`0x80..0xfe`), and control (`1..31`) glyphs.

Reset invalidation, warm `system_reset` reboot recovery (`ae65363` resetting
DSP56371/SHI/INTC/on-chip peripherals + readout/VFD clearing in `vfd.c`),
VMState save/restore (`test_cdj850_snapshot.py`), change-only VFD redraw guards
(`vfd.c`, `test_cdj850_vfd.py::test_change_only_redraws`), and automated live
rekordbox export coverage (`test_live_scif1_bpm_matches_replay` and
`test_live_scif1_playing_fields_match_replay` in `tests/test_cdj850_usb.py`) are
implemented and verified.

Remaining hardware-reference gaps (waiting on physical-player video from the
user): IC1007-side scrolling cadence, other panel-side blink behavior, and
waveform animation timing. Do not invent timings and call them 1:1 behavior.
Broader DSP/audio real-time performance validation also remains separate from
this display work and is tracked separately.

## Return path

Keep display changes on `codex/display-integration` until reviewed. Cherry-pick
only the display commit into the user's chosen integration branch after their
DSP work settles; don't merge or reset their dirty checkout automatically.
Use `git show --stat` before transfer to verify no DSP, QEMU-generated tree,
firmware, personal configuration or image artifacts entered this commit.
