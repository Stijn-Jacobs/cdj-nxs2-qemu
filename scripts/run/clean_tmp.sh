#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Reclaim scratch space in /tmp between batches. A full disk yields zero-byte
# logs that look like a dead instrument.
#   KEEP=<chars>  tag prefixes whose logs are kept (a character class; default uv)
set -uo pipefail
before=$(df -h / | awk 'NR==2{print $4}')
KEEP="${KEEP:-uv}"                    # character class: keep every prefix listed
find /tmp -maxdepth 1 -name 'bridge-main-*.log' ! -name "bridge-main-[$KEEP]*" -delete
find /tmp -maxdepth 1 -name 'bridge-gui-*.log'  ! -name "bridge-gui-[$KEEP]*"  -delete
find /tmp -maxdepth 1 -name 'ram-*.bin' -mmin +120 -delete
# RAMSNAPs live in /tmp/<tag>/; -mmin +25 keeps the current batch's.
find /tmp -mindepth 2 -maxdepth 2 -name 'ram-*.bin' -mmin +25 -delete 2>/dev/null
# Never blanket-delete /tmp/media-*: each running run has its own medium there.
# Only removed when no qemu is running.
if command -v pgrep >/dev/null 2>&1 && ! pgrep qemu >/dev/null 2>&1; then
    rm -rf /tmp/media-* 2>/dev/null || true
else
    echo "qemu is running -- leaving /tmp/media-* alone"
fi
echo "free: $before -> $(df -h / | awk 'NR==2{print $4}')"
