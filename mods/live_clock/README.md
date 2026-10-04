# live_clock

Repaints the REMAIN / elapsed clock on every frame instead of about three
times a second. The digits count smoothly with the waveform instead of
jumping.

| | |
|---|---|
| knob | `CDJ_GUI_CLOCK_DT` |
| on / off | `43` / `0` |
| default | on |

**Turning it on or off.** `./setup.sh` asks about it in step 7 and saves the
answer to `cdj.conf`; `./setup.sh --reconfigure` asks again. For one run,
`CDJ_GUI_CLOCK_DT=0 ./start.sh` switches it off. `22` repaints every second
frame.

**Why the stock clock is slow here.** The display firmware times how long each
frame took to draw and adds that to two accumulators, one that posts the
deck-screen redraw while a track plays and one that repaints the clock widget
otherwise. Each acts only once its sum passes 42 ms. On the real board drawing
is slow enough for that to be every frame or two; the emulated board draws a
frame in a few milliseconds, so the clock was repainted about three times a
second while the waveform ran at about 45 frames.

**What it changes.** With the knob, both accumulators add a fixed number of
milliseconds per frame instead of the measured time, so 43 passes 42 ms on
every frame. The parts of the firmware that budget a frame still see the
measured time.

**Emulator only.** This corrects a pacing difference of the emulated board, not
a firmware behaviour, and does not appear in `mods/patch_update.py`.

Back to the [mods](../../README.md#mods).
