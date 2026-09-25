# SPDX-License-Identifier: GPL-2.0-or-later
"""midi/leds.py: deck state lines -> which controller LEDs are lit (dry run)."""
import time

import pytest

pytest.importorskip("mido")
import leds  # noqa: E402
import state_watch  # noqa: E402


def note(number, led=True):
    return {"type": "note", "channel": 0, "number": number, "led": led}


CONTROLS = {
    "fx1": note(1), "fx2": note(2), "fx3": note(3), "tap": note(4),
    "play": note(10), "cue": note(11), "slip": note(12), "mt": note(13), "ring": note(14),
    "deck1/pad_1": note(20), "deck1/pad_2": note(21, led=False),
    "fader": {"type": "cc", "channel": 0, "number": 30},
}
ROLES = {"beat": ["fx1", "fx2", "fx3", "tap"], "play": "play", "cue": "cue",
         "slip": "slip", "master_tempo": "mt", "ring": "ring"}


def panel(b0=0, b1=0, b2=0):
    return bytes([b0, b1, b2]) + bytes(37)


def lit(surface, *names):
    return {n: surface.lit.get(surface.address(n)) == 127 for n in names}


@pytest.fixture
def deck():
    surface = leds.LedSurface(CONTROLS, "unused", dry_run=True)
    d = leds.DeckLeds("cdjA", "show1", ROLES, surface, pads=["deck1/pad_1"])
    return d, surface


# -- parse_state ---------------------------------------------------------------

def test_parse_frm_keeps_integer_fields_only():
    assert leds.parse_state("frm beat=3 bars=13 bpm=1280 junk=x flag") == \
        ("frm", {"beat": 3, "bars": 13, "bpm": 1280})


def test_parse_pnl_hex():
    kind, value = leds.parse_state("pnl " + "0102" + "00" * 38)
    assert kind == "pnl" and value[:2] == b"\x01\x02" and len(value) == 40


@pytest.mark.parametrize("payload", ["pnl zz", "hello world", ""])
def test_parse_rejects_what_it_does_not_know(payload):
    assert leds.parse_state(payload) == (None, None)


# -- the surface -----------------------------------------------------------------

def test_surface_addresses_only_note_controls():
    s = leds.LedSurface(CONTROLS, "unused", dry_run=True)
    assert s.address("play") == (0, 10)
    assert s.address("fader") is None and s.address("missing") is None
    s.set("fader", True)
    assert s.lit == {}


def test_surface_forget_drops_the_cache_entry():
    s = leds.LedSurface(CONTROLS, "unused", dry_run=True)
    s.set("play", True)
    s.forget(0, 10)
    assert s.lit == {}
    s.set("play", True)
    s.all_off()
    assert s.lit == {}


# -- lamps -------------------------------------------------------------------------

@pytest.mark.parametrize("frame, on", [
    (panel(0x01), {"play"}),
    (panel(0x02), {"cue"}),
    (panel(0x80), {"slip"}),
    (panel(0, 0x04), {"mt"}),
    (panel(0, 0, 0x0C), {"ring"}),
    (panel(0, 0, 0x01), {"ring"}),
    (panel(0x83, 0x04, 0x0F), {"play", "cue", "slip", "mt", "ring"}),
    (panel(0x7C, 0xFB, 0xF0), set()),
])
def test_panel_bits_drive_their_lamps(deck, frame, on):
    d, surface = deck
    d.update("pnl", frame)
    d.render(time.time())
    names = ["play", "cue", "slip", "mt", "ring"]
    assert lit(surface, *names) == {n: n in on for n in names}


def test_no_panel_frame_yet_keeps_lamps_dark(deck):
    d, surface = deck
    d.update("frm", {"beat": 1})
    d.render(time.time())
    assert not any(lit(surface, "play", "cue").values())


def test_stale_deck_goes_dark_but_pads_stay_lit(deck):
    d, surface = deck
    d.update("pnl", panel(0x01))
    d.update("frm", {"beat": 2, "bars": 10})
    d.render(d.seen + leds.STALE_S + 1)
    assert lit(surface, "play", "fx2", "deck1/pad_1") == \
        {"play": False, "fx2": False, "deck1/pad_1": True}


# -- the beat ----------------------------------------------------------------------

@pytest.mark.parametrize("beat, bars, on", [
    (1, 10, {"fx1"}),
    (3, 10, {"fx3"}),
    (4, 5, {"tap"}),
    (2, 4, {"fx1", "fx2"}),              # last bar of the phrase: fill
    (4, 1, {"fx1", "fx2", "fx3", "tap"}),
    (3, 0, {"fx3"}),                     # bars 0 is no countdown, not a fill
    (0, 3, set()),
    (5, 3, set()),
])
def test_beat_leds(deck, beat, bars, on):
    d, surface = deck
    d.update("frm", {"beat": beat, "bars": bars})
    d.render(time.time())
    names = ROLES["beat"]
    assert lit(surface, *names) == {n: n in on for n in names}


# -- the whole mapping ---------------------------------------------------------------

def test_leds_routes_state_by_tag_and_lights_bound_pads():
    mapping = {
        "decks": {"_note": "x", "cdjA": "show1"},
        "bindings": {"_n": {}, "deck1/pad_1": {"action": "browse", "deck": "cdjA"},
                     "deck1/pad_2": {"action": "back", "deck": "cdjA"}},
        "leds": {"cdjA": ROLES},
    }
    all_leds = leds.Leds(CONTROLS, mapping, "unused", dry_run=True)
    deck = all_leds.by_tag["show1"]
    assert deck.pads == ["deck1/pad_1"]          # pad_2's control has no LED
    all_leds.feed("show9", "pnl " + "01" * 40)   # nobody by that tag
    all_leds.feed("show1", "pnl " + "01" + "00" * 39)
    all_leds.feed("show1", "bogus")
    all_leds.render()
    assert lit(all_leds.surface, "play", "deck1/pad_1", "deck1/pad_2") == \
        {"play": True, "deck1/pad_1": True, "deck1/pad_2": False}
    all_leds.close()
    assert all_leds.surface.lit == {}


def test_state_watch_lamp_word():
    word = state_watch.lamp_word(panel(0x01, 0x04))
    assert word == "play=# cue=. slip=. master_tempo=# ring=."
