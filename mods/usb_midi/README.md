# usb_midi

Sends MIDI real-time messages from the deck over its rear USB-B port, so a DAW
or a drum machine can follow the deck: **Start** (`FA`) the first time the deck
plays after boot, **Continue** (`FB`) on every later play, **Stop** (`FC`) when
it stops, and **Timing Clock** (`F8`) 24 times per beat while it plays, at the
tempo the deck is actually playing (track BPM scaled by the pitch).

| | |
|---|---|
| knob | `CDJ_MAIN_USBMIDI` |
| on / off | `1` / `0` |
| default | off |

**Turning it on.** `./setup.sh` asks about it in step 7 and saves the answer to
`cdj.conf`; `./setup.sh --reconfigure` asks again. For one run,
`CDJ_MAIN_USBMIDI=1 ./start.sh`.

**What it changes.** Two MAIN patches. One sits in the firmware's 1 ms timer
interrupt: every millisecond it reads the deck's status record (play flag, BPM,
pitch), advances its own phase and counts the clocks and Start / Stop /
Continue events that fall due, then wakes the deck's own MIDI task. The other
sits at the top of that task's loop: it appends one 4-byte USB-MIDI event packet
(`0F <status> 00 00`, cable 0) per event to the buffer the stock MIDI events
use and commits it, so the task sends them. Clocks therefore leave on a 1 ms
grid, normally one per USB transfer, whatever the panel or the display is
doing; the first clock goes out with Start or Continue. The deck only arms its
MIDI task in PC control mode, so once the host has configured the rear port the
patch posts the task's own "configured" event to the MIDI interface (and only
to that one, which leaves audio and the other classes alone), once per
connection. The knob selects both patches (`usbmidi` and `usbmiditick` in
`patch_main.py`); `python mods/patch_update.py C2KNXS2.UPD out.UPD
usb_midi` writes both into a real update (firmware 1.87).

**Status: the 1 ms clock has not run yet, in the emulator or on a deck.** The
previous version, which sent from the panel task, worked in the emulator
(Start, Timing Clock at the expected average rate, Stop on PAUSE, Continue on
PLAY, playback unchanged) but in clumps at the panel's pace, which a DAW reads
as a wavering tempo. The new timing is checked in an SH-4 interpreter only.

Back to the [mods](../../README.md#mods).
