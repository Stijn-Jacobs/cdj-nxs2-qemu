#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Per run: the real DSP's exit oracles (boot, link, MAIN's view of it, the
# DSP-side struct fields) and the E-8302 / E-7010 counts in MAIN's firmware log.
# The error codes are matched with their "E-" prefix: a bare 7010 also matches
# timestamps such as "37010 ms".
#   usage: bash scripts/run/report_dsp.sh <tag-prefix> <n>
set -uo pipefail
P="${1:?usage: report_dsp.sh <prefix> <n>}"
N="${2:?}"
for i in $(seq 1 "$N"); do
    log="/tmp/bridge-main-$P$i.log"
    [ -f "$log" ] || { echo "  $P$i  (log missing)"; continue; }
    echo "  $P$i  E-8302=$(grep -ac 'E-8302' "$log") E-7010=$(grep -ac 'E-7010' "$log") arm-busy=$(grep -ac 'arm reply.*BUSY' "$log") arm-ok=$(grep -ac 'arm reply.*armed' "$log")"
    grep -a 'c6x\[exit\]: MAIN:\|c6x\[exit\]: DSP struct\|c6x\[exit\]: reply ids\|DSP thread\|c6x\[exit\]: [0-9.]* s run\|halt\]' "$log" \
        | sed 's/^qemu-system-sh4: info: /      /' | cut -c1-300
done
