# osc_beat

Sends each beat as an OSC message, so lighting desks, drum machines and
scripts can follow the deck without decoding Pro DJ Link. The message goes out
with every Pro DJ Link beat packet, as one UDP datagram broadcast to the deck's
subnet on **UDP port 50010**.

| | |
|---|---|
| knob | `CDJ_MAIN_OSCBEAT` |
| on / off | `1` / `0` |
| default | off |

**Turning it on.** `./setup.sh` asks about it in step 7 and saves the answer to
`cdj.conf`; `./setup.sh --reconfigure` asks again. For one run,
`CDJ_MAIN_OSCBEAT=1` in the environment.

## Message

| address | tags | arguments |
|---|---|---|
| `/cdj/beat` | `,iiii` | player number; beat in bar, 1 to 4; BPM x 100; pitch (`0x100000` = 0 %) |

The four values are copied from the beat packet MAIN has just built; nothing is
recomputed, and the packet carries no running beat count, so there is none here.

## On the emulator

A real deck's broadcast reaches every OSC app on its network. The emulated
decks' network is QEMU's multicast segment, which no OSC app can read, so
unwrap it on the host while the deck runs:

```sh
python3 scripts/net/osc_relay.py                                  # to 127.0.0.1:50010
python3 scripts/net/osc_relay.py 239.77.77.1:45000 192.168.1.20:50010
```

## In the firmware

One MAIN patch, hooked at the send of the Pro DJ Link beat packet, sending
through MAIN's own network send routine. `python mods/patch_update.py
C2KNXS2.UPD out.UPD oscbeat` writes it into a real update (firmware 1.87).

**What is verified.** Checked live in the emulator: one matching message per
beat, with the deck's beat timing unchanged. Not tried on a real deck.

Back to the [mods](../README.md#mods).
