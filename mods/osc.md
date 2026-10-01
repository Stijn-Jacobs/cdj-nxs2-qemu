# osc

Sends the deck's beats and state as OSC messages, so lighting desks, VJ
software and scripts can follow the deck without decoding Pro DJ Link. Every
message is one plain OSC message per UDP datagram (no bundles), broadcast to
the deck's subnet on **UDP port 50010**. `<n>` is the deck's player number as
one digit, 1 to 4; integers are OSC `i` (big-endian int32).

| | |
|---|---|
| knobs | `CDJ_MAIN_OSC`, `CDJ_MAIN_OSCBEAT` |
| on / off | `1` / `0` |
| default | off |

**Turning it on.** `./setup.sh` asks about it in step 7 and saves the answer to
`cdj.conf` (under `CDJ_MAIN_OSC`; the beat knob follows it). `./setup.sh
--reconfigure` asks again. For one run, set both knobs to `1` in the
environment.

## Messages

Sent with every Pro DJ Link beat packet, so while a track plays:

| address | tags | arguments |
|---|---|---|
| `/cdj/<n>/beat` | `,i` | beat in bar, 1 to 4 |
| `/cdj/<n>/bar` | `,i` | `1`, only on beat 1 of the bar |
| `/cdj/<n>/timing` | `,iiiiii` | beat in bar; track BPM x 100; pitch (`0x100000` = 0 %); ms to the next beat; ms to the next bar (both at normal speed, `0xFFFFFFFF` = none); the deck's clock in ms |

Sent when the value changes, and again in a slow rotation (one every 250 ms
while nothing changes), so a receiver that starts late has all of them within
two seconds:

| address | tags | arguments |
|---|---|---|
| `/cdj/<n>/playing` | `,i` | 1 while playing |
| `/cdj/<n>/state` | `,i` | the firmware's play-state code: 0 no track, 2 loading, 3 playing, 4 loop, 5 paused, 6 cued, 7 cue play, 8 platter held, 9 search, 0x11 track ended |
| `/cdj/<n>/loaded` | `,i` | 1 when a track is loaded (state is not 0) |
| `/cdj/<n>/end` | `,i` | 1 when state is 0x11 |
| `/cdj/<n>/master` | `,i` | 1 when this deck is the tempo master |
| `/cdj/<n>/sync` | `,i` | 1 when SYNC is on |
| `/cdj/<n>/masterdeck` | `,i` | player number of the tempo master, 0 for none |
| `/cdj/<n>/track` | `,i` | rekordbox id of the loaded track, 0 for none |

At most two state messages go out per pass; the rest follow on the next ones.

Sent once, when it happens (a deck at rest sends none of them, and the first
status pass after boot only records the starting values):

| address | tags | sent when | argument |
|---|---|---|---|
| `/cdj/<n>/load` | `,i` | a track starts loading (state becomes 2) | rekordbox id |
| `/cdj/<n>/play` | `,i` | playback starts | BPM x 100, 0 when the deck has none |
| `/cdj/<n>/stop` | `,i` | playback stops | 0 |
| `/cdj/<n>/cue` | `,i` | the deck goes to its cue point (state 6) or holds CUE (7) | the state |
| `/cdj/<n>/loop` | `,i` | a loop starts (1) or ends (0) | 1 or 0 |

These are the events a [beat-link-trigger](https://github.com/Deep-Symmetry/beat-link-trigger)
trigger fires on. They do not count against the two-per-pass limit. Hot cue
reached, phrase start, on-air and memory-cue reached are left out: the firmware
signal for each has not been found.

## On the emulator

A real deck's broadcast reaches every OSC app on its network. The emulated
decks' network is QEMU's multicast segment, which no OSC app can read, so
unwrap it on the host while the deck runs:

```sh
python3 scripts/net/osc_relay.py                                  # to 127.0.0.1:50010
python3 scripts/net/osc_relay.py 239.77.77.1:45000 192.168.1.20:50010
```

## In the firmware

Two MAIN patches: one hooked at the send of the Pro DJ Link beat packet, one
at the routine that serialises the deck's status record. Both send through
MAIN's own network send routine, with the payload built inside the patch.
Nothing is sent when the packet's player is not 1 to 4. `python
mods/patch_update.py C2KNXS2.UPD out.UPD osc` writes both into a real
update (firmware 1.87).

**What is verified.** The beat half (`beat`, `bar`, `timing`) was checked live
in the emulator: one matching message per beat, with the deck's beat timing
unchanged. The state and event messages are tested offline only, and nothing has run on
a real deck. The deck clock in `timing` is the firmware's millisecond tick
word, not measured against a real clock.

Back to the [mods](../README.md#mods).
