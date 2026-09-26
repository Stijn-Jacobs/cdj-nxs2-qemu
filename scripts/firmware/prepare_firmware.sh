#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Turn Pioneer's public CDJ-2000NXS2 v1.87 update (C2KNXS2.UPD) into the six
# images the rig reads, and prove each one against its known SHA-256.
#
# Only the update file is needed, never a device dump.
#
# By default everything is written to a NEW directory and nothing in the
# repository is touched. The output mirrors the repository layout:
#
#   <outdir>/extract/{main_unpacked,gui_unpacked,flash,settings,resblob,artblob}.bin
#   <outdir>/extract/section*.bin ...   intermediates
#   <outdir>/log/<step>.log             each tool's own output
#
# --install then copies the six verified images into the repository's
# extract/. It refuses if any of them already exists, unless
# --force is also given, and it installs nothing unless every image passed.
#
#   usage: scripts/firmware/prepare_firmware.sh [--install [--force]] <C2KNXS2.UPD> [outdir]
#
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

# The supported update. Every firmware address in this project is for v1.87.
UPD_SHA256=f211191a573b7a5c0e694936871f76011a58d5861dd061301cdb0d8b83a8b3e5

# Output path relative to <outdir> -> expected SHA-256. settings.bin is the
# settings block flash.bin is built from.
EXPECTED=(
    "extract/main_unpacked.bin 78182ce6e552ff4ba65c8c845a62317cecbec86c35e312c6ac9e9af9796a2076"
    "extract/gui_unpacked.bin  20b52af38f29b2a59255e1ce4238cab896f28edca2ad5707650a8c8bb9d0457e"
    "extract/flash.bin         c52253efd5e926968b892fcf1b762279f7f2119bc89426280e90d301ce8ee203"
    "extract/resblob.bin         cb61896e8d6b3d3fe317e2af14a05c5729593beb084d0e53b98968181272d768"
    "extract/artblob.bin         929372309ea24f8d30255876ad46f7b684470009a3b022eed03179db3d4161a7"
    "extract/settings.bin      73af511542c0637da543f6bfec8513cf92befe0053552471411bb4e31854b56d"
)

die() { echo "prepare_firmware: $*" >&2; exit 1; }

usage() { sed -n '/^#   usage:/,/^# Exit status/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//' >&2; exit 1; }

INSTALL=0
FORCE=0
ARGS=()
for a in "$@"; do
    case "$a" in
        --install) INSTALL=1 ;;
        --force)   FORCE=1 ;;
        -h|--help) usage ;;
        -*)        die "unknown option $a" ;;
        *)         ARGS+=("$a") ;;
    esac
done
[ "${#ARGS[@]}" -ge 1 ] && [ "${#ARGS[@]}" -le 2 ] || usage
[ "$FORCE" = 1 ] && [ "$INSTALL" = 0 ] && die "--force only means something with --install"

UPD="${ARGS[0]}"
[ -f "$UPD" ] || die "no such file: $UPD"
UPD="$(cd "$(dirname "$UPD")" && pwd)/$(basename "$UPD")"

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

got="$(sha256 "$UPD")"
if [ "$got" != "$UPD_SHA256" ]; then
    die "$UPD is not the v1.87 update (sha256 $got, want $UPD_SHA256).
Only v1.87 is supported: every address in the model and the harness is for it."
fi

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
echo "update : $UPD"
echo "outdir : $OUT"
echo "update : sha256 ok (v1.87)"
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
    if (cd "$OUT" && "$PY" "$@") >"$OUT/log/$name.log" 2>&1; then
        echo "done ($((SECONDS - t0)) s)"
    else
        echo "FAILED -- see $OUT/log/$name.log"
        tail -20 "$OUT/log/$name.log" >&2
        exit 1
    fi
}

echo "steps:"
step split_update   "$HERE/split_update.py"   "$UPD" extract
step srec_coverage  "$HERE/srec_coverage.py"  extract/section3.bin
step lzss_decode    "$HERE/lzss_decode.py"    extract/section3.sparse.bin 0x50004
step gui_decode     "$HERE/gui_decode.py"     extract/section1.bin extract/gui_unpacked.bin
step gui_resources  "$HERE/gui_resources.py"  extract/resblob.bin
step gui_artwork    "$HERE/gui_artwork.py"    extract/artblob.bin
step make_settings  "$HERE/make_settings.py"  extract/settings.bin
step make_flash     "$HERE/make_flash.py"     extract/flash.bin extract/settings.bin
echo

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
    echo "all images match. To use them:  $0 --install $UPD"
    echo "(or copy the six images from $OUT/extract/ into the repository's extract/ by hand)"
    exit 0
fi

# Check every destination before copying anything, so a refusal leaves the
# repository exactly as it was.
if [ "$FORCE" = 0 ]; then
    clash=""
    for entry in "${EXPECTED[@]}"; do
        read -r rel _ <<<"$entry"
        [ -e "$REPO/$rel" ] && clash="$clash $rel"
    done
    [ -z "$clash" ] || die "refusing to overwrite:$clash (add --force to replace them)"
fi

mkdir -p "$REPO/extract" "$REPO/notes"
for entry in "${EXPECTED[@]}"; do
    read -r rel _ <<<"$entry"
    cp "$OUT/$rel" "$REPO/$rel"
    echo "installed $rel"
done
