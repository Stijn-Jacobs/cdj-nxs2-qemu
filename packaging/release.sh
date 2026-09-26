#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# A release build for this machine's OS: the QEMUs and the DSP core library
# with ./build.sh, then the portable package with packaging/package.py. The
# release workflow (.github/workflows/release.yml) runs exactly this, and so
# can you, from the shell ./build.sh needs (MSYS2 MINGW64 on Windows).
#
#   usage: bash packaging/release.sh [package.py options]   (e.g. --out dist)
set -euo pipefail
E="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bash "$E/build.sh"
PY=python3
command -v python3 >/dev/null 2>&1 || PY=python
exec "$PY" "$E/packaging/package.py" "$@"
