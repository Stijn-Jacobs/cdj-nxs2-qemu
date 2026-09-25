# SPDX-License-Identifier: GPL-2.0-or-later
"""midi/learn.py: classifying a captured burst, and agreeing with the bridge."""
import pytest

mido = pytest.importorskip("mido")
import bridge  # noqa: E402
import learn  # noqa: E402
import profiles  # noqa: E402

PORT = "Fake 0"


def ccs(values, channel=0, number=7):
    return [(PORT, mido.Message("control_change", channel=channel, control=number, value=v))
            for v in values]


def test_button_is_a_note():
    burst = [(PORT, mido.Message("note_on", channel=2, note=9, velocity=127)),
             (PORT, mido.Message("note_off", channel=2, note=9, velocity=0))]
    r = learn.classify(burst)
    assert (r["type"], r["channel"], r["number"], r["behaviour"]) == ("note", 2, 9, "button")
    assert (r["value_min"], r["value_max"], r["messages"]) == (0, 127, 2)


def test_fader_swept_end_to_end_is_absolute():
    assert learn.classify(ccs(range(0, 128, 8)))["behaviour"] == learn.ABSOLUTE


def test_offset_64_encoder():
    assert learn.classify(ccs([65, 65, 66, 63, 63]))["behaviour"] == learn.RELATIVE_OFFSET


def test_sign_magnitude_encoder():
    assert learn.classify(ccs([1, 1, 2, 65, 66]))["behaviour"] == learn.RELATIVE_SIGNBIT


@pytest.mark.parametrize("values", [[1, 1, 2, 1], [127, 127, 126, 127]])
def test_twos_complement_encoder_turned_one_way(values):
    assert learn.classify(ccs(values))["behaviour"] == learn.RELATIVE


def test_twos_complement_turned_both_ways_reads_as_absolute():
    """A known limit of the heuristic: 1..127 in one burst looks like a fader
    swept end to end, so an encoder must be learned turning one way."""
    assert learn.classify(ccs([1, 1, 2, 127, 126]))["behaviour"] == learn.ABSOLUTE


def test_too_few_messages_fall_back_to_absolute():
    assert learn.classify(ccs([65, 65, 63]))["behaviour"] == learn.ABSOLUTE


def test_dominant_stream_wins_and_the_rest_is_reported():
    burst = ccs([65] * 6) + [(PORT, mido.Message("note_on", channel=0, note=40, velocity=127))]
    r = learn.classify(burst)
    assert r["type"] == "cc" and r["number"] == 7
    assert r["also_seen"] == ["note:0:40"]


def test_pitchwheel_counts_only_when_nothing_else_arrived():
    pitch = [(PORT, mido.Message("pitchwheel", channel=3, pitch=p)) for p in (-8192, 0, 8191)]
    alone = learn.classify(pitch)
    assert (alone["type"], alone["behaviour"]) == ("pitch", learn.ABSOLUTE)
    assert (alone["value_min"], alone["value_max"]) == (0, 127)
    assert learn.classify(pitch + ccs([65] * 4))["type"] == "cc"


def test_empty_or_realtime_only_burst_is_none():
    assert learn.classify([]) is None
    assert learn.classify([(PORT, mido.Message("clock"))]) is None


@pytest.mark.parametrize("values, delta", [([65] * 4, 1), ([63] * 4, -1),
                                           ([1] * 4 + [65], None), ([1] * 4, 1),
                                           ([127] * 4, -1)])
def test_learned_encoding_decodes_back_to_the_same_direction(values, delta, capsys):
    """The bridge must read a control the way learn labelled it: a wrong
    encoding turns one detent into a jump of 64."""
    captured = learn.classify(ccs(values))
    captured["port"] = PORT
    sent = []

    class Relay:
        def send(self, tag, datagram):
            sent.append(datagram)

    b = bridge.Bridge({"controls": {"knob": captured}},
                      {"decks": {"a": "show1"}, "bindings": {"knob": {"action": "select_turn", "deck": "a"}}},
                      Relay(), 150, False)
    for _, msg in ccs(values):
        b.handle(PORT, msg)
    deltas = [int(d.split(":")[1]) for d in sent]
    assert all(abs(d) == 1 for d in deltas), (captured["behaviour"], deltas)
    if delta is not None:
        assert set(deltas) == {delta}


def test_read_checklist_skips_comments_and_splits_hints(tmp_path):
    p = tmp_path / "c.txt"
    p.write_text("# header\n\ndeck1/play -- the big button\nmixer/xfader\n", encoding="utf-8")
    assert learn.read_checklist(str(p)) == [("deck1/play", "the big button"), ("mixer/xfader", "")]


def test_shipped_checklist_parses():
    entries = learn.read_checklist(profiles.checklist_path(profiles.DEFAULT))
    assert entries and all(name for name, _ in entries)


def test_report_collisions_names_both_controls(capsys):
    c = {"port": PORT, "type": "note", "channel": 0, "number": 1}
    learn.report_collisions({"a": c, "b": dict(c), "c": dict(c, number=2)})
    out = capsys.readouterr().out
    assert "note ch0 #1: a, b" in out and "#2" not in out
    learn.report_collisions({"a": c})
    assert capsys.readouterr().out == ""
