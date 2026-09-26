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
