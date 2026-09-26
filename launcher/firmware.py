# SPDX-License-Identifier: GPL-2.0-or-later
"""Turn one of Pioneer's public firmware updates into the images the emulator
reads, and prove each one against its known SHA-256. What the update is, how
it unpacks and what comes out are in the model's profile (models/<id>.conf);
the default model is the CDJ-2000NXS2 v1.87 (C2KNXS2.UPD).

Only the update is needed, never a device dump.

By default everything is written to a NEW directory and nothing in the
repository is touched. The output mirrors the repository layout:

  <outdir>/extract/<image>.bin       the verified images
  <outdir>/extract/section*.bin ...  intermediates
  <outdir>/log/<step>.log            each tool's own output

--install then copies the verified images into the model's folder in the
repository (extract/ for the CDJ-2000NXS2, extract/<model>/ for the others).
It refuses if any of them already exists, unless --force is also given, and
it installs nothing unless every image passed.

  usage: scripts/firmware/prepare_firmware.sh [--model <id>] [--install [--force]] <update> [outdir]

  update     the .UPD file, or for a model whose update is several files
             (the CDJ-2000) the folder holding them
  --model    a profile in models/ (default: $CDJ_MODEL, else cdj2000nxs2)
  outdir     must not exist yet, or be empty; default: a new mktemp dir.
             It may not be the repository root or lie inside its extract/,
             fw/ or notes/ -- use --install for that.
  PYTHON=    interpreter to use (default: the one running this).
             Only the standard library is needed.

Exit status: 0 when every image matches, 1 on any mismatch or error.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time

from . import model as _model
from .layout import Layout

# The default model's profile, read once: the update sha256 and the expected
# images, kept as module attributes for setup.py and the packager, which only
# ever deal with the CDJ-2000NXS2.
_DEFAULT = _model.load(_model.DEFAULT)
UPD_SHA256 = _DEFAULT.upd_sha256[0]
EXPECTED = _DEFAULT.expected
IMAGES = tuple(os.path.basename(rel) for rel, _ in EXPECTED)

# (step name -> tool, its arguments relative to <outdir>). "sections" is not
# here: it either runs split_update.py on a one-file update or just copies the
# several-file update's files, so it is handled on its own below.
STEPS = {
    "srec_coverage": lambda m: ("srec_coverage.py", ["extract/section%s.bin" % m.main_section]),
    "lzss_decode": lambda m: ("lzss_decode.py", ["extract/section%s.sparse.bin" % m.main_section, m.main_lzss]),
    "gui_decode": lambda m: ("gui_decode.py", ["extract/section%s.bin" % m.gui_section, "extract/gui_unpacked.bin"]),
    "gui_resources": lambda m: ("gui_resources.py", ["extract/resblob.bin"]),
    "gui_artwork": lambda m: ("gui_artwork.py", ["extract/artblob.bin"]),
    "make_settings": lambda m: ("make_settings.py", ["extract/settings.bin"]),
    "make_flash": lambda m: ("make_flash.py", ["extract/flash.bin", "extract/settings.bin"]),
}


class FirmwareError(Exception):
    pass


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def installed(extract, model_id=None):
    images = IMAGES if model_id is None else tuple(
        os.path.basename(rel) for rel, _ in _model.load(model_id).expected)
    return all(os.path.isfile(os.path.join(extract, f)) for f in images)


def _python():
    if os.environ.get("PYTHON"):
        return [os.environ["PYTHON"]]
    from . import host

    return host.python_argv()


def _upd_files(m, src):
    """The update file(s) `src` names, in section order: `src` itself for a
    one-file update, else every MODEL_UPD name found next to it (or inside it,
    when `src` is the folder that holds them)."""
    if not os.path.exists(src):
        raise FirmwareError("no such file or folder: %s" % src)
    if len(m.upd) == 1 and os.path.isfile(src):
        return [os.path.abspath(src)]
    upd_dir = os.path.abspath(src) if os.path.isdir(src) else os.path.dirname(os.path.abspath(src))
    files = []
    for n in m.upd:
        p = os.path.join(upd_dir, n)
        if not os.path.isfile(p):
            raise FirmwareError("%s needs %s next to the other update files in %s" % (m.title, n, upd_dir))
        files.append(p)
    return files


def _do_sections(py, tools, out, upd_files, log):
    """extract/section<N>.bin, one per section, from the container or the
    files."""
    if len(upd_files) == 1:
        argv = py + [os.path.join(tools, "split_update.py"), upd_files[0], "extract"]
        with open(log, "wb") as f:
            return subprocess.run(argv, cwd=out, stdout=f, stderr=subprocess.STDOUT).returncode == 0
    with open(log, "w", encoding="utf-8") as f:
        for i, src in enumerate(upd_files, 1):
            shutil.copyfile(src, os.path.join(out, "extract", "section%d.bin" % i))
            f.write("section%d.bin <- %s\n" % (i, os.path.basename(src)))
    return True


def prepare(upd, out=None, install_to=None, force=False, root=None, model=None, say=print, prog=None):
    """Unpack and verify `upd` for `model` (the default profile when None)
    into `out` (a new temporary directory when None); with install_to, copy
    the verified images there. Raises FirmwareError with the script's message
    on any failure."""
    m = model or _model.load(None)
    tools = os.path.join(Layout().scripts, "firmware")
    if len(m.upd) != len(m.upd_sha256):
        raise FirmwareError("models/%s.conf: MODEL_UPD and MODEL_UPD_SHA256 differ in length" % m.id)
    upd_files = _upd_files(m, upd)
    for f, want in zip(upd_files, m.upd_sha256):
        got = sha256(f)
        if got != want:
            raise FirmwareError(
                "%s is not the %s v%s update (sha256 %s, want %s).\n"
                "Only v%s is supported: every address in the model and the harness is for it."
                % (f, m.title, m.fw_version, got, want, m.fw_version))
    if out:
        outc = os.path.realpath(out)
        if root:
            rootc = os.path.realpath(root)
            if os.path.normcase(outc) == os.path.normcase(rootc):
                raise FirmwareError("outdir is the repository root; write elsewhere and use --install")
            for guarded in ("extract", "fw", "notes"):
                g = os.path.normcase(os.path.join(rootc, guarded)) + os.sep
                if (os.path.normcase(outc) + os.sep).startswith(g):
                    raise FirmwareError("outdir is inside %s/; write elsewhere and use --install" % guarded)
        if os.path.exists(out):
            if not os.path.isdir(out):
                raise FirmwareError("outdir exists and is not a directory: %s" % out)
            if os.listdir(out):
                raise FirmwareError("outdir is not empty: %s" % out)
        os.makedirs(out, exist_ok=True)
    else:
        out = tempfile.mkdtemp(prefix="cdj-firmware.")
    out = os.path.abspath(out)
    for d in ("extract", "notes", "log"):
        os.makedirs(os.path.join(out, d), exist_ok=True)

    py = _python()
    version = subprocess.run(py + ["--version"], capture_output=True, text=True)
    say("python : %s (%s)" % ((version.stdout or version.stderr).strip(), " ".join(py)))
    say("model  : %s (%s)" % (m.title, m.id))
    for f in upd_files:
        say("update : %s" % f)
    say("outdir : %s" % out)
    say("update : sha256 ok (v%s)" % m.fw_version)
    say("")
    say("steps:")
    for name in m.fw_steps:
        t0 = time.time()
        log = os.path.join(out, "log", name + ".log")
        if name == "sections":
            ok = _do_sections(py, tools, out, upd_files, log)
        elif name in STEPS:
            tool, args = STEPS[name](m)
            argv = py + [os.path.join(tools, tool)] + [str(a) for a in args]
            with open(log, "wb") as f:
                ok = subprocess.run(argv, cwd=out, stdout=f, stderr=subprocess.STDOUT).returncode == 0
        else:
            raise FirmwareError("models/%s.conf: unknown step '%s'" % (m.id, name))
        if ok:
            say("  %-14s done (%d s)" % (name, time.time() - t0))
        else:
            say("  %-14s FAILED -- see %s" % (name, log))
            with open(log, encoding="utf-8", errors="replace") as f:
                sys.stderr.write("".join(f.readlines()[-20:]))
            raise FirmwareError(None)
    say("")
    say("verify:")
    fail = False
    for rel, want in m.expected:
        p = os.path.join(out, rel)
        if not os.path.isfile(p):
            say("  FAIL  %-26s missing" % rel)
            fail = True
            continue
        got = sha256(p)
        if got == want:
            say("  PASS  %-26s %s" % (rel, got))
        else:
            say("  FAIL  %-26s %s (want %s)" % (rel, got, want))
            fail = True
    say("")
    if fail:
        raise FirmwareError("at least one image does not match; nothing installed.")
    if not install_to:
        say("all images match. To use them:  %s --model %s --install %s" % (prog or "prepare_firmware", m.id, upd))
        say("(or copy the images from %s/extract/ into the repository's %s/ by hand)" % (out, m.extract))
        return out
    # Check every destination before copying anything, so a refusal leaves
    # the repository exactly as it was.
    if not force:
        clash = [rel for rel, _ in m.expected
                 if os.path.exists(os.path.join(install_to, os.path.basename(rel)))]
        if clash:
            raise FirmwareError("refusing to overwrite: %s (add --force to replace them)"
                                % " ".join(m.extract + "/" + rel[len("extract/"):] for rel in clash))
    os.makedirs(install_to, exist_ok=True)
    for rel, _ in m.expected:
        shutil.copyfile(os.path.join(out, rel), os.path.join(install_to, os.path.basename(rel)))
        say("installed %s/%s" % (m.extract, rel[len("extract/"):]))
    return out


def main(argv):
    """scripts/firmware/prepare_firmware.sh"""
    usage = __doc__[__doc__.index("  usage:"):].rstrip()
    install = force = False
    model_id = os.environ.get("CDJ_MODEL")
    args = []
    a_iter = iter(argv)
    for a in a_iter:
        if a == "--install":
            install = True
        elif a == "--force":
            force = True
        elif a == "--model":
            try:
                model_id = next(a_iter)
            except StopIteration:
                sys.stderr.write("prepare_firmware: --model needs a model id\n")
                return 1
        elif a in ("-h", "--help"):
            sys.stderr.write(usage + "\n")
            return 1
        elif a.startswith("-"):
            sys.stderr.write("prepare_firmware: unknown option %s\n" % a)
            return 1
        else:
            args.append(a)
    if not 1 <= len(args) <= 2:
        sys.stderr.write(usage + "\n")
        return 1
    if force and not install:
        sys.stderr.write("prepare_firmware: --force only means something with --install\n")
        return 1
    lay = Layout()
    try:
        m = _model.load(model_id)
    except _model.ModelError as e:
        sys.stderr.write(str(e) + "\n")
        return 1
    try:
        prepare(args[0], args[1] if len(args) == 2 else None,
                install_to=_model.extract_dir(lay, m) if install else None, force=force, root=lay.root,
                model=m, say=lambda s: print(s, flush=True), prog=os.environ.get("LAUNCHER_PROG"))
    except FirmwareError as e:
        if e.args and e.args[0]:
            sys.stderr.write("prepare_firmware: %s\n" % e.args[0])
        return 1
    return 0
