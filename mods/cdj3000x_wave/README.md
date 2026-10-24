# cdj3000x_wave

**Experimental.** For the CDJ-3000X identity (`cdj3000x`): stops the centre
waveform going black every time Serato sends more of it. Serato streams the
detail waveform as the track plays, so the deck sends the new part of it to the
display about every 1,500 points and after a seek. The display hides the strip
from the first chunk of an update until the beat grid that follows it is in.
This display patch leaves out the two stores that hide it, so the old picture
stays until the new one is complete.

| | |
|---|---|
| knobs | `CDJ_GUI_X3000KEEPWAVE`, `CDJ_GUI_X3000TRAINPOS` |
| on / off | `1` / `0` |
| default | off |

`CDJ_GUI_X3000TRAINPOS=1` is an optional second display hook, set in addition to
`CDJ_GUI_X3000KEEPWAVE`. While an update is on the link the display holds the
centre waveform's position for about 78 ms (around 4 renders) and the waveform
stops. The hook moves the position on from the display's own clock for as long
as the update is on the link.

**What is verified.** The two stores and what they do were read from the display
firmware 1.81; the patch applies to it and the update validator passes. The strip
stays drawn through an update on the emulator with Serato; whether loading a
second track clears the first one's waveform is not yet measured.

`CDJ_GUI_X3000TRAINPOS=1` was measured on a replay of a captured Serato session,
3 runs per arm, host load 36 to 75 percent. Around the updates the longest run of
renders without a position step is 1 to 2, the same as in steady play (1 to 3),
against about 4 renders per update without the hook. The position never steps
backwards. At the end of an update it is 3 to 7 counts off on a quiet host, and
the catch-up jumps are 14 to 29 counts on a loaded one. The time readout and the
minimap still hold for the length of an update. Not tried on a real deck.

Back to the [mods](../../README.md#mods).
