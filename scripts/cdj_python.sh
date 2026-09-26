# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced, not run, by setup.sh and start.sh: runs the launcher (launcher/,
# plain Python 3.8+, standard library only) with the entry point's arguments.
# Kept to bash 3.2, which is macOS's /bin/bash.
#
#   cdj_launcher <command> "$@"
#
# CDJ_PYTHON names the interpreter; otherwise the first of python3 and python
# that actually runs (on Windows a bare python3 can be the Microsoft Store stub,
# which exists on PATH but runs nothing).
cdj_launcher() {
    _cdj_emu="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
    # A native Windows Python under MSYS2's terminal sees pipes, not a
    # terminal, so it is told what this shell sees: whether to ask questions,
    # draw the spinner, and how wide.
    LAUNCHER_TTY_IN=0; [ -t 0 ] && LAUNCHER_TTY_IN=1
    LAUNCHER_TTY_OUT=0; [ -t 1 ] && LAUNCHER_TTY_OUT=1 && COLUMNS="${COLUMNS:-$(tput cols 2>/dev/null)}"
    export LAUNCHER_TTY_IN LAUNCHER_TTY_OUT COLUMNS
    for _cdj_py in "${CDJ_PYTHON:-}" python3 python; do
        [ -n "$_cdj_py" ] || continue
        if "$_cdj_py" -c 'import sys; sys.exit(sys.version_info < (3, 8))' >/dev/null 2>&1; then
            LAUNCHER_PROG="$0" PYTHONPATH="$_cdj_emu${PYTHONPATH:+:$PYTHONPATH}" exec "$_cdj_py" -m launcher "$@"
        fi
    done
    echo "no Python 3.8 or newer found (python3 or python); install one, or name it in CDJ_PYTHON=" >&2
    exit 1
}

# For the scripts under scripts/run/: bash stays the parent instead of exec'ing,
# because callers find and stop a run by its command line
# (pgrep -f 'run/rig.sh <tag> '). INT or TERM here becomes a stop request the
# launcher (and every script it starts) sees in LAUNCHER_STOP_FILE, so the decks
# are shut down in order -- MAIN through its monitor -- rather than orphaned.
cdj_launcher_script() {
    local rc
    if [ -z "${LAUNCHER_STOP_FILE:-}" ]; then
        LAUNCHER_STOP_FILE="/tmp/cdj-stop-$$"
        rm -f "$LAUNCHER_STOP_FILE"
        trap 'rm -f "$LAUNCHER_STOP_FILE"' EXIT
        export LAUNCHER_STOP_FILE
    fi
    ( cdj_launcher run "$@" ) &
    _cdj_child=$!
    trap ': > "$LAUNCHER_STOP_FILE"' INT TERM
    wait "$_cdj_child"; rc=$?
    # A signal ends the first wait early; the launcher is still stopping.
    while kill -0 "$_cdj_child" 2>/dev/null; do wait "$_cdj_child"; rc=$?; done
    return "$rc"
}
