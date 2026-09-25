# SPDX-License-Identifier: GPL-2.0-or-later
"""midi/cdj_actions.py: the wire strings, name resolution and the platter maths."""
import pytest

import cdj_actions as ca


# -- datagrams -----------------------------------------------------------------

def test_key_datagram_is_timed_or_with_decimal_mask():
    cue = ca.ACTIONS["cue"]
    assert cue.datagram() == "0x10:2:150:or"
    assert cue.datagram(dur_ms=40) == "0x10:2:40:or"


def test_key_hold_and_release_datagrams():
    scan = ca.ACTIONS["scan_rev"]
    assert scan.hold_datagram() == "0x12:16:0:hold"
    assert scan.release_datagram() == "0x12:16:0:rel"


@pytest.mark.parametrize("value, expected", [(1, "0x0e:1:0:rot"), (-3, "0x0e:-3:0:rot"),
                                             (2.9, "0x0e:2:0:rot")])
def test_rotary_datagram_carries_the_signed_delta(value, expected):
    assert ca.ACTIONS["select_turn"].datagram(value) == expected


@pytest.mark.parametrize("value, byte", [(0, 0), (127, 254), (64, 128), (-5, 0), (200, 254)])
def test_level_datagram_scales_7_bit_midi_to_the_byte_and_clamps(value, byte):
    assert ca.ACTIONS["tempo"].datagram(value) == "0x04:%d:0:lvl" % byte


def test_level_full_scale_is_configurable():
    lvl = ca.LevelAction("x", 0x02, ca.GUESS, full_scale=127)
    assert lvl.datagram(127) == "0x02:127:0:lvl"


def test_jog_datagram_direction_is_the_round_fwd_bit():
    jog = ca.ACTIONS["jog"]
    assert jog.datagram(1) == "0x0f:%d:150:or" % (ca.JogAction.MOV | ca.JogAction.ROUND_FWD)
    assert jog.datagram(-1) == "0x0f:%d:150:or" % ca.JogAction.MOV


def test_touch_tap_sends_hex_x_and_clamps_to_the_screen():
    tap = ca.TouchTapAction("t", ca.PARTIAL, x=400, y=240)
    assert tap.datagram() == "0x190:240:150:tap"
    assert tap.datagram(dur_ms=0) == "0x190:240:1:tap"
    edge = ca.TouchTapAction("t", ca.PARTIAL, x=5000, y=-3)
    assert (edge.x, edge.y) == (799, 0)


def test_unbound_is_not_sendable_and_has_no_datagram():
    needle = ca.ACTIONS["needle_search"]
    assert needle.status == ca.UNBOUND
    assert not needle.sendable
    with pytest.raises(NotImplementedError):
        needle.datagram()


# -- resolve -------------------------------------------------------------------

def test_resolve_named_action_returns_the_table_entry():
    assert ca.resolve("cue") is ca.ACTIONS["cue"]


@pytest.mark.parametrize("old, new", [("select_push", "rotary_push"), ("menu", "menu_utility"),
                                      ("usb", "dev_usb")])
def test_old_aliases_still_resolve(old, new):
    assert ca.resolve(old) is ca.ACTIONS[new]


def test_raw_key_on_a_named_bit_inherits_its_status():
    a = ca.resolve("key_0x10_0x02")
    assert isinstance(a, ca.KeyAction)
    assert (a.off, a.mask, a.status) == (0x10, 0x02, ca.CONFIRMED)
    assert "'cue'" in a.note


def test_raw_key_on_an_unnamed_bit_is_a_guess():
    a = ca.resolve("KEY_0x1A_0x01")
    assert (a.off, a.mask, a.status) == (0x1A, 0x01, ca.GUESS)
    assert a.datagram() == "0x1a:1:150:or"


def test_raw_prefix_is_accepted_like_key():
    assert ca.resolve("raw_0x14_0x01").status == ca.CONFIRMED


def test_raw_level_and_rot():
    lvl = ca.resolve("level_0x02")
    assert isinstance(lvl, ca.LevelAction) and lvl.off == 0x02 and lvl.status == ca.GUESS
    rot = ca.resolve("rot_0x0e")
    assert isinstance(rot, ca.RotaryAction) and rot.off == 0x0E and rot.status == ca.GUESS


def test_touch_tap_takes_its_pixel_from_the_spec():
    a = ca.resolve("touch_tap", {"x": 10, "y": 20})
    assert (a.x, a.y) == (10, 20)
    assert a is not ca.ACTIONS["touch_tap"]
    assert (ca.resolve("touch_tap").x, ca.resolve("touch_tap").y) == (400, 240)


@pytest.mark.parametrize("name", ["nope", "key_0x100_0x01", "key_0x10", "level_0x", "rot_12"])
def test_unknown_names_raise_with_the_raw_forms_named(name):
    with pytest.raises(KeyError, match="key_0xOFF_0xMASK"):
        ca.resolve(name)


def test_named_bit_ignores_aliases_and_non_key_actions():
    assert ca.named_bit(0x11, 0x01).name == "rotary_push"
    assert ca.named_bit(0x1A, 0x01) is None


def test_raw_action_is_always_a_guess():
    a = ca.raw_action(0x10, 0x02)
    assert a.status == ca.GUESS and a.name == "raw_0x10_0x02"


# -- catalogue -----------------------------------------------------------------

def test_catalogue_lists_every_named_action_once_and_no_alias():
    rows = ca.catalogue()
    names = [r[0] for r in rows]
    assert len(names) == len(set(names))
    primary = {n for n, a in ca.ACTIONS.items() if a.name == n}
    assert primary <= set(names)
    assert not {"select_push", "menu", "usb"} & set(names)


def test_catalogue_covers_every_bit_of_the_key_bytes_exactly_once():
    covered = {}
    for name, kind, where, status, _note in ca.catalogue():
        if kind == "key":
            off, mask = (int(x, 16) for x in where.split(":"))
            assert (off, mask) not in covered, (name, covered.get((off, mask)))
            covered[(off, mask)] = name
    for off in ca.KEY_BYTES:
        for bit in range(8):
            assert (off, 1 << bit) in covered


def test_catalogue_raw_rows_resolve_to_what_they_say():
    for name, kind, where, status, _note in ca.catalogue():
        if name.startswith("key_0x"):
            a = ca.resolve(name)
            assert "0x%02x:0x%02x" % (a.off, a.mask) == where
            assert status == ca.GUESS


def test_every_action_has_a_known_status():
    known = {ca.CONFIRMED, ca.PARTIAL, ca.GUESS, ca.UNBOUND, ca.DECODED}
    assert all(a.status in known for a in ca.ACTIONS.values())


def test_no_two_named_keys_share_a_bit():
    seen = {}
    for name, a in ca.ACTIONS.items():
        if type(a) is ca.KeyAction and a.name == name:
            assert (a.off, a.mask) not in seen, (name, seen.get((a.off, a.mask)))
            seen[(a.off, a.mask)] = name


# -- Platter -------------------------------------------------------------------

def _grams_by_offset(grams):
    out = {}
    for g in grams:
        off, val, dur, op = g.split(":")
        out[int(off, 16)] = (int(val), int(dur), op)
    return out


def _spin(platter, n, delta=1, start=0.0, step=0.01):
    for i in range(n):
        platter.tick(delta, start + i * step)
    return start + (n - 1) * step


def test_platter_at_nominal_rate_reports_period_278():
    p = ca.Platter(nominal_tps=100.0, pulses_per_tick=48)
    now = _spin(p, 8)                      # 8 ticks in the 0.08 s window = 100/s
    g = _grams_by_offset(p.datagrams(now))
    assert (g[0x0A][0] << 8) | g[0x0B][0] == 278
    assert p.last_period == 278 and p.moving


def test_platter_twice_as_fast_halves_the_period():
    p = ca.Platter(nominal_tps=100.0)
    now = _spin(p, 16, step=0.005)
    assert p.datagrams(now) and p.last_period == 139


def test_platter_period_formula_is_27778_over_speed():
    # platter speed % = 27778 / P, so P = 27778 / (100 * rate / nominal)
    p = ca.Platter(nominal_tps=160.0)
    now = _spin(p, 8)
    p.datagrams(now)
    assert p.last_period == round(27778 / 100 * 160 / 100)


def test_platter_counter_is_big_endian_and_scaled():
    p = ca.Platter(pulses_per_tick=48)
    now = _spin(p, 3)
    g = _grams_by_offset(p.datagrams(now))
    assert (g[0x08][0] << 8) | g[0x09][0] == 144
    assert all(v[2] == "lvl" for k, v in g.items() if k != 0x0F)


def test_platter_counter_wraps_at_16_bits_going_backwards():
    p = ca.Platter(pulses_per_tick=48)
    p.tick(-1, 0.0)
    g = _grams_by_offset(p.datagrams(0.0))
    assert (g[0x08][0], g[0x09][0]) == (0xFF, 0xD0)
    assert g[0x0F] == (ca.JogAction.MOV, 150, "or")


def test_platter_counter_wraps_forwards():
    p = ca.Platter(pulses_per_tick=0x8000)
    p.tick(1, 0.0)
    p.tick(1, 0.001)
    assert p.count == 0


def test_platter_forward_sets_round_fwd():
    p = ca.Platter()
    now = _spin(p, 4)
    g = _grams_by_offset(p.datagrams(now, dur_ms=60))
    assert g[0x0F] == (ca.JogAction.MOV | ca.JogAction.ROUND_FWD, 60, "or")


def test_platter_stop_zeroes_the_period_once():
    p = ca.Platter()
    now = _spin(p, 4)
    assert p.datagrams(now)
    stop = p.datagrams(now + 0.5)
    assert stop == ["0x0a:0:0:lvl", "0x0b:0:0:lvl"]
    assert not p.moving
    assert p.datagrams(now + 1.0) == []


def test_platter_idle_from_the_start_sends_nothing():
    assert ca.Platter().datagrams(10.0) == []


def test_platter_period_is_clamped():
    slow = ca.Platter(nominal_tps=1e9)
    now = _spin(slow, 2)
    slow.datagrams(now)
    assert slow.last_period == 0xFFFE
    fast = ca.Platter(nominal_tps=1e-6)
    now = _spin(fast, 2)
    fast.datagrams(now)
    assert fast.last_period == 1
