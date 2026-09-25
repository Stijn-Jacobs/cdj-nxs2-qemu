# SPDX-License-Identifier: GPL-2.0-or-later
"""midi/profiles.py, and every shipped controller profile and mapping: each
binding must name a learned control and a CDJ action that resolves."""
import glob
import json
import os

import pytest

import cdj_actions as ca
import profiles
from helpers import path

MAPPINGS = sorted(glob.glob(path("midi", "mappings", "*.json")))
CONTROLLERS = sorted(glob.glob(path("midi", "controllers", "*.json")))
LAMP_ROLES = {"play", "cue", "slip", "master_tempo", "ring"}   # leds.LAMPS, no mido import
# learn.py writes the first five; mode_select is set by hand for the DJ-202 pad
# mode buttons, whose velocity names the mode (the bridge reads any note as a button).
BEHAVIOURS = {"button", "absolute", "relative", "relative_offset", "relative_signbit",
              "mode_select"}


def live(d):
    return {k: v for k, v in d.items() if not k.startswith("_")}


# -- profiles.py -------------------------------------------------------------------

@pytest.mark.parametrize("name, want", [("Roland DJ-202", "roland-dj-202"),
                                        ("  Pioneer DDJ/400!! ", "pioneer-ddj-400"),
                                        ("***", "controller")])
def test_slug(name, want):
    assert profiles.slug(name) == want


def test_names_resolve_into_their_directories_and_paths_pass_through():
    assert profiles.controller_path("x") == os.path.join(profiles.CONTROLLER_DIR, "x.json")
    assert profiles.mapping_path("x") == os.path.join(profiles.MAPPING_DIR, "x.json")
    assert profiles.mapping_path("some/where.json") == "some/where.json"
    assert profiles.controller_path("mine.json") == "mine.json"
    assert profiles.name_of("some/where/roland-dj-202.json") == "roland-dj-202"
    assert profiles.name_of("roland-dj-202") == "roland-dj-202"


def test_save_then_load_round_trips(tmp_path):
    p = str(tmp_path / "deep" / "m.json")
    data = profiles.empty_mapping("thing", prefix="deck")
    profiles.save_json(p, data)
    assert profiles.load_json(p, "mapping") == data
    with open(p, "rb") as fh:
        raw = fh.read()
    assert raw.endswith(b"}\n") and b"\r\n" not in raw


def test_load_json_missing_exits_with_a_hint(tmp_path):
    with pytest.raises(SystemExit, match="learn.py new"):
        profiles.load_json(str(tmp_path / "none.json"), "profile")


def test_empty_mapping_has_two_decks_and_nothing_bound():
    m = profiles.empty_mapping("c")
    assert m["decks"] == {"cdjA": "show1", "cdjB": "show2"}
    assert m["focus"] == "cdjA" and m["bindings"] == {} and m["leds"] == {}


def test_detect_matches_port_names_case_insensitively():
    first, hits = profiles.detect(["loopMIDI 0", "dj-202 1"])
    assert first == "roland-dj-202" and "roland-dj-202" in hits
    assert profiles.detect(["Some Keyboard"]) == (None, [])


def test_available_lists_every_shipped_profile():
    names = [n for n, _ in profiles.available()]
    assert names == sorted(os.path.basename(p)[:-5] for p in CONTROLLERS)
    assert profiles.DEFAULT in names


# -- shipped files ----------------------------------------------------------------

@pytest.mark.parametrize("controller", CONTROLLERS, ids=os.path.basename)
def test_profile_controls_are_well_formed_and_unique(controller):
    with open(controller, encoding="utf-8") as fh:
        prof = json.load(fh)
    assert prof.get("match")
    seen = {}
    for name, c in prof["controls"].items():
        assert c["type"] in ("note", "cc", "pitch"), name
        assert 0 <= c["channel"] <= 15 and 0 <= c["number"] <= 127, name
        assert c["behaviour"] in BEHAVIOURS, name
        addr = (c["port"], c["type"], c["channel"], c["number"])
        assert addr not in seen, "%s and %s share a MIDI address" % (name, seen.get(addr))
        seen[addr] = name


@pytest.mark.parametrize("mapping_file", MAPPINGS, ids=os.path.basename)
def test_mapping_bindings_resolve(mapping_file):
    with open(mapping_file, encoding="utf-8") as fh:
        mapping = json.load(fh)
    with open(profiles.controller_path(mapping["controller"]), encoding="utf-8") as fh:
        controls = json.load(fh)["controls"]
    decks = live(mapping["decks"])
    assert mapping["focus"] in decks
    bindings = live(mapping["bindings"])
    assert bindings
    for control, spec in bindings.items():
        assert control in controls, "%s is bound but was never learned" % control
        if spec["action"] == "set_focus":
            assert spec["deck"] in decks, control
            continue
        assert spec["deck"] in decks or spec["deck"] == "focus", control
        action = ca.resolve(spec["action"], spec)
        assert action.sendable, "%s is bound to an action with no report field" % control
        if controls[control]["behaviour"] != "button":
            assert not isinstance(action, ca.KeyAction) or action.name.startswith("jog_"), control


@pytest.mark.parametrize("mapping_file", MAPPINGS, ids=os.path.basename)
def test_mapping_led_roles_name_lit_note_controls(mapping_file):
    with open(mapping_file, encoding="utf-8") as fh:
        mapping = json.load(fh)
    with open(profiles.controller_path(mapping["controller"]), encoding="utf-8") as fh:
        controls = json.load(fh)["controls"]
    decks = live(mapping["decks"])
    for deck, roles in live(mapping.get("leds", {})).items():
        assert deck in decks
        for role, target in roles.items():
            assert role == "beat" or role in LAMP_ROLES, role
            for name in (target if role == "beat" else [target]):
                assert controls[name]["type"] == "note", name


def test_bridge_builds_from_the_shipped_mapping(capsys):
    """The bridge's --dry-run path: load the default profile and mapping and
    resolve every binding."""
    pytest.importorskip("mido")
    import bridge
    capture = profiles.load_json(profiles.controller_path(profiles.DEFAULT), "profile")
    mapping = profiles.load_json(profiles.mapping_path(profiles.DEFAULT), "mapping")
    b = bridge.Bridge(capture, mapping, bridge.Relay("127.0.0.1", 1, dry_run=True), 150, False)
    assert len(b.index) == len(live(mapping["bindings"]))
    assert "never learned" not in capsys.readouterr().out
