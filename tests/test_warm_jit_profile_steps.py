# SPDX-License-Identifier: GPL-2.0-or-later
"""launcher/warm_jit.py: what a profile run does beyond a snapshot's steps."""
import os
import sys
from types import SimpleNamespace

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import model, warm_jit  # noqa: E402


def _m(load, profile):
    return SimpleNamespace(load_steps=load, profile_steps=profile)


def test_a_model_without_extra_steps_profiles_its_load_steps():
    assert warm_jit._profile_steps(_m("wait 1", "")) == "wait 1"


def test_the_extra_steps_are_appended_in_order():
    assert warm_jit._profile_steps(_m("wait 1", "sendkey k 400; wait 15")) == \
        "wait 1; sendkey k 400; wait 15"


def test_only_the_models_that_need_it_set_the_field():
    # The one-window players take their module from a plain load-and-play, so a
    # mode like MASTER TEMPO needs the field to be profiled at all.
    assert model.load("cdj2000nxs").profile_steps
    assert model.load("cdj2000").profile_steps == ""
    # The rig players sweep MASTER TEMPO and the fader in warm_jit.py itself.
    assert model.load("cdj2000nxs2").profile_steps == ""


def test_the_extra_steps_parse_as_run_steps():
    # scripts/run/snapshot_deck.py:run_steps splits on ';' then whitespace.
    steps = warm_jit._profile_steps(model.load("cdj2000nxs"))
    ops = [s.split()[0] for s in steps.split(";")]
    assert set(ops) <= {"wait", "sendkey", "key"}
    assert "sendkey" in ops and "key" in ops
