#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# From a fresh clone to a running, customised CDJ-2000NXS2, in one command.
#
#   ./setup.sh                 walk through everything, asking as it goes
#   ./setup.sh --dry-run       show every step and command, change nothing
#
# Five steps, each one safe to re-run and each one skipped when its result is
# already there:
#
#   1 prerequisites   compilers, libraries and Python packages, with the exact
#                     pacman / apt command for whatever is missing
#   2 build           QEMU 9.1.0 fetched and patched, the MAIN and display-board
#                     emulators, the DSP core library (logs in logs/)
#   3 firmware        your own C2KNXS2.UPD (v1.87) turned into the images the
#                     emulator boots, each checked against a known SHA-256
#   4 USB stick       a disk image made from a folder of your own music
#                     (a rekordbox USB export)
#   5 your setup      one deck or two, Pro DJ Link, audio, a MIDI controller,
#                     saved to cdj.conf -- which ./start.sh then uses
#
# options:
#   --dry-run              print what would happen; run and write nothing
#   -y, --yes              never ask: take the defaults and the options below
#   --skip-build           leave step 2 out (a build tree you made yourself)
#   --rebuild              run step 2 even when the emulators are already built
#   --reconfigure          ask the step 5 questions again
#   --firmware <file>      the C2KNXS2.UPD to use (re-installs the images)
#   --music <folder>       the rekordbox USB export to image (re-makes the stick)
#   --decks 1|2            --name <deck name>    --djlink on|off   --audio on|off
#   --controller none|<profile>|learn            --relay-port <port>
#   --build-dir <dir>      where the two QEMU build trees go
#   -h, --help
#
# Nothing from Pioneer DJ / AlphaTheta is in this repository: the firmware file
# and the music are yours, and they stay in extract/ on your machine.
set -uo pipefail

E="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HERE="$E/scripts"
. "$HERE/cdj_paths.sh"
CONF="$E/cdj.conf"
LOGDIR="$E/logs"
EXTRACT="$CDJ_ROOT/extract"
USB_IMG="$EXTRACT/usbmedia3.img"
FIRMWARE_IMAGES="main_unpacked.bin gui_unpacked.bin flash.bin resblob.bin artblob.bin settings.bin"

# ---------------------------------------------------------------- options ----
DRY=0; YES=0; SKIP_BUILD=0; REBUILD=0; RECONFIGURE=0
OPT_FIRMWARE=""; OPT_MUSIC=""; OPT_DECKS=""; OPT_NAME=""; OPT_DJLINK=""
OPT_AUDIO=""; OPT_CONTROLLER=""; OPT_RELAY=""; OPT_BUILD_DIR=""

usage() { sed -n '/^#   \.\/setup\.sh  /,/^#   -h, --help/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [ "$#" -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1 ;;
        -y | --yes | --non-interactive) YES=1 ;;
        --skip-build) SKIP_BUILD=1 ;;
        --rebuild) REBUILD=1 ;;
        --reconfigure) RECONFIGURE=1 ;;
        --firmware) OPT_FIRMWARE="${2:?--firmware needs a file}"; shift ;;
        --music) OPT_MUSIC="${2:?--music needs a folder}"; shift ;;
        --decks) OPT_DECKS="${2:?--decks needs 1 or 2}"; shift ;;
        --name) OPT_NAME="${2:?--name needs a word}"; shift ;;
        --djlink) OPT_DJLINK="${2:?--djlink needs on or off}"; shift ;;
        --audio) OPT_AUDIO="${2:?--audio needs on or off}"; shift ;;
        --controller) OPT_CONTROLLER="${2:?--controller needs a name}"; shift ;;
        --relay-port) OPT_RELAY="${2:?--relay-port needs a number}"; shift ;;
        --build-dir) OPT_BUILD_DIR="${2:?--build-dir needs a directory}"; shift ;;
        -h | --help) usage; exit 0 ;;
        *) echo "unknown option: $1 (see ./setup.sh --help)" >&2; exit 2 ;;
    esac
    shift
done

INTERACTIVE=1
{ [ "$YES" = 1 ] || [ ! -t 0 ]; } && INTERACTIVE=0

# ----------------------------------------------------------------- output ----
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ] && [ "${TERM:-dumb}" != dumb ]; then
    B=$'\e[1m'; D=$'\e[2m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; C=$'\e[36m'; N=$'\e[0m'
    TTY=1
else
    B=""; D=""; G=""; Y=""; R=""; C=""; N=""
    TTY=0
fi
case "${LC_ALL:-${LC_CTYPE:-${LANG:-}}}" in
    *UTF-8* | *utf8* | *UTF8*) OK="✔"; BAD="✖"; WARN="!"; SPIN='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏' ;;
    *) OK="ok"; BAD="XX"; WARN="!!"; SPIN='|/-\' ;;
esac

TOTAL_STEPS=5
step() { printf '\n%s[%s/%s] %s%s\n' "$B$C" "$1" "$TOTAL_STEPS" "$2" "$N"; }
info() { printf '  %s\n' "$*"; }
good() { printf '  %s%s%s %s\n' "$G" "$OK" "$N" "$*"; }
warn() { printf '  %s%s%s %s\n' "$Y" "$WARN" "$N" "$*"; }
fail() { printf '  %s%s%s %s\n' "$R" "$BAD" "$N" "$*"; }
dim()  { printf '  %s%s%s\n' "$D" "$*" "$N"; }
die()  { printf '\n%s%s %s%s\n' "$R" "$BAD" "$*" "$N" >&2; exit 1; }
would() { printf '  %s(dry run) would run:%s %s\n' "$Y" "$N" "$*"; }

# ask "<question>" "<default>" -> the answer on stdout (the default when not asking)
ask() {
    local q="$1" def="${2:-}" a
    if [ "$INTERACTIVE" = 0 ]; then printf '%s' "$def"; return; fi
    printf '  %s%s%s%s ' "$B" "$q" "$N" "${def:+ [$def]}" >&2
    IFS= read -r a || a=""
    printf '%s' "${a:-$def}"
}
# ask_yn "<question>" y|n -> status 0 for yes
ask_yn() {
    local a
    a="$(ask "$1 (y/n)" "$2")"
    case "$a" in y | Y | yes | Yes) return 0 ;; *) return 1 ;; esac
}
# choose "<question>" "<default>" option... -> one of the options
choose() {
    local q="$1" def="$2" a; shift 2
    while :; do
        a="$(ask "$q ($*)" "$def")"
        for o in "$@"; do [ "$a" = "$o" ] && { printf '%s' "$a"; return; }; done
        [ "$INTERACTIVE" = 0 ] && { printf '%s' "$def"; return; }
        printf '  %splease answer one of: %s%s\n' "$Y" "$*" "$N" >&2
    done
}
elapsed() { local s=$(( SECONDS - $1 )); printf '%d:%02d' $(( s / 60 )) $(( s % 60 )); }

# run_phase <label> <log file> <hint> -- <command...>
# Runs a noisy command with its output in a log. On a terminal it shows a
# spinner, the elapsed time and the build's own progress ([n/m] from ninja);
# without one it prints a line every 30 s. On failure: the log's tail and a hint.
run_phase() {
    local label="$1" log="$2" hint="$3"; shift 4
    if [ "$DRY" = 1 ]; then would "$* > ${log#"$E"/}"; return 0; fi
    mkdir -p "$(dirname "$log")"
    local t0=$SECONDS i=0 last=0 detail pid rc cols
    "$@" > "$log" 2>&1 &
    pid=$!
    cols=$(tput cols 2>/dev/null || echo 80)
    while kill -0 "$pid" 2>/dev/null; do
        detail="$(tail -c 4000 "$log" 2>/dev/null | tr '\r' '\n' | grep -v '^\s*$' | tail -1)"
        local pct
        pct="$(tail -c 20000 "$log" 2>/dev/null | grep -o '^\[[0-9]*/[0-9]*\]' | tail -1 | tr -d '[]')"
        if [ -n "$pct" ]; then
            detail="$(( ${pct%/*} * 100 / ${pct#*/} ))% (${pct} steps)  ${detail#*] }"
        fi
        if [ "$TTY" = 1 ]; then
            local line
            line="$(printf '  %s %s  %s  %s' "${SPIN:i%${#SPIN}:1}" "$(elapsed "$t0")" "$label" "$detail")"
            printf '\r\e[K%s' "${line:0:$(( cols - 1 ))}"
            i=$(( i + 1 ))
        elif [ $(( SECONDS - last )) -ge 30 ]; then
            last=$SECONDS
            printf '  ... %s  %s  %.80s\n' "$(elapsed "$t0")" "$label" "$detail"
        fi
        sleep 0.25
    done
    wait "$pid"; rc=$?
    [ "$TTY" = 1 ] && printf '\r\e[K'
    if [ "$rc" = 0 ] && ! grep -q '^FAILED ' "$log" 2>/dev/null; then
        good "$label  ${D}($(elapsed "$t0"), log ${log#"$E"/})${N}"
        return 0
    fi
    fail "$label failed after $(elapsed "$t0") -- the end of ${log#"$E"/}:"
    tail -25 "$log" | sed 's/^/      /'
    [ -n "$hint" ] && warn "$hint"
    return 1
}

# ---------------------------------------------------------------- platform ---
EXE=""
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) PLATFORM=windows; EXE=".exe" ;;
    Linux) PLATFORM=linux ;;
    Darwin) PLATFORM=macos ;;
    *) PLATFORM=unknown ;;
esac
WSL=0
[ "$PLATFORM" = linux ] && grep -qi microsoft /proc/version 2>/dev/null && WSL=1
PKG=""
if [ "$PLATFORM" = windows ]; then
    command -v pacman >/dev/null 2>&1 && PKG=pacman
else
    for p in apt-get dnf pacman zypper; do command -v "$p" >/dev/null 2>&1 && { PKG="$p"; break; }; done
fi
CORES="$(nproc 2>/dev/null || getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"

# Where the QEMU build trees go. On Windows QEMU's tracetool cannot relate paths
# across drives, so they must share this source's drive (and be on NTFS).
if [ -n "$OPT_BUILD_DIR" ]; then
    BUILD_BASE="$OPT_BUILD_DIR"
elif [ "$PLATFORM" = windows ]; then
    # The drive this folder is really on: /home/... inside MSYS2 is C:/msys64/...
    drive="$(cygpath -m "$E" 2>/dev/null | cut -c1 | tr 'A-Z' 'a-z')"
    BUILD_BASE="/${drive:-c}"
else
    BUILD_BASE="$HOME"
fi
if [ "$PLATFORM" = windows ]; then
    DEF_QEMU_BUILD="$BUILD_BASE/qemu-build-mingw"
else
    DEF_QEMU_BUILD="$BUILD_BASE/qemu-build"
fi

# The previous answers, so a re-run changes only what you ask it to.
CDJ_DECKS=""; CDJ_NAME=""; CDJ_DJLINK=""; CDJ_AUDIO=""; CDJ_CONTROLLER=""
CDJ_RELAY_PORT=""; CDJ_GROUP=""; CDJ_MIDI_PYTHON=""; CDJ_TOOLS_PYTHON=""
QEMU_BUILD=""; QEMU_EB_BUILD=""
# shellcheck source=/dev/null
[ -f "$CONF" ] && . "$CONF"
[ -n "$OPT_BUILD_DIR" ] && QEMU_BUILD=""
QEMU_BUILD="${QEMU_BUILD:-$DEF_QEMU_BUILD}"
QEMU_EB_BUILD="${QEMU_EB_BUILD:-$QEMU_BUILD-eb}"
C66X_JIT_LIBDIR="${C66X_JIT_LIBDIR:-$HOME/build/c6x}"
export QEMU_BUILD QEMU_EB_BUILD C66X_JIT_LIBDIR

printf '%sCDJ-2000NXS2 emulator setup%s\n' "$B" "$N"
dim "the real firmware of a Pioneer CDJ-2000NXS2, on emulated hardware"
info "platform: $PLATFORM$([ "$WSL" = 1 ] && echo ' (WSL)'), $CORES CPU threads${PKG:+, packages via $PKG}"
info "this folder: $E"
[ "$DRY" = 1 ] && warn "dry run: nothing is installed, built or written"
case "$PLATFORM" in
    macos | unknown) die "only Windows (MSYS2) and Linux are supported." ;;
esac

# ======================================================= 1. prerequisites ====
step 1 "Prerequisites"
MISSING_TOOLS=(); MISSING_LIBS=()
if [ "$PLATFORM" = windows ]; then
    if [ -z "$PKG" ]; then
        fail "this shell has no pacman, so it is not MSYS2 (Git Bash, perhaps)."
        info "Install MSYS2 from https://www.msys2.org, open ${B}MSYS2 MINGW64${N} from"
        info "the Start menu, and run ./setup.sh from there."
        [ "$DRY" = 1 ] || exit 1
    elif [ "${MSYSTEM:-}" != MINGW64 ]; then
        fail "this is the MSYS2 ${MSYSTEM:-?} shell; the emulator is built in ${B}MSYS2 MINGW64${N}."
        [ "$DRY" = 1 ] || exit 1
    fi
    export PATH="/mingw64/bin:$PATH"
fi

tools="gcc make ninja meson pkg-config flex bison git patch"
[ "$PLATFORM" = windows ] && tools="$tools python diff" || tools="$tools python3"
for t in $tools; do command -v "$t" >/dev/null 2>&1 || MISSING_TOOLS+=("$t"); done
libs="glib-2.0 pixman-1 zlib gtk+-3.0"
[ "$PLATFORM" = windows ] && libs="$libs sdl2" || libs="$libs libpulse"
if command -v pkg-config >/dev/null 2>&1; then
    for l in $libs; do pkg-config --exists "$l" 2>/dev/null || MISSING_LIBS+=("$l"); done
else
    read -r -a MISSING_LIBS <<< "$libs"
fi

pkg_for() {  # <tool or pkg-config module> -> the package that provides it here
    case "$PLATFORM:$1" in
        windows:gcc) echo mingw-w64-x86_64-gcc ;;
        windows:ninja) echo mingw-w64-x86_64-ninja ;;
        windows:meson) echo mingw-w64-x86_64-meson ;;
        windows:pkg-config) echo mingw-w64-x86_64-pkgconf ;;
        windows:python) echo mingw-w64-x86_64-python ;;
        windows:glib-2.0) echo mingw-w64-x86_64-glib2 ;;
        windows:pixman-1) echo mingw-w64-x86_64-pixman ;;
        windows:zlib) echo mingw-w64-x86_64-zlib ;;
        windows:gtk+-3.0) echo mingw-w64-x86_64-gtk3 ;;
        windows:sdl2) echo mingw-w64-x86_64-SDL2 ;;
        windows:diff) echo diffutils ;;
        windows:*) echo "$1" ;;
        *:gcc | *:make) echo build-essential ;;
        *:ninja) echo ninja-build ;;
        *:python3) echo "python3 python3-venv python3-pip" ;;
        *:glib-2.0) echo libglib2.0-dev ;;
        *:pixman-1) echo libpixman-1-dev ;;
        *:zlib) echo zlib1g-dev ;;
        *:gtk+-3.0) echo "libgtk-3-dev libepoxy-dev" ;;
        *:libpulse) echo libpulse-dev ;;
        *) echo "$1" ;;
    esac
}
if [ "${#MISSING_TOOLS[@]}" -eq 0 ] && [ "${#MISSING_LIBS[@]}" -eq 0 ]; then
    good "build tools and libraries"
else
    fail "missing: ${MISSING_TOOLS[*]} ${MISSING_LIBS[*]}"
    pkgs=""
    for x in "${MISSING_TOOLS[@]}" "${MISSING_LIBS[@]}"; do pkgs="$pkgs $(pkg_for "$x")"; done
    # shellcheck disable=SC2086  # one package name per word
    pkgs="$(printf '%s\n' $pkgs | sort -u | tr '\n' ' ')"
    case "$PLATFORM:$PKG" in
        windows:*) info "install them:  ${B}pacman -S --needed $pkgs${N}" ;;
        *:apt-get) info "install them:  ${B}sudo apt-get install -y $pkgs${N}" ;;
        *) info "install the equivalents of: $pkgs (Debian package names)" ;;
    esac
    if [ "$SKIP_BUILD" = 0 ] && [ "$DRY" = 0 ]; then
        die "install the packages above, then run ./setup.sh again."
    fi
fi

# Python. The rig itself needs only the standard library; requirements.txt says
# which Python needs what else:
#   pyfatfs         the USB image builder (step 4); any Python 3
#   mido, rtmidi    midi/bridge.py. On Windows a native Python (python.org or
#                   Microsoft Store): the bridge opens the USB MIDI device, and
#                   python-rtmidi does not build for MSYS2's Python, which pip
#                   refuses to install into anyway (EXTERNALLY-MANAGED).
#   Pillow          the playhead scorer, run by the rig's own python3
# A candidate is either a file (a recorded absolute path, possibly with spaces)
# or a command such as "py -3".
py_argv() {  # <python> -> PYARGV
    if [ -f "$1" ]; then PYARGV=("$1"); else read -r -a PYARGV <<< "$1"; fi
}
py_has() {  # <python> module...
    local code="import sys"
    py_argv "$1"; shift
    for m in "$@"; do code="$code; import $m"; done
    "${PYARGV[@]}" -c "$code" >/dev/null 2>&1
}

# Native Windows Pythons, as absolute MSYS paths. MSYS2's default PATH has
# neither the py launcher's folder nor the Store's aliases, so their usual
# places are searched too. MSYS2's own Python is told apart by the compiler in
# sys.version (GCC, not MSC).
WIN_PYTHONS=()
add_win_python() {  # <python command...>
    local p q
    p="$("$@" -c "import sys; assert 'MSC' in sys.version and sys.version_info[0] == 3; print(sys.executable)" \
        2>/dev/null | tr -d '\r')"
    [ -n "$p" ] || return 0
    p="$(cygpath -u "$p")"
    for q in "${WIN_PYTHONS[@]}"; do [ "$q" = "$p" ] && return 0; done
    WIN_PYTHONS+=("$p")
}
PY_LAUNCHER=""
if [ "$PLATFORM" = windows ]; then
    local_app="$(cygpath -u -F 28 2>/dev/null)"
    for c in "${PYTHON:-}" "$CDJ_MIDI_PYTHON" "$CDJ_TOOLS_PYTHON"; do
        [ -n "$c" ] && [ -f "$c" ] && add_win_python "$c"
    done
    for c in py.exe /c/Windows/py.exe "$local_app/Programs/Python/Launcher/py.exe"; do
        if command -v "$c" >/dev/null 2>&1; then
            PY_LAUNCHER="$(command -v "$c")"
            add_win_python "$PY_LAUNCHER" -3
            break
        fi
    done
    while IFS= read -r c; do
        case "$c" in /mingw* | /ucrt64/* | /clang* | /usr/* | /bin/*) continue ;; esac
        add_win_python "$c"
    done < <(type -ap python.exe python3.exe 2>/dev/null)
    for c in "$local_app"/Programs/Python/Python3*/python.exe \
             "/c/Program Files"/Python3*/python.exe \
             "$local_app/Microsoft/WindowsApps/python3.exe"; do
        [ -f "$c" ] && add_win_python "$c"
    done
fi

PY_CANDIDATES=()
[ -n "${PYTHON:-}" ] && PY_CANDIDATES+=("$PYTHON")
[ -n "$CDJ_TOOLS_PYTHON" ] && PY_CANDIDATES+=("$CDJ_TOOLS_PYTHON")
[ -x "$E/.venv/bin/python" ] && PY_CANDIDATES+=("$E/.venv/bin/python")
PY_CANDIDATES+=("${WIN_PYTHONS[@]}" python3 python)
first_python_with() {  # module... -> the first candidate that imports them all
    for c in "${PY_CANDIDATES[@]}"; do py_has "$c" "$@" && { printf '%s' "$c"; return 0; }; done
    return 1
}
PY_ANY="$(first_python_with)" || PY_ANY=""
[ -n "$PY_ANY" ] || die "no working Python 3 found (see above for the package)."
if [ "$PLATFORM" = windows ]; then
    [ "${#WIN_PYTHONS[@]}" -gt 0 ] && info "Windows Python: ${WIN_PYTHONS[0]}" ||
        info "no Windows Python found (python.org or the Microsoft Store)"
fi

# pip_fix <what> -> the command that installs <what> into a suitable Python.
# On Windows that is a native Python only.
pip_fix() {
    if [ "$PLATFORM" != windows ]; then
        case "$1" in
            -r*) echo "python3 -m venv .venv && .venv/bin/python -m pip install $1" ;;
            *) echo "python3 -m pip install --user $1" ;;
        esac
    elif [ -n "$PY_LAUNCHER" ]; then
        echo "py -3 -m pip install --user $1"
    elif [ "${#WIN_PYTHONS[@]}" -gt 0 ]; then
        echo "$(printf '%q' "${WIN_PYTHONS[0]}") -m pip install --user $1"
    fi
}
WIN_PY_HINT="install Python 3 for Windows (python.org, or the Microsoft Store), then run:"

PY_TOOLS="$(first_python_with pyfatfs)" || PY_TOOLS=""
if [ -n "$PY_TOOLS" ]; then
    good "Python for the USB image builder: $PY_TOOLS (pyfatfs)"
else
    warn "no Python with pyfatfs yet -- step 4 (the USB image) needs it"
    fix="$(pip_fix "-r requirements.txt")"
    if [ -z "$fix" ]; then
        info "$WIN_PY_HINT"
        info "  ${B}py -3 -m pip install -r requirements.txt${N}   and ./setup.sh again"
    else
        info "fix:  ${B}$fix${N}"
        if [ "$DRY" = 1 ]; then
            would "$fix"
        elif ask_yn "run that now?" y; then
            ( cd "$E" && eval "$fix" ) &&
                { PY_TOOLS="$(first_python_with pyfatfs)" ||
                  { [ -x "$E/.venv/bin/python" ] && PY_TOOLS="$E/.venv/bin/python"; }; } &&
                good "installed"
        fi
    fi
fi

if [ "$PLATFORM" = windows ]; then
    PY_MIDI=""
    for c in "${WIN_PYTHONS[@]}"; do py_has "$c" mido rtmidi && { PY_MIDI="$c"; break; }; done
else
    PY_MIDI="$(first_python_with mido rtmidi)" || PY_MIDI=""
fi
MIDI_FIX="$(pip_fix "mido python-rtmidi")"
MIDI_FIX="${MIDI_FIX:-py -3 -m pip install mido python-rtmidi}"
if [ -n "$PY_MIDI" ]; then
    good "Python for a MIDI controller: $PY_MIDI (mido, python-rtmidi)"
else
    dim "no Python with mido + python-rtmidi: fine unless you want a MIDI controller"
    [ "$PLATFORM" = windows ] && [ "${#WIN_PYTHONS[@]}" -eq 0 ] && dim "$WIN_PY_HINT"
    dim "for one:  $MIDI_FIX"
fi

if py_has python3 PIL; then
    good "Pillow for the playhead scorer (python3)"
else
    case "$PLATFORM:$PKG" in
        windows:*) fix="pacman -S --needed mingw-w64-x86_64-python-pillow" ;;
        *:apt-get) fix="sudo apt-get install -y python3-pil" ;;
        *) fix="python3 -m pip install --user Pillow" ;;
    esac
    dim "no Pillow for python3: the rig runs, only its end-of-run playhead score fails ($fix)"
fi

# The build trees and the DSP code cache take about 3 GB.
free_mb="$(df -Pm "$(dirname "$QEMU_BUILD")" 2>/dev/null | awk 'NR==2 {print $4}')"
if [ -n "$free_mb" ] && [ "$free_mb" -lt 4000 ]; then
    warn "only ${free_mb} MB free where the build goes ($(dirname "$QEMU_BUILD")); it needs ~3 GB"
fi
if [ "$CORES" -lt 4 ]; then
    warn "$CORES CPU threads: one deck may not keep up with real time"
fi

# ================================================================ 2. build ====
step 2 "Build the emulators"
MAIN_BIN="$QEMU_BUILD/qemu-system-sh4$EXE"
GUI_BIN="$QEMU_EB_BUILD/qemu-system-sh4eb$EXE"
DSP_LIB="$C66X_JIT_LIBDIR/libc66x.so"
info "build trees: $QEMU_BUILD, $QEMU_EB_BUILD"
if [ "$SKIP_BUILD" = 1 ]; then
    dim "skipped (--skip-build)"
else
    ts="$(date +%Y%m%d-%H%M%S)"
    phase() {  # <n> <name> <artifact or ''> <hint> <label>
        local n="$1" name="$2" artifact="$3" hint="$4" label="$5"
        if [ -n "$artifact" ] && [ -e "$artifact" ] && [ "$REBUILD" = 0 ]; then
            good "$label  ${D}(already built: ${artifact})${N}"
            return 0
        fi
        printf '  %s2.%s%s %s\n' "$B" "$n" "$N" "$label"
        run_phase "$label" "$LOGDIR/build-$ts-$n-$name.log" "$hint" -- \
            env JOBS="$CORES" bash "$E/build.sh" "$name"
    }
    phase 1 source "$CDJ_ROOT/qemu-src/hw/sh4" \
        "needs git and network access to gitlab.com; delete a half-cloned qemu-src/ and retry" \
        "QEMU 9.1.0 source (a shallow git clone, ~100 MB)" || die "the build stopped at phase 2.1"
    phase 2 patches "" \
        "a patch that does not apply means qemu-src/ is not a clean v9.1.0: delete it and re-run" \
        "this project's patches to QEMU" || die "the build stopped at phase 2.2"
    phase 3 main "$MAIN_BIN" \
        "a missing library shows as a meson 'Dependency ... not found' above; step 1 lists the package" \
        "the MAIN emulator (SH-4 + the CDJ-2000NXS2 board; the long one, 10-40 min)" || die "the build stopped at phase 2.3"
    phase 4 display "$GUI_BIN" \
        "same toolchain as 2.3; if that worked, look for a disk-full or gtk3 error above" \
        "the display-board emulator (SH-2A)" || die "the build stopped at phase 2.4"
    phase 5 dsp "$DSP_LIB" \
        "the DSP library needs only gcc and make" \
        "the DSP core library for the run-time JIT" || die "the build stopped at phase 2.5"
fi
info "${B}About speed:${N} the C66x DSP runs through a JIT that compiles its hot code"
info "while you play, into ~/c14gen. The first minutes of your first sessions run"
info "slower than real time (audio gaps) while that cache fills; after that the"
info "deck keeps up. A faster, profile-guided module can be built from a recording"
info "of your own firmware's DSP; none is shipped here."

# ============================================================= 3. firmware ====
step 3 "Firmware (your own update file)"
have_firmware() { for f in $FIRMWARE_IMAGES; do [ -f "$EXTRACT/$f" ] || return 1; done; }
install_firmware() {  # <C2KNXS2.UPD> [--force]
    run_phase "unpacking and verifying $(basename "$1")" "$LOGDIR/firmware.log" \
        "only the v1.87 update (sha256 f211191a...) is supported; the log says which image differed" \
        -- bash "$HERE/firmware/prepare_firmware.sh" --install ${2:+"$2"} "$1"
}
if have_firmware && [ -z "$OPT_FIRMWARE" ]; then
    good "firmware images in ${EXTRACT#"$CDJ_ROOT"/}/ (checked when they were installed)"
else
    info "The emulator boots the CDJ-2000NXS2's own firmware, and this repository"
    info "contains none of it. You need Pioneer DJ's public update file for the"
    info "CDJ-2000NXS2, ${B}version 1.87${N} (C2KNXS2.UPD), from their support site."
    upd="$OPT_FIRMWARE"
    while [ -z "$upd" ] || [ ! -f "$upd" ]; do
        if [ "$INTERACTIVE" = 0 ]; then
            [ "$DRY" = 1 ] && { would "scripts/firmware/prepare_firmware.sh --install <your C2KNXS2.UPD>"; break; }
            die "no firmware: pass --firmware /path/to/C2KNXS2.UPD"
        fi
        [ -n "$upd" ] && warn "no such file: $upd"
        upd="$(ask "path to C2KNXS2.UPD (drag the file here):" "")"
        upd="${upd%\"}"; upd="${upd#\"}"; upd="${upd%\'}"; upd="${upd#\'}"
        [ "$PLATFORM" = windows ] && command -v cygpath >/dev/null 2>&1 && [ -n "$upd" ] && upd="$(cygpath -u "$upd")"
    done
    if [ -n "$upd" ] && [ -f "$upd" ]; then
        force=""
        for f in $FIRMWARE_IMAGES; do [ -e "$EXTRACT/$f" ] && force="--force"; done
        install_firmware "$upd" $force || die "the firmware was not installed; nothing in extract/ changed"
        have_firmware && [ "$DRY" = 0 ] && good "six images installed in extract/"
    fi
fi

# ============================================================ 4. USB stick ====
step 4 "USB stick (your own music)"
make_image() {  # <folder>
    local dir="$1" mb size
    [ -d "$dir/PIONEER" ] || warn "no PIONEER/ folder in $dir: the deck browses rekordbox's database, so
      loose files will not show up. Export a playlist to a folder with rekordbox (Export
      mode, 'USB') and use that folder."
    mb="$(du -sm "$dir" 2>/dev/null | cut -f1)"; mb="${mb:-0}"
    size=$(( (mb * 115 / 100 + 64 + 63) / 64 * 64 ))
    [ "$size" -lt 256 ] && size=256
    if [ "$size" -gt 2000 ]; then
        fail "$dir holds ${mb} MB; the stick image is FAT16, at most 2 GB. Export fewer tracks."
        return 1
    fi
    local py="$PY_TOOLS"
    if [ -z "$py" ]; then
        [ "$DRY" = 1 ] || { fail "no Python with pyfatfs (see step 1)"; return 1; }
        py="<a python with pyfatfs>"
    fi
    info "${mb} MB of music -> a ${size} MB FAT16 image (copied fresh for every run)"
    py_argv "$py"
    run_phase "USB image from $(basename "$dir")" "$LOGDIR/usb-image.log" \
        "pyfatfs failing on import is usually setuptools>=81: pip install 'setuptools<81'" \
        -- "${PYARGV[@]}" "$HERE/media/make_usb_image.py" "$dir" "$USB_IMG" "$size"
}
if [ -f "$USB_IMG" ] && [ -z "$OPT_MUSIC" ]; then
    good "USB image: ${USB_IMG#"$CDJ_ROOT"/} ($(du -m "$USB_IMG" | cut -f1) MB)"
    if [ "$INTERACTIVE" = 1 ] && ask_yn "make a new one from another folder?" n; then
        OPT_MUSIC="$(ask "folder of your rekordbox USB export:" "")"
    fi
fi
if [ ! -f "$USB_IMG" ] || [ -n "$OPT_MUSIC" ]; then
    music="$OPT_MUSIC"
    while [ -z "$music" ] || [ ! -d "$music" ]; do
        if [ "$INTERACTIVE" = 0 ]; then
            [ "$DRY" = 1 ] && { would "scripts/media/make_usb_image.py <your rekordbox export> ${USB_IMG#"$CDJ_ROOT"/}"; break; }
            die "no USB image: pass --music /path/to/your/rekordbox/export"
        fi
        info "Point me at a folder with your own music exported by rekordbox (the"
        info "folder that holds PIONEER/ and the tracks). Nothing is uploaded anywhere."
        [ -n "$music" ] && warn "no such folder: $music"
        music="$(ask "music folder:" "")"
        music="${music%\"}"; music="${music#\"}"; music="${music%\'}"; music="${music#\'}"
        [ "$PLATFORM" = windows ] && command -v cygpath >/dev/null 2>&1 && [ -n "$music" ] && music="$(cygpath -u "$music")"
    done
    if [ -n "$music" ] && [ -d "$music" ]; then
        mkdir -p "$EXTRACT" 2>/dev/null || true
        make_image "$music" || die "no USB image was made"
    fi
fi

# ============================================================ 5. your setup ====
step 5 "Your setup"
# Windows reserves whole port ranges for Hyper-V and WSL, and a bind inside one
# fails (the relay's TCP port, Pro DJ Link's UDP one). Read them once.
reserved_ranges() {  # <tcp|udp> -> "start end" lines
    [ "$PLATFORM" = windows ] && command -v netsh >/dev/null 2>&1 || return 0
    netsh int ipv4 show excludedportrange protocol="$1" 2>/dev/null | tr -d '\r' |
        awk 'NF>=2 && $1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ {print $1, $2}'
}
TCP_RESERVED="$(reserved_ranges tcp)"
UDP_RESERVED="$(reserved_ranges udp)"
unreserved() {  # <port> <step +1|-1> <ranges>: the first port from <port> on outside them
    printf '%s\n' "$3" | awk -v p="$1" -v d="$2" '
        NF == 2 { s[NR] = $1; e[NR] = $2 }
        END {
            for (n = 0; n < 2000; n++) {
                hit = 0
                for (j in s) if (s[j] <= p && e[j] >= p) { hit = 1; break }
                if (!hit) { print p; exit }
                p += d
            }
            print -1
        }'
}
pick_tcp_port() {  # <wanted>: it, or the next one that is not reserved and not in use
    local p="$1"
    for _ in 1 2 3 4 5 6 7 8; do
        p="$(unreserved "$p" 1 "$TCP_RESERVED")"
        [ "$p" = -1 ] && break
        py_argv "$PY_ANY"
        "${PYARGV[@]}" -c "import socket; s=socket.socket(); s.bind(('127.0.0.1', $p)); s.close()" \
            2>/dev/null && { echo "$p"; return; }
        p=$(( p + 1 ))
    done
    echo "$1"
}
pick_udp_port() {  # <wanted>: it, or the next one BELOW it (Windows reserves high ranges)
    local p
    p="$(unreserved "$1" -1 "$UDP_RESERVED")"
    [ "$p" = -1 ] && p="$1"
    echo "$p"
}
onoff() { case "$1" in on | 1 | yes | y) echo 1 ;; *) echo 0 ;; esac; }

if [ -f "$CONF" ] && [ "$RECONFIGURE" = 0 ] && [ -z "$OPT_DECKS$OPT_NAME$OPT_DJLINK$OPT_AUDIO$OPT_CONTROLLER$OPT_RELAY" ]; then
    good "keeping your setup in cdj.conf (./setup.sh --reconfigure to change it)"
    ASK5=0
else
    ASK5=1
fi
if [ "$ASK5" = 1 ]; then
    info "Two decks run on a shared Pro DJ Link network and SYNC to each other, but"
    info "need roughly twice the CPU of one (this machine: $CORES threads)."
    case "$OPT_DECKS" in "" | 1 | 2) ;; *) die "--decks takes 1 or 2" ;; esac
    CDJ_DECKS="${OPT_DECKS:-$(choose "how many decks?" "${CDJ_DECKS:-1}" 1 2)}"
    CDJ_NAME="${OPT_NAME:-$(ask "a name for the deck windows and logs:" "${CDJ_NAME:-show}")}"
    CDJ_NAME="$(printf '%s' "$CDJ_NAME" | tr -cd 'A-Za-z0-9')"; CDJ_NAME="${CDJ_NAME:-show}"
    if [ -n "$OPT_DJLINK" ]; then
        CDJ_DJLINK="$(onoff "$OPT_DJLINK")"
    else
        def=$([ "$CDJ_DECKS" = 2 ] && echo on || echo off)
        CDJ_DJLINK="$(onoff "$(choose "Pro DJ Link network?" "$def" on off)")"
    fi
    if [ -n "$OPT_AUDIO" ]; then
        CDJ_AUDIO="$(onoff "$OPT_AUDIO")"
    else
        CDJ_AUDIO="$(onoff "$(choose "sound?" "$([ "${CDJ_AUDIO:-1}" = 1 ] && echo on || echo off)" on off)")"
    fi

    # The controller: a profile from midi/controllers/, none, or learn one now.
    profiles=()
    for f in "$E"/midi/controllers/*.json; do [ -f "$f" ] && profiles+=("$(basename "$f" .json)"); done
    detected=""
    if [ -n "$PY_MIDI" ]; then
        py_argv "$PY_MIDI"
        inputs="$("${PYARGV[@]}" -c "import mido; print('\n'.join(mido.get_input_names()))" 2>/dev/null)"
        for p in "${profiles[@]}"; do
            m="$(sed -n 's/.*"match": *"\([^"]*\)".*/\1/p' "$E/midi/controllers/$p.json" | head -1)"
            [ -n "$m" ] && printf '%s\n' "$inputs" | grep -qi -- "$m" && { detected="$p"; break; }
        done
        [ -n "$detected" ] && good "found a connected controller: $detected"
    fi
    info "MIDI controller: 'none', one of: ${profiles[*]}, or 'learn' to teach it a new one"
    def="${CDJ_CONTROLLER:-${detected:-none}}"
    CDJ_CONTROLLER="${OPT_CONTROLLER:-$(ask "controller?" "$def")}"
    if [ "$CDJ_CONTROLLER" = learn ]; then
        if [ -z "$PY_MIDI" ] && [ "$DRY" = 0 ]; then
            warn "learning needs a Python with mido + python-rtmidi ($MIDI_FIX);"
            warn "no controller for now -- run ./setup.sh --reconfigure after installing them"
            CDJ_CONTROLLER=none
        else
            cname="$(ask "a short name for it (e.g. ddj-400):" "my-controller")"
            cname="$(printf '%s' "$cname" | tr 'A-Z ' 'a-z-' | tr -cd 'a-z0-9-')"
            if [ "$DRY" = 1 ]; then
                would "${PY_MIDI:-python} midi/learn.py new --controller $cname; ${PY_MIDI:-python} midi/learn.py map --controller $cname"
            else
                py_argv "$PY_MIDI"
                ( cd "$E" && "${PYARGV[@]}" midi/learn.py new --controller "$cname" --name "$cname" &&
                  "${PYARGV[@]}" midi/learn.py map --controller "$cname" --prefix "$CDJ_NAME" ) ||
                    warn "the learn session did not finish; the controller is saved as far as it got"
            fi
            CDJ_CONTROLLER="$cname"
        fi
    elif [ "$CDJ_CONTROLLER" != none ] && [ ! -f "$E/midi/controllers/$CDJ_CONTROLLER.json" ]; then
        warn "no profile midi/controllers/$CDJ_CONTROLLER.json -- no controller for now"
        CDJ_CONTROLLER=none
    fi
    if [ "$CDJ_CONTROLLER" != none ] && [ -z "$PY_MIDI" ]; then
        warn "the bridge needs a Python with mido + python-rtmidi; until then ./start.sh runs without it"
        warn "  $MIDI_FIX"
    fi

    want="${OPT_RELAY:-${CDJ_RELAY_PORT:-7202}}"
    CDJ_RELAY_PORT="$(pick_tcp_port "$want")"
    [ "$CDJ_RELAY_PORT" != "$want" ] && warn "TCP $want is reserved or in use here; the controller relay uses $CDJ_RELAY_PORT"
    gport="${CDJ_GROUP##*:}"; gport="${gport:-45000}"
    gfree="$(pick_udp_port "$gport")"
    [ "$gfree" != "$gport" ] && [ "$CDJ_DJLINK" = 1 ] && warn "UDP $gport is reserved by Windows; Pro DJ Link uses $gfree"
    CDJ_GROUP="239.77.77.1:$gfree"
    CDJ_MIDI_PYTHON="$PY_MIDI"
    CDJ_TOOLS_PYTHON="$PY_TOOLS"
fi

write_conf() {
    printf '# Written by ./setup.sh on %s. ./start.sh reads it.\n' "$(date '+%Y-%m-%d %H:%M')"
    printf '# Edit it, or run ./setup.sh --reconfigure.\n'
    printf 'CDJ_DECKS=%q\nCDJ_NAME=%q\nCDJ_DJLINK=%q\nCDJ_AUDIO=%q\n' \
        "$CDJ_DECKS" "$CDJ_NAME" "$CDJ_DJLINK" "$CDJ_AUDIO"
    printf 'CDJ_CONTROLLER=%q\nCDJ_RELAY_PORT=%q\nCDJ_GROUP=%q\n' \
        "$CDJ_CONTROLLER" "$CDJ_RELAY_PORT" "$CDJ_GROUP"
    printf 'CDJ_MIDI_PYTHON=%q\nCDJ_TOOLS_PYTHON=%q\n' "$CDJ_MIDI_PYTHON" "$CDJ_TOOLS_PYTHON"
    printf 'QEMU_BUILD=%q\nQEMU_EB_BUILD=%q\n' "$QEMU_BUILD" "$QEMU_EB_BUILD"
}
# A kept setup still takes the Pythons found now, e.g. after installing mido.
if [ "$ASK5" = 0 ] && { { [ -n "$PY_MIDI" ] && [ "$PY_MIDI" != "$CDJ_MIDI_PYTHON" ]; } ||
                        { [ -n "$PY_TOOLS" ] && [ "$PY_TOOLS" != "$CDJ_TOOLS_PYTHON" ]; }; }; then
    CDJ_MIDI_PYTHON="${PY_MIDI:-$CDJ_MIDI_PYTHON}"
    CDJ_TOOLS_PYTHON="${PY_TOOLS:-$CDJ_TOOLS_PYTHON}"
    ASK5=1
fi
if [ "$ASK5" = 1 ]; then
    if [ "$DRY" = 1 ]; then
        info "(dry run) would write cdj.conf:"
        write_conf | sed 's/^/      /'
    else
        write_conf > "$CONF"
        good "saved to cdj.conf"
    fi
fi

# ================================================================ summary ====
yesno() { [ "$1" = 1 ] && echo on || echo off; }
printf '\n%sReady.%s\n' "$B$G" "$N"
info "decks        ${CDJ_DECKS:-1} (${CDJ_NAME:-show}1$([ "${CDJ_DECKS:-1}" = 2 ] && echo ", ${CDJ_NAME:-show}2"))"
info "Pro DJ Link  $(yesno "${CDJ_DJLINK:-0}")$([ "${CDJ_DJLINK:-0}" = 1 ] && echo " (${CDJ_GROUP})")"
info "sound        $(yesno "${CDJ_AUDIO:-1}")"
info "controller   ${CDJ_CONTROLLER:-none}$([ "${CDJ_CONTROLLER:-none}" != none ] && echo " (relay on 127.0.0.1:${CDJ_RELAY_PORT})")"
info "start it:    ${B}./start.sh${N}      stop: Ctrl-C (or ./start.sh stop from another shell)"
if [ "$DRY" = 0 ] && [ "$INTERACTIVE" = 1 ] && have_firmware && [ -f "$USB_IMG" ] &&
   [ -e "$MAIN_BIN" ] && ask_yn "start the deck now?" y; then
    exec bash "$E/start.sh"
fi
