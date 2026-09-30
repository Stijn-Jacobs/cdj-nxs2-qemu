# three_band

Draws the centre waveform in three bands, CDJ-3000 style: low in blue, mid in
amber, high in white. It needs a track with rekordbox's colour waveform: the
bands are worked out from each column's RGB colour, so a track without one
keeps the stock drawing.

| | |
|---|---|
| knob | `CDJ_GUI_WAVE3` |
| on / off | `1` / `0` |
| default | off |

**Turning it on.** `./setup.sh` asks about it in step 7 and saves the answer to
`cdj.conf`; `./setup.sh --reconfigure` asks again. For one run,
`CDJ_GUI_WAVE3=1 ./start.sh`.

**What it changes.** It patches the display firmware's RGB centre-waveform
renderer, from the height clamp to the end of the column fill, so each column
is drawn as three stacked bands instead of one coloured bar. MAIN is untouched.

It is a real code patch: `python mods/patch_update.py C2KNXS2.UPD out.UPD
wave3` writes it into a real update. It is verified against display firmware
1.81.

**What is verified.** It runs in the emulator. It has not run on a real deck.

Back to the [mods](../README.md#mods).
