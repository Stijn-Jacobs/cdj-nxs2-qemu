# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced, not run, first thing by the entry points (setup.sh, start.sh,
# build.sh), with their own arguments still in "$@".
#
# The scripts need bash 4 or newer: bash 3.2 treats "${array[@]}" of an empty
# array as unbound under set -u, and has no mapfile. macOS ships 3.2 as
# /bin/bash, so there the entry point re-runs itself under Homebrew's bash, and
# that bash's directory goes first on PATH so every nested `bash script` and
# `#!/usr/bin/env bash` gets it too.
if [ "${BASH_VERSINFO[0]:-0}" -lt 4 ]; then
    for _cdj_bash in /opt/homebrew/bin/bash /usr/local/bin/bash; do
        if [ -x "$_cdj_bash" ] && "$_cdj_bash" -c '[ "${BASH_VERSINFO[0]}" -ge 4 ]' 2>/dev/null; then
            PATH="$(dirname "$_cdj_bash"):$PATH"
            export PATH
            exec "$_cdj_bash" "$0" "$@"
        fi
    done
    echo "these scripts need bash 4 or newer, and this is bash $BASH_VERSION." >&2
    [ "$(uname -s)" = Darwin ] && echo "install it with:  brew install bash   (then run this again)" >&2
    exit 1
fi
