# high_fps

Draws the zoomed-in waveform about twice as often: roughly 70 frames a second
instead of 33. Zoomed fully in, the centre waveform scrolls about 146 pixels a
second, so at 33 frames it moves in visible steps; at 70 it is smooth. At the
default zoom it scrolls about 35 pixels a second and there is little to gain
(32 to 36 frames). The price is about 20 % of one CPU core.

| | |
|---|---|
| knob | `CDJ_GUI_FRAME_MS` |
| on / off | `6` / `0` |
| default | on |

**Turning it on or off.** `./setup.sh` asks about it in step 7 and saves the
answer to `cdj.conf`; `./setup.sh --reconfigure` asks again. For one run,
`CDJ_GUI_FRAME_MS=0 ./start.sh` switches it off. Any value from 1 to 14 is a
shortest frame in milliseconds; 0 leaves the firmware's own 15.

**What it changes.** The display firmware's UI task sleeps out the rest of a
15 ms frame after drawing. The emulator rewrites the frame length the firmware
compares against and the sleep it takes to `<ms>`, in the display processor's
code before it runs. Animations that step once per frame would speed up with
it, so the BROWSE list's scrolling title is held to its old pace.

**Emulator only.** This is a knob of the emulated display board, not a firmware
patch: a real deck is paced by its own hardware, and the mod does not appear in
`mods/patch_update.py`.

Back to the [mods](../README.md#mods).
