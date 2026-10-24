# cdj3000x

**Experimental.** Makes a
CDJ-2000NXS2 present itself to Serato DJ Pro as a CDJ-3000X, so that Serato
sends the richer data a 3000X gets: the windowed detail waveform, the 3-band
waveforms and the 1200-column overview. It needs no CDJ-3000X firmware file:
a stock 3000X v1.40 answers Serato's authentication under an all-zero key, and
so does this mod (see the key, below).

| | |
|---|---|
| knobs | `CDJ_MAIN_X3000IDENT`, `CDJ_MAIN_X3000BCD`, `CDJ_MAIN_X3000HS`, `CDJ_MAIN_X3000IN`, `CDJ_MAIN_X3000OUT`, `CDJ_MAIN_X3000RXBUF`, `CDJ_MAIN_X3000RXVIEW` |
| on / off | `1` / `0` |
| default | off |
| apply-time inputs | `CDJ_3000X_PID` (hex, default `004e`) |

**Status.** With the identity and the handshake on, Serato DJ Pro recognises
the deck as a CDJ-3000X and connects to it: it no longer asks which deck to
assign, and it streams the control, overview, beat grid, detail waveform, album
art and library list reports. The deck shows the track's BPM and tempo, the
overview, the beat grid, the detail waveform and the browse list; BACK and push
to load work live. The three-band waveforms are not handled: Serato has not sent
them so far. Tried live with Serato DJ Pro over USB/IP on the emulator, not on a
real deck.

| part | state |
|---|---|
| USB identity: product id, strings, bcdDevice | done, applied to the update and checked by the update validator |
| Serato authentication | done. Serato authenticates the 3000X over the same HID handshake the NXS2 already runs, not a separate MIDI channel: it answers the deck's hello with a host-identity message (stage 0x30) and the deck replies with its own identity (stage 0x31). `x3000hs` sends that reply and marks the deck authenticated. No key is involved in this exchange. (A MIDI SysEx certificate path also exists in the 3000X firmware, but Serato does not use it for this.) |
| 1024-byte HID Output report | done, live with Serato. Serato sends the detail waveform only to a deck whose report descriptor declares a 1024-byte Output report. `x3000out` declares it (the descriptor keeps its length); the endpoint stays at 64 bytes, so each report arrives as 16 packets of one transfer. `x3000rxbuf` has the HID OUT task read the whole transfer into a 1024-byte buffer of its own, and `x3000rxview` gives the stock handlers the first 64 bytes of each report in their own buffer, so they see what a 64-byte read gave them |
| Report translation: the 3000X's larger reports into the NXS2's handlers | done for the library list, the control report, the overview, the beat grid and the detail waveform, live with Serato. The 3-band waveforms are not handled (Serato has not sent them so far) |

**The key.** The authentication key is 128 bits and the same in every unit of a
model, but the CDJ-3000X v1.40 package does not contain it: the file its loader
reads is absent, so a stock 3000X answers Serato under a 16-byte all-zero key.
The answer routine does the same, so the authentication part needs no key file
and no `CDJ_3000X_FW`. If a unit does carry a key file and Serato enforces it,
the all-zero slot in the patched image can be filled from that unit's own file;
`keysource.py` is the reader for that case, and a key placed there would live
only in the update you patch for your own deck, never here.

**What it changes.** Table patches and hooks, found by the bytes around them and
checked against what the 1.87 firmware holds before anything is written:

- `x3000ident` rewrites the device descriptor's product id (the 3000X's own
  fallback `0x004E`, or `CDJ_3000X_PID`) and the manufacturer, product, MIDI
  and HID strings to "AlphaTheta Corporation", "CDJ-3000X", "CDJ-3000X MIDI" and
  "CDJ-3000X HID". The vendor id stays `0x2B73`. The driver sends a string as
  long as its first byte says, so the shorter names leave the table's layout
  alone. The second device descriptor in the firmware, the vendor-class one the
  Serato path does not use, keeps its identity.
- `x3000bcd` replaces the routine that stamps the firmware version into
  bcdDevice with the constant `0x0140`, the 3000X's "1.40".
- `x3000in` sends the deck's state report in the 3000X layout: the hot cue
  pads and the analog block (browse encoder, tempo, jog, needle) one byte
  further on, and the two reverse bits the 3000X way round. It is hooked in the
  HID IN task, where the report is taken from the sender's queue, because
  everything before that queue runs at boot. Without it Serato never sees the
  encoder turn that selects a deck, and plays every track backwards.
- `x3000out` makes the HID report descriptor's Output report 1024 bytes
  instead of 64. The descriptor cannot grow, so the Output block's Physical
  Minimum 0 is written as a zero-length item to make room for the longer
  Report Count; its length and every `wDescriptorLength` stay as they are.
- `x3000rxbuf` hooks the HID OUT task in front of its read call: the read goes
  to a 1024-byte buffer at the end of the free range the routines are placed
  in, and asks for 1024 bytes. The firmware's own pipe reader keeps taking
  64-byte packets until the length is reached or a short packet ends the
  transfer, so it returns the whole report.
- `x3000rxview` hooks the task after the read returns: it copies the report's
  first 64 bytes into the task's own buffer and caps the count at 64, so the
  report router and the handshake code run exactly as on a 64-byte read. It
  also rewrites the list Serato shows on the deck (the prompt, the deck choice and
  the library), which a 3000X gets as records in one report or in fragments,
  into the NXS2's record form and hands it to the firmware's own report
  handling, so the deck draws it. The NXS2 list has a header and eight rows;
  Serato sends a header and up to eleven rows with the cursor four rows down, so
  the eight rows from four above the cursor are shown. A track row gets its BPM
  and key as text; a major key is left out, because which key number is which
  is not known. Serato's control report gets the same treatment: its BPM,
  tempo, cue point and second time sit one byte further on than the NXS2 reads
  them and are moved back, so the deck shows the right BPM and tempo. Serato's
  elapsed time is negative before the track starts, which the NXS2 cannot read
  (it would show 0:00.000 for both times); it is shown as 0:00.000. The loop
  size is shown only for a beat loop of a size the NXS2 can show (1/128 to 512
  beats); any other loop shows as no loop. Quantize passes through as Serato
  sends it. The overview
  and the beat grid arrive in fragments of 1018 bytes and are handed to the
  firmware's report handling whole, cut at the end of the message; the detail
  waveform arrives as a track length and then windows of 19 points streamed
  ahead of the playhead, and goes into the firmware's own detail buffer. The
  stock firmware sends that whole buffer to the display each time and the
  display takes in no deck state until it has gone through, so a train of only
  the new chunks is sent instead, about every 1,500 points (10 s of track). The
  first 67 s of a track arrive in a few seconds, and while it plays the detail
  stays at least 57 s ahead of the playhead. Each train holds the playhead on
  the display for 101 to 123 ms (measured on a replay of a captured session
  with the deck playing and a host load of 42 to 96 percent), against 165 to
  522 ms for sending the whole buffer every 4,500 points. The track length
  (type 0x30), which the firmware would read as the deck's 8 hot cue slots, is
  dropped: the control report already carries the length.

The real 3000X's product id is one of `0x004E` to `0x0051` and is not in any
firmware file; if you have read it off a deck, set `CDJ_3000X_PID`.

**In the firmware.** `python mods/patch_update.py C2KNXS2.UPD out.UPD cdj3000x`
writes them into a real update (firmware 1.87). An image that already carries
them is left as it is.

**What is verified.** The patched descriptors and strings read back as a 3000X
and everything else in the table is byte for byte the stock one; the bcdDevice
routine runs on an SH-4 interpreter and gives `0x0187` stock and `0x0140`
patched; the patched update passes the update validator, and the validator
recognises the change as this patcher's. It runs on the emulator with Serato
DJ Pro, which the owner has tried live. It has not been flashed to a real
deck.

Related mods, each off by default and for this identity only:
[`cdj3000x_wave`](../cdj3000x_wave/README.md) keeps the centre waveform on
screen, [`cdj3000x_browse`](../cdj3000x_browse/README.md) moves the browse list
ahead of Serato, [`cdj3000x_beat`](../cdj3000x_beat/README.md) fills the beat
boxes and [`cdj3000x_key`](../cdj3000x_key/README.md) shows the key.

Back to the [mods](../../README.md#mods).
