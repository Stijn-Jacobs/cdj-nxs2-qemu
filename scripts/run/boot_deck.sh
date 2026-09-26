#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Boot one deck: the MAIN board and the GUI board joined by the SPI link, with
# no gdbstub attached (a gdbstub client halts MAIN and starves the link). The
# boards run for [seconds] and are stopped with SIGTERM so their exit counters
# print.
#
# The model (CDJ_MODEL, default cdj2000nxs2) names both machines and the
# folder its images are in. This launcher needs a GUI board; a model still in
# bring-up boots its MAIN board alone with boot_main.sh.
#
#   usage: ./scripts/run/boot_deck.sh <tag> [seconds]
#
# The work is done by launcher/boot_deck.py.
. "$(dirname "${BASH_SOURCE[0]}")/../cdj_python.sh"
cdj_launcher_script boot_deck "$@"
