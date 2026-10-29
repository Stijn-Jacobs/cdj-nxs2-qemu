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
`cdj2000nxs2/patch.py`); `python mods/patch_update.py C2KNXS2.UPD out.UPD
usb_midi` writes both into a real update (firmware 1.87).

**What is verified.** On the emulated deck, with the rear port read through
the emulator's UDP relay (two runs with the mod on, two with it off, one deck):
3614 and 7161 Timing Clocks, never more than one per USB transfer over 10777
transfers, and a mean gap of 12.202 ms against 12.203 ms expected. The gap is
quantised to whole milliseconds, so the spread is 0.43 ms; 98.7 and 99.0 % of
the gaps are within 1 ms. Start, Stop and Continue went out once each in
order, nothing was sent with the mod off, there was no E-8302 and the playhead
kept moving. Over a pitch sweep (about -10 % to +10 %) the clock gap followed
BPM x pitch within 0.5 %.

**Not tried yet.** Everything above was counted at the relay, not by a MIDI
client on the computer: the port has not been read through Windows' own MIDI
stack (see [testing against the host](../../docs/mods.md#usb-midi-host)), and the
jitter against the earlier build, which sent from the panel task, has not been
measured. Nothing has run on a real deck, and the 1 ms hook adds code to a
real-time interrupt whose cost on the hardware is unmeasured.

Back to the [mods](../../docs/mods.md).
