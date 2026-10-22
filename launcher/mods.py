# SPDX-License-Identifier: GPL-2.0-or-later
"""The mods registry: every on/off tweak to how the deck behaves, one row per
mod folder, read from its mods/<mod>/mod.conf. setup.py asks about each row
the chosen model is offered and saves the choice to cdj.conf; start.py applies
it; rig.py falls back to a row's own default when nothing else set the knob.

A mod.conf sets, in the form of models/*.conf:

  MOD_ENV          the CDJ_* (or plain) knob the board actually reads, or
                   several '+'-joined for a mod that spans more than one image
  MOD_ON, MOD_OFF  the value that knob gets when the mod is enabled/disabled
  MOD_DEFAULT      "on" or "off": what a fresh install ships with
  MOD_MODELS       comma-separated model ids (models/*.conf) the mod is offered
                   for, or * for every model; setup and start leave it out for
                   any other
  MOD_DESCRIPTION  one line, asked as "<description>?" at setup and shown in
                   the README's Mods section

The folder's name is the row's key, used for the setup prompt. A mod that
patches the display firmware has a routine gui_<name> in one of its player
folders, switched by the knob CDJ_GUI_<NAME> with on = 1; a MAIN routine
main_<name> likewise has CDJ_MAIN_<NAME>. A mod with several routines joins
their knobs with '+' in MOD_ENV, e.g. CDJ_GUI_WAVE3+CDJ_MAIN_WAVE3DATA; the
choice is still saved once, under the first knob.

cdj.conf keeps each choice under CDJ_<ENV> (already true for env names that
start with CDJ_). SERVICE MODE is not a mod: it is a boot option (--service /
CDJ_SERVICE), not a behaviour change, and setup does not ask about it.
"""

import glob
import os
from collections import namedtuple

from . import model

Mod = namedtuple("Mod", "key env on off default models description")


def supports(mod, model_id):
    return mod.models == "*" or model_id in mod.models.split(",")


def load(lay, model_id=None):
    """Every row of the registry, or with a model id only the rows offered for
    that model: the mods that are on by default first, so setup asks about
    those before the optional ones."""
    mods = []
    for conf in sorted(glob.glob(os.path.join(lay.emu, "mods", "*", "mod.conf"))):
        with open(conf, encoding="utf-8") as f:
            v = model.parse(f.read())
        mods.append(Mod(os.path.basename(os.path.dirname(conf)), v["MOD_ENV"], v["MOD_ON"], v["MOD_OFF"],
                        v["MOD_DEFAULT"], v["MOD_MODELS"], v["MOD_DESCRIPTION"]))
    mods.sort(key=lambda m: m.default != "on")
    return [m for m in mods if model_id is None or supports(m, model_id)]


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
