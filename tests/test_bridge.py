# SPDX-License-Identifier: GPL-2.0-or-later
"""midi/bridge.py: MIDI message -> datagram, with a fake relay and no device."""
import pytest

mido = pytest.importorskip("mido")
import bridge  # noqa: E402
import cdj_actions as ca  # noqa: E402

PORT = "Fake 0"


class FakeRelay:
    def __init__(self):
        self.sent = []

    def send(self, tag, datagram):
        self.sent.append((tag, datagram))


def control(kind, channel, number, behaviour):
    return {"port": PORT, "type": kind, "channel": channel, "number": number,
            "behaviour": behaviour}


CAPTURE = {"controls": {
    "enc": control("cc", 15, 0, "relative"),
    "enc_off": control("cc", 15, 1, "relative_offset"),
    "enc_sign": control("cc", 15, 2, "relative_signbit"),
    "platter": control("cc", 0, 6, "relative_offset"),
    "fader": control("cc", 0, 9, "absolute"),
    "pitch": control("pitch", 1, 0, "absolute"),
    "cue": control("note", 0, 1, "button"),
    "load_b": control("note", 0, 2, "button"),
    "needle": control("note", 0, 3, "button"),
    "tap": control("note", 0, 4, "button"),
    "learned_only": control("note", 0, 5, "button"),
}}

MAPPING = {
    "decks": {"_note": "ignored", "cdjA": "show1", "cdjB": "show2"},
    "focus": "cdjA",
    "bindings": {
        "_note": "ignored",
        "enc": {"action": "select_turn", "deck": "focus"},
        "enc_off": {"action": "select_turn", "deck": "cdjA"},
        "enc_sign": {"action": "select_turn", "deck": "cdjA", "scale": 2},
        "platter": {"action": "jog", "deck": "cdjB"},
        "fader": {"action": "tempo", "deck": "cdjA"},
        "pitch": {"action": "tempo", "deck": "cdjB"},
        "cue": {"action": "cue", "deck": "cdjA"},
        "load_b": {"action": "set_focus", "deck": "cdjB"},
        "needle": {"action": "needle_search", "deck": "cdjA"},
        "tap": {"action": "touch_tap", "deck": "cdjA", "x": 16, "y": 32},
        "not_learned": {"action": "cue", "deck": "cdjA"},
    },
}


@pytest.fixture
def rig(capsys):
    relay = FakeRelay()
    b = bridge.Bridge(CAPTURE, MAPPING, relay, dur_ms=150, verbose=False)
    capsys.readouterr()
    return b, relay


def cc(channel, control_, value):
    return mido.Message("control_change", channel=channel, control=control_, value=value)


def test_index_skips_notes_and_unlearned_controls(capsys):
    b = bridge.Bridge(CAPTURE, MAPPING, FakeRelay(), 150, False)
    out = capsys.readouterr().out
    assert "not_learned: in the map but never learned" in out
    assert "learned_only" in out
    assert len(b.index) == 10
    assert b.decks == {"cdjA": "show1", "cdjB": "show2"}


@pytest.mark.parametrize("value, delta", [(1, 1), (3, 3), (127, -1), (120, -8)])
def test_twos_complement_encoder(rig, value, delta):
    b, relay = rig
    b.handle(PORT, cc(15, 0, value))
    assert relay.sent == [("show1", "0x0e:%d:0:rot" % delta)]


@pytest.mark.parametrize("value, delta", [(65, 1), (63, -1), (70, 6)])
def test_offset_64_encoder(rig, value, delta):
    b, relay = rig
    b.handle(PORT, cc(15, 1, value))
    assert relay.sent == [("show1", "0x0e:%d:0:rot" % delta)]


@pytest.mark.parametrize("value, delta", [(1, 2), (65, -2), (66, -4)])
def test_sign_magnitude_encoder_with_scale(rig, value, delta):
    b, relay = rig
    b.handle(PORT, cc(15, 2, value))
    assert relay.sent == [("show1", "0x0e:%d:0:rot" % delta)]


def test_zero_delta_sends_nothing(rig):
    b, relay = rig
    b.handle(PORT, cc(15, 1, 64))
    b.handle(PORT, cc(15, 0, 0))
    assert relay.sent == []


def test_focus_switch_moves_the_focus_deck(rig):
    b, relay = rig
    b.handle(PORT, mido.Message("note_on", channel=0, note=2, velocity=127))
    b.handle(PORT, cc(15, 0, 1))
    assert b.focus == "cdjB"
    assert relay.sent == [("show2", "0x0e:1:0:rot")]


def test_button_holds_on_note_on_and_releases_on_note_off(rig):
    b, relay = rig
    b.handle(PORT, mido.Message("note_on", channel=0, note=1, velocity=100))
    b.handle(PORT, mido.Message("note_on", channel=0, note=1, velocity=0))
    b.handle(PORT, mido.Message("note_on", channel=0, note=1, velocity=100))
    b.handle(PORT, mido.Message("note_off", channel=0, note=1))
    assert relay.sent == [("show1", "0x10:2:0:hold"), ("show1", "0x10:2:0:rel")] * 2


def test_timed_press_is_rearmed_while_held(rig):
    b, relay = rig
    down = mido.Message("note_on", channel=0, note=4, velocity=127)
    b.handle(PORT, down)
    assert relay.sent == [("show1", "0x10:32:150:tap")]
    b.refresh_holds()
    assert len(relay.sent) == 2
    b.handle(PORT, mido.Message("note_off", channel=0, note=4))
    b.refresh_holds()
    assert len(relay.sent) == 2


def test_unbound_action_is_refused_once_and_never_sent(rig, capsys):
    b, relay = rig
    for _ in range(3):
        b.handle(PORT, mido.Message("note_on", channel=0, note=3, velocity=127))
    assert relay.sent == []
    assert capsys.readouterr().out.count("NOT SENT") == 1


def test_absolute_fader_only_sends_changes(rig):
    b, relay = rig
    for v in (10, 10, 11, 11, 127):
        b.handle(PORT, cc(0, 9, v))
    assert relay.sent == [("show1", "0x04:20:0:lvl"), ("show1", "0x04:22:0:lvl"),
                          ("show1", "0x04:254:0:lvl")]


@pytest.mark.parametrize("pitch, seven_bit", [(-8192, 0), (0, 64), (8191, 127)])
def test_pitchwheel_is_scaled_to_seven_bits(rig, pitch, seven_bit):
    b, relay = rig
    b.handle(PORT, mido.Message("pitchwheel", channel=1, pitch=pitch))
    want = ca.ACTIONS["tempo"].datagram(seven_bit)
    assert relay.sent == ([("show2", want)] if seven_bit else [])


def test_platter_ticks_feed_the_period_counter(rig):
    b, relay = rig
    b.handle(PORT, cc(0, 6, 65))
    assert relay.sent == []                 # the jog goes out on refresh, not per tick
    assert "cdjB" in b.platters
    import time
    b.refresh_platters(time.time())
    offsets = [g.split(":")[0] for _, g in relay.sent]
    assert offsets == ["0x08", "0x09", "0x0a", "0x0b", "0x0f"]
    assert {t for t, _ in relay.sent} == {"show2"}


def test_unknown_messages_and_controls_are_ignored(rig):
    b, relay = rig
    b.handle(PORT, mido.Message("program_change", program=3))
    b.handle("Other port", cc(15, 0, 1))
    b.handle(PORT, cc(3, 3, 1))
    assert relay.sent == []


def test_binding_naming_a_missing_deck_raises(capsys):
    mapping = {"decks": {"cdjA": "show1"}, "bindings": {"cue": {"action": "cue", "deck": "cdjZ"}}}
    b = bridge.Bridge(CAPTURE, mapping, FakeRelay(), 150, False)
    with pytest.raises(KeyError, match="cdjZ"):
        b.handle(PORT, mido.Message("note_on", channel=0, note=1, velocity=1))


def test_relay_dry_run_never_connects():
    r = bridge.Relay("127.0.0.1", 1, dry_run=True)
    assert r.connect()
    r.send("show1", "0x10:2:0:hold")
    assert r.sock is None and r.lines() == []
