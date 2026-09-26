# SPDX-License-Identifier: GPL-2.0-or-later
"""models/<id>.conf as launcher/model.py reads it: the profiles committed in
models/ must parse the way cdj_model.sh's shell sourcing does, including the
one multi-line quoted field (MODEL_EXPECTED)."""

import os
import sys

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import model  # noqa: E402


def test_lists_the_committed_profiles():
    assert model.list_models() == ["cdj2000", "cdj2000nxs", "cdj2000nxs2"]


def test_default_is_the_nxs2():
    m = model.load()
    assert (m.id, m.title, m.main_machine, m.gui_machine) == (
        "cdj2000nxs2", "CDJ-2000NXS2", "cdj2000nxs2", "sh7269gui")
    assert m.extract == "extract"
    assert m.upd == ["C2KNXS2.UPD"]
    assert m.fw_steps == ["sections", "srec_coverage", "lzss_decode", "gui_decode",
                          "gui_resources", "gui_artwork", "make_settings", "make_flash"]
    assert ("extract/flash.bin", "c52253efd5e926968b892fcf1b762279f7f2119bc89426280e90d301ce8ee203") in m.expected


def test_bring_up_models_have_no_gui_board():
    for mid in ("cdj2000", "cdj2000nxs"):
        m = model.load(mid)
        assert m.gui_machine == ""
        assert m.fw_steps == ["sections", "srec_coverage", "lzss_decode", "gui_decode"]


def test_cdj2000_update_is_four_files_in_section_order():
    m = model.load("cdj2000")
    assert m.upd == ["C2KGUI.UPD", "C2KDRIV.UPD", "C2KMAIN.UPD", "C2KPANL.UPD"]
    assert len(m.upd) == len(m.upd_sha256)
    assert m.extract == "extract/cdj2000"


def test_cdj_model_env_names_the_profile(monkeypatch):
    monkeypatch.setenv("CDJ_MODEL", "cdj2000nxs")
    assert model.load().id == "cdj2000nxs"
    assert model.load("cdj2000").id == "cdj2000"  # an explicit id wins over CDJ_MODEL


def test_unknown_model_names_the_known_ones():
    with pytest.raises(model.ModelError, match=r"unknown model 'nope'; known: cdj2000 "):
        model.load("nope")


def test_incomplete_profile_names_the_first_missing_field(tmp_path, monkeypatch):
    (tmp_path / "half.conf").write_text('MODEL_TITLE="Half Player"\nMODEL_MAIN_MACHINE=half\n', encoding="utf-8")
    monkeypatch.setattr(model, "models_dir", lambda: str(tmp_path))
    with pytest.raises(model.ModelError, match="does not set MODEL_EXTRACT"):
        model.load("half")


def test_crlf_profile_still_parses(tmp_path, monkeypatch):
    text = model.models_dir()
    with open(os.path.join(text, "cdj2000nxs2.conf"), encoding="utf-8") as f:
        original = f.read()
    (tmp_path / "crlf.conf").write_bytes(original.replace("\n", "\r\n").encode("utf-8"))
    monkeypatch.setattr(model, "models_dir", lambda: str(tmp_path))
    m = model.load("crlf")
    assert m.main_machine == "cdj2000nxs2"
    assert m.gui_machine == "sh7269gui"


def test_extract_dir_is_the_layout_extract_for_the_default_model():
    class Lay:
        root = "/repo"
        extract = "/repo/extract"

    default = model.load()
    other = model.load("cdj2000")
    assert model.extract_dir(Lay(), default) == "/repo/extract"
    assert model.extract_dir(Lay(), other) == os.path.join("/repo", "extract/cdj2000")
