#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Controller profiles and mappings: where they live and which one is plugged in.

A controller is two JSON files, and neither knows about the other's job:

    controllers/<name>.json   the PROFILE: what the hardware sends. Every
                              control by name, with its MIDI port, message type
                              (note / cc / pitch), channel, number and how it
                              moves (button, absolute, one of three relative
                              encodings). Captured off the device by learn.py,
                              never transcribed from a chart. Also the port
                              name to look for ("match") and, if the unit has
                              lamps, the port its LEDs listen on.
    mappings/<name>.json      the MAPPING: which control drives which CDJ
                              action (cdj_actions.py) on which deck. No MIDI
                              numbers at all, so re-learning a controller
                              never breaks its mapping.

Adding a controller is therefore: `learn.py new` (write the profile), then
`learn.py map` (write the mapping). bridge.py picks the profile whose "match"
appears in a connected MIDI port's name.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CONTROLLER_DIR = os.path.join(HERE, "controllers")
MAPPING_DIR = os.path.join(HERE, "mappings")
DEFAULT = "roland-dj-202"


def slug(name):
    """'Roland DJ-202' -> 'roland-dj-202': the file name for a controller."""
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "controller"


def _resolve(name_or_path, directory):
    if os.path.sep in name_or_path or "/" in name_or_path or name_or_path.endswith(".json"):
        return name_or_path
    return os.path.join(directory, name_or_path + ".json")


def name_of(name_or_path):
    """The controller's name whether it was given as a name or a path."""
    return os.path.splitext(os.path.basename(name_or_path))[0]


def controller_path(name_or_path):
    """A profile by name (controllers/<name>.json) or by explicit path."""
    return _resolve(name_or_path, CONTROLLER_DIR)


def mapping_path(name_or_path):
    """A mapping by name (mappings/<name>.json) or by explicit path."""
    return _resolve(name_or_path, MAPPING_DIR)


def checklist_path(name):
    """The learn checklist that ships with a profile, if there is one."""
    return os.path.join(CONTROLLER_DIR, name + ".checklist.txt")


def load_json(path, what):
    if not os.path.exists(path):
        sys.exit(f"no {what} at {path}\n"
                 "  make one with:  python midi/learn.py new")
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def save_json(path, data):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(data, fh, indent=2, sort_keys=True)
        fh.write("\n")


def available():
    """[(name, profile)] for every profile in controllers/, sorted by name."""
    out = []
    if os.path.isdir(CONTROLLER_DIR):
        for f in sorted(os.listdir(CONTROLLER_DIR)):
            if f.endswith(".json"):
                with open(os.path.join(CONTROLLER_DIR, f), encoding="utf-8") as fh:
                    out.append((f[:-5], json.load(fh)))
    return out


def detect(input_names):
    """The profile whose "match" appears in a connected MIDI input's name.

    None when nothing plugged in has a profile. With several matches the first
    by name wins and the caller should say so -- two controllers at once is
    something to choose explicitly with --controller.
    """
    hits = [name for name, prof in available()
            if prof.get("match") and any(prof["match"].lower() in n.lower()
                                         for n in input_names)]
    return hits[0] if hits else None, hits


def empty_mapping(controller, prefix="show"):
    """A mapping with no bindings yet, for the two decks the launchers start."""
    return {
        "controller": controller,
        "decks": {"cdjA": f"{prefix}1", "cdjB": f"{prefix}2"},
        "focus": "cdjA",
        "bindings": {},
        "leds": {},
    }
