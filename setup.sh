#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# From a fresh clone to a running, customised CDJ-2000NXS2, in one command.
#
#   ./setup.sh                 walk through everything, asking as it goes
#   ./setup.sh --dry-run       show every step and command, change nothing
#
# Seven steps, each one safe to re-run and each one skipped when its result is
# already there:
#
#   1 prerequisites   compilers, libraries and Python packages, with the exact
#                     pacman / apt / brew command for whatever is missing
#   2 build           QEMU 9.1.0 fetched and patched, the MAIN and display-board
#                     emulators, the DSP core library (logs in logs/)
#   3 firmware        your own C2KNXS2.UPD (v1.87) turned into the images the
#                     emulator boots, each checked against a known SHA-256
#   4 USB stick       a disk image made from a folder of your own music: either
#                     a rekordbox USB export, or a plain folder of music files
#                     that baken (github.com/M-Igashi/baken, MIT) analyses --
#                     no rekordbox needed
#   5 DSP module      the DSP's hot code compiled to a native module in ~/c14gen
#                     from two 30 s plays of your own firmware (20-40 min; asked,
#                     or skipped with --skip-dsp). Built again when it was made
#                     from other code
#   6 your setup      one deck or two, Pro DJ Link, audio, a MIDI controller,
#                     saved to cdj.conf -- which ./start.sh then uses
#   7 mods            small on/off tweaks to how the deck behaves (see
#                     mods/mods.conf), all defaults in one answer or one
#                     at a time, saved alongside your setup
#
# options:
#   --dry-run              print what would happen; run and write nothing
#   -y, --yes              never ask: take the defaults and the options below
#   --skip-build           leave step 2 out (a build tree you made yourself)
#   --rebuild              run step 2 even when the emulators are already built
#   --skip-dsp             leave step 5 out (./start.sh says how to build the module later)
#   --reconfigure          ask the step 6 and 7 questions again
#   --model <id>           the player: cdj2000nxs2 (default) or an older one in models/;
#                          asked when not given. An older player gets steps 1-4
#                          and a one-window start; the ones with a DSP profile also
#                          get step 5, a DSP module
#   --firmware <file>      your update file for it (C2KNXS2.UPD for the CDJ-2000NXS2)
#   --music <folder>       the rekordbox USB export to image (re-makes the stick)
#   --tracks <folder>      a plain folder of music to image instead, analysed by baken
#   --decks 1|2            --name <deck name>    --djlink on|off   --audio on|off
#   --controller none|<profile>|learn            --relay-port <port>
#   --build-dir <dir>      where the two QEMU build trees go
#   -h, --help
#
# Nothing from Pioneer DJ / AlphaTheta is in this repository: the firmware file
# and the music are yours, and they stay in extract/ on your machine.
#
# The work is done by launcher/setup.py (python -m launcher setup), the same
# code the packaged program runs.
. "$(dirname "${BASH_SOURCE[0]}")/scripts/cdj_python.sh"
cdj_launcher setup "$@"
