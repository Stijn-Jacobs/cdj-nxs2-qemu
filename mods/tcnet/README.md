# tcnet

Makes the deck a TCNet Master node, so TCNet receivers (ShowKontrol, Resolume,
lighting desks and the open TCNet libraries) see it in their node list and can
follow its play position, BPM, speed and track. The packets follow the TCNet Link Specification
V3.5, little-endian; the node packets are broadcast to the deck's subnet and
the data packets sent to the nodes that ask for them. The deck has one
layer, its own player number (1 to 4); the other seven layers are sent empty.

| | |
|---|---|
| knobs | `CDJ_MAIN_TCNET`, `CDJ_MAIN_TCNETTIME`, `CDJ_MAIN_TCNETDATA` |
| on / off | `1` / `0` |
| default | off |

**Turning it on.** `./setup.sh` asks about it in step 7 and saves the answer to
`cdj.conf` (under `CDJ_MAIN_TCNET`; the Time and data knobs follow it).
`./setup.sh --reconfigure` asks again. For one run, set all three knobs to `1`
in the environment.

## Packets

Every packet starts with TCNet's header: node id (the low 16 bits of the deck's
IPv4 address), protocol 3.5, node name `NXS2-<n>`, a sequence number, node type
Master and a timestamp in microseconds into the current second of the deck's
1 ms clock.

| packet | port | rate | carries |
|---|---|---|---|
| Opt-IN (68 bytes) | 60000 | once a second | listener port 65023, uptime in seconds (wraps at 12 h), vendor `cdj-nxs2-qemu`, device `CDJ-2000NXS2`, version 1.87.0 |
| Status (300 bytes) | 60000 | once a second | for the deck's layer: source = player number, layer state, rekordbox track id, name `DECK <n>` |
| Time (162 bytes) | 60001 | every 20 ms, and at once when the layer state changes | for the deck's layer: play position in ms, track length in ms, beat in the bar (1 to 4, 0 none), layer state |
| Opt-IN (68 bytes) | each node's listener port | once a second | the same Opt-IN, sent to every node in the deck's node list |
| Metrics (122 bytes) | the requester / each node | on request; to each node every 50 ms while playing or looping and at once when the state, speed or BPM changes | layer state, sync master, beat in the bar, track length and position in ms, speed (32768 = 100 %), beat number, BPM x 100 at the current pitch, rekordbox track id |
| MetaData (548 bytes) | the requester / each node | on request; to each node when the track changes | title and artist of the loaded track (as UTF-16), rekordbox track id |
| Error (30 bytes) | the requester | on a request for anything else | code 14, empty |

**Listening.** The deck opens the listener port it advertises, UDP 65023, and
reads it. A node that sends the deck its Opt-IN joins the deck's node list at
the listener port it names; a node that sends a Request joins at the address
and port it sent from, and is answered there. The list holds four nodes, a new
one replacing the oldest. Requests are answered for the deck's own layer only,
for Metrics (data type 2) and MetaData (4).

The layer state is the deck's play state where TCNet has the same state:
playing, looping, paused, stopped at the cue, cue play, cue scratch, searching
forward and back, hold. Anything else (no track, loading, spun down, ended) is
sent as 0, idle.

Not sent, because the deck has no source for them: SMPTE mode and timecode,
on-air (the deck cannot see the mixer's fader), the pitch bend, the key, and
the beat grid, cue, waveform and mixer data a receiver can
request. The node count in the Opt-IN stays 0. The deck hears only what is
sent to its listener port, not the Opt-INs other nodes broadcast to 60000.

## In the firmware

Three MAIN patches, `tcnet` (Opt-IN and Status, on the task that sends the
Pro DJ Link status packet), `tcnettime` (Time, on the task that recomputes the
play position from the DSP's position frames, right after it does) and
`tcnetdata` (the listener, on the same task, after `tcnettime`):
`python mods/patch_update.py C2KNXS2.UPD out.UPD tcnet` writes all three into a
real update (firmware 1.87). The broadcasts go through the same firmware
routine as the Pro DJ Link beat packet, from MAIN's Pro DJ Link socket; the
listener is a second socket of the firmware's own UDP layer, opened and polled
with the firmware's calls, and sends from port 65023. It is a firmware patch
like the other [mods](../../README.md#mods).

**What is verified.** The patch sites are found by signature in firmware 1.87,
and the routines are run in an SH-4 interpreter against packets decoded from
the specification's offsets (`tests/test_main_tcnet.py`). On the emulated
deck, Resolume Arena lists it as a Pioneer player and follows its time. The
listener answers Metrics and MetaData, but to the port a request came from
rather than the listener port Arena names in it, so Arena does not show the
BPM or the title yet. None of it has been tried on a real deck.

Back to the [mods](../../README.md#mods).
