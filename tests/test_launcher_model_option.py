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

from launcher import conf, deck, dsp_module, firmware, host, model, rig, start  # noqa: E402
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
    s.values = {"CDJ_DECKS": "1", "CDJ_MODEL": stored} if conf_exists else {}
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


def test_reconfigure_saves_the_new_player_before_the_later_steps(tmp_path, monkeypatch):
    _answer(monkeypatch, "xdj1000mk2\n")
    s = _setup(tmp_path, stored="xdj1000", conf_exists=True, answers=True, reconfigure=True)
    s.choose_model()
    saved = conf.load(s.lay.conf)
    assert saved["CDJ_MODEL"] == "xdj1000mk2" and saved["CDJ_DECKS"] == "1"


def test_unknown_model_is_refused(tmp_path):
    s = _setup(tmp_path, model="nope")
    with pytest.raises(SystemExit):
        s.choose_model()


def _install(lay, m, **changed):
    """A module of `m` with a stamp that fits the current code."""
    folder = os.path.join(lay.jit_cache, m.module_dir)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, "m.so"), "wb"):
        pass
    dsp_module.write(folder, dict(dsp_module.current(lay, m), **changed))


@pytest.fixture
def tree(tmp_path, monkeypatch):
    lay = SimpleNamespace(emu=EMU, root=str(tmp_path), extract=str(tmp_path / "extract"), logs=str(tmp_path / "logs"),
                          usb_image=str(tmp_path / "extract" / "usbmedia3.img"), jit_cache=str(tmp_path / "c14gen"),
                          packaged=False)
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


def test_cdj900_deck_gets_its_display_processor_flash(tree):
    m = model.load("cdj900")
    assert m.images == ("main_unpacked.bin", "gui.flat")
    argv, env = deck.command(tree, m, {"MAIN_QEMU": "/q/qemu-system-sh4"})
    assert env["CDJ_M16C_GUI"].endswith("cdj900/gui.flat") and "CDJ_BF531_UPD" not in env


def test_deck_runs_another_main_image_with_the_knobs_of_that_image(tree, tmp_path, monkeypatch):
    m = model.load("cdj900nxs")
    os.makedirs(os.path.join(tree.extract, "cdj900nxs"))
    installed = os.path.join(tree.extract, "cdj900nxs", "main_unpacked.bin")
    with open(installed, "wb") as f:
        f.write(b"stock")
    other = tmp_path / "under_test.bin"
    other.write_bytes(b"modified")
    # The installed image is the supported update's; the other one is not.
    stock = firmware.stock_main_sha256(m)
    real = firmware.installed_sha256
    monkeypatch.setattr(firmware, "installed_sha256",
                        lambda path: stock if os.path.normpath(path) == os.path.normpath(installed) else real(path))
    env0 = {"MAIN_QEMU": "/q/qemu-system-sh4", "MODULE": "none"}
    argv, env = deck.command(tree, m, env0)
    assert argv[argv.index("-kernel") + 1] == host.native(installed) and env["CDJ_C6747_IDLE"] == m.dsp_idle
    argv, env = deck.command(tree, m, env0, main=str(other))
    assert argv[argv.index("-kernel") + 1] == host.native(str(other))
    # The DSP idle-loop skip is only armed on the supported update's image.
    assert "CDJ_C6747_IDLE" not in env


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


def test_deck_dsp_env_comes_from_the_profile(tree, tmp_path):
    lay = tree
    m = model.load("cdj2000nxs")
    knobs = deck.dsp_env(lay, m, {})
    assert knobs == {"CDJ_C6747_IDLE": "0xC004CB8C:0x11804AE0:0x11805C00", "C66X_IDLE_ISR_FAST": "1"}
    _install(lay, m)
    assert deck.dsp_env(lay, m, {})["C66X_JIT"].endswith("curated-cdj2000nxs/m.so")
    assert "C66X_JIT" not in deck.dsp_env(lay, m, {"MODULE": "none"})
    assert deck.dsp_env(lay, m, {"C66X_IDLE_ISR_FAST": "0"})["C66X_IDLE_ISR_FAST"] == "0"


RIGS_WITH_OWN_DSP = [mid for mid in model.list_models()
                     if model.load(mid).is_rig and model.load(mid).dsp_idle]


@pytest.mark.parametrize("model_id", RIGS_WITH_OWN_DSP)
def test_a_rig_player_with_its_own_dsp_program_has_its_own_module(model_id, tree, tmp_path, monkeypatch):
    monkeypatch.setattr(rig, "Layout", lambda: tree)
    own = model.load(model_id)
    assert not own.has_dsp_module
    _install(tree, own)
    _install(tree, model.load())
    (tmp_path / "c14gen" / "g23n").mkdir()
    (tmp_path / "c14gen" / "g23n" / "m.so").write_bytes(b"")
    assert rig.jit_module({}, own).replace("\\", "/").endswith("curated-%s/m.so" % model_id)
    assert rig.jit_module({}, model.load()).replace("\\", "/").endswith("c14gen/curated/m.so")
    assert rig.jit_module({"MODULE": "none"}, own) == ""
    os.remove(tmp_path / "c14gen" / ("curated-" + model_id) / "m.so")
    assert rig.jit_module({}, own) == ""


def _step_dsp(tmp_path, model_id, **opts):
    s = _setup(tmp_path, **opts)
    s.model = model.load(model_id)
    s.lay.jit_cache = str(tmp_path / "c14gen")
    s.lay.root = s.lay.extract = str(tmp_path)
    return s


@pytest.mark.parametrize("model_id", RIGS_WITH_OWN_DSP)
def test_setup_keeps_a_module_that_fits_the_current_code(model_id, tmp_path, capsys):
    s = _step_dsp(tmp_path, model_id)
    _install(s.lay, s.model)
    s.step_dsp()
    assert "built from this code" in capsys.readouterr().out


def test_every_rig_player_and_every_player_with_a_dsp_profile_builds_a_module():
    assert all(model.load(mid).builds_dsp_module for mid in [model.DEFAULT] + RIGS_WITH_OWN_DSP)
    assert model.load("cdj2000").builds_dsp_module
    assert not model.load("cdj900").builds_dsp_module


def test_setup_builds_a_missing_module_unless_told_to_skip(tmp_path, monkeypatch, capsys):
    s = _step_dsp(tmp_path, "xdj1000", skip_dsp=True)
    built = []
    monkeypatch.setattr(s, "_build_module", lambda: built.append(1))
    s.step_dsp()
    assert not built and "--skip-dsp" in capsys.readouterr().out
    s = _step_dsp(tmp_path, "xdj1000")
    monkeypatch.setattr(s, "deck_ready", lambda: True)
    monkeypatch.setattr(s, "_build_module", lambda: built.append(1))
    s.step_dsp()
    assert built == [1]


def test_setup_builds_a_stale_module_again(tmp_path, monkeypatch, capsys):
    s = _step_dsp(tmp_path, "xdj1000")
    _install(s.lay, s.model, generator="0")
    built = []
    monkeypatch.setattr(s, "deck_ready", lambda: True)
    monkeypatch.setattr(s, "_build_module", lambda: built.append(1))
    s.step_dsp()
    assert built == [1] and "not built from this code" in capsys.readouterr().out


def test_the_recording_options_are_gone(capsys):
    for old in ("--curated-jit", "--keep-recording", "--warm", "--no-warm"):
        with pytest.raises(SystemExit) as e:
            parse_args([old])
        assert e.value.code == 2
    assert parse_args(["--skip-dsp"]).skip_dsp
    assert parse_args(["--any-firmware"]).any_firmware and not parse_args([]).any_firmware


def test_cdj2000_names_its_idle_loop_for_the_c6727(tree):
    knobs = deck.dsp_env(tree, model.load("cdj2000"), {})
    assert knobs == {"CDJ_C6727_IDLE": "0x80047B80:0x10005000:0x10006600", "C66X_IDLE_ISR_FAST": "1"}


def _deck_tree(tree, mid):
    m = model.load(mid)
    os.makedirs(os.path.join(tree.extract, mid), exist_ok=True)
    for name in m.images:
        open(os.path.join(tree.extract, mid, name), "wb").close()
    open(tree.usb_image, "wb").close()
    return m


def test_app_dry_run_for_the_cdj2000nxs_shows_the_display_on_vnc_and_the_app(tree, capsys):
    m = _deck_tree(tree, "cdj2000nxs")
    app_cmd = ["python", "app/virtual_deck.py", "--model", m.app_face, "--panel-port", "40124"]
    env = {"MAIN_QEMU": "/q/qemu-system-sh4", "GUI_DISPLAY": "vnc=127.0.0.1:21", "CDJ_PANEL_KEYSOCK": "40124"}
    assert deck.start(m, env, dry=True, app_cmd=app_cmd) == 0
    out = capsys.readouterr().out
    assert "-display vnc=127.0.0.1:21" in out
    assert "with:       GUI_DISPLAY=vnc=127.0.0.1:21, then python app/virtual_deck.py --model cdj2000nxs" in out
    assert "virtual deck app are" not in out


def test_start_app_for_an_older_deck_passes_the_app_through(tmp_path, monkeypatch):
    lay = SimpleNamespace(conf=str(tmp_path / "cdj.conf"), emu=EMU, root=str(tmp_path), packaged=False, logs=str(tmp_path))
    (tmp_path / "cdj.conf").write_text("CDJ_NAME=show\n")
    monkeypatch.setattr(start, "Layout", lambda: lay)
    seen = []
    monkeypatch.setattr(start, "_start_deck", lambda lay, player, c, dry, app=False: seen.append((player.id, app)) or 0)
    load = model.load

    def without_face(mid):
        player = load(mid)
        player.app_face = ""
        return player

    for mid, argv, want, loader in (("cdj2000nxs", ["--app"], True, load), ("cdj2000nxs", [], False, load),
                                    ("xdj1000", ["--app"], False, without_face),
                                    ("cdj2000nxs", ["--no-app"], False, load)):
        monkeypatch.setattr(model, "load", loader)
        start.main(["--model", mid, "--dry-run"] + argv)
        assert seen.pop() == (mid, want)


def test_start_app_builds_the_older_decks_app_command(tmp_path, monkeypatch):
    lay = SimpleNamespace(emu=EMU, packaged=False, logs=str(tmp_path))
    m = model.load("cdj2000nxs")
    monkeypatch.setattr(start, "_offer_rebuild", lambda *a: True)
    monkeypatch.setattr(start, "_app_python", lambda lay, c: ["py"])
    monkeypatch.setattr(start.host, "pick_port_block", lambda want, n: want)
    monkeypatch.setattr(start.host, "pick_udp_port", lambda want: want)
    got = []
    monkeypatch.setattr(start.deck, "start", lambda m, env, dry, app_cmd, decks: got.append((env, app_cmd)) or 0)
    start._start_deck(lay, m, {"CDJ_DECKS": "1", "CDJ_DJLINK": "0", "CDJ_GROUP": "", "CDJ_AUDIO": "0", "CDJ_NAME": "show",
                               "QEMU_BUILD": "", "QEMU_EB_BUILD": ""}, True, True)
    env, app_cmd = got[0]
    assert env["GUI_DISPLAY"] == "vnc=127.0.0.1:21"
    assert app_cmd[app_cmd.index("--model") + 1] == "cdj2000nxs"
    assert app_cmd[app_cmd.index("--vnc-base") + 1] == "5920"
    assert app_cmd[app_cmd.index("--panel-port") + 1] == env["CDJ_PANEL_KEYSOCK"] == "40124"


@pytest.mark.parametrize("mid,face", [("cdj2000nxs2", None)])
def test_the_rig_app_draws_the_players_own_face_when_it_has_one(tmp_path, monkeypatch, mid, face):
    (tmp_path / "extract").mkdir()
    images = tmp_path / model.load(mid).extract
    images.mkdir(parents=True, exist_ok=True)
    for name in ("main_unpacked.bin", "gui_unpacked.bin", "flash.bin"):
        (images / name).write_bytes(b"")
    (tmp_path / "extract" / "usbmedia3.img").write_bytes(b"")
    (tmp_path / "cdj.conf").write_text("CDJ_NAME=show\n")
    lay = SimpleNamespace(conf=str(tmp_path / "cdj.conf"), emu=EMU, root=str(tmp_path), packaged=False, logs=str(tmp_path),
                          extract=str(tmp_path / "extract"), usb_image=str(tmp_path / "extract" / "usbmedia3.img"),
                          tmp=str(tmp_path))
    monkeypatch.setattr(start, "Layout", lambda: lay)
    monkeypatch.setattr(start, "_offer_rebuild", lambda *a: True)
    monkeypatch.setattr(start, "_app_python", lambda lay, c: ["py"])
    monkeypatch.setattr(start.host, "pick_port_block", lambda want, n: want)
    got = []
    monkeypatch.setattr(start, "run", lambda lay, env, launch, bridge, app_cmd: got.append(app_cmd) or 0)
    assert start.main(["--model", mid, "--app"]) == 0
    app_cmd = got[0]
    assert (app_cmd[app_cmd.index("--model") + 1] if "--model" in app_cmd else None) == face
