#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# From a fresh clone to a running, customised CDJ-2000NXS2, in one command.
#
#   ./setup.sh                 walk through everything, asking as it goes
#   ./setup.sh --dry-run       show every step and command, change nothing
#
# Six steps, each one safe to re-run and each one skipped when its result is
# already there:
#
#   1 prerequisites   compilers, libraries and Python packages, with the exact
#                     pacman / apt / brew command for whatever is missing
#   2 build           QEMU 9.1.0 fetched and patched, the MAIN and display-board
#                     emulators, the DSP core library (logs in logs/)
#   3 firmware        your own C2KNXS2.UPD (v1.87) turned into the images the
#                     emulator boots, each checked against a known SHA-256
#   4 USB stick       a disk image made from a folder of your own music
#                     (a rekordbox USB export)
#   5 DSP code        one headless deck plays for a few minutes so the DSP JIT
#                     compiles its hot code into ~/c14gen; with --curated-jit,
#                     a profile-guided module built from a recording instead
#   6 your setup      one deck or two, Pro DJ Link, audio, a MIDI controller,
#                     saved to cdj.conf -- which ./start.sh then uses
#
# options:
#   --dry-run              print what would happen; run and write nothing
#   -y, --yes              never ask: take the defaults and the options below
#   --skip-build           leave step 2 out (a build tree you made yourself)
#   --rebuild              run step 2 even when the emulators are already built
#   --no-warm              leave step 5 out
#   --warm                 run step 5's warm-up even when the cache is warm
#   --curated-jit          step 5 builds the profile-guided DSP module (~1 h,
#                          ~16 GB free disk while it runs)
#   --keep-recording       keep that build's DSP recording (~10 GB) afterwards
#   --reconfigure          ask the step 6 questions again
#   --firmware <file>      the C2KNXS2.UPD to use (re-installs the images)
#   --music <folder>       the rekordbox USB export to image (re-makes the stick)
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
