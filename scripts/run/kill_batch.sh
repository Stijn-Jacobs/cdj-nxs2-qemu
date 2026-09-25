#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Kill a batch and its driver shells. Killing only QEMU leaves the driver loop
# alive to launch the next runs.
TAG="${1:?usage: kill_batch.sh <tag-prefix>}"
# -o command, not cmd: Linux's ps knows both, macOS's only command.
ps -eo pid,command \
    | grep -E "[z]92_[a-z]+\.sh $TAG|[b]oot_decks\.sh $TAG|[b]oot_deck\.sh $TAG" \
    | awk '{print $1}' | xargs -r kill -9
sleep 2
pkill -9 -f '[q]emu-system-sh4'
sleep 2
echo "remaining qemu: $(pgrep qemu-system-sh4 | wc -l | tr -d ' ')"
ps -eo pid,command | grep -E "[b]oot_decks\.sh $TAG|[b]oot_deck\.sh $TAG" | head
