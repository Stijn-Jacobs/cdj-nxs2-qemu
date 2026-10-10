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
    assert model.list_models() == ["cdj2000nxs2", "xdj1000mk2", "xdj700", "xdj1000", "cdj900nxs",
                                  "cdj2000nxs", "cdj900", "cdj2000"]


def test_default_first_then_newest_first():
    released = {m: model._released(m) for m in model.list_models()}
    rest = model.list_models()[1:]
    assert model.list_models()[0] == model.DEFAULT
    assert rest == sorted(rest, key=lambda m: -released[m])
    assert released["cdj900"] > released["cdj2000"]


def test_default_is_the_nxs2():
    m = model.load()
    assert (m.id, m.title, m.main_machine, m.gui_machine) == (
        "cdj2000nxs2", "CDJ-2000NXS2", "cdj2000nxs2", "sh7269gui")
    assert m.extract == "extract"
    assert m.upd == ["C2KNXS2.UPD"]
    assert m.fw_steps == ["sections", "srec_coverage", "lzss_decode", "gui_decode",
                          "gui_resources", "gui_artwork", "make_settings", "make_flash"]
    assert ("extract/flash.bin", "481d42e5843bc70d3a93df640878adb0d7035497e3b233b5cc85c14030a82563") in m.expected


def test_bring_up_models_have_no_gui_board():
    for mid in ("cdj2000", "cdj2000nxs"):
        m = model.load(mid)
        assert m.gui_machine == ""
        assert m.fw_steps == ["sections", "srec_coverage", "lzss_decode", "gui_decode"]


def test_every_profile_names_its_images_and_decodes_the_sparse_section():
    # No MODEL_EXPECTED made firmware.installed() vacuously true, and
    # lzss_decode reads the section<N>.sparse.bin that srec_coverage writes.
    for mid in model.list_models():
        m = model.load(mid)
        assert "extract/main_unpacked.bin" in [rel for rel, _ in m.expected], mid
        if "lzss_decode" in m.fw_steps:
            assert "srec_coverage" in m.fw_steps[:m.fw_steps.index("lzss_decode")], mid


def test_the_nxs2_platform_models_start_the_two_board_rig():
    # A profile without MODEL_LAUNCH=deck falls back to the rig, which boots
    # the NXS2 machines whatever the title says.
    rigs = [mid for mid in model.list_models() if model.load(mid).is_rig]
    assert rigs == [model.DEFAULT]


def test_deck_model_may_not_share_the_default_extract_folder(tmp_path, monkeypatch):
    text = open(os.path.join(model.models_dir(), "cdj2000.conf"), encoding="utf-8").read()
    (tmp_path / "shared.conf").write_text(text.replace("MODEL_EXTRACT=extract/cdj2000", "MODEL_EXTRACT=extract"),
                                          encoding="utf-8")
    monkeypatch.setattr(model, "models_dir", lambda: str(tmp_path))
    with pytest.raises(model.ModelError, match="deck model installs to its own folder"):
        model.load("shared")


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
    with pytest.raises(model.ModelError, match=r"unknown model 'nope'; known: cdj2000nxs2 "):
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


def test_dsp_module_models_name_their_idle_loop():
    for mid, head in (("cdj2000nxs", "0xC004CB8C"), ("xdj1000", "0xC004CD0C")):
        m = model.load(mid)
        assert m.has_dsp_module
        assert m.dsp_idle == head + ":0x11804AE0:0x11805C00"
        assert m.dsp_isr_fast == "1"
        assert m.dsp_gen_args.split() == ["--wide-mem", "--ret-predict", "16"]
        assert m.module_dir == "curated-" + mid
    c2k = model.load("cdj2000")
    assert c2k.has_dsp_module
    assert c2k.dsp_idle == "0x80047B80:0x10005000:0x10006600"
    assert c2k.dsp_idle_knob == "CDJ_C6727_IDLE"
    assert c2k.module_dir == "curated-cdj2000"
    for mid in ("cdj2000nxs2", "xdj700"):
        assert not model.load(mid).has_dsp_module
    assert model.load().module_dir == "curated"
