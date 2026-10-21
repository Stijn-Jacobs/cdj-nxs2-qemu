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
    assert not firmware.installed(str(tmp_path), "cdj2000")  # the display update is part of the install
    (tmp_path / model.DISPLAY_UPD_IMAGE).write_bytes(b"\0")
    assert firmware.installed(str(tmp_path), "cdj2000")
    # A different model's images are not enough.
    assert not firmware.installed(str(tmp_path))


def test_a_profile_naming_no_images_is_not_installed(tmp_path, monkeypatch):
    m = model.load("xdj1000mk2")
    monkeypatch.setattr(m, "expected", ())
    monkeypatch.setattr(model, "load", lambda mid=None: m)
    assert not firmware.installed(str(tmp_path), "xdj1000mk2")


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


def _fake_unpack(monkeypatch, image):
    """A one-step unpack that leaves `image` as the update's only image."""
    def sections(py, tools, out, upd_files, log):
        with open(os.path.join(out, "extract", "main_unpacked.bin"), "wb") as f:
            f.write(image)
        return True
    monkeypatch.setattr(firmware, "_do_sections", sections)


def _one_image_model(monkeypatch, official_image):
    m = model.load()
    monkeypatch.setattr(m, "fw_steps", ["sections"])
    monkeypatch.setattr(m, "expected", (("extract/main_unpacked.bin", _sha256(official_image)),))
    return m


def test_version_labels_are_listed_once_in_file_order(tmp_path):
    upd = tmp_path / "C2KNXS2.UPD"
    upd.write_bytes(b"GUI Ver1.81 xx Ver1.00 yy Ver1.85 zz Ver1.00")
    assert firmware.version_labels(str(upd)) == ["1.81", "1.00", "1.85"]


def test_a_foreign_update_is_refused_unless_any_firmware_is_given(tmp_path):
    upd = tmp_path / "C2KNXS2.UPD"
    upd.write_bytes(b"Ver1.70")
    with pytest.raises(firmware.FirmwareError, match=r"(?s)Only v1\.87 is supported.*--any-firmware"):
        firmware.prepare(str(upd), model=model.load(), say=lambda s: None)


def test_any_firmware_installs_a_different_image_and_it_shows_as_differing(tmp_path, monkeypatch):
    m = _one_image_model(monkeypatch, b"official")
    _fake_unpack(monkeypatch, b"modded")
    upd = tmp_path / "C2KNXS2.UPD"
    upd.write_bytes(b"Ver1.99")
    dest = tmp_path / "extract"
    said = []
    firmware.prepare(str(upd), out=str(tmp_path / "out"), install_to=str(dest), model=m,
                     say=said.append, any_firmware=True)
    assert (dest / "main_unpacked.bin").read_bytes() == b"modded"
    assert firmware.differing_images(str(dest), m) == ["main_unpacked.bin"]
    assert any(line.startswith("WARNING: C2KNXS2.UPD is not") and "Ver1.99" in line for line in said)
    assert any(line.startswith("  DIFF  extract/main_unpacked.bin") for line in said)


def test_any_firmware_still_fails_when_an_image_is_not_produced(tmp_path, monkeypatch):
    m = _one_image_model(monkeypatch, b"official")
    monkeypatch.setattr(firmware, "_do_sections", lambda py, tools, out, upd_files, log: True)
    upd = tmp_path / "C2KNXS2.UPD"
    upd.write_bytes(b"x")
    with pytest.raises(firmware.FirmwareError, match="is missing"):
        firmware.prepare(str(upd), out=str(tmp_path / "out"), install_to=str(tmp_path / "extract"), model=m,
                         say=lambda s: None, any_firmware=True)
    assert not (tmp_path / "extract").exists()


def test_an_official_reinstall_no_longer_differs(tmp_path, monkeypatch):
    m = _one_image_model(monkeypatch, b"official")
    dest = tmp_path / "extract"
    dest.mkdir()
    (dest / "main_unpacked.bin").write_bytes(b"modded")
    assert firmware.differing_images(str(dest), m) == ["main_unpacked.bin"]
    _fake_unpack(monkeypatch, b"official")
    monkeypatch.setitem(m.__dict__, "upd_sha256", [_sha256(b"Ver1.87")])
    upd = tmp_path / "C2KNXS2.UPD"
    upd.write_bytes(b"Ver1.87")
    firmware.prepare(str(upd), out=str(tmp_path / "out"), install_to=str(dest), force=True, model=m,
                     say=lambda s: None)
    assert firmware.differing_images(str(dest), m) == []


def test_main_accepts_any_firmware(tmp_path, capsys):
    missing = str(tmp_path / "no.upd")
    assert firmware.main(["--any-firmware", missing]) == 1
    assert "no such file or folder" in capsys.readouterr().err
