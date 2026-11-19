# SPDX-License-Identifier: GPL-2.0-or-later
"""app/ faces for the older players: the face file against the board's key
table, and the window drawn off screen with the mouse driven by posted events."""
import os
import re
import sys
import time

import pytest

os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
pygame = pytest.importorskip("pygame")

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APP = os.path.join(EMU, "app")
if APP not in sys.path:
    sys.path.insert(0, APP)

import face as FACE  # noqa: E402
import virtual_deck as V  # noqa: E402


def board_keys(source, table):
    """{key name: (report byte, bit)} from a CdjGuiKey table in a board's source."""
    with open(os.path.join(EMU, "hw", "cdj", "boards", *source.split("/")), encoding="utf-8") as f:
        text = f.read()
    body = text[text.index("%s[] = {" % table):]
    body = body[:body.index("};")]
    return {name: (int(off, 16), int(mask, 16)) for off, mask, name in
            re.findall(r'Q_KEY_CODE_\w+,\s*(0x\w+),\s*(0x\w+),\s*"([^"]+)"', body)}


SH7724_KEYS = ("xdj1000/board.c", "sh7724_deck_keys")

# The model's key table, and the entries in it that the player has no key for.
KEY_TABLES = {
    "cdj2000nxs": ("cdj2000/display_bf531.c", "cdj_bf531_nxs_keys", set()),
    "cdj2000": ("cdj2000/display_bf531.c", "cdj_bf531_keys", set()),
    "xdj1000": (*SH7724_KEYS, {"USB"}),
    "xdj1000mk2": (*SH7724_KEYS, {"USB"}),
    "xdj700": (*SH7724_KEYS, {"USB"}),
    "cdj900nxs": (*SH7724_KEYS, set()),
    "cdj900": ("cdj900/display_m16c.c", "cdj900_keys", set()),
}


@pytest.mark.parametrize("model", sorted(KEY_TABLES))
def test_wired_keys_are_the_boards_key_table(model):
    source, table_name, absent = KEY_TABLES[model]
    face = FACE.load(model)
    table = board_keys(source, table_name)
    wired = {k.name: (k.off, k.mask) for k in face.keys if k.wired}
    assert wired and set(table) - set(wired) == absent, "a key in the board's table is missing from the face"
    assert {n: wired[n] for n in table if n in wired} == {n: table[n] for n in table if n in wired}


@pytest.mark.parametrize("model", ["cdj2000nxs", "cdj2000"])
def test_no_two_controls_share_a_report_bit(model):
    bits = [(k.off, k.mask) for k in FACE.load(model).keys if k.wired]
    assert len(bits) == len(set(bits))


# The controls with no report bit the firmware's decoder is known to give a
# name: USB STOP is named in the key table but no decoder branch sets it, the
# two pots and JOG ADJUST are analogue fields, and the CDJ-2000's eject lock
# is a level whose polarity is not known.
DARK = {"cdj2000nxs": {"USB STOP", "TOUCH/BRAKE", "RELEASE/START", "JOG ADJUST"},
        "cdj2000": {"USB STOP", "TOUCH/BRAKE", "RELEASE/START", "JOG ADJUST", "UNLOCK/LOCK"}}
LATCHED = {"cdj2000nxs": {"DIRECTION"}, "cdj2000": {"DIRECTION"}}


@pytest.mark.parametrize("model", ["cdj2000nxs", "cdj2000"])
def test_dark_keys_carry_no_report_bit(model):
    face = FACE.load(model)
    assert {k.name for k in face.keys if not k.wired} == DARK[model]
    assert all(k.mask is None for k in face.keys if not k.wired)
    assert {k.name for k in face.keys if k.latch} == LATCHED[model]


def test_each_player_wires_its_own_bits():
    nxs, older = FACE.load("cdj2000nxs"), FACE.load("cdj2000")
    assert {"SYNC", "MASTER", "QUANTIZE", "SLIP", "LOOP MODE", "PC"} <= {k.name for k in nxs.keys if k.wired}
    assert not any(k.name in ("SYNC", "MASTER", "QUANTIZE", "SLIP", "LOOP MODE", "PC") for k in older.keys)
    assert (nxs.key_named("SLIP").off, nxs.key_named("SLIP").mask) == (0x11, 0x08)
    assert (older.key_named("MASTER TEMPO").off, older.key_named("MASTER TEMPO").mask) == (0x13, 0x40)


def test_the_cdj900nxs_top_row_and_select_knob_send_what_the_cdj2000nxs_ones_do():
    # The same four keys over the screen, at the CDJ-2000NXS's places (INFO and
    # MENU print LINK INFO and UTILITY above them), and the same knob byte.
    nxs, older = FACE.load("cdj900nxs"), FACE.load("cdj2000nxs")
    for name in ("BROWSE", "TAG LIST", "INFO", "MENU"):
        a, b = nxs.key_named(name), older.key_named(name)
        assert (a.off, a.mask) == (b.off, b.mask)
        assert abs(a.rect[0] - b.rect[0]) < 3 * FACE.UNITS_PER_MM
    assert nxs.turn_datagram(1) == older.turn_datagram(1) == "0x0e:1:0:rot"


def test_every_face_file_loads_with_its_controls_inside_it():
    for name in FACE.available():
        face = FACE.load(name)
        w, h = face.size
        for k in face.keys:
            x0, y0, x1, y1 = (k.circle[0] - k.circle[2], k.circle[1] - k.circle[2],
                              k.circle[0] + k.circle[2], k.circle[1] + k.circle[2]) if k.circle else k.rect
            assert 0 <= x0 < x1 <= w and 0 <= y0 < y1 <= h, (name, k.name)


def make_app(model):
    a = V.App(V.parse_args(["--model", model, "--decks", "1", "--panel-port", "1", "--vnc-base", "1", "--hidden"]))
    deck = a.decks[0]
    deck.sent = []
    deck.link.send = lambda tag, dg: deck.sent.append(dg)
    return a


@pytest.fixture
def app(tmp_path, monkeypatch):
    monkeypatch.setenv("CDJ_APP_CACHE", str(tmp_path / "art"))
    a = make_app("cdj2000nxs")
    yield a
    a.close()


@pytest.fixture
def dark_app(tmp_path, monkeypatch):
    monkeypatch.setenv("CDJ_APP_CACHE", str(tmp_path / "art"))
    a = make_app("cdj900")
    yield a
    a.close()


def post(app, kind, **kw):
    kw.setdefault("window", app.window)
    app._event(pygame.event.Event(kind, **kw))


def at(app, x, y):
    s = app.decks[0].scale
    return app._left() + x * s, y * s


def test_the_older_deck_gets_its_own_face_and_title(app):
    assert app.window.title == "CDJ-2000NXS Virtual Deck"
    assert (app.face_w, app.face_h) == FACE.load("cdj2000nxs").size
    assert app.mode == "face"
    assert app.snapshot().size[0] > 100


def test_a_click_holds_and_releases_the_keys_report_bit(app):
    face = app.decks[0].face
    key = face.key_named("PLAY/PAUSE")
    pos = at(app, key.circle[0], key.circle[1])
    post(app, pygame.MOUSEBUTTONDOWN, pos=pos, button=1)
    post(app, pygame.MOUSEBUTTONUP, pos=pos, button=1)
    assert app.decks[0].sent == ["0x10:1:0:hold", "0x10:1:0:rel"]


def test_a_newly_wired_key_holds_its_firmware_bit(app):
    key = app.decks[0].face.key_named("SYNC")
    pos = at(app, key.circle[0], key.circle[1])
    post(app, pygame.MOUSEBUTTONDOWN, pos=pos, button=1)
    post(app, pygame.MOUSEBUTTONUP, pos=pos, button=1)
    assert app.decks[0].sent == ["0x15:2:0:hold", "0x15:2:0:rel"]


def test_the_direction_lever_stays_where_it_is_put(app):
    deck = app.decks[0]
    key = deck.face.key_named("DIRECTION")
    x0, y0, x1, y1 = key.rect
    pos = at(app, (x0 + x1) / 2, (y0 + y1) / 2)
    for _ in range(2):
        post(app, pygame.MOUSEBUTTONDOWN, pos=pos, button=1)
        post(app, pygame.MOUSEBUTTONUP, pos=pos, button=1)
    assert deck.sent == ["0x0f:2:0:hold", "0x0f:2:0:rel"]
    assert deck.latched == set()


def test_a_dark_key_sends_nothing(app):
    key = app.decks[0].face.key_named("USB STOP")
    pos = at(app, key.circle[0], key.circle[1])
    post(app, pygame.MOUSEBUTTONDOWN, pos=pos, button=1)
    post(app, pygame.MOUSEBUTTONUP, pos=pos, button=1)
    assert app.decks[0].sent == []


def test_the_wheel_turns_the_selector_down_the_list(app):
    cx, cy = app.decks[0].face.rotary
    app.mouse = at(app, cx, cy)
    post(app, pygame.MOUSEWHEEL, x=0, y=-1, flipped=False)
    assert app.decks[0].sent == ["0x0e:1:0:rot"]


def test_the_nxs2_keeps_its_own_face():
    a = V.parse_args(["--decks", "1"])
    assert a.model == "cdj2000nxs2"


def test_cdj2000nxs_face_is_the_specified_chassis_in_millimetres():
    face = FACE.load("cdj2000nxs")
    assert face.size == pytest.approx((320 * FACE.UNITS_PER_MM, 405.7 * FACE.UNITS_PER_MM))


def test_the_face_body_is_drawn_at_the_chassis_size_with_printed_labels(app):
    deck = app.decks[0]
    assert deck.body.get_size() == deck.surf.get_size() == tuple(round(v * deck.px) for v in deck.face.size)
    assert deck.face.printed and deck.face.slots


def test_a_pressed_key_is_drawn_down_and_restored_on_release(app):
    deck = app.decks[0]
    key = deck.face.key_named("PLAY/PAUSE")
    x, y, r = key.circle
    px = (round(x * deck.px), round(y * deck.px))
    rest = deck.surf.get_at(px)
    deck.down((x * deck.scale, y * deck.scale))
    deck.compose()
    deck.up((x * deck.scale, y * deck.scale))
    deck.compose()
    assert deck.surf.get_at(px) == rest


def test_every_deck_with_a_face_names_it_in_its_profile():
    from launcher import model
    for mid in FACE.available():
        assert model.load(mid).app_face == mid


def drag_jog(deck, radius_share):
    cx, cy, r = deck.face.jog
    deck.down(((cx + r * radius_share) * deck.scale, cy * deck.scale))
    deck.drag(((cx + r * radius_share * 0.9) * deck.scale, (cy + r * radius_share * 0.43) * deck.scale))
    deck.up((0, 0))


def test_dragging_the_jog_of_a_player_without_a_measured_counter_only_turns_it(dark_app):
    deck = dark_app.decks[0]
    drag_jog(deck, 0.5)
    assert deck.jog.angle != 0
    assert deck.compose()
    assert deck.sent == []


def test_dragging_the_platter_sends_the_counter_with_a_touch(app):
    deck = app.decks[0]
    drag_jog(deck, 0.2)
    assert deck.jog.angle != 0
    assert any(dg.startswith("0x08:") for dg in deck.sent)
    assert any(dg.startswith("0x0a:") for dg in deck.sent)
    assert "0x0f:32:0:hold" in deck.sent and "0x0f:32:0:rel" in deck.sent


def test_dragging_the_ring_bends_without_a_touch(app):
    deck = app.decks[0]
    drag_jog(deck, 0.95)
    assert any(dg.startswith("0x08:") for dg in deck.sent)
    assert "0x0f:32:0:hold" not in deck.sent


def test_dragging_the_slider_sends_the_position_word_after_the_centre(app):
    deck = app.decks[0]
    x, top, bottom = deck.face.slider[:3]
    deck.down((x * deck.scale, bottom * deck.scale))
    deck.up((0, 0))
    assert deck.sent == ["0x06:128:0:lvl", "0x07:0:0:lvl", "0x04:255:0:lvl", "0x05:255:0:lvl"]
    deck.sent.clear()
    deck.down((x * deck.scale, (top + bottom) / 2 * deck.scale))
    deck.up((0, 0))
    assert deck.sent == ["0x04:128:0:lvl", "0x05:0:0:lvl"]


def test_the_slider_of_a_player_without_a_measured_word_does_not_move(dark_app):
    deck = dark_app.decks[0]
    x, top, bottom = deck.face.slider[:3]
    deck.down((x * deck.scale, bottom * deck.scale))
    deck.up((0, 0))
    assert deck.sent == []


def test_the_jog_and_slider_are_wired_where_they_were_measured():
    measured = {"cdj850", "cdj2000nxs", "cdj2000", "xdj1000", "xdj1000mk2", "xdj700", "cdj900nxs"}
    assert {m for m in FACE.available() if FACE.load(m).jog_counter} == measured
    assert {m for m in FACE.available() if FACE.load(m).slider_off is not None} == measured


@pytest.fixture
def cdj850_app():
    a = make_app("cdj850")
    try:
        yield a
    finally:
        a.close()


def test_cdj850_jog_tracks_touch_on_the_top_plate_only(cdj850_app):
    deck = cdj850_app.decks[0]
    drag_jog(deck, 0.75)
    assert "0x0d:32:0:hold" in deck.sent
    assert "0x0d:32:0:rel" in deck.sent
    deck.sent.clear()
    drag_jog(deck, 0.95)
    assert "0x0d:32:0:hold" not in deck.sent


def test_cdj850_vinyl_knob_turns_and_sends_its_analog_level(cdj850_app):
    deck = cdj850_app.decks[0]
    x, y, _ = deck.vinyl_key.circle
    at_knob = (x * deck.scale, y * deck.scale)
    before = pygame.image.tobytes(deck.vinyl_layer[0], "RGBA")
    deck.hover(at_knob)
    assert "brake and release ramps" in cdj850_app.hover_text
    deck.down(at_knob)
    deck.drag((at_knob[0], -100))
    deck.up((at_knob[0], -100))
    assert deck.sent == ["0x18:255:0:lvl"]
    assert pygame.image.tobytes(deck.vinyl_layer[0], "RGBA") != before
    deck.wheel(at_knob, -1)
    assert deck.sent[-1] == "0x18:247:0:lvl"
    deck.rescale(deck.scale, deck.ratio)
    assert deck.vinyl_speed == 247


def test_cdj850_jog_display_uses_panel_state_without_a_lcd_frame(cdj850_app):
    deck = cdj850_app.decks[0]
    deck.screen.take_frame = lambda: None
    now = time.monotonic()
    deck.link.seen = now
    # MAIN page 1 captures after LOOP IN and after PLAY brakes to a stop.
    deck.link.frame = bytes.fromhex("010f581901001c420080000000000061")
    deck.tick(now)
    active = deck.jog_display
    assert active.position == 24 and active.cue == 14
    assert active.vinyl and active.audio_memory and not active.touch
    before = pygame.image.tobytes(deck.jog_display_layer[0], "RGBA")

    deck.link.frame = bytes.fromhex("010f784001001a2a008000000000008e")
    deck.tick(now)
    assert deck.jog_display.position == 63 and deck.jog_display.touch
    assert pygame.image.tobytes(deck.jog_display_layer[0], "RGBA") != before
    layer = deck.jog_display_layer
    deck.tick(now + 1)
    assert deck.jog_display_layer is layer

    # Firmware turns the memory ring off briefly while buffering the track.
    deck.link.frame = bytes.fromhex("0100180101001f0000800000000000ba")
    deck.tick(now + 1)
    assert deck.jog_display.position == 0 and not deck.jog_display.audio_memory
    assert deck.jog_display.vinyl
    deck.link.frame = bytes.fromhex("01004027010015250080000000000024")
    deck.tick(now + 1)
    assert deck.jog_display.audio_memory and not deck.jog_display.vinyl
    deck.tick(now + 4)
    assert deck.jog_display_layer is None


def test_cdj850_mode_lamps_and_rev_button_follow_firmware(cdj850_app):
    deck = cdj850_app.decks[0]
    deck.screen.take_frame = lambda: None
    rev = deck.face.key_named("DIRECTION REV")
    x0, y0, x1, y1 = rev.rect
    pos = ((x0 + x1) * deck.scale / 2, (y0 + y1) * deck.scale / 2)
    rev_pixel = (round((x0 + 2) * deck.px), round((y0 + y1) * deck.px / 2))
    modes = {rev.name, "VINYL", "MASTER TEMPO"}

    def show(frame_hex):
        now = time.monotonic()
        deck.link.page2 = bytes.fromhex(frame_hex), now
        deck.tick(now)
        deck.compose()
        return deck.lit & modes

    assert show("0200581101f2204f00040010d02f4a2d") == {"VINYL"}
    dark_red = deck.surf.get_at(rev_pixel).r
    assert show("0200582702f2304f00040090d02f4ad4") == {"VINYL", "MASTER TEMPO"}
    assert show("0200404f03f2104f00020090d02f4ac3") == {"MASTER TEMPO"}

    # Each click sends a fresh press and release. The lamp follows the report.
    deck.down(pos)
    deck.up(pos)
    assert rev.name not in deck.key_layers
    assert show("0200401006f0184f00060090d02f4a91") == {rev.name, "MASTER TEMPO"}
    assert deck.surf.get_at(rev_pixel).r > dark_red
    assert show("0200584003f0384f00040090d02f4af4") == modes
    assert show("0200581900f2284f00040010d02f4a3c") == {rev.name, "VINYL"}
    deck.down(pos)
    deck.up(pos)
    assert rev.name in deck.key_layers
    assert show("0200580101f2204f00040010d02f4a1d") == {"VINYL"}
    assert deck.surf.get_at(rev_pixel).r == dark_red
    assert deck.sent == ["0x12:32:0:hold", "0x12:32:0:rel"] * 2


def test_cdj850_transport_lamps_follow_captured_page_two(cdj850_app, monkeypatch):
    deck = cdj850_app.decks[0]
    deck.screen.take_frame = lambda: None
    beat_lamps = {f"AUTO BEAT LOOP {beats}" for beats in (1, 2, 4, 8)}
    transport = beat_lamps | {"PLAY/PAUSE", "CUE", "LOOP IN", "LOOP OUT", "RELOOP/EXIT"}

    def show(frame_hex):
        now = time.monotonic()
        deck.link.page2 = bytes.fromhex(frame_hex), now
        deck.tick(now)
        return deck.lit & transport

    draw = deck.art._k_big
    painted = []

    def record(p, key, lit, down):
        painted.append((key.name, lit, down))
        draw(p, key, lit, down)

    monkeypatch.setattr(deck.art, "_k_big", record)
    assert show("0200583602f2204f00040010d02f4a53") == beat_lamps | {
        "PLAY/PAUSE", "LOOP IN", "LOOP OUT"}
    key = deck.face.key_named("PLAY/PAUSE")
    pos = tuple(x * deck.scale for x in key.circle[:2])
    deck.down(pos)
    # PLAY/PAUSE turns off on the next pause phase, even with the button held.
    assert show("0200783107e8204f00060010d02f4a6b") == beat_lamps | {
        "CUE", "LOOP IN", "LOOP OUT"}
    assert ("PLAY/PAUSE", False, True) in painted
    deck.up(pos)
    assert "PLAY/PAUSE" not in deck.key_layers
    assert "PLAY/PAUSE" in show("021f583107f8204f00020810d82f4a86")
    # Fine-adjusting the cue point has CUE on and off phases of its own.
    assert "CUE" in show("021f788705ea204f00020810d82f4aec")
    assert "CUE" not in show("021f788705e0204f00020810d82f4ae2")

    assert show("023058050bb8214f00060a10d82f4a36") == beat_lamps | {
        "PLAY/PAUSE", "CUE", "LOOP IN", "RELOOP/EXIT"}
    assert show("023058590998214700060a10d82f4a60") == (beat_lamps - {"AUTO BEAT LOOP 8"}) | {
        "PLAY/PAUSE", "CUE", "RELOOP/EXIT"}
    assert show("0230580d0cd8214f00020a10d82f4a5b") == beat_lamps | {
        "PLAY/PAUSE", "CUE", "LOOP OUT", "RELOOP/EXIT"}
    assert show("020c58361398214b00061310d82f4a30") == (beat_lamps - {"AUTO BEAT LOOP 4"}) | {
        "PLAY/PAUSE", "CUE", "RELOOP/EXIT"}


def test_cdj850_source_and_usb_activity_lamps_follow_captured_page_two(cdj850_app):
    deck = cdj850_app.decks[0]
    deck.screen.take_frame = lambda: None
    activity = deck.face.key_named("USB ACTIVITY")
    x0, y0, x1, y1 = activity.rect
    pixel = tuple(round(v * deck.px / 2) for v in (x0 + x1, y0 + y1))
    sources = {"DISC", "USB", "PC", activity.name}

    def show(frame_hex):
        now = time.monotonic()
        deck.link.page2 = bytes.fromhex(frame_hex), now
        deck.tick(now)
        deck.compose()
        return deck.lit & sources

    # The stick can be accessed even while DISC is selected.
    assert show("020058000062221f00000004502f4acb") == {"DISC", activity.name}
    lit_red = deck.surf.get_at(pixel).r
    assert show("020058000060221f00000004502f4ac9") == {"DISC"}
    assert deck.surf.get_at(pixel).r < lit_red
    assert show("0200580000e2201f00000004502f4a4a") == {"USB", activity.name}
    # After USB STOP, USB remains selected while the activity light goes out.
    assert show("0200580000e0204000000000d02f4ae5") == {"USB"}
    # PC flashes while the firmware asks for confirmation.
    assert show("0200584208f4204f00060010d02f4a69") == {"USB", "PC"}
    assert show("0200584c09f0204f00060010d02f4a70") == {"USB"}
    assert show("020058880064200000000000502f4a31") == {"PC"}
    pos = ((x0 + x1) * deck.scale / 2, (y0 + y1) * deck.scale / 2)
    deck.down(pos)
    deck.up(pos)
    assert deck.sent == []


def test_cdj850_navigation_buttons_light_from_page_two(cdj850_app):
    deck = cdj850_app.decks[0]
    deck.screen.take_frame = lambda: None
    navigation = {"BROWSE", "TAG LIST", "INFO", "MENU"}
    x0, y0, x1, y1 = deck.face.key_named("BROWSE").rect
    pixel = (round((x0 + x1) * deck.px / 2), round((y0 + 0.35) * deck.px))

    def show(frame_hex, expected):
        now = time.monotonic()
        deck.link.page2 = bytes.fromhex(frame_hex), now
        deck.tick(now)
        deck.compose()
        assert deck.lit & navigation == expected

    show("0200581101f2204f00040010d02f4a2d", {"INFO"})
    dark = deck.surf.get_at(pixel).g
    pos = ((x0 + x1) * deck.scale / 2, (y0 + y1) * deck.scale / 2)
    deck.down(pos)
    deck.compose()
    held = deck.surf.get_at(pixel).g
    deck.up(pos)

    show("0200582202f2201f00040014502f4a92", {"BROWSE"})
    assert deck.surf.get_at(pixel).g > max(dark, held)
    show("0200585303f2202f00020018502f4ad6", {"TAG LIST"})
    assert deck.surf.get_at(pixel).g == dark
    # LIST MENU keeps TAG LIST lit. UTILITY lights MENU on its own.
    show("0200584708f020af00060018502f4a52", {"TAG LIST", "MENU"})
    show("0200583a0ef0208f00020018502f4a27", {"MENU"})
    show("020058880064200000000000502f4a31", set())


def test_cdj850_eject_and_standby_lamps_follow_firmware(cdj850_app):
    deck = cdj850_app.decks[0]
    deck.screen.take_frame = lambda: None
    eject = deck.face.key_named("DISC EJECT")
    standby = deck.face.key_named("STANDBY")
    eject_pos = tuple(v * deck.scale for v in eject.circle[:2])
    standby_pos = tuple(v * deck.scale for v in standby.circle[:2])
    standby_pixel = tuple(round(v * deck.px) for v in standby.circle[:2])
    lamps = {eject.name, standby.name}

    def show(frame_hex, expected):
        now = time.monotonic()
        deck.link.page2 = bytes.fromhex(frame_hex), now
        deck.tick(now)
        deck.compose()
        assert deck.lit & lamps == expected

    # The homing sequence flashes the eject symbol and then leaves it dark.
    show("0200588a0060621f00000004502f4a94", {eject.name})
    show("0200588a0060221f00000004502f4a54", set())
    idle_red = deck.surf.get_at(standby_pixel).r

    deck.down(eject_pos)
    assert eject.name not in deck.lit
    show("020058000060621f00000004502f4a0a", {eject.name})
    show("0200588a0060221f00000004502f4a54", set())
    deck.up(eject_pos)

    # MAIN requests standby only after the idle timer expires.
    show("020058000060221f01000004502f4aca", {standby.name})
    assert deck.surf.get_at(standby_pixel).r > idle_red
    deck.down(standby_pos)
    deck.up(standby_pos)
    assert deck.sent == ["0x13:1:0:hold", "0x13:1:0:rel"]
    show("020058000060221f00000004402f4ab9", set())
    assert deck.surf.get_at(standby_pixel).r == idle_red


@pytest.mark.parametrize("model", FACE.available())
def test_no_label_or_key_runs_into_the_jog_surround(model):
    import math
    import art
    face = FACE.load(model)
    cx, cy, r = face.jog
    labels = [(x, y, text, size) for x, y, text, size in face.printed]
    labels += [(x, y, text, 7.5) for x, y, text in face.pills + face.frames]
    for x, y, text, size in labels:
        w = max(art.text_width(line, size) for line in text.split("\n"))
        h = size * 1.12 * (text.count("\n") + 1)
        nearest = math.hypot(min(max(cx, x - w / 2), x + w / 2) - cx, min(max(cy, y - h / 2), y + h / 2) - cy)
        assert nearest >= r, (model, text)
    for k in face.keys:
        if k.circle:
            assert math.hypot(k.circle[0] - cx, k.circle[1] - cy) - k.circle[2] >= r, (model, k.name)


def test_a_touch_screen_sends_the_finger_in_lcd_pixels(app):
    deck = app.decks[0]
    deck.face = FACE.load("xdj1000")
    x0, y0, x1, y1 = deck.face.lcd
    deck.down(((x0 + x1) / 2 * deck.scale, (y0 + y1) / 2 * deck.scale))
    corner = ((x1 - 0.01) * deck.scale, (y1 - 0.01) * deck.scale)
    deck.drag(corner)
    deck.up(corner)
    assert deck.sent == ["400:240:1:touch", "799:479:1:touch", "799:479:0:touch"]


def test_a_player_without_a_touch_screen_ignores_the_lcd(app):
    deck = app.decks[0]
    x0, y0, x1, y1 = deck.face.screen
    deck.down(((x0 + x1) / 2 * deck.scale, (y0 + y1) / 2 * deck.scale))
    deck.up((0, 0))
    assert deck.sent == []


@pytest.mark.parametrize("model", ["xdj1000", "xdj1000mk2", "xdj700", "cdj900nxs", "cdj2000", "cdj2000nxs"])
def test_the_lcd_is_shown_at_the_aspect_of_the_boards_frame(model):
    face = FACE.load(model)
    x0, y0, x1, y1 = face.lcd
    fw, fh = face.frame
    assert (x1 - x0) / (y1 - y0) == pytest.approx(fw / fh)
    sx0, sy0, sx1, sy1 = face.screen
    assert sx0 <= x0 and sy0 <= y0 and x1 <= sx1 and y1 <= sy1


@pytest.mark.parametrize("model", sorted(FACE.available()))
def test_no_control_straddles_a_seam_between_the_panel_plates(model):
    face = FACE.load(model)
    x0, y0, x1, y1 = face.panel
    h = face.size[1]
    boxes = {k.name: k.rect or (k.circle[0] - k.circle[2], k.circle[1] - k.circle[2],
                                k.circle[0] + k.circle[2], k.circle[1] + k.circle[2]) for k in face.keys}
    if face.has_key("JOG MODE"):
        boxes["JOG MODE box"] = face.jog_mode_box
    for name, (a, b, c, d) in boxes.items():
        for seam in (x0, x1):
            assert not (a < seam < c and d > y0 and b < h), (model, name, "crosses the side plate edge")
        assert not (a < x1 and c > x0 and b < y1 < d), (model, name, "crosses the panel's lower edge")


@pytest.fixture
def mk2_app(tmp_path, monkeypatch):
    monkeypatch.setenv("CDJ_APP_CACHE", str(tmp_path / "art"))
    a = make_app("xdj1000mk2")
    yield a
    a.close()


def publish(deck, *bits):
    frame = bytearray(32)
    for off, mask in bits:
        frame[off] |= mask
    deck.link.frame, deck.link.seen = bytes(frame), time.monotonic()
    deck.tick(time.monotonic())


def test_a_key_lights_while_the_panel_frame_sets_its_lamp_bit(mk2_app):
    deck = mk2_app.decks[0]
    publish(deck, (0x00, 0x01))
    assert deck.lit == {"PLAY/PAUSE"} and "PLAY/PAUSE" in deck.key_layers
    publish(deck, (0x00, 0x02), (0x01, 0x04))
    assert deck.lit == {"CUE", "MASTER TEMPO"} and "PLAY/PAUSE" not in deck.key_layers


def test_the_lamps_go_dark_when_the_deck_stops_publishing(mk2_app):
    deck = mk2_app.decks[0]
    publish(deck, (0x00, 0x03))
    deck.link.seen -= 10
    deck.tick(time.monotonic())
    assert deck.lit == set() and deck.key_layers == {}


def test_a_lamp_lit_under_a_held_key_stays_lit_on_release(mk2_app):
    deck = mk2_app.decks[0]
    key = deck.face.key_named("CUE")
    publish(deck, (0x00, 0x02))
    deck.down((key.circle[0] * deck.scale, key.circle[1] * deck.scale))
    deck.up((key.circle[0] * deck.scale, key.circle[1] * deck.scale))
    assert "CUE" in deck.key_layers


def test_the_mk2_lamps_are_the_bits_measured_on_its_panel_frame():
    lamps = {k.name: k.lamp for k in FACE.load("xdj1000mk2").keys if k.lamp}
    assert lamps == {"PLAY/PAUSE": (0x00, 0x01), "CUE": (0x00, 0x02), "LOOP IN": (0x00, 0x10),
                     "LOOP OUT": (0x00, 0x20), "RELOOP/EXIT": (0x00, 0x40), "MASTER TEMPO": (0x01, 0x04),
                     "QUANTIZE": (0x01, 0x08)}


def test_the_panel_link_reads_the_frame_main_publishes():
    import socket
    import panel_link
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    link = panel_link.PanelLink(1, port)
    link.start()
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as main:
            deadline = time.monotonic() + 5
            while not link.lit(0x05, 0x10, 2) and time.monotonic() < deadline:
                main.sendto(b"pnl 0201" + b"00" * 30, ("127.0.0.1", port))
                main.sendto(b"pnl2 0200583602f2204f00040010d02f4a53", ("127.0.0.1", port))
                time.sleep(0.05)
        assert link.lit(0x00, 0x02) and link.lit(0x01, 0x01) and not link.lit(0x00, 0x01)
        assert link.lit(0x05, 0x10, 2) and not link.lit(0x05, 0x10)
        frame, seen = link.page2
        link.page2 = frame, seen - 4
        assert link.lit(0x00, 0x02) and not link.lit(0x05, 0x10, 2)
    finally:
        link.close()


@pytest.mark.parametrize("model", ["xdj1000", "xdj1000mk2", "xdj700"])
def test_the_xdj_select_knob_counts_in_the_byte_the_browse_list_follows(model):
    assert FACE.load(model).turn_datagram(-1) == "0x0e:-1:0:rot"


@pytest.mark.parametrize("model", ["xdj1000", "xdj700"])
def test_the_xdj1000_and_xdj700_lamps_are_the_bits_measured_on_their_panel_frame(model):
    lamps = {k.name: k.lamp for k in FACE.load(model).keys if k.lamp}
    assert lamps == {"PLAY/PAUSE": (0x02, 0x40), "CUE": (0x02, 0x20), "LOOP IN": (0x01, 0x02),
                     "LOOP OUT": (0x01, 0x04), "RELOOP/EXIT": (0x02, 0x80), "MASTER TEMPO": (0x02, 0x08)}


def test_the_cdj900nxs_lamps_are_the_xdj_bits_and_sync():
    lamps = {k.name: k.lamp for k in FACE.load("cdj900nxs").keys if k.lamp}
    assert lamps == {"PLAY/PAUSE": (0x02, 0x40), "CUE": (0x02, 0x20), "LOOP IN": (0x01, 0x02),
                     "LOOP OUT": (0x01, 0x04), "RELOOP/EXIT": (0x02, 0x80), "MASTER TEMPO": (0x02, 0x08),
                     "SYNC": (0x0D, 0x02)}


def test_the_cdj2000nxs_lamps_are_the_bits_measured_on_its_panel_frame():
    lamps = {k.name: k.lamp for k in FACE.load("cdj2000nxs").keys if k.lamp}
    assert lamps == {"PLAY/PAUSE": (0x02, 0x40), "CUE": (0x02, 0x20), "LOOP IN": (0x01, 0x02),
                     "LOOP OUT": (0x01, 0x04), "RELOOP/EXIT": (0x02, 0x80), "MASTER TEMPO": (0x02, 0x08),
                     "QUANTIZE": (0x01, 0x10), "SYNC": (0x0D, 0x02), "TEMPO RESET": (0x03, 0x01),
                     "LINK": (0x04, 0x80), "SD": (0x04, 0x08), "DISC": (0x04, 0x02), "PC": (0x03, 0x80)}


def test_the_cdj2000_lamps_are_the_bits_measured_on_its_panel_frame():
    lamps = {k.name: k.lamp for k in FACE.load("cdj2000").keys if k.lamp}
    assert lamps == {"PLAY/PAUSE": (0x01, 0x20), "CUE": (0x01, 0x10), "LOOP IN": (0x01, 0x02),
                     "LOOP OUT": (0x01, 0x04), "RELOOP/EXIT": (0x01, 0x40), "MASTER TEMPO": (0x02, 0x20),
                     "TEMPO RESET": (0x01, 0x80), "LINK": (0x03, 0x80), "SD": (0x03, 0x08), "DISC": (0x03, 0x02)}
