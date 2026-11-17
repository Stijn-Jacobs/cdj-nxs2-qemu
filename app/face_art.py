# SPDX-License-Identifier: GPL-2.0-or-later
"""The drawing of a face file (face.py), in art.py's materials.

Art's key drawers and print helpers do the work: each face key names the kind
of key it is (art.py's `_k_<kind>`), and the printed labels, plates and rules
come from the face file in millimetres. The greys, the metal and the lamp
colours are sampled from a straight-down product photo of the player. The
manufacturer's logos and names are not drawn.
"""

from PIL import Image, ImageEnhance

import art as ART
import layout as L
from art import Painter

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
                     color=color, symbol=k.symbol, font=k.font, dot=color if k.lamp else None)

    def key_sprite(self, key, lit, down, look):
        return super().key_sprite(self.keys[key.name], lit, down, look)

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
            p.text(x, y, text, size, ART.PRINT)
        for x, y, text in self.face.pills:
            self._pill(p, x, y, text)
        for x, y, text, color in self.face.badges:
            self._badge(p, x, y, text, getattr(L, color.upper()))
        for x, y, text in self.face.frames:
            self._frame(p, x, y, text)
        for pts in self.face.rules:
            p.line(pts, ART.PRINT, 1.2)
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
        elif k.wired:
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
