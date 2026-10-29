# Mods

Optional changes to the firmware, each with a folder of its own under `mods/`
(see [adding a mod](#adding-a-mod)). Two small display fixes are on by default;
the mods below are off until you switch them on. Setup and start offer a player
only the mods made for it.

**CDJ-2000NXS2**

| mod | what it does |
|---|---|
| [`three_band`](../mods/three_band/README.md) | Three-band waveforms (low blue, mid amber, high white), CDJ-3000 style, from the track's own 3-band data. |
| [`phrase`](../mods/phrase/README.md) | A coloured phrase band under the overview, CDJ-3000 style, from the track's phrase analysis. |
| [`osc`](../mods/osc/README.md) | Beats, state and transport events as OSC messages, for lighting desks and scripts. |
| [`usb_midi`](../mods/usb_midi/README.md) | MIDI clock and Start/Stop over the rear USB-B port, so a DAW or drum machine follows the deck. |
| [`ableton_link`](../mods/ableton_link/README.md) | Joins Ableton Link, so Live or any Link app follows the deck's tempo and beat. |
| [`tcnet`](../mods/tcnet/README.md) | A TCNet Master node, so ShowKontrol, Resolume and other TCNet receivers follow the deck. |
| [`gate_cue`](../mods/gate_cue/README.md) | Gate Cue, CDJ-3000 style: from pause, a hot cue pad plays only while held and letting go returns to the cue. |
| [`beat_jump_long`](../mods/beat_jump_long/README.md) | Long beat jumps, CDJ-3000 style: the 2 and 4 beat jump keys jump 32 and 64 beats. |
| [`key_shift`](../mods/key_shift/README.md) | Key shift, CDJ-3000 style: hold MASTER TEMPO and press pad A or B to move the key a semitone down or up, pad C to reset, while the tempo stays put. |
| [`cdj3000x`](../mods/cdj3000x/README.md) | Experimental. Serato DJ Pro sees the deck as a CDJ-3000X and shows its browse list, detail waveform, beat boxes and key on it. |

**XDJ-700**

| mod | what it does |
|---|---|
| [`track_filter`](../mods/track_filter/README.md) | A long press of INFO in the track list shows only tracks within 6 % of the loaded deck's BPM. |

Each mod has a page of its own: its knobs, what it patches, and what has and has
not been verified. `osc` needs a relay on the emulator
(`python3 scripts/net/osc_relay.py`, see its page); `ableton_link` is reached
through [a TAP adapter](pro-dj-link.md#rekordbox-ableton).

<a id="usb-midi-host"></a>

**Testing `usb_midi` against the host.** The rear USB-B port is relayed by the
machine's built-in host: start the deck with `CDJ_USBF_HOST=27020` (deck 1;
deck 2 uses 27021) together with `CDJ_MAIN_USBMIDI=1`, for example
`CDJ_USBF_HOST=27020 CDJ_MAIN_USBMIDI=1 bash scripts/run/rig.sh show`.
`python3 scripts/net/usb_midi_port.py --count` then prints the messages per
second by status byte, and `--monitor` one line per message. To have Windows'
own MIDI stack read the clock instead, put
`python3 scripts/net/usbip_server.py` in front of the same port and run
`usbip.exe attach -r 127.0.0.1 -b 1-1` ([usbip-win2](https://github.com/vadimgrn/usbip-win2));
the deck shows up as a "CDJ-2000NXS2" device with a MIDI port that any MIDI
monitor can open. Attach after the deck has booted, and check `usbip port`
first so the deck is not attached twice. The bridge keeps at most 256 unread
packets per endpoint, so a monitor that reads slower than the clock loses
messages. The relay is covered in [HID mode](hid-mode.md).

`./setup.sh` shows the defaults (step 7) and either takes them or asks about
each one, and saves your answers to `cdj.conf`; `./setup.sh --reconfigure`
asks again. You can also edit `cdj.conf` directly,
or override any of them for one run with the environment, e.g.
`CDJ_GUI_FRAME_MS=0 ./start.sh` (a caller's own environment always wins over
`cdj.conf`).

[SERVICE MODE](using-the-deck.md#service-mode) is a separate boot option, not a mod: it changes
what the deck boots into, not how it behaves once it is up.

## Patching a real firmware update

`three_band`, `phrase`, `osc`, `usb_midi`, `ableton_link`, `tcnet`,
`gate_cue`, `beat_jump_long` and `track_filter` are firmware code patches, not emulator knobs: the launcher applies the display ones to a copy of
the display image at boot (`mods/patch_gui.py`) and the MAIN ones to a copy of
the MAIN image (`mods/patch_main.py`), the same code a real update would carry.
`mods/patch_update.py` makes the same patches to a real Pioneer `.UPD` file, so
they end up on an actual player instead of a copy this emulator throws away on
exit; each mod's page gives its command, for example:

```sh
python mods/patch_update.py C2KNXS2.UPD C2KNXS2-link.UPD ableton_link
```

MAIN has no scatter-load table of its own: it runs in place, so a mod's routine
goes straight into a run of erased flash inside its own address space rather
than being copied out to RAM the way a display patch is.

Only a mod that is an actual firmware code patch is offered this way. An
emulator-only knob such as `high_fps` or `live_clock` has nothing to write into
a real update, and does not appear. `python mods/patch_update.py --list` shows
what is available (`--model xdj700 --list` for another player) and which firmware version it was verified against; a
mismatched version is refused unless you pass `--force-version`.

**Combining mods.** Name as many as you like in one command, e.g.
`python mods/patch_update.py C2KNXS2.UPD out.UPD three_band osc ableton_link`.
Each name brings every patch its `mod.conf` switches on. All routines
of one image are placed in a single pass into one free area, so they cannot
overlap, and mods that hook the same spot in the firmware share it: the spot
is replaced once by a stub that runs the original code and then each mod in
turn. The launcher combines the mods you switched on the same way.

**This is untested on real hardware.** The container repacking and the LZSS
re-encoding have been checked against the real update file byte for byte, and
the patched image has been checked against `patch_gui.py`'s/`patch_main.py`'s
own output (see `tests/`), but nobody has flashed a patched update into an
actual player. Flashing a modified firmware update is entirely at your own
risk. Keep the original file, and expect that a mistake here could mean a
trip through service mode's recovery path, or worse.

**Checking an update someone else modded.** One command checks any `.UPD`
before you flash it, ours or not:

```sh
python mods/validate.py downloaded.UPD
python mods/validate.py --stock official/XDJ700.UPD downloaded.UPD   # name the official file yourself
```

The player and version are read from the file's own section labels (for the
CDJ-2000 and CDJ-900, whichever of their four files you name). If the file is
byte for byte the official update, it says so and passes. Otherwise it looks for
the official file of that player and version in the folder of the file, in `fw/`
and in the player's firmware folder, and compares. A modded file never has the
official checksums, so none are compared with the official ones: every
checksum in the file (S-record lines, section CRC-16, packed MAIN sum, emergency
image sum) is recomputed from the file's own bytes and has to agree with what
the file stores. Then it prints what changed (sections, size growth, the
firmware address ranges, whether our patcher writes the same bytes) and fails a
change that lands in code the boot, the updater, the panel, the display link or
the USB stick can run, a rewrite of existing code bigger than a call site, a
write into "erased" bytes that something points at or a task can reach, and a
packed MAIN that no longer fits the updater's window. A player nobody wrote
patches for is checked the same way, from the erased bytes and the tasks in the
image.

| exit | meaning |
|---|---|
| 0 | safe as far as this checks |
| 1 | a check failed; do not flash |
| 2 | not shown safe: nothing failed, but there is no official copy of that version to compare with (only the checksums and the updater's window were checked), or a write could not be proven |

NOT CHECKED, and printed after every run: indirect calls through RAM tables
(the code a task reaches is a lower bound), what the firmware itself writes
into a span at run time, that the boot loader and emergency updater in your
unit are the ones in the file, whether the updater takes an update of equal
version, and the display image's boot path (no call graph). Nothing here boots
the file. A pass does not replace testing on a real deck, with a way back
(the official file and service mode) at hand.

**Then run it.** `mod_check.py` boots a modified MAIN image beside the
installed stock one on a one-window deck, takes its pictures at the same points
in both (idle, the browser, a cued track, the menus), and passes when the image
matches stock outside the `--allow` regions the change is meant to touch:

```sh
python scripts/test/mod_check.py --model cdj900nxs my_main.bin /tmp/check \
    --allow 131,144,662,262
```

`<out>/summary.txt` gives a verdict per picture and `<out>/diff/` marks every
difference; `mod_check.py --help` has the steps and the verdicts.

<a id="adding-a-mod"></a>

## Adding a mod

Everything about one mod sits in its own folder, with a folder per player it
patches:

```
mods/track_filter/
  mod.conf                  its row: knobs, on/off values, default, players, description
  README.md                 its page
  xdj700/
    patch.py                where each routine hooks the firmware and what it imports
    main_trackfilter.s      a MAIN routine (gui_<name>.s for the display)
    main_trackfilter.bin    the same, assembled (patch_main.py --assemble)
```

The tools find mods by these folders: setup reads every `mod.conf`, and
`patch_main.py`, `patch_gui.py`, `patch_update.py` and the validator read every
`<player>/patch.py`. A `patch.py` holds a `MODS` dict, one `sigpatch.Mod` per
routine, keyed by the routine's name, whose knob is `CDJ_MAIN_<NAME>` or
`CDJ_GUI_<NAME>` (list them all in `MOD_ENV`, joined by `+`). What a mod finds
in the firmware by signature and needs only for itself stays in its own
`patch.py`. What several mods of one player share lives in
`mods/players/<player>.py`, where each mod imports it from: the image profile,
a hook site two mods sit on (both are then called from one stub at that site),
and an address more than one mod imports (`malloc`, `free`, the tick word, the
UDP send wrapper, ...). Words a mod's routines hand each other at the end of
the free space (`FreeEnd`) belong to that mod; no two mods may take the same
bytes, which the tests check. An emulator-only knob (`high_fps`) has just its
`mod.conf` and page.
