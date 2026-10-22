# SPDX-License-Identifier: GPL-2.0-or-later
"""Two decks of an older player: two QEMU processes that share the stick, the
DSP module and the Pro DJ Link segment and nothing else."""

import os
import sys
from types import SimpleNamespace

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import deck, host, model, sites, start  # noqa: E402

QEMU = {"MAIN_QEMU": "/q/qemu-system-sh4", "NOSOUND": "1", "GUI_DISPLAY": "none"}


@pytest.fixture
def tree(tmp_path):
    (tmp_path / "extract" / "xdj1000mk2").mkdir(parents=True)
    return SimpleNamespace(emu=EMU, root=str(tmp_path), extract=str(tmp_path / "extract"), logs=str(tmp_path / "logs"),
                           tmp=str(tmp_path), usb_image=str(tmp_path / "extract" / "usbmedia3.img"),
                           jit_cache=str(tmp_path / "c14gen"), packaged=False)


def _arg(argv, flag):
    return argv[argv.index(flag) + 1]


def test_the_first_deck_is_named_as_before(tree):
    m = model.load("xdj1000mk2")
    argv, env = deck.command(tree, m, dict(QEMU, DJLINK="1"))
    assert _arg(argv, "-name") == "XDJ-1000MK2"
    assert os.path.basename(_arg(argv, "-serial")) == "xdj1000mk2.log"
    assert env["CDJ_ETHER_MAC"] == "02:00:00:00:00:01"


def test_each_deck_has_its_own_log_window_panel_port_and_mac(tree):
    m = model.load("xdj1000mk2")
    shared = dict(QEMU, DJLINK="1", CDJ_PANEL_KEYSOCK="40124")
    (argv1, env1), (argv2, env2) = (deck.command(tree, m, shared, n) for n in (1, 2))
    assert _arg(argv2, "-name") == "XDJ-1000MK2 2"
    assert os.path.basename(_arg(argv1, "-serial")) == "xdj1000mk2.log"
    assert os.path.basename(_arg(argv2, "-serial")) == "xdj1000mk2-2.log"
    assert (env1["CDJ_PANEL_KEYSOCK"], env2["CDJ_PANEL_KEYSOCK"]) == ("40124", "40125")
    assert env2["CDJ_ETHER_MAC"] == "02:00:00:00:00:02"
    assert _arg(argv1, "-netdev") == _arg(argv2, "-netdev") and env1["GROUP"] == env2["GROUP"]
    assert shared["CDJ_PANEL_KEYSOCK"] == "40124"


def test_a_vnc_screen_moves_up_one_display_per_deck():
    assert deck.deck_display("vnc=127.0.0.1:21", 1) == "vnc=127.0.0.1:21"
    assert deck.deck_display("vnc=127.0.0.1:21", 2) == "vnc=127.0.0.1:22"
    assert deck.deck_display("none", 2) == "none"


def test_a_mac_set_by_hand_is_taken_as_given_unless_it_names_the_deck(tree):
    m = model.load("xdj1000mk2")
    env = dict(QEMU, DJLINK="1", CDJ_ETHER_MAC="02:aa:00:00:00:0%N%")
    assert deck.command(tree, m, env, 2)[1]["CDJ_ETHER_MAC"] == "02:aa:00:00:00:02"


def test_only_the_first_deck_saves_a_snapshot_point(tree, tmp_path):
    m = model.load("xdj1000mk2")
    qemu, kernel = tmp_path / "qemu", tmp_path / "kernel"
    for f in (qemu, kernel, tree.usb_image):
        open(str(f), "wb").close()
    tree.run = str(tmp_path)
    env = {"SNAPSHOT": "loaded"}
    first, second = ([str(qemu), "-kernel", str(kernel)] for _ in range(2))
    assert deck.plan_snapshot(tree, m, first, dict(env), 1)
    assert deck.plan_snapshot(tree, m, second, dict(env), 2) is None and "-monitor" not in second


def test_panel_port_block_skips_a_reserved_range(monkeypatch):
    monkeypatch.setattr(host, "windows_reserved_ranges", lambda proto: [(40125, 40130)])
    assert host.pick_udp_block(40124, 1) == 40124
    assert host.pick_udp_block(40124, 2) == 40123
    monkeypatch.setattr(host, "windows_reserved_ranges", lambda proto: [])
    assert host.pick_udp_block(40124, 2) == 40124


def _deck_tree(tree):
    m = model.load("xdj1000mk2")
    for name in m.images:
        open(os.path.join(tree.extract, "xdj1000mk2", name), "wb").close()
    open(tree.usb_image, "wb").close()
    return m


def test_dry_run_lists_both_decks(tree, monkeypatch, capsys):
    m = _deck_tree(tree)
    monkeypatch.setattr(deck, "Layout", lambda: tree)
    monkeypatch.setattr(host, "windows_reserved_ranges", lambda proto: [])
    assert deck.start(m, dict(QEMU), dry=True, decks=2) == 0
    out = capsys.readouterr().out
    assert "XDJ-1000MK2: 2 windows sharing one with the USB stick" in out
    assert out.count("would run:") == 2
    assert "CDJ_PANEL_KEYSOCK=40124" in out and "CDJ_PANEL_KEYSOCK=40125" in out
    assert "A second deck" not in out


def test_one_deck_still_says_a_second_one_is_skipped(tree, monkeypatch, capsys):
    m = _deck_tree(tree)
    monkeypatch.setattr(deck, "Layout", lambda: tree)
    assert deck.start(m, dict(QEMU), dry=True) == 0
    out = capsys.readouterr().out
    assert "XDJ-1000MK2: one window with the USB stick. A second deck, MIDI controllers" in out
    assert out.count("would run:") == 1


def test_start_passes_the_decks_setting_and_leaves_the_app_out_of_two(tmp_path, monkeypatch, capsys):
    lay = SimpleNamespace(emu=EMU, packaged=False, logs=str(tmp_path))
    m = model.load("xdj1000mk2")
    monkeypatch.setattr(start, "_offer_rebuild", lambda *a: True)
    got = []
    monkeypatch.setattr(start.deck, "start", lambda m, env, dry, app_cmd, decks: got.append((app_cmd, decks)) or 0)
    c = {"CDJ_DECKS": "2", "CDJ_DJLINK": "0", "CDJ_GROUP": "", "CDJ_AUDIO": "0", "CDJ_NAME": "show",
         "QEMU_BUILD": "", "QEMU_EB_BUILD": ""}
    start._start_deck(lay, m, c, True, True)
    assert got == [([], 2)]
    assert "plain windows" in capsys.readouterr().out


def _own_mac_at(monkeypatch, address):
    monkeypatch.setattr(sites, "find", lambda lay, m, main=None: sites.Sites({"own_mac": address}, True, True))


def test_the_firmwares_own_mac_is_held_to_each_decks_wire_address(tree, monkeypatch):
    m = model.load("xdj1000mk2")
    _own_mac_at(monkeypatch, 0x0C7A491C)
    env = dict(QEMU, DJLINK="1")
    assert deck.command(tree, m, env, 1)[1]["CDJ_MPOKE"] == "0x0C7A491C:0x02:1,0x0C7A4921:0x01:1"
    assert deck.command(tree, m, env, 2)[1]["CDJ_MPOKE"] == "0x0C7A491C:0x02:1,0x0C7A4921:0x02:1"
    assert "CDJ_MPOKE" not in env


def test_the_own_mac_hold_follows_a_pokes_already_set_and_needs_the_link(tree, monkeypatch):
    m = model.load("xdj1000mk2")
    _own_mac_at(monkeypatch, 0x0C7A491C)
    held = deck.command(tree, m, dict(QEMU, DJLINK="1", CDJ_MPOKE="0x100:1:1"), 1)[1]["CDJ_MPOKE"]
    assert held.startswith("0x100:1:1,0x0C7A491C:0x02:1")
    assert "CDJ_MPOKE" not in deck.command(tree, m, dict(QEMU), 1)[1]


def test_two_linked_decks_say_when_the_firmwares_own_mac_was_not_found(tree, monkeypatch, capsys):
    m = _deck_tree(tree)
    monkeypatch.setattr(deck, "Layout", lambda: tree)
    monkeypatch.setattr(host, "windows_reserved_ranges", lambda proto: [])
    deck.start(m, dict(QEMU, DJLINK="1"), dry=True, decks=2)
    assert "own MAC was not found" in capsys.readouterr().out
    _own_mac_at(monkeypatch, 0x0C7A491C)
    deck.start(m, dict(QEMU, DJLINK="1"), dry=True, decks=2)
    out = capsys.readouterr().out
    assert "own MAC was not found" not in out and "CDJ_MPOKE=0x0C7A491C:0x02:1" in out
