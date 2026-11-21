# SPDX-License-Identifier: GPL-2.0-or-later
"""The drawing of a face file (face.py), in art.py's materials.

Art's key drawers and print helpers do the work: each face key names the kind
of key it is (art.py's `_k_<kind>`), and the printed labels, plates and rules
come from the face file in millimetres. The greys, the metal and the lamp
colours are sampled from a straight-down product photo of the player. The
manufacturer's logos and names are not drawn.
"""

import math

from PIL import Image, ImageChops, ImageEnhance

import art as ART
import layout as L
from art import Painter
from cdj850_panel import CUE_SEGMENTS, JOG_FRAMES

LEFT_PLATE = (46, 46, 47)
RIGHT_PLATE = (32, 32, 33)
LOWER_PLATE = (8, 8, 9)
DIM = 0.8                               # how much of its brightness a dark key keeps


class FaceArt(ART.Art):
    def __init__(self, scale, face, deck_label):
        super().__init__(scale, deck_label)
        self.face = face
        self.size = (round(face.size[0] * scale), round(face.size[1] * scale))
        self.keys = {k.name: self._layout_key(k) for k in face.keys}
        self._arts = {}
        # The measured jog circle is the surround's edge; the NXS2's jog sprite
        # sits 10 units inside its own surround.
        self.jog = face.jog[:2] + (face.jog[2] - 10,)

    @staticmethod
    def _layout_key(k):
        color = getattr(L, k.color.upper()) if k.color else L.LIME
        # A dome or ring key shows its lamp as the centre LED art.py draws for a dot.
        return L.Key(k.name, k.kind, None, box=k.rect, circle=k.circle, label=k.label,
                     color=color, symbol=k.symbol, font=k.font, lamp=k.name if k.lamp else None,
                     dot=color if k.lamp else None)

    def key_sprite(self, key, lit, down, look):
        return super().key_sprite(self.keys[key.name], lit, down, look)

    def _ink(self, x, y):
        return ART.PRINT

    # -- the body ---------------------------------------------------------------

    def body(self, keys_rest=None):
        w, h = self.face.size
        p = Painter((0, 0, w, h), self.scale)
        self._plates(p)
        self._panel(p)
        if self.face.needle:
            self._needle(p)
        self._usb(p, self.face.slots[0])
        if len(self.face.slots) > 1:
            self._sd(p, self.face.slots[1])
        if self.face.standby:
            self._standby(p)
        self._jog_surround(p, self.jog)
        self._slider_face(p)
        if self.face.has_key("JOG MODE"):
            self._jog_mode_panel(p)
        for x, y, text, size in self.face.printed:
            p.text(x, y, text, size, self._ink(x, y))
        for x, y, text in self.face.pills:
            self._pill(p, x, y, text)
        for x, y, text, color in self.face.badges:
            self._badge(p, x, y, text, getattr(L, color.upper()))
        for x, y, text in self.face.frames:
            self._frame(p, x, y, text)
        for pts in self.face.rules:
            p.line(pts, self._ink(*pts[0]), 1.2)
        for k in self.face.keys:
            self._body_key(p, k)
        self._names(p)
        body = p.finish()
        if self.face.slider_off is None:
            body.alpha_composite(self.slider_cap(), self.cap_pos(0.5))
        return body.convert("RGB")

    def cap_pos(self, pos):
        """Where the slider cap's sprite goes with the fader `pos` of the way down."""
        x, top, bottom = self.face.slider[:3]
        cap_w, cap_h = self.SLIDER_CAP
        return (round((x - cap_w / 2 - 4) * self.scale),
                round((top + (bottom - top) * pos - cap_h / 2 - 4) * self.scale))

    def _plates(self, p):
        w, h = self.face.size
        x0, y0, x1, y1 = self.face.panel
        p.rrect((0, 0, w, h), 10, fill=(4, 4, 5))
        if x0 > 20:
            p.lit((2, y0, x0 - 2, h - 2), LEFT_PLATE, 40, (0, 150, 400, 700), radius=8)
            p.lit((x1 + 2, y0, w - 2, h - 2), RIGHT_PLATE, 14, (x1, 150, 300, 900), radius=8)
        p.lit((x0 + 2, y1 + 4, x1 - 2, h - 2), LOWER_PLATE, 150, (x0, y1 + 8, 300, 190), brushed=True)
        p.line([(x0, y1), (x0, h)], (8, 8, 9), 2)
        p.line([(x1, y1), (x1, h)], (8, 8, 9), 2)
        p.rrect((x0 + 20, h - 14, x1 - 20, h + 4), 8, fill=(18, 18, 20), outline=(60, 62, 66), width=1)

    def _panel(self, p):
        x0, y0, x1, y1 = self.face.panel
        p.rrect((x0, y0, x1, y1 + 4), 8, fill=(4, 4, 5))
        p.gradient((x0 + 2, y0 + 2, x1 - 2, y1), (22, 22, 24), ART.GLOSS, radius=7)
        p.gradient((x0 + 4, y0 + 2, x1 - 4, y0 + 22), (48, 49, 52), (14, 14, 15), radius=5)
        p.line([(x0 + 6, y0 + 23), (x1 - 6, y0 + 23)], (70, 72, 76), 1)
        p.gradient((x0, y1 - 3, x1, y1 + 5), (170, 172, 178), (40, 41, 44))
        lx0, ly0, lx1, ly1 = self.face.screen
        p.rrect((lx0 - 10, ly0 - 12, lx1 + 10, ly1 + 10), 4, fill=(2, 2, 3), outline=(44, 45, 48), width=1)
        sx, sy, r = self.face.rotary + (self.face.rotary_radius,)
        p.rrect((sx - r - 12, sy - r - 12, sx + r + 12, sy + r + 12), 10, fill=(24, 25, 27),
                outline=(60, 62, 66), width=1)
        p.glow((x0, y0, x0 + 383, y1), lambda d, c: d.polygon(
            [p.p(x0, y0 + 0.62 * (y1 - y0)), p.p(x0 + 293, y0), p.p(x0 + 383, y0), p.p(x0 + 93, y1), p.p(x0, y1)], fill=c),
            (255, 255, 255), 30, 0.05, over=True)

    def _needle(self, p):
        x0, y0, x1, y1 = self.face.needle
        mid = (y0 + y1) / 2
        p.rrect((x0 - 2, y0 - 2, x1 + 2, y1 + 3), (y1 - y0) / 2 + 2, fill=(6, 6, 7))
        p.gradient((x0, y0, x1, y1), (22, 22, 24), (8, 8, 9), radius=(y1 - y0) / 2)
        p.rrect((x0, y0, x1, y1), (y1 - y0) / 2, outline=(70, 72, 76), width=1)
        n = 41
        for i in range(n):
            x = x0 + 24 + i * (x1 - x0 - 48) / (n - 1)
            half = 7 if i % 5 == 0 else 4
            p.line([(x, mid - half), (x, mid + half)], ART.PRINT_DIM, 1)
        for x, d in ((x0 + 14, 1), (x1 - 14, -1)):
            p.d.polygon([p.p(x, mid - 9), p.p(x - d * 5, mid + 9), p.p(x + d * 5, mid + 9)], fill=ART.PRINT)

    # The USB port and the SD slot are drawn as the NXS2's are, from the box
    # their recess occupies on the face.

    def _usb(self, p, box):
        x0, y0, x1, y1 = box
        p.rrect(box, 5, fill=(10, 10, 11))
        p.gradient((x0 + 2, y0 + 2, x1 - 2, y1 - 2), (200, 202, 208), (110, 112, 118), radius=4)
        p.rrect((x0 + 9, y0 + 10, x1 - 9, y1 - 16), 3, fill=(14, 14, 15))
        p.rrect((x0 + 15, y0 + 16, x1 - 15, y0 + 26), 1, fill=(62, 64, 68))
        cx = (x0 + x1) / 2
        if self.face.usb_label:
            p.text(cx, self.face.usb_label[1], self.face.usb_label[0], 7, ART.PRINT)
            return
        p.text(cx, y1 + 12, "5V = 2.1A", 7, ART.PRINT)
        p.line([(cx - 8, y1 + 24), (cx + 8, y1 + 24)], ART.PRINT, 1)
        if self.face.has_key("USB STOP"):
            key = self.face.key_named("USB STOP")
            sx, sy = key.circle[0] - 4, key.circle[1] - 69
            p.rrect((sx, sy, sx + 14, sy + 10), 1, outline=ART.PRINT, width=1)
            p.line([(sx - 3, sy + 12), (sx + 17, sy + 12)], ART.PRINT, 1)

    def _sd(self, p, box):
        x0, y0, x1, y1 = box
        p.rrect(box, 4, fill=(8, 8, 9), outline=(70, 72, 76), width=1)
        p.rrect((x0 + 8, y0 + 8, x1 - 8, y1 - 18), 2, fill=(20, 20, 22))
        p.rrect((x0 + 12, y1 - 9, x1 - 12, y1 - 5), 1, fill=(50, 52, 56))
        sd = next(t for t in self.face.printed if t[2] == "SD")
        p.rrect((sd[0] - 22, sd[1] - 6, sd[0] - 14, sd[1] + 6), 1, outline=ART.PRINT, width=1)

    def _standby(self, p):
        x, y = self.face.standby
        ix, iy = x + self.face.standby_icon[0], y + self.face.standby_icon[1]
        p.circle(x, y, 2.5, fill=(200, 20, 20))
        p.d.arc(p.c(ix, iy, 6), 300, 240, fill=ART.PRINT, width=p.w(1.3))
        p.line([(ix, iy - 8), (ix, iy)], ART.PRINT, 1.3)

    def _jog_mode_panel(self, p):
        p.rrect(self.face.jog_mode_box, 4, fill=(16, 16, 17), outline=(70, 72, 76), width=1)

    def _names(self, p):
        """The deck's number under the jog, and the model's name in the
        right column where the NXS2 has its own."""
        cx, cy, r = self.face.jog
        p.text(cx, min(cy + r + 50, self.face.size[1] - 22), self.deck_label, 16, (150, 154, 160), bold=True)
        w, h = self.face.size
        name = self.face.title + " VIRTUAL DECK"
        size = 11
        if self.face.name_at:
            x, y, room = self.face.name_at
            above, below = y - 7, y + 8
        else:
            x = self.face.standby[0] if self.face.standby else (w + self.face.panel[2]) / 2
            above, below, room = h - 30, h - 15, w - self.face.panel[2] - 24
        while ART.text_width(name, size) > room:
            size -= 0.5
        p.text(x, above, "MULTI PLAYER", 8, ART.PRINT)
        p.text(x, below, name, size, ART.PRINT)

    # The jog and the rotary selector are the NXS2's own sprites (art.py),
    # drawn at the scale that fits this model's measured size and centred on
    # its jog and selector.

    def _nxs2(self, radius, nxs2_radius):
        k = radius / nxs2_radius
        if k not in self._arts:
            self._arts[k] = ART.Art(self.scale * k, self.deck_label)
        return self._arts[k]

    def _centred(self, sprite, cx, cy):
        img, _ = sprite
        return img, (round(cx * self.scale - img.size[0] / 2), round(cy * self.scale - img.size[1] / 2))

    def jog_sprite(self, phase):
        return self._centred(self._nxs2(self.jog[2], L.JOG[2]).jog_sprite(phase), *self.jog[:2])

    def centre_sprite(self, live):
        return self._centred(self._nxs2(self.jog[2], L.JOG[2]).centre_sprite(live),
                             *self.jog[:2])

    def select_sprite(self, phase, down):
        return self._centred(self._nxs2(self.face.rotary_radius, L.SELECT[2]).select_sprite(phase, down),
                             *self.face.rotary)

    # -- the tempo slider ---------------------------------------------------------

    def _slider_face(self, p):
        x, top, bottom = self.face.slider[:3]
        above, below = self.face.slider_pad or (41, 42)
        x0, y0, x1, y1 = x - 32, top - above, x + 32, bottom + below
        p.rrect((x0, y0, x1, y1), 5, fill=(8, 8, 9), outline=(64, 66, 70), width=1)
        p.rrect((x0 + 5, y0 + 6, x1 - 5, y1 - 22), 3, fill=(20, 21, 23))
        for i in range(24):
            y = y0 + 16 + i * (y1 - y0 - 44) / 23
            p.line([(x0 + 8, y), (x1 - 8, y)], (44, 45, 48), 1.2)
        p.rrect((x - 3, y0 + 12, x + 3, y1 - 28), 3, fill=(0, 0, 0))
        for i in range(28):
            p.circle(x - 43, top + i * (bottom - top) / 27, 1, fill=ART.PRINT_DIM)
        if self.face.zero_mark:
            mid = (top + bottom) / 2
            p.rrect((x - 62, mid - 2.5, x - 50, mid + 2.5), 1, fill=(200, 230, 40))

    def _body_key(self, p, k):
        key = self.keys[k.name]
        if k.kind in ("vinyl", "jog_adjust"):
            self._knob(p, k.kind, *k.circle)
        elif k.name == "ROTARY PUSH":
            return
        elif k.wired or k.lamp:
            self._key(p, key, "live")
        else:
            self._dark_key(p, key)

    def _dark_key(self, p, key):
        """The key as drawn for the others, at DIM of its brightness: no
        signal is known for it."""
        layer = Painter(self.key_box(key), self.scale)
        self._key(layer, key, "inert")
        red, green, blue, alpha = layer.img.split()
        rgb = ImageEnhance.Brightness(Image.merge("RGB", (red, green, blue))).enhance(DIM)
        layer.img = Image.merge("RGBA", (*rgb.split(), alpha))
        offset = ((layer.px[0] - p.px[0]) * ART.SS, (layer.px[1] - p.px[1]) * ART.SS)
        p.img.alpha_composite(layer.img, (round(offset[0]), round(offset[1])))


class CDJ850Art(FaceArt):
    """Silver CDJ-850 chassis and its model-specific moving parts."""

    INK = (27, 28, 29)
    JOG_DIMPLES = 24
    JOG_PHASES = 15

    def __init__(self, scale, face, deck_label):
        super().__init__(scale, face, deck_label)
        self.SLIDER_CAP = face.slider[3:]

    def _key(self, p, key, look, lit=False, down=False):
        if key.lamp:
            getattr(self, "_k_" + key.kind)(p, key, lit, down)
        else:
            super()._key(p, key, look, lit, down)

    def _ink(self, x, y):
        x0, y0, x1, y1 = self.face.panel
        return ART.PRINT if x0 < x < x1 and y0 < y < y1 else self.INK

    def _plates(self, p):
        w, h = self.face.size
        x0, y0, x1, _ = self.face.panel
        foot = h - 8

        def silver_wings(width, height):
            base = p._ramp(width, height, (199, 200, 204), (148, 150, 155))
            grain = Image.effect_noise((width, height), 18).convert("RGB")
            return ImageChops.add(base, grain, offset=-128).convert("RGBA")

        p.gradient((0, y0 + 13, w, h), (215, 216, 220), (113, 115, 118), radius=22)
        p._paste(silver_wings, (2, y0 + 15, w - 2, h - 3), radius=20)
        # The centre plate is a separate, brighter sheet between the textured wings.
        p.lit((x0, y0 + 8, x1, h - 52), (172, 174, 181), 69, (x0 + 95, 245, 450, 540))
        p.line([(x0, y0 + 14), (x0, h - 52), (x1, h - 52), (x1, y0 + 14)],
               (103, 106, 112), 1.3)
        # Recessed front edge between the two feet.
        front = [(3, foot), (w * 0.202, foot), (w * 0.213, h - 21), (w * 0.228, h - 24),
                 (w * 0.770, h - 24), (w * 0.786, h - 21), (w * 0.797, foot), (w - 3, foot)]
        p.d.polygon([p.p(*point) for point in (*front, (w, h + 1), (0, h + 1))], fill=(13, 14, 16))
        p.line(front, (90, 91, 96), 2)

    def _panel(self, p):
        x0, y0, x1, y1 = self.face.panel
        p.gradient(self.face.panel, (213, 214, 219), (121, 124, 130), radius=5)
        p.gradient((x0 + 6, y0 + 5, x1 - 6, y1 - 10), (21, 22, 23), (8, 9, 10), radius=2)
        p.line([(x0 + 6, y0 + 6), (x1 - 6, y0 + 6)], (61, 62, 65), 1)
        p.gradient((x0 + 1, y1 - 10, x1 - 1, y1), (60, 61, 64), (216, 217, 221))
        p.rrect(self.face.lcd, 1, fill=(6, 11, 16))
        bx, by, _, _ = self.face.key_named("BACK").rect
        p.line([(bx + 4, by - 19), (bx + 12, by - 19), (bx + 13.5, by - 21),
                (bx + 12, by - 23), (bx + 5, by - 23), (bx + 8, by - 26)],
               ART.PRINT, 1.3)
        p.line([(bx + 5, by - 23), (bx + 8, by - 20)], ART.PRINT, 1.3)
        p.text(x1 - 27, y1 - 21, "MP3 / AAC / WAV / AIFF", 11, ART.PRINT, anchor="rm", bold=True)

    def _usb(self, p, box):
        x0, y0, x1, y1 = box
        cx = (x0 + x1) / 2
        p.rrect((x0 - 2, y0 - 1, x1 + 2, y1 + 2), 6, fill=(221, 222, 225), outline=(100, 102, 108), width=1)
        p.gradient((x0, y0, x1, y1 - 3), (9, 10, 11), (66, 67, 70), radius=5)
        p.d.arc(p.b((cx - 15, y1 - 19, cx + 15, y1 + 7)), 190, 350,
                fill=(19, 20, 21), width=p.w(2))

    def _standby(self, p):
        x, y = self.face.standby
        ix, iy = x + self.face.standby_icon[0], y + self.face.standby_icon[1]
        p.d.arc(p.c(ix, iy, 5.7), 299, 241, fill=self.INK, width=p.w(1.2))
        p.line([(ix, iy - 7), (ix, iy)], self.INK, 1.2)

    def _pill(self, p, x, y, text):
        tw = ART.text_width(text, p.n(7.5), True) / p.k
        p.rrect((x - tw / 2 - 4, y - 5.5, x + tw / 2 + 4, y + 5.5), 2, fill=(34, 35, 37))
        p.text(x, y, text, 7.5, ART.PRINT, bold=True)

    def _frame(self, p, x, y, text):
        tw = ART.text_width(text, p.n(7.5), True) / p.k
        p.rrect((x - tw / 2 - 4, y - 5.5, x + tw / 2 + 4, y + 5.5), 2, outline=self.INK, width=1)
        p.text(x, y, text, 7.5, self.INK, bold=True)

    def _names(self, p):
        w, h = self.face.size
        p.text(self.face.jog[0], h - 77, self.deck_label, 13, (72, 75, 81), bold=True)
        p.text(w - 129, h - 35, "MULTI PLAYER", 8, self.INK, anchor="rm")
        p.text(w - 16, h - 34, self.face.title, 18, self.INK, anchor="rm")

    def _jog_surround(self, p, jog):
        cx, cy, _ = jog
        r = self.face.jog[2]
        p.circle(cx, cy, r, fill=(32, 33, 34))
        p.gradient((cx - r + 2, cy - r + 2, cx + r - 2, cy + r - 2),
                   (154, 156, 160), (116, 118, 122), ellipse=True)
        for a0, a1, tip, base in ((124, 141, 120, 125), (39, 56, 60, 55)):
            radius = r + 12
            p.d.arc(p.c(cx, cy, radius), a0, a1, fill=self.INK, width=p.w(1.3))
            pts = [(radius, tip), (radius - 3, base), (radius + 3, base)]
            p.d.polygon([p.p(cx + rr * math.cos(math.radians(a)), cy + rr * math.sin(math.radians(a)))
                         for rr, a in pts], fill=self.INK)

    def jog_box(self):
        cx, cy, r = self.jog
        return (cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2)

    jog_sprite = ART.Art.jog_sprite

    def _jog(self, phase):
        cx, cy, r = self.jog
        top = self.face.platter_radius
        p = Painter(self.jog_box(), self.scale)
        p.gradient((cx - r, cy - r, cx + r, cy + r), (79, 79, 79), (56, 56, 57), ellipse=True)
        p.circle(cx, cy, r, outline=(23, 24, 25), width=2)
        rim = (r + top) / 2
        for i in range(self.JOG_DIMPLES):
            a = math.radians(i * self.JOG_PHASES + phase - 90)
            self._bump(p, cx + rim * math.cos(a), cy + rim * math.sin(a), a, 16, 25)
        p.circle(cx, cy, top + 4, fill=(17, 18, 19))
        p.circle(cx, cy, top + 2.5, fill=(91, 92, 94))
        p.gradient((cx - top, cy - top, cx + top, cy + top), (98, 99, 99), (63, 64, 64), ellipse=True)
        return p.finish(), p.px

    def centre_sprite(self, live):
        # The JOG FL is a separate display. Draw its glass and unlit segments.
        k = ("jog_glass",)
        if k not in self._sprites:
            cx, cy, _ = self.jog
            r = self.face.platter_radius * 0.47
            p = Painter((cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2), self.scale)
            p.gradient((cx - r, cy - r, cx + r, cy + r), (18, 19, 19), (35, 36, 36), ellipse=True)
            p.circle(cx, cy, r, outline=(17, 18, 18), width=1.5)
            p.d.arc(p.c(cx, cy, r - 1.8), 215, 300, fill=(109, 110, 110), width=p.w(1))
            fl = r * 0.58
            for count, inner, outer in ((JOG_FRAMES, 0.60, 1.0), (CUE_SEGMENTS, 0.40, 0.58)):
                for i in range(count):
                    self._jog_tick(p, cx, cy, fl, i / count, inner, outer, (44, 45, 45), 0.7)
            p.circle(cx, cy, fl * 0.25, fill=(12, 13, 14), outline=(49, 52, 55), width=1)
            self._sprites[k] = (p.finish(), p.px)
        return self._sprites[k]

    @staticmethod
    def _jog_tick(p, cx, cy, r, turn, inner, outer, color, width):
        angle = math.tau * turn - math.pi / 2
        dx, dy = math.cos(angle) * r, math.sin(angle) * r
        p.line([(cx + dx * inner, cy + dy * inner), (cx + dx * outer, cy + dy * outer)],
               color, width)

    def jog_display_sprite(self, display):
        cx, cy, _ = self.jog
        r = self.face.platter_radius * 0.47 * 0.58
        p = Painter((cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2), self.scale)
        white = (237, 239, 235)
        if display.position is not None:
            # Leave a short moving gap in the outer operation ring.
            for i in range(1, JOG_FRAMES - 5):
                turn = (display.position + i) / JOG_FRAMES
                self._jog_tick(p, cx, cy, r, turn, 0.60, 1.0, white, 1.25)
        if display.cue is not None:
            self._jog_tick(p, cx, cy, r, display.cue / CUE_SEGMENTS,
                           0.40, 0.58, (255, 61, 49), 2.6)
        if display.audio_memory:
            for a in range(0, 360, 90):
                p.d.arc(p.c(cx, cy, r * 0.34), a + 2, a + 88, fill=white, width=p.w(1.6))
        if display.touch:
            for a in range(0, 360, 45):
                p.d.arc(p.c(cx, cy, r * 0.27), a + 3, a + 42, fill=white, width=p.w(1.8))
        if display.vinyl:
            p.circle(cx, cy, r * 0.235, fill=(49, 101, 157))
            p.text(cx, cy, "Vinyl", r * 0.17, (237, 244, 251), bold=True)
        return p.finish(), p.px

    def select_sprite(self, phase, down):
        phase = int(phase) % 12
        k = ("select", phase, down)
        if k not in self._sprites:
            cx, cy = self.face.rotary
            r = self.face.rotary_radius
            p = Painter((cx - r - 3, cy - r - 3, cx + r + 3, cy + r + 3), self.scale)
            p.circle(cx, cy, r, fill=(14, 14, 15))
            p.chrome(cx, cy, r - 1, tint=(-100, -100, -97), spin=6)
            p.circle(cx, cy, r * 0.69, fill=(5, 5, 6))
            p.chrome(cx, cy, r * (0.52 if down else 0.56), tint=(-91, -91, -89), lobes=3, spin=phase * 10)
            self._sprites[k] = (p.finish(), p.px)
        return self._sprites[k]

    def _slider_face(self, p):
        x, top, bottom = self.face.slider[:3]
        x0, x1 = x - 32, x + 32
        y0, y1 = top - 38, bottom + 37
        p.gradient((x0, y0, x1, y1), (69, 70, 72), (42, 43, 45), radius=7)
        p.rrect((x0, y0, x1, y1), 7, outline=(39, 40, 42), width=1.5)
        for i in range(61):
            y = top + i * (bottom - top) / 60
            mark = (180, 181, 184) if i % 5 == 0 else (123, 125, 128)
            p.line([(x0 + 5, y), (x1 - 5, y)], mark, 0.8)
            if i % 2 == 0:
                p.circle(x0 - 8, y, 0.85, fill=self.INK)
        p.rrect((x - 6, top - 12, x + 6, bottom + 10), 1, fill=(8, 8, 9))
        p.text(x, y1 - 15, "TEMPO", 9, ART.PRINT)

    def slider_cap(self):
        k = ("cap",)
        if k not in self._sprites:
            w, h = self.SLIDER_CAP
            mid = 4 + h / 2
            p = Painter((0, 0, w + 8, h + 8), self.scale)
            p.rrect((2, 5, w + 6, h + 8), 5, fill=(12, 13, 15, 170))
            p.gradient((4, 4, w + 4, h + 4), (224, 226, 231), (136, 138, 145), radius=5)
            p.gradient((6, mid - 13, w + 2, mid - 2), (140, 142, 150), (226, 228, 232), radius=2)
            p.gradient((6, mid + 3, w + 2, mid + 15), (183, 185, 192), (109, 111, 118), radius=2)
            p.line([(7, mid), (w + 1, mid)], (15, 16, 17), 2.4)
            p.line([(8, h + 1), (w, h + 1)], (232, 233, 236), 1.4)
            self._sprites[k] = (p.finish(), None)
        return self._sprites[k][0]

    def _knob(self, p, style, cx, cy, r):
        for i in range(9):
            a = math.radians(150 + i * 30)
            p.line([(cx + (r - 1) * math.cos(a), cy + (r - 1) * math.sin(a)),
                    (cx + (r + 5) * math.cos(a), cy + (r + 5) * math.sin(a))], self.INK, 1.2)
        p.circle(cx, cy, r, fill=(22, 22, 24))
        self._vinyl_speed_cap(p, cx, cy, r, 0)

    def _vinyl_speed_cap(self, p, cx, cy, r, value):
        p.gradient((cx - r + 2, cy - r + 2, cx + r - 2, cy + r - 2),
                   (83, 84, 86), (44, 45, 47), ellipse=True)
        a = math.radians(150 + value * 240 / 255)
        dx, dy = math.cos(a), math.sin(a)
        p.line([(cx + dx * (r - 4), cy + dy * (r - 4)), (cx + dx * 3, cy + dy * 3)],
               (233, 234, 235), 2.6)

    def vinyl_speed_sprite(self, key, value):
        cx, cy, r = key.circle
        p = Painter((cx - r, cy - r, cx + r, cy + r), self.scale)
        self._vinyl_speed_cap(p, cx, cy, r, value)
        return p.finish(), p.px

    def _k_src(self, p, key, lit, down):
        super()._k_src(p, key, lit, down)
        dim = 0.24 if key.lamp else 0.75
        p.rrect(key.box, 3, outline=key.color if lit else ART._dim(key.color, dim), width=2.4)

    _k_top = _k_src

    def _k_led(self, p, key, lit, down):
        if key.circle:
            self._led(p, *key.circle, key.color, lit)
        else:
            if lit:
                p.glow(key.box, lambda d, color: d.rectangle(p.b(key.box), fill=color),
                       key.color, 2.3, 0.65)
            p.rrect(key.box, 1, fill=key.color if lit else ART._dim(key.color, 0.30))

    def _k_arc(self, p, key, lit, down):
        super()._k_arc(p, key, lit, down)
        p.rrect(key.box, 12, outline=(190, 192, 196), width=1.6)

    def _k_chrome(self, p, key, lit, down):
        cx, cy, r = key.circle
        p.circle(cx, cy, r + 2, fill=(61, 63, 67), outline=(207, 209, 212), width=1.2)
        p.circle(cx, cy, r, fill=(12, 13, 14))
        if key.symbol == "eject":
            p.dome(cx, cy, r - 3)
            color = key.color if lit else ART._dim(key.color, 0.72)
        else:
            p.chrome(cx, cy, r - 3, lobes=2, spin=10 if down else 0)
            color = (20, 21, 22)
        self._symbol(p, key.symbol, cx, cy, r * 0.36, color)

    def _k_big(self, p, key, lit, down):
        cx, cy, r = key.circle
        p.circle(cx, cy, r + 1, fill=(20, 21, 22), outline=(99, 101, 106), width=2)
        p.chrome(cx, cy, r - 6, tint=(5, 6, 9), lobes=4, spin=8 if down else 0)
        color = key.color if lit else ART._dim(key.color, 0.36)
        self._symbol(p, key.symbol, cx, cy, r * 0.17, color)
        p.text(cx, cy, key.label, key.font, color, bold=True)

    def _k_beat(self, p, key, lit, down):
        cx, cy, r = key.circle
        p.circle(cx, cy, r, fill=(12, 13, 14))
        p.dome(cx, cy, r - 2, (10, 10, 11) if down else (28, 29, 30))
        color = ART._mix(key.color, (255, 245, 210), 0.5) if lit else ART._dim(key.color, 0.45)
        p.text(cx, cy, key.label, key.font, color, bold=True)

    def _k_indicator(self, p, key, lit, down):
        self._k_ring(p, key, lit, down)
        if not key.dot:
            x, y, r = key.circle
            self._led(p, x, y, r * 0.33, key.color, lit)

    def _k_inset(self, p, key, lit, down):
        x0, y0, x1, y1 = key.box
        p.rrect((x0 - 6, y0 - 6, x1 + 6, y1 + 6), 6, fill=(130, 132, 137),
                outline=(222, 223, 225), width=1.5)
        p.rrect(key.box, 3, fill=(20, 21, 22))
        if key.name == "DIRECTION REV":
            tint = key.color if lit else ART._dim(key.color, 0.28)
            color = (19, 20, 20) if lit else (222, 177, 177)
        else:
            tint = (24, 25, 28)
            color = key.color if lit else ART._dim(key.color, 0.35)
        p.gradient((x0 + 2, y0 + 2, x1 - 2, y1 - 2), ART._mix(tint, (192, 193, 196), 0.28),
                   tint, radius=2)
        p.text((x0 + x1) / 2, (y0 + y1) / 2 + int(down), key.label, key.font, color, bold=True)


def make_face_art(scale, face, deck_label):
    art_class = CDJ850Art if face.title == "CDJ-850" else FaceArt
    return art_class(scale, face, deck_label)
