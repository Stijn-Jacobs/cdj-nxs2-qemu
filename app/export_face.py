#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A player's face for the browser page: its geometry as JSON and its art as PNGs.

    python app/export_face.py <dir> [model ...]

writes into <dir>, for each model (default cdj2000nxs2; the others are the
names in app/faces/):

  * face-<model>.json: the face's geometry in face units, each key's look,
    its keycap, its lamp and its sprites, and the lamp table, so the page
    retypes none of them. The NXS2's comes from layout.py and leds.py, every
    other player's from its face file (face.py);
  * body-<model>.png: the face at rest, drawn by art.py or face_art.py exactly
    as the app draws it (the body, the jog, the selector, the centre display,
    the slider cap at its centre), at art.REF_SCALE pixels a unit;
  * keys/<model>-<key>-<state>.png: each working key pressed ("down") and,
    for a key with a lamp, lit ("lit") and both ("lit_down"), placed on the
    body at the pixel position the JSON gives.

The page draws only what changes on top of these. web_deck.py runs the export
when it starts and scripts/web/build_site.sh for the hosted page; the files
are plain static files, so any web server can serve them.
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import art as ART  # noqa: E402
import controls as C  # noqa: E402
import face as F  # noqa: E402
import layout as L  # noqa: E402
from face_art import FaceArt  # noqa: E402
from frames import LCD_SIZE  # noqa: E402
from relay_link import leds  # noqa: E402

NXS2 = "cdj2000nxs2"
STATES = {"down": (False, True), "lit": (True, False), "lit_down": (True, True)}


def face_name(model):
    return f"face-{model}.json"


def body_name(model):
    return f"body-{model}.png"


def _hex(rgb):
    return "#%02x%02x%02x" % tuple(rgb)


def _slug(name):
    return re.sub(r"[^a-z0-9]+", "_", name.lower()).strip("_")


def _shape(circle, box):
    # Key.hit's margins baked into the shape the page hit-tests.
    if circle:
        cx, cy, r = circle
        return {"circle": [cx, cy, r + 3]}
    x0, y0, x1, y1 = box
    return {"box": [x0 - 2, y0 - 2, x1 + 2, y1 + 2]}


def _nxs2_keys(looks):
    return [dict(name=k.name, kind=k.kind, action=k.action, look=looks[k.name], keycap=k.keycap,
                 lamp=k.lamp, color=_hex(k.color), **_shape(k.circle, k.box)) for k in L.KEYS]


def _face_keys(face):
    return [dict(name=k.name, kind=k.kind, look=C.LIVE if k.wired else C.INERT, keycap=k.keycap,
                 lamp=k.name if k.lamp else None, **_shape(k.circle, k.rect)) for k in face.keys]


def face(looks, scale, sprites):
    """The NXS2's JSON document; sprites: key name -> {state: [file, x, y]}."""
    keys = _nxs2_keys(looks)
    for k in keys:
        if k["name"] in sprites:
            k["sprites"] = sprites[k["name"]]
    return {
        "model": NXS2,
        "title": "CDJ-2000NXS2",
        "units": [L.W, L.H],
        "scale": scale,
        "body": body_name(NXS2),
        "lcd": list(L.LCD),
        "lcd_pixels": list(LCD_SIZE),
        "jog": list(L.JOG),
        "jog_top": L.JOG_TOP,
        "select": list(L.SELECT),
        "slider": list(L.SLIDER),
        "slider_frame": list(L.SLIDER_FRAME),
        "keys": keys,
        "lamps": {role: list(v) for role, v in leds.LAMPS.items()},
    }


def face_file(model, scale, sprites):
    """An older player's JSON document, from app/faces/<model>.json."""
    f = F.load(model)
    keys = _face_keys(f)
    for k in keys:
        if k["name"] in sprites:
            k["sprites"] = sprites[k["name"]]
    lcd = f.lcd
    return {
        "model": model,
        "title": f.title,
        "units": list(f.size),
        "scale": scale,
        "body": body_name(model),
        "lcd": list(lcd),
        # A player whose frame size is unmeasured gets a canvas a unit a pixel.
        "lcd_pixels": list(f.frame) if f.frame else [round(lcd[2] - lcd[0]), round(lcd[3] - lcd[1])],
        "jog": list(f.jog),
        "select": list(f.rotary) + [f.rotary_radius],
        "slider": list(f.slider[:3]),
        "keys": keys,
        "lamps": {k.name: list(k.lamp) for k in f.keys if k.lamp},
    }


def _paste(base, sprite):
    img, pos = sprite
    base.paste(img, tuple(pos), img if img.mode == "RGBA" else None)


def _sprites(out_dir, model, art, keys):
    """Write each working key's sprites; key name -> {state: [file, x, y]}."""
    sprites = {}
    for key, look, lamp in keys:
        if look == C.INERT:
            continue
        states = {}
        for state, (lit, down) in STATES.items():
            if lit and not lamp:
                continue
            img, pos = art.key_sprite(key, lit, down, look)
            name = f"keys/{model}-{_slug(key.name)}-{state}.png"
            img.save(os.path.join(out_dir, name))
            states[state] = [name, *pos]
        sprites[key.name] = states
    return sprites


def export(out_dir, model=NXS2, deck_label="DECK 1"):
    """Write one model's face into out_dir; the path of its JSON."""
    os.makedirs(os.path.join(out_dir, "keys"), exist_ok=True)
    scale = ART.REF_SCALE
    if model == NXS2:
        looks = {k.name: C.look_for(k.action) for k in L.KEYS}
        art = ART.make_art(scale, deck_label)
        body = art.body([(k, looks[k.name]) for k in L.KEYS]).copy()
        _paste(body, art.jog_sprite(0))
        x, top, bottom = L.SLIDER
        w, h = art.SLIDER_CAP
        _paste(body, (art.slider_cap(), (round((x - w / 2 - 4) * scale),
                                         round((top + (bottom - top) / 2 - h / 2 - 4) * scale))))
        keys = [(k, looks[k.name], k.lamp) for k in L.KEYS]
    else:
        f = F.load(model)
        art = FaceArt(scale, f, deck_label)
        body = art.body().convert("RGBA")
        _paste(body, art.jog_sprite(0))
        if f.slider_off is not None:
            _paste(body, (art.slider_cap(), art.cap_pos(0.5)))
        keys = [(k, C.LIVE if k.wired else C.INERT, k.lamp) for k in f.keys]
    _paste(body, art.select_sprite(0, False))
    _paste(body, art.centre_sprite(False))
    body.convert("RGB").save(os.path.join(out_dir, body_name(model)))
    sprites = _sprites(out_dir, model, art, keys)
    doc = face(looks, scale, sprites) if model == NXS2 else face_file(model, scale, sprites)
    path = os.path.join(out_dir, face_name(model))
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1)
    return path


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: python app/export_face.py <dir> [model ...]")
    for m in sys.argv[2:] or [NXS2]:
        print(export(sys.argv[1], m))
