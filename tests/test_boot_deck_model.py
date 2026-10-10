# SPDX-License-Identifier: GPL-2.0-or-later
"""boot_deck.py's model check: it needs a GUI board, so a model still in
bring-up (cdj2000, cdj2000nxs) refuses before anything is started."""

import os
import sys

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import boot_deck, model  # noqa: E402


def test_refuses_a_model_without_a_gui_board(monkeypatch, capsys):
    monkeypatch.setenv("CDJ_MODEL", "cdj2000")
    assert boot_deck.main(["zbdt1"]) == 1
    err = capsys.readouterr().err
    assert err.strip() == "boot_deck.sh: CDJ-2000 has no GUI board yet; use scripts/run/boot_main.sh"


def test_refuses_an_unknown_model(monkeypatch, capsys):
    monkeypatch.setenv("CDJ_MODEL", "not-a-model")
    assert boot_deck.main(["zbdt2"]) == 1
    err = capsys.readouterr().err
    assert err.strip().startswith("unknown model 'not-a-model'")


def test_default_model_has_a_gui_board():
    assert model.load().gui_machine == "sh7269gui"


def test_flash_boot_needs_a_flash_with_the_bootloader(tmp_path):
    old = tmp_path / "old.bin"
    old.write_bytes(b"\xff" * 16)
    new = tmp_path / "new.bin"
    new.write_bytes(b"\x09\x00\x05\xa0" + b"\xff" * 12)
    assert not boot_deck.flash_has_bootloader(str(old))
    assert boot_deck.flash_has_bootloader(str(new))


def test_snapshot_key_follows_the_mods_and_their_source(tmp_path):
    from types import SimpleNamespace
    files = {n: tmp_path / n for n in ("qemu", "main.bin", "gui.bin", "media.img")}
    for f in files.values():
        f.write_bytes(f.name.encode())
    mod = tmp_path / "mods" / "three_band" / "wave3.s"
    mod.parent.mkdir(parents=True)
    mod.write_text("v1")

    def root(mods):
        deck = SimpleNamespace(env={"SNAPSHOT": "idle"}, tag="zbdt3", notes=[], main_mon="m",
                               lay=SimpleNamespace(tmp=str(tmp_path), emu=str(tmp_path)), main_qemu=str(files["qemu"]),
                               gui_qemu=str(files["qemu"]), main_argv=[], gui_argv=[])
        boot_deck.Deck._plan_snapshot(deck, [str(files["main.bin"]), str(files["gui.bin"])],
                                      mods, str(files["media.img"]))
        return deck.env["SNAPSHOT_ROOT"]

    assert root(["wave3"]) == root(["wave3"])
    assert root(["wave3"]) != root([])
    assert root(["wave3"]) != root(["wave3", "phrase"])
    before = root(["wave3"])
    mod.write_text("v2")
    assert root(["wave3"]) != before
    assert root([]) == root([])
