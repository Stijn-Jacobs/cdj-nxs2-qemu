# Driving the virtual CDJs from a MIDI controller

Any MIDI controller can play the emulated decks. The bridge knows nothing about
a particular unit: a controller is a **profile** (what the hardware sends) plus
a **mapping** (which CDJ key each control presses), both plain JSON. The
Roland DJ-202 ships with both and is the reference; another controller is two
commands away (see *Adding your controller*).

The path has three pieces, and each knows only its own half of the problem:

```
controller --MIDI--> midi/bridge.py --TCP--> scripts/run/midi_relay.py --UDP--> machine
                     (where the USB        (runs with the decks;            cdj_panelkeys.h
                      device enumerates)    a dumb pipe)
```

`bridge.py` holds the profile and the mapping and no transport detail;
`midi_relay.py` holds the transport and no map knowledge. It forwards
`<tag> <off>:<val>:<dur_ms>:<op>` lines verbatim, so it can be driven by hand
for a sweep:

```
printf 'show1 0x12:16:150:or\n' | nc 127.0.0.1 7202
```

`tag` is what selects the deck: each machine binds its own panel socket named
after the tag it launched with (`show1`, `show2`). Two tags, two decks. That is
the whole of the "two virtual CDJs" mechanism. `./start.sh` starts the relay
with the decks and, when a controller is configured, the bridge too.

## Profiles, mappings and actions

| file | holds | comes from |
|---|---|---|
| `controllers/<name>.json` | the **profile**: every control by name, with its MIDI port, type (`note` / `cc` / `pitch`), channel, number and behaviour; the port-name word the bridge looks for (`match`); the LED output port, if the unit has lamps | **captured off the device** by `learn.py` |
| `mappings/<name>.json` | the **mapping**: control name → CDJ action and deck. No MIDI numbers in it | written by `learn.py map`, or by hand |
| `cdj_actions.py` | every CDJ action and how sure this project is that it works | the firmware's own panel decoder and key-name table |

Re-learning a controller rewrites its profile and the mapping keeps working.
No published MIDI chart is transcribed anywhere: the numbers come from the
hardware in front of you.

A control's **behaviour** is one of `button` (note on/off; held for as long as
your finger is), `absolute` (a fader or knob, 7-bit CC or 14-bit pitchwheel),
or a relative encoder in one of three encodings: `relative` (two's complement,
1 = +1, 127 = −1), `relative_offset` (65 = +1, 63 = −1) or `relative_signbit`
(1 = +1, 65 = −1). `learn.py` works out which from what the control sends.

**Every CDJ key is mappable**, including the ones nothing binds today.
`python midi/learn.py actions` prints the full catalogue:

| status | meaning |
|---|---|
| `confirmed` | seen to move the firmware's own state in this emulator (Cue, Browse, Sync, Track/Scan, Loop In/Out, the select knob and its push, the jog …) |
| `partial` | does something, but not all its name implies (Play/Pause, the tempo slider, the touch screen tap) |
| `decoded` | the firmware's panel decoder reads the bit into a setter the firmware itself names (Master, Delete, Eject, Call </>, Dev Link, Time/A.Cue …), but nothing here has pressed it yet |
| `guess` | a raw bit with no name yet |
| `unbound` | named by the firmware, but no report field is known (the needle strip) |

Beyond the named actions, `key_0xOFF_0xMASK` presses any single bit of the
front-panel report (for example `key_0x1a_0x04`), `level_0xOFF` sets any whole
analogue byte and `rot_0xOFF` turns any counter byte. `touch_tap` taps the
touch screen at a pixel given in the mapping entry. So a controller can reach a
key before anyone has worked out what it is called, and binding an unnamed bit
is exactly how the next one gets named.

### The mapping format

```json
{
  "controller": "pioneer-ddj-400",
  "decks": {"cdjA": "show1", "cdjB": "show2"},
  "focus": "cdjA",
  "bindings": {
    "left/play":      {"action": "play_pause",   "deck": "cdjA"},
    "left/cue":       {"action": "cue",          "deck": "cdjA"},
    "left/jog":       {"action": "jog",          "deck": "cdjA"},
    "left/tempo":     {"action": "tempo",        "deck": "cdjA"},
    "left/pad_1":     {"action": "key_0x1a_0x01", "deck": "cdjA"},
    "left/pad_8":     {"action": "touch_tap",    "deck": "cdjA", "x": 700, "y": 60},
    "left/load":      {"action": "set_focus",    "deck": "cdjA"},
    "right/load":     {"action": "set_focus",    "deck": "cdjB"},
    "browse/turn":    {"action": "select_turn",  "deck": "focus", "scale": 1},
    "browse/push":    {"action": "rotary_push",  "deck": "focus"}
  },
  "leds": {
    "cdjA": {"play": "left/play", "cue": "left/cue", "beat": ["left/pad_5", "left/pad_6", "left/pad_7", "left/pad_8"]}
  }
}
```

`decks` names the machines by their tags; `bridge.py --prefix <name>` rewrites
them to `<name>1` / `<name>2`, which is how `start.sh` points a mapping at the
deck name you chose. `deck` is a deck id, or `focus`: the deck the last
`set_focus` control picked (a CDJ pair shares one browse knob in practice, so
LOAD chooses which deck it drives). `scale` multiplies an encoder's detents.
`leds` is optional and binds the lamp roles in `leds.py` to controls whose
lights answer to the note they send; a controller that lights differently
simply leaves it out.

## Adding your controller

On the machine the controller is plugged into (on Windows, a native Python
from python.org or the Microsoft Store, not MSYS2's, with
`py -3 -m pip install mido python-rtmidi`):

```
python midi/learn.py ports                        # is it there? note a word from its port name
python midi/learn.py new                          # name it, then move each control and name it
python midi/learn.py map --controller <name>      # bind controls to CDJ actions ('?' lists them)
python midi/bridge.py --controller <name> --dry-run   # check: prints every binding it resolved
```

`new` writes `controllers/<name>.json`; `map` writes `mappings/<name>.json`
(move a control or type its name, then pick an action and a deck; `all` walks
every control). `show` prints a mapping as a table. `./setup.sh --reconfigure`
offers the same walk-through under *Controller → learn a new one*.

Without `--controller`, the bridge picks the profile whose `match` word appears
in a connected MIDI port's name, and falls back to the DJ-202.

## Running it

```
./start.sh                                        # decks + relay (+ the bridge, if configured)

# or by hand, in a second shell once the deck windows are up:
python -u midi/bridge.py --relay 127.0.0.1:7202 --prefix show
```

`bridge.py --dry-run` decodes and prints without sending, which is the right
way to check a fresh profile before any machine is running; with nothing
plugged in it still loads the profile and mapping and resolves every binding.

---

# The Roland DJ-202, in detail

The DJ-202 enumerates on Windows (`VID_0582&PID_0205`) as two MIDI ports,
`DJ-202 0` (the surface) and `DJ-202 1` (its LEDs). Its profile is
`controllers/roland-dj-202.json`, its learn checklist
`controllers/roland-dj-202.checklist.txt` and its mapping
`mappings/roland-dj-202.json`.

## The surface

The DJ-202 splits cleanly by MIDI channel, which is what makes two decks easy:

```
ch15  shared: browse encoder (cc 0, relative), its push (note 6),
      LOAD deck 1 (note 2), LOAD deck 2 (note 3), SHIFT (note 0)
ch0   deck 1: PLAY note 0, CUE note 1, SYNC 2, SLIP 7, KEY LOCK 13,
      headphone CUE 27                              ch1  deck 2, same numbers
ch4   deck 1 pads, note == pad number (1-8)         ch5  deck 2 pads
ch8   deck 1 FX 1-3 notes 0/1/2, TAP 4              ch9  deck 2
```

The unit uses two relative encodings: the browse encoder is two's complement
(`1` = +1, `127` = −1) and the platters are offset-64 (`65` = +1, `63` = −1).
The platters also send a redundant `pitchwheel` stream, which the bridge
ignores in favour of `cc 6`. The unit free-runs MIDI clock, and the pad-mode
buttons send ch4/5 `note 0` with velocity 3/19/32/51; the learn tool drops
both. Pads send different notes per pad mode (+16 and +32 in the other modes);
the capture was taken in **HOT CUE** mode, so switch back to it or re-learn.

## The mapping

The CDJ has panel keys the DJ-202 has no button for, so each deck's pads
become that deck's panel:

```
pad 1 Browse    pad 2 Menu      pad 3 USB       pad 4 Back
pad 5 TrackRev  pad 6 TrackFwd  pad 7 ScanRev   pad 8 ScanFwd
```

On deck 1, pads 7 and 8 are TEMPO RANGE and MASTER TEMPO instead, two keys
whose effect is visible on the screen at once. PLAY, CUE, SYNC, SLIP and the
tempo slider map to their CDJ namesakes; KEY LOCK is the NXS2's MASTER TEMPO;
FX TAP toggles JOG MODE (VINYL/CDJ). LOAD does not load (on a CDJ the rotary
push loads): it picks which deck the shared browse knob drives, and the bridge
prints the focus change. The mixer section (faders, EQ, filter, crossfader)
is unmapped by design: on real hardware that is a separate DJM.

**The jog.** The platter's ticks drive a 16-bit position counter (report bytes
8–9) and a pulse period (bytes 10–11) that the firmware's own jog engine
reads, plus the `JogMov` / `JogRoundFwd` level bits at byte `0x0F`. `JogTouch`
is bound separately to the platter's touch sensor, so a resting hand does not
claim motion. `--jog-nominal` sets which tick rate counts as nominal platter
speed.

**Held keys.** A key is a bit that is set while held. The bridge sends a
`hold` when the note goes down and a `rel` when it comes up, so a tap is as
long as your finger: BROWSE tapped toggles the browse screen, BROWSE held
jumps to the category shortcut, as on the hardware.

**Analogue fields.** The panel decoder reads two whole bytes as values:
`0x02` (release/start, setter `0x084E1654`) and `0x04` (tempo slider, setter
`0x084E1668`). An `or` key op cannot express a value, so these use the `lvl`
op, which assigns the byte and holds it until changed.

## LEDs

The DJ-202 lights a button when it receives the same channel and note that
button sends, on `DJ-202 1`: velocity 127 on, 0 off. So an LED is addressed
by its control's name and needs no numbers of its own.

What to light comes from the deck, over the relay's TCP link in reverse. Each
machine publishes its state to `/tmp/cdj-panel-state-<tag>.sock` (override
with `CDJ_PANEL_STATESOCK`), which the relay binds and forwards as
`<tag> state ...` lines. Two datagrams, each sent on change, re-sent every
second and throttled to 50 Hz:

* `frm beat bars tempo bpm`: read off the MAIN→display heartbeat, so `beat` is
  the box the screen lights;
* `pnl <hex>`: MAIN's 40-byte frame to the panel microcontroller, which
  carries the NXS2's own lamps. MAIN blinks a lamp by toggling its bit, so
  mirroring the bit reproduces the NXS2's own blink rate.

| frame bits | lamp | firmware slot / setter |
|---|---|---|
| byte 0 bit 0 | PLAY: steady while playing, blinking while paused | `0x0B54C05C` / `0x084DC2D8` |
| byte 0 bit 1 | CUE: blinks at twice PLAY's rate while paused | `0x0B54C060` / `0x084DC2E0` |
| byte 0 bit 7 | SLIP | `0x0B54C08C` / `0x084DC32C` |
| byte 1 bit 2 | MASTER TEMPO | getter `0x084DC628` |
| byte 2 bits 3:2, 1:0 | jog ring light, two 2-bit levels | slots `0x0B54C09C` / `0x0B54C0A0`, set from JOG BRIGHTNESS (`0x0A35F6E0`) |
| byte 9 | centre jog display pointer, 135 positions | `0x08450270` |
| bytes 0x16–0x18 | RGB media-slot lamp | `0x084DC0E8` / `0x084DC1C0` |

The frame is built by `0x0844F808`. `midi/leds.py` renders it, with roles
bound in the mapping's `"leds"`:

| role | LEDs | shows |
|---|---|---|
| play | PLAY | the NXS2's PLAY lamp |
| cue | CUE | the NXS2's CUE lamp |
| slip | SLIP | the NXS2's SLIP lamp |
| master_tempo | KEY LOCK | the NXS2's MASTER TEMPO lamp |
| ring | headphone CUE | the jog ring light, lit at any level |
| beat | FX 1, FX 2, FX 3, TAP | the firmware's beat in the bar; in the last bar of a phrase they fill 1, 1-2, 1-2-3, all |
| (pads) | every pad with a binding | lit, so the unit shows what it can do |

`--no-leds` leaves the lamps alone, and `midi/led_replay.py <log>` replays a
recorded state log through the LED rules offline.
