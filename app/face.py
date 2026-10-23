# SPDX-License-Identifier: GPL-2.0-or-later
"""A player's face as data: app/faces/<model>.json.

The CDJ-2000NXS2's face is laid out in layout.py and drawn by art.py. Every
other player gets a file instead: its size and the position of every part in
millimetres from the top left of the chassis (measured on the manual's
top-down drawing), and its controls. A control with `off` and `mask` is a
report bit the firmware's panel decoder turns into a key the firmware names;
one without is drawn dark and does nothing, because no signal is known for it.
A control with `lamp` [byte, mask] lights while MAIN's frame to the panel MCU
sets that bit, measured on the player; one without never lights.
A lever (`latch`) stays where it was put: a click holds its bit, the next one
lets go.
"""

import json
import os
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))

# Face units a millimetre: a 320 mm chassis is then about as wide as the NXS2's
# 972-unit face, so the window scales both the same way.
UNITS_PER_MM = 3.0


@dataclass
class FaceKey:
    name: str
    label: str
    rect: tuple | None = None
    circle: tuple | None = None
    off: int | None = None
    mask: int | None = None
    keycap: str = ""
    kind: str = "rect"              # art.py's key look
    color: str = ""                 # layout.py's colour name
    symbol: str = ""
    font: int = 9
    latch: bool = False
    lamp: tuple | None = None       # the panel frame's (byte, mask) that lights it

    @property
    def wired(self):
        return self.off is not None

    def hit(self, x, y):
        if self.circle:
            cx, cy, r = self.circle
            return (x - cx) ** 2 + (y - cy) ** 2 <= r * r
        x0, y0, x1, y1 = self.rect
        return x0 <= x <= x1 and y0 <= y <= y1

    def datagram(self, op):
        return f"0x{self.off:02x}:{self.mask}:0:{op}"


@dataclass
class Face:
    title: str
    size: tuple
    panel: tuple
    screen: tuple
    needle: tuple | None            # the needle search strip, where the player has one
    rotary: tuple
    rotary_radius: float
    rotary_off: int | None          # the report byte the selector counts in, where it is known
    jog: tuple
    platter_radius: float
    slider: tuple                   # x, top and bottom of the cap's travel, cap w, h
    keys: list
    slots: list                     # recesses: the USB port, the SD slot
    printed: list                   # x, y, text, size
    pills: list                     # x, y, text
    badges: list                    # x, y, text, colour name
    frames: list                    # x, y, text
    rules: list                     # polylines
    standby: tuple | None           # the standby lamp
    touch: tuple | None = None      # the LCD's touch coordinate space, pixels
    touch_via: str = ""             # "panel": the panel MCU digitises it; "vnc": the display board's pointer
    usb_label: tuple | None = None  # the USB port's rating (text, y), where the NXS2's position is not the player's
    name_at: tuple | None = None    # x, y and width of the model name, where it sits on the panel
    slider_pad: tuple | None = None # the slider plate's margin above and below its scale
    zero_mark: bool = True          # the lamp at the slider's centre
    standby_icon: tuple = (-18.0, -29.0)    # the power symbol, from the standby lamp
    frame: tuple | None = None      # the LCD's pixels as the board draws them
    jog_counter: dict | None = None # the platter's report scale, where the jog counter has been measured
    slider_off: int | None = None   # the report byte of the tempo slider's position word, where it has been measured

    @property
    def lcd(self):
        """The part of `screen` the frame fills: the frame's own aspect, centred,
        with the bezel's black around it."""
        x0, y0, x1, y1 = self.screen
        if not self.frame:
            return self.screen
        w, h = x1 - x0, y1 - y0
        fw, fh = self.frame
        if w * fh > h * fw:
            w = h * fw / fh
        else:
            h = w * fh / fw
        cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
        return cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2

    @property
    def jog_mode_box(self):
        """The dark plate behind the JOG MODE key and its VINYL/CDJ lamps."""
        x0, y0, x1, y1 = self.key_named("JOG MODE").rect
        return x0 - 61, y0 - 8.5, x1 + 10, y1 + 8.5

    def key_named(self, name):
        return next(k for k in self.keys if k.name == name)

    def has_key(self, name):
        return any(k.name == name for k in self.keys)

    def turn_datagram(self, detents):
        return f"0x{self.rotary_off:02x}:{int(detents)}:0:rot" if self.rotary_off is not None else None


def available():
    return sorted(f[:-5] for f in os.listdir(os.path.join(HERE, "faces")) if f.endswith(".json"))


def _units(values):
    return tuple(v * UNITS_PER_MM for v in values)


def load(model):
    with open(os.path.join(HERE, "faces", model + ".json"), encoding="utf-8") as f:
        d = json.load(f)
    keys = [FaceKey(k["name"], k["label"],
                    rect=_units(k["rect"]) if "rect" in k else None,
                    circle=_units(k["circle"]) if "circle" in k else None,
                    off=int(k["off"], 16) if k.get("off") else None, mask=k.get("mask"),
                    keycap=k.get("keycap", ""), kind=k.get("kind", "rect"), color=k.get("color", ""),
                    symbol=k.get("symbol", ""), font=k.get("font", 9), latch=k.get("latch", False),
                    lamp=(int(k["lamp"][0], 16), k["lamp"][1]) if "lamp" in k else None)
            for k in d["keys"]]
    r, jog, sl, touch = d["rotary"], d["jog"], d["slider"], d.get("touch")
    return Face(
        title=d["title"], size=_units(d["size"]), panel=_units(d["panel"]), screen=_units(d["screen"]),
        needle=_units(d["needle"]) if d.get("needle") else None,
        rotary=_units(r["center"]), rotary_radius=r["radius"] * UNITS_PER_MM,
        rotary_off=int(r["turn_off"], 16) if "turn_off" in r else None,
        jog=_units(jog["center"] + [jog["radius"]]), platter_radius=jog["platter"] * UNITS_PER_MM,
        jog_counter=jog.get("counter"),
        slider=_units([sl["x"], sl["top"], sl["bottom"]] + sl["cap"]),
        slider_pad=_units(sl["pad"]) if "pad" in sl else None, zero_mark=sl.get("zero_mark", True),
        slider_off=int(sl["report"], 16) if "report" in sl else None,
        keys=keys,
        slots=[_units(box) for box in d["slots"]],
        printed=[_units(t[:2]) + (t[2], t[3]) for t in d["print"]],
        pills=[_units(t[:2]) + (t[2],) for t in d["pills"]],
        badges=[_units(t[:2]) + tuple(t[2:]) for t in d["badges"]],
        frames=[_units(t[:2]) + (t[2],) for t in d["frames"]],
        rules=[[_units(pt) for pt in line] for line in d["rules"]],
        standby=_units(d["standby"]) if d.get("standby") else None,
        touch=tuple(touch["size"]) if touch else None, touch_via=touch["via"] if touch else "",
        usb_label=(d["usb_label"]["text"], d["usb_label"]["y"] * UNITS_PER_MM) if "usb_label" in d else None,
        name_at=_units(d["name"]) if d.get("name") else None,
        standby_icon=_units(d["standby_icon"]) if "standby_icon" in d else Face.standby_icon,
        frame=tuple(d["frame"]) if "frame" in d else tuple(touch["size"]) if touch else None)
