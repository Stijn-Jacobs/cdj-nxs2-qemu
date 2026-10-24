# cdj3000x_beat

**Experimental.** For the CDJ-3000X identity (`cdj3000x`): fills the beat boxes
at the top of the screen. The NXS2 draws them from four fields of the control
report that a CDJ-3000X does not send, because a 3000X works the beat out
itself from the beat grid and the playing position. This mod does the same on
the deck: it finds the beat the position is in with the grid Serato sends and
writes the length of the bar and the position in it into the report before the
firmware reads it.

| | |
|---|---|
| knobs | `CDJ_MAIN_X3000SEGMENTS` |
| on / off | `1` / `0` |
| default | off |

**What is verified.** The routine runs on the interpreter against grids built
by hand and against the grids and positions of two recorded Serato sessions
(the segment it writes matches a model of the grid on every position tried, and
cycles 1 to 4 with the beats). The update validator passes with it applied. It
has not been tried on a deck: whether the boxes fill the way the stock
firmware's own segment code says they should is not yet measured. The second
row of boxes and the phase meter stay empty.

Back to the [mods](../../README.md#mods).
