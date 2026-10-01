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


def env_names(env):
    """A row's `env` field is usually one board knob, but a mod that patches
    more than one image (e.g. a MAIN fetch feeding a GUI draw routine) names
    all of them, '+'-joined, so one setup answer and one cdj.conf line drive
    every image together -- see wave3data's row."""
    return env.split("+")


def conf_key(env):
    """The cdj.conf key a knob is saved under: CDJ_-prefixed, so SERVICE
    becomes CDJ_SERVICE, the key setup used to hand-add for it before this
    registry existed. A multi-image row is saved under its first knob only;
    the others are derived from the same saved choice, never saved twice."""
    first = env_names(env)[0]
    return first if first.startswith("CDJ_") else "CDJ_" + first


def default(mods, env):
    """The value to fall back to for `env` when nothing else set it: that
    mod's own on or off value, whichever the registry defaults to."""
    for m in mods:
        if m.env == env:
            return m.on if m.default == "on" else m.off
    return ""


def apply(mods, c, env):
    """Set every mod's knob(s) in `env` (a dict, normally os.environ's own)
    from cdj.conf's values `c`, unless `env` already has a knob set -- the
    caller's own environment, or something start.py itself set (its
    --service flag)."""
    for m in mods:
        saved = c.get(conf_key(m.env), "")
        value = saved if saved in (m.on, m.off) else default(mods, m.env)
        for name in env_names(m.env):
            if env.get(name, "") == "":
                env[name] = value
