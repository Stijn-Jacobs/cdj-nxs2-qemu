# SPDX-License-Identifier: GPL-2.0-or-later
"""The stamp beside an installed DSP module, and what start-up does with it."""

import os
import sys
from types import SimpleNamespace

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import deck, dsp_module, model  # noqa: E402


@pytest.fixture
def lay(tmp_path):
    return SimpleNamespace(emu=EMU, jit_cache=str(tmp_path / "c14gen"), packaged=False)


def _install(lay, m, **changed):
    folder = os.path.join(lay.jit_cache, m.module_dir)
    os.makedirs(folder)
    with open(os.path.join(folder, "m.so"), "wb"):
        pass
    dsp_module.write(folder, dict(dsp_module.current(lay, m), **changed))
    return folder


def test_the_current_stamp_names_generator_core_firmware_and_player(lay):
    stamp = dsp_module.current(lay, model.load("xdj1000"))
    assert set(stamp) == {"generator", "abi", "firmware", "model"}
    assert stamp["generator"].isdigit() and stamp["abi"].isdigit()
    assert stamp["model"] == "xdj1000" and len(stamp["firmware"]) == 64


def test_a_module_built_from_the_current_code_loads_silently(lay):
    m = model.load("xdj1000")
    _install(lay, m)
    found = dsp_module.check(lay, m)
    assert found.path.endswith("curated-xdj1000" + os.sep + "m.so") and not found.notice


@pytest.mark.parametrize("key", ["generator", "abi", "firmware", "model"])
def test_a_module_built_from_something_else_is_not_loaded(lay, key):
    m = model.load("xdj1000")
    _install(lay, m, **{key: "other"})
    found = dsp_module.check(lay, m)
    assert found.path == "" and key in " ".join(found.notice)


def test_a_module_without_a_stamp_loads_with_a_notice(lay):
    m = model.load("xdj1000")
    folder = _install(lay, m)
    os.remove(os.path.join(folder, dsp_module.STAMP))
    found = dsp_module.check(lay, m)
    assert found.path and "stamped" in " ".join(found.notice)


def test_a_missing_module_says_how_to_build_it(lay):
    found = dsp_module.check(lay, model.load("cdj2000"))
    assert found.path == "" and "./setup.sh --model cdj2000" in " ".join(found.notice)


def test_start_up_leaves_a_stale_module_out(lay, capsys):
    m = model.load("xdj1000")
    _install(lay, m, generator="0")
    assert "C66X_JIT" not in deck.dsp_env(lay, m, {})
    assert "not loaded" in capsys.readouterr().out
    assert "C66X_JIT" not in deck.dsp_env(lay, m, {"MODULE": "none"})
    assert capsys.readouterr().out == ""


def test_dsp_stamp_writes_the_stamp_the_check_reads(tmp_path, monkeypatch):
    monkeypatch.setenv("CDJ_MODEL", "xdj1000")
    assert dsp_module.main([str(tmp_path)]) == 0
    lay = SimpleNamespace(emu=EMU)
    assert dsp_module.differences(str(tmp_path), dsp_module.current(lay, model.load("xdj1000"))) == []
    assert dsp_module.differences(str(tmp_path / "nowhere"), {}) is None
