# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetching baken (github.com/M-Igashi/baken, MIT), the tool that writes a
Pioneer USB export straight from a plain folder of music. Installed into
this project's own tools/ (never system-wide), from the matching prebuilt
release asset, checksum-verified against the release's own checksums file;
`cargo install baken` is the documented fallback when no asset matches this
OS/CPU or the download fails.
"""

import hashlib
import json
import os
import platform
import shutil
import stat
import subprocess
import tarfile
import urllib.error
import urllib.request
import zipfile

from . import host

REPO = "M-Igashi/baken"
_API_LATEST = "https://api.github.com/repos/%s/releases/latest" % REPO
_UA = {"User-Agent": "cdj-nxs2-re-setup"}


def binary_name():
    return "baken.exe" if host.is_windows() else "baken"


def _install_dir(lay):
    return os.path.join(lay.tools, "baken")


def find(lay):
    """An already-usable baken: this project's own copy, then one already on
    PATH (a `cargo install baken` done by hand puts it in ~/.cargo/bin)."""
    own = os.path.join(_install_dir(lay), binary_name())
    if os.path.isfile(own) and (host.is_windows() or os.access(own, os.X_OK)):
        return own
    return shutil.which("baken")


def _asset_suffix():
    """The release asset name's tail for this OS and CPU, or None when baken
    ships no prebuilt binary for it (cargo install is then the only route)."""
    machine = platform.machine().lower()
    k = host.kind()
    if k == host.WINDOWS:
        return "windows-x86_64.zip" if machine in ("amd64", "x86_64") else None
    if k == host.MACOS:
        return "macos-universal.tar.gz"  # one universal binary, either CPU
    if k == host.LINUX:
        if machine in ("x86_64", "amd64"):
            return "linux-x86_64.tar.gz"
        if machine in ("aarch64", "arm64"):
            return "linux-aarch64.tar.gz"
        return None
    return None


def _get(url, timeout):
    req = urllib.request.Request(url, headers=_UA)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _extract(archive, into):
    if archive.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(into)
    else:
        with tarfile.open(archive) as t:
            t.extractall(into)


def _find_binary(root, name):
    for d, _dirs, files in os.walk(root):
        if name in files:
            return os.path.join(d, name)
    return None


def fetch(con, lay):
    """Download and verify the release asset for this OS/CPU into
    lay.tools/baken/. Returns the binary's path, or None (the reason is
    already printed with con.warn)."""
    suffix = _asset_suffix()
    if not suffix:
        con.warn("no baken release asset for this OS/CPU (%s, %s)" % (host.kind(), platform.machine()))
        return None
    try:
        release = json.loads(_get(_API_LATEST, 20))
    except (urllib.error.URLError, OSError, ValueError) as e:
        con.warn("could not reach github.com/%s/releases: %s" % (REPO, e))
        return None
    assets = {a["name"]: a["browser_download_url"] for a in release.get("assets", [])}
    asset = next((n for n in assets if n.endswith(suffix)), None)
    checks = next((n for n in assets if n.endswith("-checksums.txt")), None)
    if not asset or not checks:
        con.warn("release %s has no %s asset" % (release.get("tag_name", "?"), suffix))
        return None

    install_dir = _install_dir(lay)
    os.makedirs(install_dir, exist_ok=True)
    archive = os.path.join(install_dir, asset)
    try:
        with open(archive, "wb") as f:
            f.write(_get(assets[asset], 120))
        checksums = _get(assets[checks], 20).decode("utf-8", "replace")
    except (urllib.error.URLError, OSError) as e:
        con.warn("download failed: %s" % e)
        return None

    want = next((ln.split()[0] for ln in checksums.splitlines() if ln.strip().endswith(asset)), None)
    got = _sha256(archive)
    if not want or got != want:
        con.warn("checksum mismatch for %s (got %s, release says %s)" % (asset, got, want or "?"))
        os.remove(archive)
        return None

    _extract(archive, install_dir)
    os.remove(archive)
    name = binary_name()
    binary = _find_binary(install_dir, name)
    if not binary:
        con.warn("%s did not contain %s" % (asset, name))
        return None
    if binary != os.path.join(install_dir, name):
        shutil.move(binary, os.path.join(install_dir, name))
        binary = os.path.join(install_dir, name)
    if not host.is_windows():
        os.chmod(binary, os.stat(binary).st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    con.good("baken %s (%s) -> %s" % (release.get("tag_name", "?"), asset, os.path.relpath(binary, lay.root)))
    return binary


def ensure(con, lay, o):
    """An installed baken, fetching it if needed; None (with the reason
    already printed) when neither the download nor cargo install worked."""
    p = find(lay)
    if p:
        return p
    if o.dry:
        con.would("download the matching baken release asset (github.com/%s) into %s"
                  % (REPO, os.path.relpath(_install_dir(lay), lay.root)))
        return None
    con.info("baken (MIT, github.com/%s) writes the USB export from a plain folder of music." % REPO)
    p = fetch(con, lay)
    if p:
        return p
    cargo = shutil.which("cargo")
    if not cargo:
        con.warn("no prebuilt baken for this machine, and no cargo: install Rust (https://rustup.rs),")
        con.warn("then run:  cargo install baken")
        return None
    con.info("falling back to the documented cargo install (needs network, a few minutes)")
    if con.ask_yn("run 'cargo install baken' now?", "y"):
        if subprocess.run([cargo, "install", "baken"]).returncode == 0:
            return shutil.which("baken")
        con.warn("cargo install baken failed; see the output above")
    return None
