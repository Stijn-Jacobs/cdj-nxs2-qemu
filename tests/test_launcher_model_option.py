# SPDX-License-Identifier: GPL-2.0-or-later
"""--model: the player is a profile in models/, chosen by setup (asked, or
--model) and stored in cdj.conf; an older player starts as one MAIN emulator
window through launcher/deck.py while the NXS2 keeps its own path."""

import io
import os
import sys
from types import SimpleNamespace

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import conf, deck, model, start  # noqa: E402
from launcher.console import Console  # noqa: E402
from launcher.layout import Layout  # noqa: E402
from launcher.setup import Options, Setup, parse_args  # noqa: E402


def test_older_players_are_single_window_decks_with_a_display_update():
    for mid, upd in (("cdj2000", "C2KGUI.UPD"), ("cdj2000nxs", "C2KNXS.UPD")):
        m = model.load(mid)
        assert not m.is_rig
        assert m.display_upd == upd
        assert m.images[-1] == model.DISPLAY_UPD_IMAGE
    nxs2 = model.load()
    assert nxs2.is_rig and nxs2.display_upd == ""
    assert model.DISPLAY_UPD_IMAGE not in nxs2.images


def test_conf_keeps_the_model_first_and_starts_without_one():
    assert conf.KEYS[0] == "CDJ_MODEL"
    assert conf.with_start_defaults({})["CDJ_MODEL"] == ""
    assert conf.parse(conf.render({"CDJ_MODEL": "cdj2000"}))["CDJ_MODEL"] == "cdj2000"


def test_setup_takes_model_as_an_option():
    assert parse_args(["--model", "cdj2000"]).model == "cdj2000"


def _setup(tmp_path, stored="", conf_exists=False, answers=None, **opts):
    o = Options()
    o.yes = answers is None
    for k, v in opts.items():
        setattr(o, k, v)
    s = Setup(o)
    s.lay = Layout()
    s.lay.conf = str(tmp_path / "cdj.conf")
    if conf_exists:
        (tmp_path / "cdj.conf").write_text("CDJ_DECKS=1\n")
    s.c = {"CDJ_MODEL": stored}
    s.con = Console(dry=False, interactive=answers is not None)
    return s


def _answer(monkeypatch, text):
    monkeypatch.setattr(sys, "stdin", io.StringIO(text))


def test_fresh_setup_asks_and_enter_keeps_the_nxs2(tmp_path, monkeypatch, capsys):
    _answer(monkeypatch, "\n")
    s = _setup(tmp_path, answers=True)
    s.choose_model()
    assert s.model.id == "cdj2000nxs2" and s.c["CDJ_MODEL"] == "cdj2000nxs2"
    out = capsys.readouterr()
    assert all(m in out.out for m in model.list_models())
    assert "which player?" in out.err


def test_fresh_setup_takes_the_players_name(tmp_path, monkeypatch):
    _answer(monkeypatch, "cdj2000\n")
    s = _setup(tmp_path, answers=True)
    s.choose_model()
    assert s.model.id == "cdj2000" and s.c["CDJ_MODEL"] == "cdj2000"


def test_model_option_skips_the_question(tmp_path, monkeypatch, capsys):
    _answer(monkeypatch, "")
    s = _setup(tmp_path, answers=True, model="cdj2000nxs")
    s.choose_model()
    assert s.model.id == "cdj2000nxs"
    assert "which player?" not in capsys.readouterr().err


def test_a_run_that_cannot_ask_keeps_the_stored_player(tmp_path):
    s = _setup(tmp_path, stored="cdj2000", conf_exists=True)
    s.choose_model()
    assert s.model.id == "cdj2000"
    s = _setup(tmp_path / "none")
    s.choose_model()
    assert s.model.id == "cdj2000nxs2"


def test_rerun_offers_another_player(tmp_path, monkeypatch, capsys):
    _answer(monkeypatch, "\n")  # "choose a different one?" defaults to no
    s = _setup(tmp_path, stored="cdj2000", conf_exists=True, answers=True)
    s.choose_model()
    assert s.model.id == "cdj2000"
    assert "choose a different one?" in capsys.readouterr().err
    _answer(monkeypatch, "y\ncdj2000nxs\n")
    s = _setup(tmp_path, stored="cdj2000", conf_exists=True, answers=True)
    s.choose_model()
    assert s.model.id == "cdj2000nxs"


def test_unknown_model_is_refused(tmp_path):
    s = _setup(tmp_path, model="nope")
    with pytest.raises(SystemExit):
        s.choose_model()


@pytest.fixture
def tree(tmp_path, monkeypatch):
    lay = SimpleNamespace(root=str(tmp_path), extract=str(tmp_path / "extract"), logs=str(tmp_path / "logs"),
                          usb_image=str(tmp_path / "extract" / "usbmedia3.img"), packaged=False)
    monkeypatch.setattr(deck, "Layout", lambda: lay)
    (tmp_path / "extract" / "cdj2000").mkdir(parents=True)
    return lay


def test_deck_wants_its_firmware_and_the_stick(tree, capsys):
    m = model.load("cdj2000")
    assert deck.start(m, {}, dry=True) == 1
    assert "./setup.sh --model cdj2000 --firmware" in capsys.readouterr().err
    for name in m.images:
        open(os.path.join(tree.extract, "cdj2000", name), "wb").close()
    assert deck.start(m, {}, dry=True) == 1
    assert "no USB image" in capsys.readouterr().err


def test_deck_command_is_one_qemu_with_the_display_update_and_stick(tree):
    m = model.load("cdj2000")
    argv, env = deck.command(tree, m, {"MAIN_QEMU": "/q/qemu-system-sh4", "MAIN_ARGS": "-monitor none"})
    assert argv[0].endswith("qemu-system-sh4") and argv[1:3] == ["-M", "cdj2000"]
    assert argv[-2:] == ["-monitor", "none"]
    assert any(a.endswith("main_unpacked.bin") for a in argv)
    assert any("usbmedia3.img" in a and "snapshot=on" in a for a in argv)
    assert env["CDJ_ATA"] == "1" and env["CDJ_BF531_UPD"].endswith("cdj2000/display.upd")


def test_deck_without_a_display_board_gets_no_display_update(tree):
    m = model.load("xdj1000")
    assert not m.is_rig and m.images == ("main_unpacked.bin",)
    argv, env = deck.command(tree, m, {"MAIN_QEMU": "/q/qemu-system-sh4"})
    assert argv[1:3] == ["-M", "xdj1000"] and "CDJ_BF531_UPD" not in env


def test_deck_without_a_display_board_plans_its_snapshot(tree, tmp_path):
    for mid in ("xdj1000", "xdj700", "cdj900nxs"):
        m = model.load(mid)
        assert m.idle_s and m.load_steps
        tree.tmp, tree.run = str(tmp_path), str(tmp_path)
        qemu, kernel = tmp_path / "qemu", tmp_path / mid
        for f in (qemu, kernel, tree.usb_image):
            os.makedirs(os.path.dirname(str(f)), exist_ok=True)
            open(str(f), "wb").close()
        argv = [str(qemu), "-kernel", str(kernel)]
        reach = deck.plan_snapshot(tree, m, argv, {"SNAPSHOT": "loaded"})
        assert reach and "--reach" in reach and "-monitor" in argv


def test_deck_plays_sound_unless_told_not_to(tree):
    m = model.load("cdj2000")
    argv, env = deck.command(tree, m, {"MAIN_QEMU": "/q/qemu-system-sh4"})
    assert "-audio" in argv and env["CDJ_DSP_AUDIO"].startswith("1:")
    argv, env = deck.command(tree, m, {"MAIN_QEMU": "/q/qemu-system-sh4", "NOSOUND": "1"})
    assert "-audio" not in argv and "CDJ_DSP_AUDIO" not in env


def test_deck_dry_run_says_what_is_skipped(tree, capsys):
    m = model.load("cdj2000nxs")
    os.makedirs(os.path.join(tree.extract, "cdj2000nxs"))
    for name in m.images:
        open(os.path.join(tree.extract, "cdj2000nxs", name), "wb").close()
    open(tree.usb_image, "wb").close()
    assert deck.start(m, {"MAIN_QEMU": "/q/qemu-system-sh4"}, dry=True) == 0
    out = capsys.readouterr().out
    assert "CDJ-2000NXS: one window" in out and "skipped" in out
    assert "-M cdj2000nxs " in out


def test_start_model_needs_a_value(capsys):
    assert start.main(["--model"]) == 2
    assert "--model needs a player" in capsys.readouterr().err
