# SPDX-License-Identifier: GPL-2.0-or-later
# Sourced by build_main.sh and build_display.sh, from inside the build tree.
# Sets QEMU_PYTHON_ARG: empty, or a --python= for QEMU's configure.
#
# QEMU 9.1's configure builds its own venv and needs two things from the Python
# it starts from: tomllib (3.11+) or tomli, and distlib, which it takes from pip
# when it is not installed. pip 25 and newer no longer carry the part it uses,
# so on a current macOS neither the system Python nor Homebrew's will do, and
# configure stops with "found no usable distlib". Then a small venv in the
# build tree gets both from PyPI. QEMU_PYTHON=<python> chooses the Python.
QEMU_PYTHON_ARG=""
_qpy="${QEMU_PYTHON:-python3}"
_qpy_ok='import sys
try:
    import tomllib
except ImportError:
    import tomli
try:
    import distlib.scripts, distlib.version
except ImportError:
    import pip._vendor.distlib.scripts, pip._vendor.distlib.version'
if [ -n "${QEMU_PYTHON:-}" ] || ! "$_qpy" -c "$_qpy_ok" >/dev/null 2>&1; then
    if ! "$_qpy" -c "$_qpy_ok" >/dev/null 2>&1; then
        _qvenv="$PWD/cdj-pyvenv"
        if ! "$_qvenv/bin/python" -c "$_qpy_ok" >/dev/null 2>&1; then
            echo "$_qpy lacks tomli or distlib for QEMU's configure: a venv with them in $_qvenv"
            rm -rf "$_qvenv"
            "$_qpy" -m venv "$_qvenv" &&
                "$_qvenv/bin/python" -m pip install --quiet --disable-pip-version-check distlib tomli ||
                { echo "could not make $_qvenv (needs network access to PyPI)" >&2; exit 1; }
        fi
        _qpy="$_qvenv/bin/python"
    fi
    QEMU_PYTHON_ARG="--python=$_qpy"
fi
unset _qpy _qpy_ok _qvenv
