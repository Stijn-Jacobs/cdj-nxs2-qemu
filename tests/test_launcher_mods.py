# SPDX-License-Identifier: GPL-2.0-or-later
"""launcher/mods.py against the real mods/mods.conf: parsing, the registry
default, the cdj.conf key a knob is saved under, and apply()'s precedence
(a caller's own environment beats cdj.conf beats the registry default)."""

import os
import sys

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import mods  # noqa: E402
from launcher.layout import Layout  # noqa: E402


def test_loads_the_registry():
    m = mods.load(Layout())
    assert {"high_fps", "live_clock", "three_band", "phrase"} <= {x.key for x in m}
    assert all(x.env.startswith("CDJ_") for x in m)


def test_conf_key_prefixes_a_bare_env_name():
    assert mods.conf_key("CDJ_GUI_FRAME_MS") == "CDJ_GUI_FRAME_MS"
    assert mods.conf_key("SOMETHING") == "CDJ_SOMETHING"


def test_default_is_the_registrys_own_on_value():
    m = mods.load(Layout())
    assert mods.default(m, "CDJ_GUI_FRAME_MS") == "6"
    assert mods.default(m, "CDJ_GUI_CLOCK_DT") == "43"
    assert mods.default(m, "CDJ_NO_SUCH_KNOB") == ""


def test_apply_falls_back_to_the_registry_default_when_conf_has_nothing():
    m = mods.load(Layout())
    env = {}
    mods.apply(m, {}, env)
    assert env["CDJ_GUI_FRAME_MS"] == "6"
    assert env["CDJ_GUI_CLOCK_DT"] == "43"


def test_apply_takes_cdj_confs_saved_choice():
    m = mods.load(Layout())
    env = {}
    mods.apply(m, {"CDJ_GUI_FRAME_MS": "0"}, env)
    assert env["CDJ_GUI_FRAME_MS"] == "0"


def test_apply_leaves_a_callers_own_env_alone():
    m = mods.load(Layout())
    env = {"CDJ_GUI_FRAME_MS": "3"}
    mods.apply(m, {"CDJ_GUI_FRAME_MS": "0"}, env)
    assert env["CDJ_GUI_FRAME_MS"] == "3"
