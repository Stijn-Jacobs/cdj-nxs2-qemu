# SPDX-License-Identifier: GPL-2.0-or-later
"""The mods registry: mods/mods.conf, one row per user-facing on/off knob,
shared with the bash tree (see that file for the row format). setup.py asks
about each row and saves the choice to cdj.conf; start.py applies it; rig.py
falls back to a row's own default when nothing else set the knob.
"""

import os
from collections import namedtuple

Mod = namedtuple("Mod", "key env on off default description")


def load(lay):
    mods = []
    with open(os.path.join(lay.emu, "mods", "mods.conf"), encoding="utf-8") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            mods.append(Mod(*line.split("|", 5)))
    return mods


def conf_key(env):
    """The cdj.conf key a knob is saved under: CDJ_-prefixed, so SERVICE
    becomes CDJ_SERVICE, the key setup used to hand-add for it before this
    registry existed."""
    return env if env.startswith("CDJ_") else "CDJ_" + env


def default(mods, env):
    """The value to fall back to for `env` when nothing else set it: that
    mod's own on or off value, whichever the registry defaults to."""
    for m in mods:
        if m.env == env:
            return m.on if m.default == "on" else m.off
    return ""


def apply(mods, c, env):
    """Set every mod's knob in `env` (a dict, normally os.environ's own) from
    cdj.conf's values `c`, unless `env` already has it -- the caller's own
    environment, or something start.py itself set (its --service flag)."""
    for m in mods:
        if env.get(m.env, "") != "":
            continue
        saved = c.get(conf_key(m.env), "")
        env[m.env] = saved if saved in (m.on, m.off) else default(mods, m.env)
