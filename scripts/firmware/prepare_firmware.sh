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
#              It may not lie inside the repository -- use --install for
#              that.
#   PYTHON=    interpreter to use (default: the first working python3/python).
#              Only the standard library is needed.
#
# Exit status: 0 when every image matches, 1 on any mismatch or error.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
. "$HERE/../cdj_paths.sh"; REPO="$CDJ_ROOT"
. "$HERE/../cdj_model.sh"

die() { echo "prepare_firmware: $*" >&2; exit 1; }

usage() { sed -n '/^#   usage:/,/^# Exit status/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 1; }

INSTALL=0
FORCE=0
MODEL="${CDJ_MODEL:-cdj2000nxs2}"
ARGS=()
while [ "$#" -gt 0 ]; do
    case "$1" in
        --install) INSTALL=1 ;;
        --force)   FORCE=1 ;;
        --model)   [ "$#" -ge 2 ] || die "--model needs a model id"; MODEL="$2"; shift ;;
        -h|--help) usage ;;
        -*)        die "unknown option $1" ;;
        *)         ARGS+=("$1") ;;
    esac
    shift
done
[ "${#ARGS[@]}" -ge 1 ] && [ "${#ARGS[@]}" -le 2 ] || usage
[ "$FORCE" = 1 ] && [ "$INSTALL" = 0 ] && die "--force only means something with --install"
cdj_model_load "$MODEL" || exit 1

# The update files, in section order. A one-file update is a container that
# split_update.py carves; a several-file update already is one file per section.
read -r -a UPD_NAMES <<<"$MODEL_UPD"
read -r -a UPD_SUMS <<<"$MODEL_UPD_SHA256"
[ "${#UPD_NAMES[@]}" = "${#UPD_SUMS[@]}" ] \
    || die "models/$MODEL.conf: MODEL_UPD and MODEL_UPD_SHA256 differ in length"
SRC="${ARGS[0]}"
[ -e "$SRC" ] || die "no such file or folder: $SRC"
if [ -d "$SRC" ]; then
    UPD_DIR="$(cd "$SRC" && pwd)"
else
    [ "${#UPD_NAMES[@]}" = 1 ] || UPD_DIR="$(cd "$(dirname "$SRC")" && pwd)"
fi
UPD_FILES=()
if [ "${#UPD_NAMES[@]}" = 1 ] && [ -f "$SRC" ]; then
    UPD_FILES=("$(cd "$(dirname "$SRC")" && pwd)/$(basename "$SRC")")
else
    for n in "${UPD_NAMES[@]}"; do
        [ -f "$UPD_DIR/$n" ] || die "$MODEL_TITLE needs $n next to the other update files in $UPD_DIR"
        UPD_FILES+=("$UPD_DIR/$n")
    done
fi

# A bare `python3` on Windows can be the Microsoft Store stub, which exists on
# PATH but cannot run anything, so probe by running rather than by `command -v`.
PY="${PYTHON:-}"
if [ -z "$PY" ]; then
    for cand in python3 python; do
        if "$cand" -c 'import sys; sys.exit(sys.version_info < (3, 6))' >/dev/null 2>&1; then
            PY="$cand"; break
        fi
    done
fi
[ -n "$PY" ] || die "no working Python 3.6+ found; set PYTHON="

sha256() {
    "$PY" -c 'import hashlib, sys
h = hashlib.sha256()
with open(sys.argv[1], "rb") as f:
    for chunk in iter(lambda: f.read(1 << 20), b""):
        h.update(chunk)
print(h.hexdigest())' "$1"
}

# Canonical form of a directory that may not exist yet, for the safety checks.
canon() {
    local d="$1" tail=""
    while [ ! -d "$d" ]; do
        tail="/$(basename "$d")$tail"
        d="$(dirname "$d")"
    done
    echo "$(cd "$d" && pwd -P)$tail"
}

for i in "${!UPD_FILES[@]}"; do
    got="$(sha256 "${UPD_FILES[$i]}")"
    if [ "$got" != "${UPD_SUMS[$i]}" ]; then
        die "${UPD_FILES[$i]} is not the $MODEL_TITLE v$MODEL_FW_VERSION update (sha256 $got, want ${UPD_SUMS[$i]}).
Only v$MODEL_FW_VERSION is supported: every address in the model and the harness is for it."
    fi
done

if [ "${#ARGS[@]}" -eq 2 ]; then
    OUT="${ARGS[1]}"
    OUTC="$(canon "$OUT")"
    REPOC="$(cd "$REPO" && pwd -P)"
    case "$OUTC/" in
        "$REPOC/"*) die "outdir is inside the repository; write elsewhere and use --install" ;;
    esac
    if [ -e "$OUT" ]; then
        [ -d "$OUT" ] || die "outdir exists and is not a directory: $OUT"
        [ -z "$(ls -A "$OUT")" ] || die "outdir is not empty: $OUT"
    fi
    mkdir -p "$OUT"
else
    OUT="$(mktemp -d "${TMPDIR:-/tmp}/cdj-firmware.XXXXXX")"
fi
OUT="$(cd "$OUT" && pwd)"
mkdir -p "$OUT/extract" "$OUT/notes" "$OUT/log"

echo "python : $("$PY" --version 2>&1) ($PY)"
echo "model  : $MODEL_TITLE ($MODEL)"
for f in "${UPD_FILES[@]}"; do echo "update : $f"; done
echo "outdir : $OUT"
echo "update : sha256 ok (v$MODEL_FW_VERSION)"
echo

# Each tool runs with <outdir> as its working directory, because gui_decode,
# gui_resources, gui_artwork (via gui_address) and make_flash default to or hard-code
# paths such as extract/gui_unpacked.bin relative to the current directory.
# Relative arguments with forward slashes also keep lzss_decode.py happy: it
# derives its output path by splitting its input on '/'.
step() {
    local name="$1"; shift
    printf '  %-14s ' "$name"
    local t0=$SECONDS
    if (cd "$OUT" && "$@") >"$OUT/log/$name.log" 2>&1; then
        echo "done ($((SECONDS - t0)) s)"
    else
        echo "FAILED -- see $OUT/log/$name.log"
        tail -20 "$OUT/log/$name.log" >&2
        exit 1
    fi
}

# extract/section<N>.bin, one per section, from the container or the files.
sections() {
    if [ "${#UPD_FILES[@]}" = 1 ]; then
        "$PY" "$HERE/split_update.py" "${UPD_FILES[0]}" extract
    else
        local i
        for i in "${!UPD_FILES[@]}"; do
            cp "${UPD_FILES[$i]}" "extract/section$((i + 1)).bin"
            echo "section$((i + 1)).bin <- $(basename "${UPD_FILES[$i]}")"
        done
    fi
}

MAIN_SEC="extract/section$MODEL_MAIN_SECTION"
GUI_SEC="extract/section${MODEL_GUI_SECTION:-1}"
echo "steps:"
for s in $MODEL_FW_STEPS; do
    case "$s" in
        sections)      step sections      sections ;;
        srec_coverage) step srec_coverage "$PY" "$HERE/srec_coverage.py" "$MAIN_SEC.bin" ;;
        lzss_decode)   step lzss_decode   "$PY" "$HERE/lzss_decode.py" "$MAIN_SEC.sparse.bin" "$MODEL_MAIN_LZSS" ;;
        gui_decode)    step gui_decode    "$PY" "$HERE/gui_decode.py" "$GUI_SEC.bin" extract/gui_unpacked.bin ;;
        gui_resources) step gui_resources "$PY" "$HERE/gui_resources.py" extract/resblob.bin ;;
        gui_artwork)   step gui_artwork   "$PY" "$HERE/gui_artwork.py" extract/artblob.bin ;;
        make_settings) step make_settings "$PY" "$HERE/make_settings.py" extract/settings.bin ;;
        make_flash)    step make_flash    "$PY" "$HERE/make_flash.py" extract/flash.bin extract/settings.bin ;;
        *)             die "models/$MODEL.conf: unknown step '$s'" ;;
    esac
done
echo

mapfile -t EXPECTED < <(printf '%s\n' "$MODEL_EXPECTED" | sed '/^[[:space:]]*$/d')

echo "verify:"
FAIL=0
for entry in "${EXPECTED[@]}"; do
    read -r rel want <<<"$entry"
    if [ ! -f "$OUT/$rel" ]; then
        printf '  FAIL  %-26s missing\n' "$rel"; FAIL=1; continue
    fi
    got="$(sha256 "$OUT/$rel")"
    if [ "$got" = "$want" ]; then
        printf '  PASS  %-26s %s\n' "$rel" "$got"
    else
        printf '  FAIL  %-26s %s (want %s)\n' "$rel" "$got" "$want"; FAIL=1
    fi
done
echo

if [ "$FAIL" != 0 ]; then
    echo "prepare_firmware: at least one image does not match; nothing installed." >&2
    exit 1
fi

if [ "$INSTALL" = 0 ]; then
    echo "all images match. To use them:  $0 --model $MODEL --install ${ARGS[0]}"
    echo "(or copy the images from $OUT/extract/ into the repository's $MODEL_EXTRACT/ by hand)"
    exit 0
fi

# Check every destination before copying anything, so a refusal leaves the
# repository exactly as it was.
dest() { printf '%s/%s/%s' "$REPO" "$MODEL_EXTRACT" "${1#extract/}"; }
if [ "$FORCE" = 0 ]; then
    clash=""
    for entry in "${EXPECTED[@]}"; do
        read -r rel _ <<<"$entry"
        [ -e "$(dest "$rel")" ] && clash="$clash $MODEL_EXTRACT/${rel#extract/}"
    done
    [ -z "$clash" ] || die "refusing to overwrite:$clash (add --force to replace them)"
fi

mkdir -p "$REPO/$MODEL_EXTRACT" "$REPO/notes"
for entry in "${EXPECTED[@]}"; do
    read -r rel _ <<<"$entry"
    cp "$OUT/$rel" "$(dest "$rel")"
    echo "installed $MODEL_EXTRACT/${rel#extract/}"
done
