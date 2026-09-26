# SPDX-License-Identifier: GPL-2.0-or-later
"""launcher/firmware.py's model-driven parts: which files an update resolves
to, how the several-file section step differs from the container split, and
that a mismatch or an unknown model is reported by its own title, not always
the CDJ-2000NXS2's."""

import hashlib
import os
import sys

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import firmware, model  # noqa: E402


def test_default_constants_are_the_nxs2_profile():
    m = model.load()
    assert firmware.UPD_SHA256 == m.upd_sha256[0]
    assert firmware.EXPECTED == m.expected
    assert firmware.IMAGES == tuple(os.path.basename(rel) for rel, _ in m.expected)


def test_installed_checks_the_named_models_images(tmp_path):
    m = model.load("cdj2000")
    assert not firmware.installed(str(tmp_path), "cdj2000")
    for rel, _ in m.expected:
        (tmp_path / os.path.basename(rel)).write_bytes(b"\0")
    assert firmware.installed(str(tmp_path), "cdj2000")
    # A different model's images are not enough.
    assert not firmware.installed(str(tmp_path))


def test_upd_files_single_file_update(tmp_path):
    m = model.load()  # cdj2000nxs2: one update file
    upd = tmp_path / "C2KNXS2.UPD"
    upd.write_bytes(b"whatever")
    assert firmware._upd_files(m, str(upd)) == [str(upd)]


def test_upd_files_several_files_from_their_folder(tmp_path):
    m = model.load("cdj2000")
    for n in m.upd:
        (tmp_path / n).write_bytes(n.encode())
    got = firmware._upd_files(m, str(tmp_path))
    assert got == [str(tmp_path / n) for n in m.upd]


def test_upd_files_reports_the_missing_name_by_model_title(tmp_path):
    m = model.load("cdj2000")
    (tmp_path / m.upd[0]).write_bytes(b"x")
    with pytest.raises(firmware.FirmwareError, match="CDJ-2000 needs %s" % m.upd[1]):
        firmware._upd_files(m, str(tmp_path))


def test_upd_files_no_such_path(tmp_path):
    with pytest.raises(firmware.FirmwareError, match="no such file or folder"):
        firmware._upd_files(model.load(), str(tmp_path / "missing.upd"))


def test_do_sections_several_files_are_copied_in_order(tmp_path):
    m = model.load("cdj2000")
    out = tmp_path / "out"
    (out / "extract").mkdir(parents=True)
    upd_files = []
    for n in m.upd:
        p = tmp_path / n
        p.write_bytes(n.encode())
        upd_files.append(str(p))
    log = out / "log-sections.log"
    assert firmware._do_sections([], "", str(out), upd_files, str(log))
    for i, n in enumerate(m.upd, 1):
        assert (out / "extract" / ("section%d.bin" % i)).read_bytes() == n.encode()
    assert "section1.bin <- %s" % m.upd[0] in log.read_text()


def test_do_sections_one_file_runs_split_update(tmp_path):
    m = model.load()
    out = tmp_path / "out"
    (out / "extract").mkdir(parents=True)
    upd = tmp_path / "C2KNXS2.UPD"
    upd.write_bytes(b"5\r\n3\r\n" + b"HELLO" + b"abc")
    tools = os.path.join(EMU, "scripts", "firmware")
    log = out / "log-sections.log"
    assert firmware._do_sections([sys.executable], tools, str(out), [str(upd)], str(log))
    assert (out / "extract" / "section1.bin").read_bytes() == b"HELLO"
    assert (out / "extract" / "section2.bin").read_bytes() == b"abc"


def _sha256(data):
    return hashlib.sha256(data).hexdigest()


def test_prepare_names_the_model_on_a_hash_mismatch(tmp_path):
    m = model.load("cdj2000")
    for n in m.upd:
        (tmp_path / n).write_bytes(b"not the real update")
    said = []
    with pytest.raises(firmware.FirmwareError, match=r"(?s)is not the CDJ-2000 v4\.33 update .*Only v4\.33 is supported"):
        firmware.prepare(str(tmp_path), model=m, say=said.append)


def test_main_unknown_model_is_reported_without_a_prefix(capsys):
    assert firmware.main(["--model", "bogus", "x.upd"]) == 1
    assert capsys.readouterr().err.startswith("unknown model 'bogus'")


def test_main_model_needs_a_value(capsys):
    assert firmware.main(["--model"]) == 1
    assert "--model needs a model id" in capsys.readouterr().err


def test_main_model_flag_is_forwarded(tmp_path, capsys):
    missing = str(tmp_path / "no.upd")
    assert firmware.main(["--model", "cdj2000", missing]) == 1
    assert "no such file or folder: %s" % missing in capsys.readouterr().err
