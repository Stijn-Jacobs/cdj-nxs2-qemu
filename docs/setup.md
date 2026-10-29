# Setup in detail

The [quick start](../README.md#quick-start) covers the common path. This page has
every step of `./setup.sh`, its options, the older players and other firmware
versions.

## The setup steps

`setup.sh` walks you through seven steps. Every one is safe to re-run and is
skipped when it is already done:

1. **Prerequisites**: checks compilers, libraries and Python packages, and
   prints the exact `pacman`, `apt` or `brew` command for whatever is missing.
2. **Build**: fetches QEMU 11.1.2, applies this project's patches and builds
   the MAIN emulator, the display-board emulator and the DSP library, with a
   progress line per phase and full logs in `logs/` (10–40 minutes).
3. **Firmware**: asks for your update file (`C2KNXS2.UPD` for the CDJ-2000NXS2)
   and turns it into the images the emulator boots, checking each against a
   known SHA-256.
4. **USB stick**: either a folder of your own music exported by rekordbox
   (the folder that holds `PIONEER/`), or a plain folder of music files
   (MP3, FLAC, AAC/M4A, WAV, AIFF, ALAC), which
   [baken](https://github.com/M-Igashi/baken) analyses instead, so no
   rekordbox is needed. Either way, a disk image is built from the result.
5. **DSP module**: asks whether to build the [DSP module](dsp-module.md) now
   (about 20-40 minutes, about 2 GB of free disk while it runs; the default is
   yes). `--skip-dsp`, or answering no, leaves it for later.
6. **Your setup**: one deck or two, Pro DJ Link, sound, a MIDI controller;
   saved to `cdj.conf`.
7. **Mods**: small on/off tweaks to how the deck behaves. Take the defaults in
   one answer or choose each (see [Mods](mods.md)); saved alongside your setup.

## On macOS

- The deck window is QEMU's native **Cocoa** window and the sound goes to the
  default output through **Core Audio**; neither needs anything from
  Homebrew. `GUI_DISPLAY=sdl` and `AUDIODEV=...` choose others, as elsewhere.
- **Pro DJ Link** runs over multicast on your network interface. The first
  time, macOS may ask whether your terminal may find devices on the local
  network: allow it, or the two decks will not see each other.
- The DSP module compiles with Apple's clang (`gcc` on macOS is clang), and
  its profile-guided build uses clang's own with Xcode's `llvm-profdata`.
- Python packages go into `.venv/` in this folder, since Homebrew's Python
  refuses `pip install` outside a virtual environment; setup offers the exact
  command. QEMU's configure needs `distlib`, which current `pip` no longer
  carries, so the build makes itself a small venv with it in the build tree.
- Over `ssh`, with no desktop session, the decks run without a window.

## Setup and start options

```text
./setup.sh --dry-run            show every step and command, change nothing
./setup.sh --reconfigure        ask the "your setup" and "mods" questions again
./setup.sh --yes                never ask; take the defaults and the options below
./setup.sh --skip-build         leave the build out (a build tree you made yourself)
./setup.sh --rebuild            build even when the emulators are already built
./setup.sh --skip-dsp           leave the DSP module out (./start.sh says how to build it later)
  --model <id>                  the player (see Other players); asked when not given
  --firmware <file>             your update file for that player (re-installs the images)
  --music <folder>              the rekordbox USB export to image
  --tracks <folder>             a plain folder of music to image instead, analysed by baken
  --decks 1|2   --name <deck name>   --djlink on|off   --audio on|off
  --controller none|<profile>|learn  --relay-port <port>
  --build-dir <dir>             where the two QEMU build trees go

./start.sh                      start what cdj.conf describes
./start.sh --model <id>         start that player
./start.sh --app | --no-app     the virtual deck app, or the plain window
./start.sh --web [lan]          the deck in a web page as well (this PC, or the LAN)
./start.sh --service | --no-service   boot into SERVICE MODE or not
./start.sh stop                 stop a running rig from another shell
./start.sh --dry-run            show what would be started
./build.sh [source|patches|main|display|dsp]   one build phase at a time
```

## Other players

The XDJ-1000MK2, XDJ-700, XDJ-1000, CDJ-900NXS, CDJ-2000NXS, CDJ-900 and
CDJ-2000 start from the same two commands with `--model`; plain `./setup.sh`
also asks which player you want (Enter keeps the CDJ-2000NXS2). Pass the
update file as `--firmware`:

| `--model` | update file | firmware |
|---|---|---|
| `xdj1000mk2` | `XDJ1KMK2.UPD` | 1.45 |
| `xdj700` | `XDJ700.UPD` | 1.15 |
| `xdj1000` | `XDJ1000.UPD` | 1.13 |
| `cdj900nxs` | `C900NXS.UPD` | 1.31 |
| `cdj2000nxs` | `C2KNXS.UPD` | 1.44 |
| `cdj900` | `C900GUI.UPD`, `C900DRIV.UPD`, `C900MAIN.UPD`, `C900PANL.UPD` | 4.32 |
| `cdj2000` | `C2KGUI.UPD`, `C2KDRIV.UPD`, `C2KMAIN.UPD`, `C2KPANL.UPD` | 4.33 |

```sh
./setup.sh --model cdj2000 --firmware path/to/C2KGUI.UPD   # the other three files beside it
./start.sh --model cdj2000
```

Setup stores the player in `cdj.conf`, so `./start.sh` alone starts it from
then on. An older player gets the build, firmware and USB steps and opens one
window with your stick; a second deck, Pro DJ Link, MIDI controllers, mods and
the virtual deck app are CDJ-2000NXS2 features and are skipped. The CDJ-2000,
CDJ-2000NXS, XDJ-1000, XDJ-1000MK2, XDJ-700 and CDJ-900NXS also get the DSP
module step. Which players exist is
whatever has a profile in `models/`.

The keys each older player has so far are listed under
[Keyboard](using-the-deck.md#keyboard).

## Another firmware version, or a modded update

Setup refuses an update file that is not the supported version (1.87 for the
CDJ-2000NXS2, the table above for the others). To try
another version, or an update that was already modded, add `--any-firmware`:

```sh
./setup.sh --firmware path/to/C2KNXS2.UPD --any-firmware
```

This is **unsupported**: setup unpacks the file anyway (a file it cannot unpack
still fails) and prints which image differed; `./start.sh` sees the same from
the installed images, says so and lists what it left out. Whether the player boots depends on
how far the firmware moved from the supported one: on the CDJ-2000NXS2, 1.85
loads a track, while 1.83 boots to the player screen but did not load one.

What the emulator pokes into the firmware is looked up in the installed MAIN
image by the instructions around each place, not by address, so it follows the
firmware when it moves: the load poke, the Pro DJ Link start call, the
firmware's own MAC, the colour capability byte, the deck-ready word that the
auto-load waits for, and the place the RTOS keeps its status register (seeded
at power-on). The mods find their sites the same way. A site that is not found
exactly once is not guessed: the poke that needs it is left out and the list at
start names it.

Some things have no way to be looked up and are switched off for any other
firmware, and the start-up list says so: the log and trace taps and the play-key
holds of the lab runs, the native `memcmp`/`strlen`/`memcpy` speed-ups (the
machine runs the firmware's own code, which is slower), the display board's
idle-loop halt when the display firmware differs, and on the older players
the DSP idle-loop skip and the status-register seed (and the DSP module is not
built). The DSP module of the CDJ-2000NXS2 is built from the installed
firmware itself, so it follows.
