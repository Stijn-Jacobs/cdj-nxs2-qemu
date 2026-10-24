# cdj3000x_browse

**Experimental.** For the CDJ-3000X identity (`cdj3000x`): moves the browse
list Serato shows on the deck as soon as the encoder turns. Serato answers each
step with a new page of the list 38 to 64 ms later (median 51), so on its own the
list follows the encoder that much late. Serato sends 11 rows with the cursor
four rows down and the deck shows eight, so the rows a step brings on screen are
mostly rows the deck already has: this mod predicts Serato's next page from the
last one and the step, shows it at once, and lets Serato's answer correct it. A
new top row on an upward step stays blank until the answer fills it in.

| | |
|---|---|
| knobs | `CDJ_MAIN_X3000BROWSE` |
| on / off | `1` / `0` |
| default | off |

The routine (`x3000browse`, a hook in the HID IN task after `x3000in`) sits in
the `cdj3000x` folder, beside `x3000rxview`, whose words it shares; this folder
holds its switch. It needs the `cdj3000x` mod on. It shows at most one list
every 30 ms and drops a step Serato has not answered after 150 ms (at the top or
the bottom of a crate Serato sends nothing).

**What is verified.** The routines run on the interpreter against a recorded
Serato session scrolling a crate up to its top: every list shown ahead of
Serato's answer is the page a model of Serato's paging predicts, and the HID OUT
task never waits on the HID IN task. Of the pages Serato sent in recorded
sessions, the prediction matched 39 of 50 exactly and 10 more but for the blank
new top row. In a replay of a captured session (1.8 s of scrolling) every list
shown matched the prediction; the only corrections were where Serato moved two
rows in one step and at the top of the crate. The owner has tried it live on the
emulator; no measurement of the live run is recorded. It has not been tried on a
real deck.

Back to the [mods](../../README.md#mods).
