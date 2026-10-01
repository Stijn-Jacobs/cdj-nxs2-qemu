# three_band

Draws the centre waveform and the overview strip in three bands, CDJ-3000
style: low in blue, mid in amber, high in white, from the track's own 3-band
data in its `.2EX` analysis file (the `PWV7` detail and `PWV6` overview tags
rekordbox writes). A track without that data, or whose `PWV7` does not match
its colour waveform in length, keeps the stock colours.

| | |
|---|---|
| knobs | `CDJ_GUI_WAVE3`, `CDJ_GUI_WAVE3OV`, `CDJ_MAIN_WAVE3DATA`, `CDJ_MAIN_WAVE3DETAIL`, `CDJ_MAIN_WAVE3OVFETCH`, `CDJ_MAIN_WAVE3OVDATA` |
| on / off | `1` / `0` |
| default | off |

**Turning it on.** `./setup.sh` asks about it in step 7 and saves the answer to
`cdj.conf` (under `CDJ_GUI_WAVE3`; the other five follow it). `./setup.sh
--reconfigure` asks again. For one run, set all six knobs to `1` in the
environment.

**What it changes.** It patches both firmware images, under one on/off choice:

- MAIN reads `PWV7` when it builds the detail waveform and `PWV6` when it reads
  the colour preview. The detail bands are written, as words flagged so the
  display can tell them from stock ones, over the entries of the waveform
  record once the firmware has re-encoded it (that step clears the bits the
  flag uses, so it has to come after). The overview bands replace the colour
  preview in the overview payload MAIN sends to the display. Nothing about the
  link to the display changes.
- The display firmware draws a flagged column or overview record in three
  bands, and everything else exactly the stock way. The waveform therefore
  falls back to stock per track, with no state to reset.

It is a real code patch: `python mods/patch_update.py C2KNXS2.UPD out.UPD
three_band` writes all six into a real update. The display patches are verified against display firmware 1.81, the MAIN patches against
1.87.

**What is verified.** The waveform keeps moving with the MAIN data patches on
in the emulator. Drawing from the track's real 3-band data has been tested
offline only, against the byte layouts the firmware uses. It has not run on a
real deck, and has not been tried on a range of tracks.

Back to the [mods](../README.md#mods).
