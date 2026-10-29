# SPDX-License-Identifier: GPL-2.0-or-later
"""The stamp beside an installed DSP module, and what start-up does with it."""

import os
import sys
from types import SimpleNamespace

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import deck, dsp_module, host, model, start  # noqa: E402
from launcher.layout import Layout  # noqa: E402


@pytest.fixture
def lay(tmp_path):
    return SimpleNamespace(emu=EMU, jit_cache=str(tmp_path / "c14gen"), packaged=False,
                           extract=str(tmp_path / "extract"), root=str(tmp_path))


def _install(lay, m, **changed):
    folder = os.path.join(lay.jit_cache, m.module_dir)
    os.makedirs(folder)
    with open(os.path.join(folder, "m.so"), "wb"):
        pass
    dsp_module.write(folder, dict(dsp_module.current(lay, m), **changed))
    return folder


def test_the_current_stamp_names_generator_core_firmware_and_player(lay):
    stamp = dsp_module.current(lay, model.load("xdj1000"))
    assert set(stamp) == {"generator", "revision", "abi", "firmware", "model"}
    assert stamp["generator"].isdigit() and stamp["revision"].isdigit() and stamp["abi"].isdigit()
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


def test_deck_loads_supplement_after_curated_and_rejects_both_if_stale(lay):
    m = model.load("cdj2000nxs")
    folder = _install(lay, m)
    extra = os.path.join(folder, "m.extra.so")
    open(extra, "wb").close()
    sep = ";" if host.is_windows() else ":"
    assert deck.dsp_env(lay, m, {})["C66X_JIT"] == sep.join(
        host.native(p) for p in [os.path.join(folder, "m.so"), extra])
    dsp_module.write(folder, dict(dsp_module.current(lay, m), generator="0"))
    assert "C66X_JIT" not in deck.dsp_env(lay, m, {})


def test_dsp_stamp_writes_the_stamp_the_check_reads(tmp_path, monkeypatch):
    monkeypatch.setenv("CDJ_MODEL", "xdj1000")
    assert dsp_module.main([str(tmp_path)]) == 0
    assert dsp_module.differences(str(tmp_path), dsp_module.current(Layout(), model.load("xdj1000"))) == []
    assert dsp_module.differences(str(tmp_path / "nowhere"), {}) is None


def test_the_stamp_follows_the_installed_firmware_image(lay):
    m = model.load()
    official = dsp_module.current(lay, m)["firmware"]
    os.makedirs(lay.extract)
    with open(os.path.join(lay.extract, "main_unpacked.bin"), "wb") as f:
        f.write(b"a modded kernel")
    assert dsp_module.current(lay, m)["firmware"] != official


def _without_revision(folder):
    path = os.path.join(folder, dsp_module.STAMP)
    with open(path, encoding="utf-8") as f:
        lines = [line for line in f if not line.startswith("revision=")]
    with open(path, "w", encoding="utf-8") as f:
        f.writelines(lines)


def test_a_module_of_an_older_revision_loads_silently_and_is_due_an_upgrade(lay):
    m = model.load("xdj1000")
    assert not dsp_module.upgrade_due(lay, m)
    folder = _install(lay, m)
    assert not dsp_module.upgrade_due(lay, m)
    _without_revision(folder)
    found = dsp_module.check(lay, m)
    assert found.path and not found.notice
    assert dsp_module.upgrade_due(lay, m)


def test_no_upgrade_is_offered_for_a_module_that_is_not_loaded(lay):
    m = model.load("xdj1000")
    folder = _install(lay, m, generator="0")
    _without_revision(folder)
    assert not dsp_module.upgrade_due(lay, m)


def test_a_declined_upgrade_is_not_offered_again_until_the_next_revision(lay):
    m = model.load("xdj1000")
    _without_revision(_install(lay, m))
    dsp_module.decline_upgrade(lay, m)
    assert not dsp_module.upgrade_due(lay, m)
    with open(os.path.join(lay.jit_cache, m.module_dir, dsp_module.DECLINED), "w") as f:
        f.write("0\n")
    assert dsp_module.upgrade_due(lay, m)


@pytest.fixture
def old_module(lay, monkeypatch):
    m = model.load("xdj1000")
    _without_revision(_install(lay, m))
    ran = []
    monkeypatch.setattr(start.subprocess, "run", lambda cmd, **kw: ran.append(cmd) or SimpleNamespace(returncode=0))
    monkeypatch.setattr(start.host, "find_bash", lambda: "bash")
    monkeypatch.setattr(start.host, "posix", lambda p: p)
    lay.scripts = os.path.join(EMU, "scripts")
    return lay, m, ran


def _ask(monkeypatch, answer, tty=True):
    monkeypatch.setattr(start, "stdin_is_tty", lambda: tty)
    monkeypatch.setattr("builtins.input", lambda prompt="": answer)


def test_start_up_builds_the_faster_module_on_yes(old_module, monkeypatch):
    lay, m, ran = old_module
    _ask(monkeypatch, "y")
    start._offer_faster_module(lay, m, {}, False)
    assert len(ran) == 1 and ran[0][1].endswith("build_dsp_module.sh")


def test_start_up_remembers_a_no(old_module, monkeypatch):
    lay, m, ran = old_module
    _ask(monkeypatch, "")
    start._offer_faster_module(lay, m, {}, False)
    assert not ran and not dsp_module.upgrade_due(lay, m)


@pytest.mark.parametrize("env, tty, dry", [({}, False, False), ({}, True, True), ({"MODULE": "none"}, True, False),
                                           ({"C66X_JIT": "x"}, True, False)])
def test_start_up_never_asks_without_a_terminal_or_a_module_to_replace(old_module, monkeypatch, env, tty, dry):
    lay, m, ran = old_module
    _ask(monkeypatch, "y", tty)
    monkeypatch.setattr("builtins.input", lambda prompt="": pytest.fail("asked"))
    start._offer_faster_module(lay, m, env, dry)
    assert not ran and dsp_module.upgrade_due(lay, m)
