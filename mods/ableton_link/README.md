# ableton_link

Joins [Ableton Link](https://www.ableton.com/en/link/) as a peer: Ableton Live,
or any Link app on the deck's network, sees the deck and follows its tempo and
beat. In Live the Link toggle shows `1 Link` at the deck's BPM. rekordbox in
Performance mode lists the deck as a Link peer too and syncs to it.

| | |
|---|---|
| knobs | `CDJ_MAIN_ABLETONLINK`, `CDJ_MAIN_ABLETONLINKPONG` |
| on / off | `1` / `0` |
| default | off |

**Turning it on.** `./setup.sh` asks about it in step 7 and saves the answer to
`cdj.conf` (under `CDJ_MAIN_ABLETONLINK`; the other knob follows it).
`./setup.sh --reconfigure` asks again. For one run, set both knobs to `1` in the
environment.

**Seeing it.** On the emulator the apps have to be on the deck's network, which
on Windows means the TAP adapter: the steps, for Live and rekordbox, are in
[Connecting rekordbox and Ableton Live on this PC](../../README.md#rekordbox-ableton).

## How it works

Link needs two things from a peer, and both are patched into MAIN's existing
Pro DJ Link code, with no new task or socket:

- **Announcing.** With every Pro DJ Link beat packet the deck sends a 107-byte
  Link ALIVE datagram to UDP port 20808 on the subnet broadcast address. It
  carries a timeline re-anchored on that beat (microseconds per beat at the
  tempo being played, pitch included, and a beat origin that follows the deck's
  downbeat), the deck's session and the address of its measurement socket. The
  deck founds its own session. It therefore announces while a track is
  playing, and play/stop is not sent.
- **Answering.** An app that sees the deck measures its clock with PING
  datagrams, and drops a peer that does not answer within 50 ms. The Link
  PINGs go to UDP port 50000, the Pro DJ Link socket, where the receive task's
  hook recognises the Link header, keeps the datagram away from the Pro DJ Link
  layer and sends the PONG from the same socket.

The deck leads: apps follow its tempo, and a tempo change on the Link side
does not reach the deck.

**In the firmware.** Two MAIN patches, `abletonlink` and `abletonlinkpong`:
`python mods/patch_update.py C2KNXS2.UPD out.UPD ableton_link`
writes both into a real update (firmware 1.87). It is a firmware patch like
the other [mods](../../README.md#mods).

**What is verified.** On the emulated deck, live: one announcement per beat at
the deck's tempo, every ping answered, the deck's beat timing unchanged, Live 12
showing `1 Link` with its tempo following the deck's, and rekordbox in
Performance mode showing the deck as a Link peer and syncing. Not tried on a
real deck.

Back to the [mods](../../README.md#mods).
