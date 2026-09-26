#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Turn one of Pioneer's public firmware updates into the images the emulator
# reads, and prove each one against its known SHA-256. What the update is, how
# it unpacks and what comes out are in the model's profile (models/<id>.conf);
# the default model is the CDJ-2000NXS2 v1.87 (C2KNXS2.UPD).
#
# Only the update is needed, never a device dump.
#
# By default everything is written to a NEW directory and nothing in the
# repository is touched. The output mirrors the repository layout:
#
#   <outdir>/extract/<image>.bin       the verified images
#   <outdir>/extract/section*.bin ...  intermediates
#   <outdir>/log/<step>.log            each tool's own output
#
# --install then copies the verified images into the model's folder in the
# repository (extract/ for the CDJ-2000NXS2, extract/<model>/ for the others).
# It refuses if any of them already exists, unless --force is also given, and
# it installs nothing unless every image passed.
#
#   usage: scripts/firmware/prepare_firmware.sh [--model <id>] [--install [--force]] <update> [outdir]
#
#   update     the .UPD file, or for a model whose update is several files
#              (the CDJ-2000) the folder holding them
#   --model    a profile in models/ (default: $CDJ_MODEL, else cdj2000nxs2)
#   outdir     must not exist yet, or be empty; default: a new mktemp dir.
#              It may not be the repository root or lie inside its extract/,
#              fw/ or notes/ -- use --install for that.
#   PYTHON=    interpreter to use (default: the one running the launcher).
#              Only the standard library is needed.
#
# Exit status: 0 when every image matches, 1 on any mismatch or error.
#
# The work is done by launcher/firmware.py, which setup and the packaged
# program also call.
. "$(dirname "${BASH_SOURCE[0]}")/../cdj_python.sh"
cdj_launcher firmware "$@"
