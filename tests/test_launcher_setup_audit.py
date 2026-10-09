# SPDX-License-Identifier: GPL-2.0-or-later
"""Answers setup accepts must reach cdj.conf and start: nothing lost when a
later step stops, nothing saved that start then ignores, nothing carried over
from the previous player."""

import io
import os
import sys

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import conf, host, model, start  # noqa: E402
from launcher.console import Console, dropped_path  # noqa: E402
from launcher.layout import Layout  # noqa: E402
from launcher.setup import Options, Setup, parse_args  # noqa: E402


def _setup(tmp_path, player, stored=None, **opts):
    """A Setup past choose_model, its cdj.conf holding `stored`."""
    o = Options()
    o.yes = True
    for k, v in opts.items():
        setattr(o, k, v)
    s = Setup(o)
    s.lay = Layout()
    s.lay.conf = str(tmp_path / "cdj.conf")
    s.lay.extract = str(tmp_path / "extract")
    s.lay.root = str(tmp_path)
    s.model = model.load(player)
    s.values = dict(stored or {})
    if stored is not None:
        conf.save(s.lay.conf, s.values)
    s.c = dict({k: "" for k in conf.KEYS}, **dict(s.values, CDJ_MODEL=player))
    s.configured = bool(s.c["CDJ_DECKS"]) and stored is not None
    s.py_midi = s.py_tools = ""
    s.jit_libdir = str(tmp_path / "c6x")
    s.cores = 4
    s.con = Console(dry=o.dry, interactive=False)
    return s


def test_a_setup_that_never_asked_for_sound_does_not_mute_start():
    c = conf.with_start_defaults(conf.parse(conf.render({"CDJ_MODEL": "xdj1000"})))
    assert (c["CDJ_AUDIO"], c["CDJ_RELAY_PORT"], c["CDJ_NAME"], c["CDJ_CONTROLLER"]) == ("1", "7202", "show", "none")


def test_an_older_players_start_has_sound_after_its_own_setup(tmp_path, monkeypatch):
    seen = {}
    monkeypatch.setattr(start.deck, "start", lambda player, env, dry: seen.update(env))
    monkeypatch.setattr(start, "_offer_rebuild", lambda *a: True)
    start._start_deck(Layout(), model.load("xdj1000"),
                      conf.with_start_defaults(conf.parse(conf.render({"CDJ_MODEL": "xdj1000"}))), True)
    assert seen.get("NOSOUND") != "1"


def test_a_new_build_dir_is_saved_when_the_setup_is_kept(tmp_path):
    stored = {"CDJ_DECKS": "1", "CDJ_MODEL": "cdj2000nxs2", "QEMU_BUILD": "/old", "QEMU_EB_BUILD": "/old-eb"}
    s = _setup(tmp_path, "cdj2000nxs2", stored)
    s.c["QEMU_BUILD"] = "/new"
    s.step_config()
    assert s.ask5


def test_an_older_players_new_build_dir_is_saved(tmp_path):
    stored = {"CDJ_MODEL": "xdj1000", "QEMU_BUILD": "/old"}
    s = _setup(tmp_path, "xdj1000", stored)
    s.c["QEMU_BUILD"] = "/new"
    s.step_config()
    assert s.ask5


def test_an_untouched_older_players_setup_is_not_rewritten(tmp_path):
    stored = {"CDJ_MODEL": "xdj1000", "QEMU_BUILD": "/old"}
    s = _setup(tmp_path, "xdj1000", stored)
    s.step_config()
    assert not s.ask5


def test_reconfigure_keeps_the_stored_pro_dj_link_answer(tmp_path):
    stored = {"CDJ_DECKS": "1", "CDJ_DJLINK": "1", "CDJ_MODEL": "cdj2000nxs2"}
    s = _setup(tmp_path, "cdj2000nxs2", stored, reconfigure=True)
    s._questions()
    assert s.c["CDJ_DJLINK"] == "1"


def test_reconfigure_keeps_a_custom_pro_dj_link_group(tmp_path):
    stored = {"CDJ_DECKS": "1", "CDJ_GROUP": "239.1.2.3:45000", "CDJ_MODEL": "cdj2000nxs2"}
    s = _setup(tmp_path, "cdj2000nxs2", stored, reconfigure=True)
    s._questions()
    assert s.c["CDJ_GROUP"].startswith("239.1.2.3:")


def test_step_6_answers_are_saved_before_the_mods_are_asked(tmp_path):
    stored = {"CDJ_DECKS": "1", "CDJ_MODEL": "cdj2000nxs2"}
    s = _setup(tmp_path, "cdj2000nxs2", stored, reconfigure=True, decks="2")
    s.step_config()
    assert conf.load(s.lay.conf)["CDJ_DECKS"] == "2"


def test_a_dry_run_writes_no_conf_at_step_6(tmp_path):
    stored = {"CDJ_DECKS": "1", "CDJ_MODEL": "cdj2000nxs2"}
    s = _setup(tmp_path, "cdj2000nxs2", stored, reconfigure=True, decks="2", dry=True)
    s.step_config()
    assert conf.load(s.lay.conf)["CDJ_DECKS"] == "1"


def test_a_dry_run_makes_no_extract_folder(tmp_path):
    music = tmp_path / "music"
    music.mkdir()
    s = _setup(tmp_path, "cdj2000nxs2", dry=True, music=str(music))
    s.make_image = lambda folder, expect_pioneer=True: True
    s.step_usb()
    assert not os.path.exists(s.lay.extract)


def test_an_older_player_is_ready_without_the_nxs2_boards(tmp_path):
    m = model.load("xdj1000")
    s = _setup(tmp_path, "xdj1000")
    folder = tmp_path / m.extract
    folder.mkdir(parents=True, exist_ok=True)
    for name in m.images:
        (folder / name).write_bytes(b"")
    (tmp_path / "extract").mkdir(exist_ok=True)
    (tmp_path / "extract" / "usbmedia3.img").write_bytes(b"")
    build = tmp_path / "build"
    build.mkdir()
    (build / ("qemu-system-sh4" + host.exe_suffix())).write_bytes(b"")
    s.c["QEMU_BUILD"] = str(build)
    assert s.deck_ready()


def test_the_nxs2_is_not_ready_on_another_players_firmware(tmp_path):
    s = _setup(tmp_path, "cdj2000nxs2")
    s.model = model.load("cdj2000nxs2")
    assert not s.deck_ready()


def test_a_player_id_is_matched_without_regard_to_case(monkeypatch):
    monkeypatch.delenv("CDJ_MODEL", raising=False)
    m = model.load(" XDJ1000MK2 ")
    assert m.id == "xdj1000mk2"
    assert model.load("CDJ2000NXS2").module_dir == "curated"


def _con(monkeypatch, text):
    monkeypatch.setattr(sys, "stdin", io.StringIO(text))
    return Console(dry=False, interactive=True)


def test_an_answer_is_trimmed_and_case_does_not_matter(monkeypatch):
    con = _con(monkeypatch, " XDJ1000MK2 \r\n")
    assert con.choose("which player?", "cdj2000nxs2", "cdj2000nxs2", "xdj1000mk2") == "xdj1000mk2"
    con = _con(monkeypatch, "YES \r\n")
    assert con.ask_yn("go?", "n")
    con = _con(monkeypatch, "  \r\n")
    assert con.ask("name:", "show") == "show"


def test_a_typed_home_folder_is_expanded(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path))
    monkeypatch.setenv("USERPROFILE", str(tmp_path))
    (tmp_path / "C2KNXS2.UPD").write_bytes(b"")
    assert os.path.normpath(dropped_path("~/C2KNXS2.UPD ", False)) == str(tmp_path / "C2KNXS2.UPD")


def test_on_and_off_options_are_read_without_regard_to_case_and_checked(capsys):
    assert parse_args(["--djlink", "ON", "--audio", "Off"]).djlink == "ON"
    with pytest.raises(SystemExit) as e:
        parse_args(["--audio", "maybe"])
    assert e.value.code == 2 and "--audio" in capsys.readouterr().err


def test_onoff_reads_upper_case():
    from launcher.setup import _onoff

    assert _onoff("ON") == "1" and _onoff("Off") == "0"


def test_every_profile_launches_and_installs_consistently():
    for mid in model.list_models():
        m = model.load(mid)
        assert m.is_rig == (m.main_machine == "cdj2000nxs2"), mid
        assert m.images, mid


def test_a_profile_without_its_launch_line_is_refused(tmp_path, monkeypatch):
    text = open(os.path.join(model.models_dir(), "xdj1000.conf"), encoding="utf-8").read()
    (tmp_path / "broken.conf").write_text(text.replace("MODEL_LAUNCH=deck", ""))
    monkeypatch.setattr(model, "models_dir", lambda: str(tmp_path))
    with pytest.raises(model.ModelError):
        model.load("broken")
    (tmp_path / "broken.conf").write_text(text.replace("MODEL_LAUNCH=deck", "MODEL_LAUNCH=decks"))
    with pytest.raises(model.ModelError):
        model.load("broken")


def test_a_snapshot_of_a_model_with_no_steps_is_refused(tmp_path):
    from launcher import deck

    m = model.load("cdj900")
    assert not (m.idle_s and m.load_steps)
    with pytest.raises(SystemExit):
        deck.plan_snapshot(None, m, [], {"SNAPSHOT": "idle"})


def test_a_player_title_typed_at_the_prompt_picks_that_player(monkeypatch):
    monkeypatch.setattr(sys, "stdin", io.StringIO("CDJ-2000NXS\n"))
    con = Console(dry=False, interactive=True)
    known = model.list_models()
    aliases = {model.load(m).title.lower(): m for m in known}
    assert con.choose("which player?", "cdj2000nxs2", *known, aliases=aliases) == "cdj2000nxs"


def test_a_relay_port_that_is_not_a_number_is_refused(capsys):
    with pytest.raises(SystemExit) as e:
        parse_args(["--relay-port", "abc"])
    assert e.value.code == 2
    assert "--relay-port needs a number" in capsys.readouterr().err
    assert parse_args(["--relay-port", "7300"]).relay == "7300"


def test_an_unknown_model_in_the_environment_is_reported(monkeypatch, capsys):
    from launcher import warm_jit

    monkeypatch.setenv("CDJ_MODEL", "bogus")
    assert warm_jit.main([]) == 1
    err = capsys.readouterr().err
    assert "unknown model 'bogus'" in err and "cdj2000nxs2" in err


def test_a_stored_unknown_model_stops_setup_with_the_list(tmp_path):
    s = _setup(tmp_path, "cdj2000nxs2", stored={"CDJ_MODEL": "bogus"})
    s.c["CDJ_MODEL"] = "bogus"
    s.con = Console(dry=False, interactive=True)
    with pytest.raises(SystemExit):
        s.choose_model()


def test_a_snapshot_is_keyed_by_the_audio_device_and_the_dsp_module(tmp_path):
    from types import SimpleNamespace

    from launcher import deck

    m = model.load("xdj1000")
    lay = SimpleNamespace(tmp=str(tmp_path), run=str(tmp_path), usb_image=str(tmp_path / "usb.img"))
    for n in ("qemu", "kernel", "usb.img", "m1.so", "m2.so"):
        (tmp_path / n).write_bytes(n.encode())

    def point_dir(audio, module):
        argv = [str(tmp_path / "qemu"), "-kernel", str(tmp_path / "kernel")] + (["-audio", audio] if audio else [])
        env = {"SNAPSHOT": "idle", "C66X_JIT": str(tmp_path / module) if module else ""}
        return deck.plan_snapshot(lay, m, argv, dict(env))[-1]

    assert point_dir("a", "m1.so") == point_dir("a", "m1.so")
    assert point_dir("a", "m1.so") != point_dir("b", "m1.so")
    assert point_dir("a", "m1.so") != point_dir("a", "m2.so")
    assert point_dir("a", "m1.so") != point_dir("a", "")
