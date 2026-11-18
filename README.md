<div align="center">

# Pioneer CDJ & XDJ Emulator

**Original firmware. Emulated hardware.**

Pioneer DJ players on your computer, from the CDJ-2000 to the NXS2 and XDJ family.

[![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)
[![Platform: Windows | Linux | macOS](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)](#quick-start)
[![QEMU 11.1](https://img.shields.io/badge/QEMU-11.1-orange.svg)](https://www.qemu.org/)
[![Firmware not included](https://img.shields.io/badge/firmware-not%20included-red.svg)](#not-included)

[Supported players](#supported-players) · [CDJ-850](#cdj-850) · [Quick start](#quick-start) · [Documentation](#documentation) · [Mods](docs/mods.md) · [How it works](docs/how-it-works.md)

<table>
  <tr>
    <td align="center"><a href="docs/img/decks/cdj2000nxs2.png"><img src="docs/img/decks/cdj2000nxs2.png" alt="CDJ-2000NXS2 screen playing a track" height="180"></a><br><sub>CDJ-2000NXS2</sub></td>
    <td align="center"><a href="docs/img/decks/xdj1000.png"><img src="docs/img/decks/xdj1000.png" alt="XDJ-1000 screen playing a track" height="180"></a><br><sub>XDJ-1000</sub></td>
    <td align="center"><a href="docs/img/decks/xdj700.png"><img src="docs/img/decks/xdj700.png" alt="XDJ-700 screen playing a track" height="180"></a><br><sub>XDJ-700</sub></td>
  </tr>
  <tr>
    <td align="center"><a href="docs/img/decks/cdj900nxs.png"><img src="docs/img/decks/cdj900nxs.png" alt="CDJ-900NXS screen playing a track" height="180"></a><br><sub>CDJ-900NXS</sub></td>
    <td align="center"><a href="docs/img/decks/cdj2000nxs.png"><img src="docs/img/decks/cdj2000nxs.png" alt="CDJ-2000NXS screen playing a track" height="180"></a><br><sub>CDJ-2000NXS</sub></td>
    <td align="center"><a href="docs/img/decks/cdj2000.png"><img src="docs/img/decks/cdj2000.png" alt="CDJ-2000 screen playing a track" height="180"></a><br><sub>CDJ-2000</sub></td>
  </tr>
</table>

<sub>Screens captured from the emulator. Select one for full size.</sub>

</div>

The **CDJ-2000NXS2** boots, mounts a virtual USB stick, and loads and plays tracks from it. The waveform
scrolls, the time counts down, and the sound you hear is computed by the
player's own DSP program on an emulated DSP. Start two and they find each other
on an emulated Pro DJ Link network, where MASTER and SYNC work between them.
Plug in a MIDI controller and it plays them, or use the [virtual deck](docs/using-the-deck.md#virtual-deck).

Seven older players have experimental emulation. The **CDJ-850** is in
active bring-up in this fork: it boots through its flash bootloader and RTOS,
browses and loads from USB, supports CUE, loops, memory cues, pitch fader and
jog bend/scratch, renders the reconstructed DATA FL display, supports live
save/restore and mod patching (`tempo_wide`, `dsp_probe`), and runs its own
DSP56371 program (exact offline audio under `-icount`; host-dependent in real
time — see [CDJ-850](#cdj-850)). The [status table](#supported-players)
separates usable features from ongoing work.

> This is a fork of [Stijn-Jacobs/cdj-nxs2-qemu](https://github.com/Stijn-Jacobs/cdj-nxs2-qemu)
> focused on the CDJ-850. Everything about the other players comes from upstream.

## Features

These are the CDJ-2000NXS2's; the older players are covered under
[Supported players](#supported-players).

| | |
|---|---|
| 🎛️ **The real firmware** | All three processors of the player run its own code: MAIN, the display board and the audio DSP. |
| 🔊 **The DSP's own sound** | A C66x emulator with a JIT runs the DSP program MAIN uploads at boot; the audio you hear is its output. |
| 📈 **Load and play** | A track from the stick loads and plays; the waveform and playhead move, the time counts down. |
| 🔗 **Pro DJ Link** | Two decks take their own addresses from a small DHCP server, announce themselves, and MASTER and SYNC work between them. |
| 🎚️ **Any MIDI controller** | A profile of what the controller sends plus a mapping to CDJ keys. A Roland DJ-202 mapping is included, with its LEDs mirroring the player's lamps. |
| 🌀 **Jog and touch** | The jog bends the DSP's playback speed through the firmware's own jog engine; a click in the window is a touch on the screen. |
| 🧪 **Built for modding** | Trace any firmware address, watch memory, change it while it runs, call firmware functions, script every input. |

<a id="screenshots"></a>

## Screenshots

<p align="center">
<img src="docs/img/app-screen.gif" alt="The CDJ-2000NXS2's own screen, emulated: the colour waveform scrolling and the time counting down while a track plays" width="720">
</p>

<p align="center">
<a href="docs/img/hero.png"><img src="docs/img/hero.png" alt="Two emulated CDJ-2000NXS2s: deck 1 in SYNC follows master deck 2 to 193.7 BPM" width="900"></a>
</p>

<details>
<summary><b>More Pro DJ Link screens</b></summary>

<table>
  <tr>
    <td align="center"><img src="docs/img/two-decks.png" alt="Deck 2 showing player 1's beat" width="400"><br><sub>Deck 2 tracks the master's beat over Pro DJ Link</sub></td>
    <td align="center"><img src="docs/img/waveform.png" alt="MASTER TEMPO and SYNC on" width="400"><br><sub>MASTER TEMPO and SYNC, colour waveform</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/img/browse.png" alt="Browsing player 1's USB from deck 2" width="400"><br><sub>Browsing the other player's USB over the link</sub></td>
    <td align="center"><img src="docs/img/link-load.png" alt="A track loaded from player 1" width="400"><br><sub>...and playing a track loaded from it</sub></td>
  </tr>
</table>

</details>

<a id="supported-players"></a>

## Supported players

The CDJ-2000NXS2 is the fully supported player; the older models are
experimental. Sound is on by default (`NOSOUND=1` turns it off). Screenshots
show the firmware interface, not a claim of feature parity.

| player | status | what works |
|---|---|---|
| **CDJ-2000NXS2** | ✅ Fully supported | Plays tracks from USB in real time with sound. Pro DJ Link, MIDI controllers and [mods](docs/mods.md). |
| **XDJ-1000MK2** | 🧪 Experimental | Loads and plays from USB with the DSP module setup builds; an MP3 breaks up about every 6 s and MASTER TEMPO runs below real time. PLAY, CUE and touch work. Two decks link. |
| **XDJ-700** | 🧪 Experimental | Loads and plays from USB with the DSP module setup builds; an MP3 breaks up about every 6 s and MASTER TEMPO runs below real time. Touch works. |
| **XDJ-1000** | 🧪 Experimental | Loads and plays from USB in real time with the DSP module setup builds. MASTER TEMPO runs below real time. PLAY, CUE, tempo, jog and touch work. |
| **CDJ-900NXS** | 🧪 Experimental | Loads and plays from USB with the DSP module setup builds, in real time until MASTER TEMPO is switched on. |
| **CDJ-2000NXS** | 🧪 Experimental | Loads and plays from USB in real time with the DSP module setup builds. PLAY, CUE, tempo and jog work. Two decks link. |
| **CDJ-900** | 🧪 Experimental | Loads and plays from USB; its display shows the overview, playhead and time. |
| **CDJ-2000** | 🧪 Experimental | Loads and plays from USB in real time with the DSP module setup builds. PLAY, CUE, tempo and jog work. Two decks link. |
| **CDJ-850** | 🧪 Experimental (bring-up) | Boots the real firmware through flash (`PERSIST=1` keeps settings), browses/loads from USB, plays with DSP56371 audio (exact under `-icount` with `CDJ850_DSP_IPS=2048`; real-time `CDJ850_DSP_IPS=750` depends on host speed). CUE, loops, memory cues, pitch fader, jog bend/scratch, `savevm`/snapshot restore, desktop face/keyboard input, DATA FL display reconstruction, and mod framework integration work. See [CDJ-850](#cdj-850). |

<!-- feature-matrix:start -->
<details>
<summary>Feature matrix per player</summary>

✅ works, measured &nbsp; ⚠️ partly, see the note &nbsp; ❌ does not work &nbsp; ➖ the player does not have it &nbsp; · not tested yet

| | CDJ-2000NXS2 | XDJ-1000MK2 | XDJ-700 | XDJ-1000 | CDJ-900NXS | CDJ-2000NXS | CDJ-900 | CDJ-2000 | CDJ-850 |
|---|---|---|---|---|---|---|---|---|---|
| **Media** | | | | | | | | |
| USB | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| **Playback** | | | | | | | | |
| Sound | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ❌ | ✅ | ⚠️<sup>10</sup> |
| Real time with the setup DSP module | ✅ | ✅ | ⚠️<sup>1</sup> | ✅ | ⚠️<sup>2</sup> | ✅ | · | ✅ | ❌<sup>10</sup> |
| **Display** | | | | | | | | |
| Waveform / overview | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️<sup>11</sup> |
| Tempo and BPM readout | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ⚠️<sup>12</sup> |
| **Transport** | | | | | | | | |
| PLAY / pause | ✅ | ✅ | ⚠️<sup>3</sup> | ✅ | ⚠️<sup>3</sup> | ✅ | ✅ | ✅ | ✅ |
| CUE | ✅ | ✅ | · | ✅ | · | ✅ | · | ✅ | ✅ |
| **Tempo** | | | | | | | | |
| Tempo fader | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | · | ✅ | ✅ |
| MASTER TEMPO | ⚠️<sup>4</sup> | ⚠️<sup>5</sup> | ⚠️<sup>5</sup> | ⚠️<sup>5</sup> | ⚠️<sup>5</sup> | · | · | · | ⚠️<sup>13</sup> |
| **Jog** | | | | | | | | |
| Pitch bend | ✅ | · | · | ✅ | · | ✅ | · | ✅ | ✅ |
| Vinyl / scratch | ⚠️<sup>6</sup> | ✅ | ✅ | ✅ | ✅ | ✅ | · | ✅ | ✅ |
| Needle search | ✅ | · | · | · | ➖ | · | ➖ | · | ➖ |
| **Performance** | | | | | | | | |
| Hot cues | ✅ | · | · | · | ➖ | · | ➖ | · | ➖ |
| Loops | ⚠️<sup>7</sup> | · | · | · | · | · | · | · | ✅ |
| Beat jump | · | · | · | · | ➖ | ➖ | ➖ | ➖ | ➖ |
| Slip | ⚠️<sup>8</sup> | · | · | · | · | · | · | ➖ | ➖ |
| **Interface** | | | | | | | | |
| Touch screen | ✅ | ✅ | ✅ | ✅ | ➖ | ➖ | ➖ | ➖ | ➖ |
| **Pro DJ Link** | | | | | | | | |
| Link with other players | ✅ | ✅ | · | · | · | ✅ | · | ✅ | ➖<sup>9</sup> |

Firmware: CDJ-2000NXS2 1.87, XDJ-1000MK2 1.45, XDJ-700 1.15, XDJ-1000 1.13, CDJ-900NXS 1.31, CDJ-2000NXS 1.44, CDJ-900 4.32, CDJ-2000 4.33, CDJ-850 1.13.

1. Real time in rate, but an MP3 underruns about every 6 s; a WAV plays clean.
2. Real time until MASTER TEMPO is switched on; no run of its own yet.
3. Only one run so far.
4. Sound can drop out after switching it on.
5. Runs at 66-94 % of real time with it on, so the sound breaks up.
6. VINYL scratching not tested yet.
7. Loops engage in only some runs.
8. The slipped playback is not checked yet.
9. The CDJ-850 has no Ethernet port.
10. The DSP output is correct (checked under `-icount` with `CDJ850_DSP_IPS=2048`). Real-time playback (`CDJ850_DSP_IPS=750`, ~66.2 MIPS) runs the DSP on a worker thread; the predecoded interpreter reaches ~95 MIPS on unthrottled CPUs (~53 MIPS at a 1.9 GHz thermal cap), while default `CDJ850_DSP_IPS=256` avoids host overload.
11. The playing overview, artist line, cue/memory strips, and cue/memory lamps are verified both in SCIF1 replay (`test_cdj850_scif1_replay.py`) and live rekordbox export playback (`test_cdj850_usb.py`); scrolling cadence, panel-side blinks, and unobserved glyphs still need hardware/panel-CPU evidence.
12. Tempo pitch percentage and range (`±6`/`±10`/`±16`/`WIDE`) and whole-BPM decoding/rendering are verified live against SCIF1 replay (`test_cdj850_usb.py`).
13. MASTER TEMPO key toggle and lamp are tested live (`test_cdj850_usb.py`); audio throughput with MASTER TEMPO active is not measured yet.

</details>
<!-- feature-matrix:end -->

<a id="cdj-850"></a>

## CDJ-850

The CDJ-850 (firmware 1.13, `C850MAIN.UPD`) is a different machine from the
rest: an SH7263 (SH-2A) MAIN CPU, a Freescale DSP56371 for audio, and a
separate panel CPU that drives the FL displays. The emulator runs MAIN's own
code and the DSP program MAIN uploads at boot, on a DSP56300 core written for
this project (`hw/cdj/boards/cdj850/dsp56300.c`).

**What works, measured:**

- Boot through the bootloader in flash, RTOS start, panel link and DSP upload;
  service USB updater tested against flash `RY/BY#` (`PC2`).
- USB stick: mount, browse, move the cursor and load a track.
- USB-B (`PC` mode): R8A66597 function controller switched via `PB10` (`USB_ABSEL`) mux (`CDJ_USBF_HOST`), enumerating PID `0x08e4:0x0157` with MIDI + HID reports and mode-switch round-trip (`test_cdj850_usbb.py`).
- Transport and deck controls: PLAY/pause (VINYL brake & spin-up, CDJ pause loop),
  CUE, LOOP IN / OUT / RELOOP, MEMORY / CALL / DELETE, pitch fader (`±6`/`±10`/`±16`/`WIDE`),
  CDJ jog pitch bend, and VINYL top touch + scratch direction.
- Live input and virtual deck: host keyboard shortcuts, desktop app face profile
  ([`app/faces/cdj850.json`](app/faces/cdj850.json)), lamp TX over `cdj_pnl_stateout`,
  live UDP control socket (`CDJ_PANEL_KEYSOCK`), and scripted `CDJ850_PANEL_SCRIPT`
  events (`pitch=`, `fader=`, `jog`, `vinyl=`).
- The DATA FL display reconstructs the glass from hardware photos and SCIF1 captures:
  3-line browser text and selection tags, track number, elapsed/remaining time,
  pitch % / range, BPM, playing overview/strips, and panel lamps (`MASTER TEMPO`,
  `A.CUE`, `INFO`, `BROWSE`, `TAG LIST`, `MENU`, `MEMORY`, `CUE`).
- The desktop jog display follows the firmware's playback position, cue marker,
  audio-memory indicator and VINYL status. Drag or scroll the VINYL SPEED
  ADJUST knob to change the firmware's brake and release ramps.
- PLAY/PAUSE, CUE, and the manual and automatic loop buttons follow the lamp
  states in the firmware's second SCIF1 status page, including flashes.
- USB, DISC, PC and the red USB activity light use the same firmware status.
- Press a deck button to wake from auto standby. The first press wakes the deck,
  and subsequent presses operate the controls as usual.
- Save/restore and persistence: QMP `savevm`/`migrate` snapshot restore with USB
  storage attached (`usb`, `intc`, `dsp_shi`, `vfd`, `panel_peer`, `dsp56300`), and
  `PERSIST=1` per-deck flash copies preserving the `0x3F8000` user settings sector.
- Firmware modding and repacking: `C850MAIN.UPD` repacker (`scripts/firmware/c850_repack.py`),
  SH-2A big-endian `bootcrit` / `validate.py`, `patch_main.py --flash` / `patch_update.py --model cdj850`,
  4-stage DSP56371 SHI segment patching and boot check (`DspShiProfile`), and initial mods
  ([`mods/tempo_wide`](mods/tempo_wide/mod.conf), [`mods/dsp_probe`](mods/dsp_probe/mod.conf)).

**Not yet:** reliable real-time audio on thermally throttled laptops (`CDJ850_DSP_IPS`
defaults to `256`; set `CDJ850_DSP_IPS=750` on fast/unthrottled hosts or use `-icount` + `2048`
for exact offline audio), remaining unmapped panel/metadata fields,
Panel CPU firmware extraction (panel side is emulated as a behavioral peer), the CD servo,
and testing packed updates on physical hardware.

**Run it:**

```sh
./setup.sh --model cdj850 --firmware path/to/C850MAIN.UPD
./start.sh --model cdj850 --app
```

Keep the emulated flash (including user settings at `0x3F8000`) across launches with
`PERSIST=1 ./start.sh --model cdj850`. Drive the panel interactively with the QEMU
window keyboard or virtual deck app (`CDJ_PANEL_KEYSOCK`), or script events in virtual
milliseconds with `CDJ850_PANEL_SCRIPT`:

```sh
CDJ850_PANEL_SCRIPT=6000:usb,9000:enter,11000:rot+1,13000:enter,19000:play,24000:play \
  ./start.sh --model cdj850
```

Pitch and jog script actions accept `<ms>:fader=<0-1023>` (`512` centre),
`<ms>:pitch=<0x0000-0xffff>` / `pitch+/-N`, `<ms>:jog+/-N[:period]` / `jog=0`, and
`<ms>:vinyl=<0-255>`. Button names include `play`, `cue`, `usb`, `enter`, `back`,
`browse`, `vinyl`, `master_tempo`, `tempo`, `time`, `eject`, the loop and beat buttons,
and the track/search keys; the full list is in `hw/cdj/boards/cdj850/xc/panel_peer.c`.

To patch a `C850MAIN.UPD` update with CDJ-850 mods and run static/boot-critical checks:

```sh
python3 mods/patch_update.py --model cdj850 path/to/C850MAIN.UPD C850MAIN_MOD.UPD tempo_wide dsp_probe
python3 mods/validate.py --model cdj850 --stock path/to/C850MAIN.UPD C850MAIN_MOD.UPD
```

**For firmware modding**, two things make runs repeatable:

- Run under `-icount shift=2,sleep=off`. Without it the virtual clock follows
  the host, so a busy host shifts when things happen (the USB mount moved from
  2.9 s to 26.7 s in testing). Under icount the mount lands at 2875 ms every
  time. `start.sh` does not add it; launch `qemu-system-sh4eb` the way
  `tests/test_cdj850_usb.py` does.
- With icount, `CDJ850_DSP_IPS=2048` gives the DSP enough instructions per
  sample to produce the exact audio. `CDJ850_DSP_DEBUG=1` prints the DSP's
  state every 500 ms of virtual time.

**Tests:** `python3 -m pytest tests/test_cdj850_*.py`. The CDJ-850 suite covers
unit/C-harness tests (`panel_peer`, `panel_link`, `vfd`, `vfd_render`, `scif1_replay`,
`dsp56300`, `c850_repack`) alongside live QEMU `-icount` end-to-end tests (`usb`,
`usbb`, `controls`, `snapshot`, `mods`). Tests that need `C850MAIN.UPD` or a built emulator
skip without them.

The bring-up log, with every address, decoded handler and environment knob, is
[`notes/cdj850-bringup.md`](notes/cdj850-bringup.md).

<a id="not-included"></a>

## Nothing from Pioneer DJ is included

> **This repository distributes no firmware, no ROM or flash images, no fonts,
> no artwork files and no manuals or schematics.** Everything here was written
> for this project. To run it you supply your own copy of the public
> firmware update file for the player, which the setup unpacks on your machine, into a folder
> that is never committed.
>
> The screenshots in this README and in `docs/` show the emulated firmware's own interface, running in
> this emulator, for illustration.
>
> This project is not affiliated with, endorsed by or supported by Pioneer DJ or
> AlphaTheta. Pioneer DJ, CDJ, rekordbox and PRO DJ LINK are trademarks of their
> respective owners.

<a id="quick-start"></a>

## Quick start

**You will need** a recent multi-core CPU (see [Limits](#limits)), about 3 GB of
disk for the build trees and the DSP module, Python 3.11 or newer, the update
file for your player, and your own music. For the CDJ-2000NXS2 that is
`C2KNXS2.UPD` **version 1.87**; every address in the model is for that version.

**1. Get a shell for your system** (only your own line applies):

| system | what to do first |
|---|---|
| **Windows** | Install [MSYS2](https://www.msys2.org) and open the **MSYS2 MINGW64** shell. `setup.sh` tells you which packages to add. |
| **Linux** (native or WSL) | Nothing. Any shell will do; `setup.sh` prints the `apt` command for anything missing. |
| **macOS** (Apple silicon or Intel) | Install the Xcode Command Line Tools and [Homebrew](https://brew.sh), then the build tools and a newer bash (macOS's own is 3.2; the scripts find Homebrew's by themselves): `xcode-select --install` then `brew install bash ninja meson pkgconf glib pixman python` |

**2. Clone and run setup** (the same on every system):

```sh
git clone https://github.com/FruityLemonade/cdj-850-qemu.git cdj-nxs2
cd cdj-nxs2
./setup.sh
```

`setup.sh` walks you through seven steps: prerequisites, the build (10–40
minutes), your update file, the USB stick (a rekordbox export or a plain folder
of music), the [DSP module](docs/dsp-module.md), your setup and the
[mods](docs/mods.md). Every one is safe to re-run and is skipped when it is
already done. [Setup in detail](docs/setup.md) has each step, every option and
the notes for macOS.

**3. Play:**

```sh
./start.sh          # Ctrl-C stops everything
```

The deck window opens and the CDJ-2000NXS2 boots to its screen. Click the window,
press `U` for the USB stick, `↓` to a track, `Enter` to load it and `Space` to
play (every key: [Keyboard](docs/using-the-deck.md#keyboard)). Click the screen to touch it.

**Updating:** `git pull`, then `./start.sh` as usual. The emulator is compiled,
so when a pull changed its code `start.sh` notices and offers to rebuild
(`./build.sh main display` does the same by hand). A pull that changed the DSP
module's generator also needs the module built again (see [DSP module](docs/dsp-module.md)).

**Another player:** `./setup.sh --model <id> --firmware <update file>`, then
`./start.sh`; [Other players](docs/setup.md#other-players) lists the ids and
files.

<a id="limits"></a>

## Limits

- **Speed depends on your CPU.** One deck runs in real time on a fast desktop.
  Two decks need roughly twice that, and on a busy or modest machine they fall
  behind real time (the audio then has gaps).
- **MASTER TEMPO is heavy.** It makes the DSP program do far more work per
  sample, and it is currently the most demanding thing you can ask of the
  emulator.
- **Only firmware v1.87** is supported for the CDJ-2000NXS2.
- **Work in progress.** The older players are experimental (see
  [Supported players](#supported-players)). On the NXS2 the USB stick is the
  only medium so far. Some panel keys are decoded by the firmware but have not
  been tried here; the controller tools say so when you bind one.

## Documentation

The pages describe the CDJ-2000NXS2 unless they say otherwise.

- [Setup in detail](docs/setup.md): every setup step and option, macOS, the
  older players, another firmware version or a modded update
- [Using the deck](docs/using-the-deck.md): the keyboard, the virtual deck app
  and its web page, service mode
- [MIDI controllers](midi/README.md): using a controller, learning a new one,
  the DJ-202 mapping and its LEDs, the deck's own USB-MIDI port
- [Pro DJ Link](docs/pro-dj-link.md): two decks, and rekordbox and Ableton Live
  on this PC
- [Mods](docs/mods.md): what each one does, patching and checking a real
  firmware update, adding a mod
- [HID mode](docs/hid-mode.md): the deck as a HID controller over its USB-B port
- [The DSP module](docs/dsp-module.md): the native module setup builds, and
  when it needs building again
- [How it works](docs/how-it-works.md): the three processors, a platform for
  modding the firmware, booting from the flash, starting from a saved deck,
  the repository layout
- [Model profiles](models/README.md): one file per player, and adding one

## Licence

This project's own files are licensed under the **GNU General Public License,
version 2 or (at your option) any later version** (`GPL-2.0-or-later`); the
full text is in [`LICENSE`](LICENSE), and each file carries an
`SPDX-License-Identifier` line.

<details>
<summary><b>Third-party files and binaries</b></summary>

- The C66x instruction decode tables in `hw/cdj/c6x/binutils/` are copied from
  GNU binutils and licensed **GPL-3.0-or-later**. They are compiled into the
  MAIN emulator, so a binary built from this tree is distributed under
  GPL-3.0-or-later terms.
- `hw/cdj/boards/nxs2/standin/minimp3.h` is minimp3, released into the public domain (CC0).

QEMU itself is GPL-2.0-or-later overall, with a few GPL-2.0-only files; if you
distribute a binary, check how those parts combine with GPL-3.0. This note
describes the files' licences; it is not legal advice.

</details>

## Credits

[QEMU](https://www.qemu.org/), which this machine plugs into; GNU binutils, for
the TI C6x opcode tables; [minimp3](https://github.com/lieff/minimp3);
[baken](https://github.com/M-Igashi/baken) (MIT), which writes the USB export
for the folder-of-music setup step.
