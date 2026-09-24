#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Per run: how many DJcont ErrEnd lines, and did the detail waveform succeed?
#   usage: ./scripts/run/report_errors.sh <tag-prefix> <n>
set -uo pipefail
P="${1:?usage: report_errors.sh <prefix> <n>}"
N="${2:?}"
for i in $(seq 1 "$N"); do
    log="/tmp/bridge-main-$P$i.log"
    [ -f "$log" ] || { echo "  $P$i  (log missing)"; continue; }
    err=$(grep -ac 'ErrEnd' "$log" || true)
    # The success line is Shift-JIS; its ASCII tail "size=" is the safe anchor.
    wav=$(grep -ac 'size=[0-9]*,\*P=' "$log" || true)
    printf '  %-8s ErrEnd=%-4s detailWaveOK=%s\n' "$P$i" "$err" "$wav"
done
