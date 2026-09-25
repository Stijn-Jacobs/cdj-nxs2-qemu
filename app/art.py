# SPDX-License-Identifier: GPL-2.0-or-later
"""Draws the deck face: every pixel of the app's chrome is made here.

Nothing is loaded from disk except a system font. Shapes are drawn with
Pillow at SS times the target size and scaled down, which gives the curves
their anti-aliasing (Tk's own canvas draws aliased circles). The materials
follow the NXS2: a gloss-black display panel, matte gunmetal side columns,
brushed dark metal around the jog, chrome search keys, translucent orange
loop keys, keys outlined by their LED, and a jog with a dimpled rubber rim
between two chrome rings.

The face is a static body plus small sprites for whatever changes: a key's
lit and pressed looks, the jog at a few rotation phases, the jog's centre
display, the rotary selector and the tempo slider's cap. The view
(deck_view.py) stacks them on a Tk canvas and swaps a sprite only when its
state changes.
"""

import hashlib
import math
import os
import random
import tempfile
from functools import lru_cache

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

import layout as L

SS = 3                                   # supersampling factor

PRINT = (225, 228, 232)                  # panel lettering
PRINT_DIM = (150, 154, 160)
GUNMETAL_TOP = (50, 51, 54)
GUNMETAL_BOTTOM = (30, 31, 33)
GLOSS = (9, 9, 10)

# Panel print is a Helvetica-style grotesque, as on the hardware: Helvetica
# itself where the system has it, Arial or a metric twin elsewhere.
# (file, .ttc index or None).
_FONT_FILES = {
    True: [("/System/Library/Fonts/Helvetica.ttc", 1),
           ("arialbd.ttf", None),
           ("/System/Library/Fonts/Supplemental/Arial Bold.ttf", None),
           ("LiberationSans-Bold.ttf", None), ("NimbusSans-Bold.otf", None),
           ("DejaVuSans-Bold.ttf", None)],
    False: [("/System/Library/Fonts/Helvetica.ttc", 0),
            ("arial.ttf", None),
            ("/System/Library/Fonts/Supplemental/Arial.ttf", None),
            ("LiberationSans-Regular.ttf", None), ("NimbusSans-Regular.otf", None),
            ("DejaVuSans.ttf", None)],
}
TRACKING = 0.05                          # letter spacing, a fraction of the size


@lru_cache(maxsize=None)
def font(px, bold=False):
    """A TrueType font at px pixels, whichever this system has."""
    px = max(4, int(round(px)))
    for name, style in _FONT_FILES[bold]:
        try:
            if isinstance(style, int):
                return ImageFont.truetype(name, px, index=style)
            f = ImageFont.truetype(name, px)
            if style:
                f.set_variation_by_name(style)
            return f
        except (OSError, ValueError):
            continue
    try:
        return ImageFont.load_default(size=px)      # Pillow >= 10.1
    except TypeError:
        return ImageFont.load_default()


def text_width(text, px, bold=False):
    """How wide `text` prints at px pixels, letter spacing included."""
    f = font(px, bold)
    return sum(f.getlength(ch) for ch in text) + TRACKING * px * max(0, len(text) - 1)


def _mix(a, b, t):
    return tuple(int(round(x + (y - x) * t)) for x, y in zip(a, b))


def _dim(c, t=0.28):
    """An LED that is off still shows its colour faintly through the lens."""
    return _mix((12, 12, 13), c, t)


def _clip_alpha(layer, under):
    """layer, visible only where `under` is opaque."""
    layer.putalpha(ImageChops.multiply(layer.getchannel("A"), under.getchannel("A")))
    return layer


@lru_cache(maxsize=64)
def _conic(lobes, spin, tint):
    """The chrome's light, small: it is smooth, so it scales up cleanly."""
    n = 128
    img = Image.new("RGB", (n, n))
    d = ImageDraw.Draw(img)
    for a in range(0, 360, 2):
        v = 0.5 + 0.5 * math.cos(math.radians(lobes * (a - 35 - spin)))
        g = int(70 + 170 * v)
        d.pieslice((-n // 2, -n // 2, n + n // 2, n + n // 2), a, a + 2.5,
                   fill=tuple(max(0, min(255, g + t)) for t in tint))
    return img.filter(ImageFilter.GaussianBlur(1.0))


class Painter:
    """A supersampled RGBA canvas over a box of face units."""

    def __init__(self, box, scale):
        self.scale = scale
        self.k = scale * SS
        x0, y0, x1, y1 = box
        self.px = (round(x0 * scale), round(y0 * scale))
        self.size = (max(1, round(x1 * scale) - self.px[0]),
                     max(1, round(y1 * scale) - self.px[1]))
        self.img = Image.new("RGBA", (self.size[0] * SS, self.size[1] * SS),
                             (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.img)
        self.off = (0, 0)               # a glow layer's origin, while drawing it

    # Unit coordinates to supersampled pixels, and unit lengths to pixels.
    def p(self, x, y):
        return ((x * self.scale - self.px[0]) * SS - self.off[0],
                (y * self.scale - self.px[1]) * SS - self.off[1])

    def n(self, v):
        return v * self.k

    def w(self, v):
        return max(1, round(self.n(v)))

    def b(self, box):
        x0, y0 = self.p(box[0], box[1])
        x1, y1 = self.p(box[2], box[3])
        return (x0, y0, x1, y1)

    def c(self, cx, cy, r):
        x, y = self.p(cx, cy)
        r = self.n(r)
        return (x - r, y - r, x + r, y + r)

    def rrect(self, box, radius, fill=None, outline=None, width=0):
        self.d.rounded_rectangle(self.b(box), radius=self.n(radius), fill=fill,
                                 outline=outline, width=self.w(width) if outline else 0)

    def circle(self, cx, cy, r, fill=None, outline=None, width=0):
        self.d.ellipse(self.c(cx, cy, r), fill=fill, outline=outline,
                       width=self.w(width) if outline else 0)

    def line(self, pts, fill, width=1):
        self.d.line([self.p(x, y) for x, y in pts], fill=fill, width=self.w(width))

    def _paste(self, fill, box, radius=0, ellipse=False):
        """Paste fill(w, h) over a rounded rectangle (or the ellipse in box)."""
        fx0, fy0, fx1, fy1 = self.b(box)
        x0, y0 = int(math.floor(fx0)), int(math.floor(fy0))
        w = max(1, int(math.ceil(fx1)) - x0)
        h = max(1, int(math.ceil(fy1)) - y0)
        mask = Image.new("L", (w, h), 0)
        local = (fx0 - x0, fy0 - y0, fx1 - x0, fy1 - y0)
        if ellipse:
            ImageDraw.Draw(mask).ellipse(local, fill=255)
        else:
            ImageDraw.Draw(mask).rounded_rectangle(local, radius=self.n(radius),
                                                   fill=255)
        self.img.paste(fill(w, h), (x0, y0), mask)

    @staticmethod
    def _ramp(w, h, top, bottom):
        ramp = Image.linear_gradient("L").resize((w, h))
        bands = [ramp.point(lambda v, a=a, b=b: int(a + (b - a) * v / 255))
                 for a, b in zip(top[:3], bottom[:3])]
        return Image.merge("RGB", bands)

    def gradient(self, box, top, bottom, radius=0, ellipse=False):
        """A vertical gradient, top colour to bottom colour."""
        self._paste(lambda w, h: self._ramp(w, h, top, bottom).convert("RGBA"),
                    box, radius, ellipse)

    def brushed(self, box, top, bottom, radius=0, seed=7):
        """Dark metal brushed top to bottom: a gradient with fine streaks."""
        def fill(w, h):
            base = self._ramp(w, h, top, bottom)
            rnd = random.Random(seed)
            cols = max(1, w // SS)
            streak = Image.new("L", (cols, 1))
            streak.putdata([rnd.randint(90, 255) for _ in range(cols)])
            streak = streak.resize((w, h), Image.NEAREST)
            lit = ImageChops.multiply(base, Image.merge("RGB", [streak] * 3))
            return Image.blend(base, ImageChops.add(base, lit, 1.6), 0.5).convert("RGBA")
        self._paste(fill, box, radius)

    def lit(self, box, base, peak, light, radius=0, brushed=False, seed=7):
        """A surface under the face's light: base colour plus `peak` more at
        the light, falling off with distance -- light = (x, y, reach across,
        reach down) in face units. brushed adds the plate's fine streaks."""
        lx, ly, rx, ry = light
        x0, y0, x1, y1 = box

        def fill(w, h):
            sw, sh = max(1, w // 8), max(1, h // 8)
            mask = Image.new("L", (sw, sh))
            mask.putdata([int(255 * math.exp(-math.hypot(
                ((x0 + (i % sw + 0.5) / sw * (x1 - x0)) - lx) / rx,
                ((y0 + (i // sw + 0.5) / sh * (y1 - y0)) - ly) / ry)))
                for i in range(sw * sh)])
            mask = mask.resize((w, h), Image.BILINEAR)
            lo = Image.new("RGB", (w, h), base)
            hi = Image.new("RGB", (w, h), tuple(min(255, c + peak) for c in base))
            out = Image.composite(hi, lo, mask)
            if brushed:
                rnd = random.Random(seed)
                cols = max(1, w // SS)
                streak = Image.new("L", (cols, 1))
                streak.putdata([rnd.randint(90, 255) for _ in range(cols)])
                streak = streak.resize((w, h), Image.NEAREST)
                streaks = ImageChops.multiply(out, Image.merge("RGB", [streak] * 3))
                out = Image.blend(out, ImageChops.add(out, streaks, 1.6), 0.5)
            return out.convert("RGBA")
        self._paste(fill, box, radius)

    def chrome(self, cx, cy, r, tint=(0, 0, 0), lobes=2, spin=0):
        """Spun chrome: light and dark lobes around the centre."""
        def fill(w, h):
            return _conic(lobes, spin, tint).resize((w, h), Image.BICUBIC).convert("RGBA")
        self._paste(fill, (cx - r, cy - r, cx + r, cy + r), ellipse=True)

    def dome(self, cx, cy, r, base=(14, 14, 15)):
        """A glossy black dome: a dark disc with a soft highlight on top."""
        self.gradient((cx - r, cy - r, cx + r, cy + r), _mix(base, (80, 82, 88), 0.5),
                      base, ellipse=True)
        self.glow((cx - r, cy - r, cx + r, cy + r), lambda d, c: d.ellipse(self.c(cx - r * 0.18, cy - r * 0.42, r * 0.42),
                                         fill=c), (255, 255, 255), r * 0.25, 0.35,
                  over=True)

    def text(self, x, y, text, px, fill, anchor="mm", bold=False):
        """Panel print, letter-spaced like the hardware's. Lines of a
        multi-line text are centred on each other."""
        if not text:
            return
        n = self.n(px)
        lines = text.split("\n")
        lead = n * 1.12
        top = self.p(x, y)[1] - {"t": 0, "a": 0, "m": lead * (len(lines) - 1) / 2,
                                 "s": lead * (len(lines) - 1),
                                 "b": lead * (len(lines) - 1),
                                 "d": lead * (len(lines) - 1)}[anchor[1]]
        f = font(n, bold)
        for i, line in enumerate(lines):
            w = text_width(line, n, bold)
            x0 = self.p(x, y)[0] - {"l": 0, "m": w / 2, "r": w}[anchor[0]]
            cy = top + i * lead
            for ch in line:
                self.d.text((x0, cy), ch, font=f, fill=fill, anchor="l" + anchor[1])
                x0 += f.getlength(ch) + TRACKING * n

    def glow(self, box, draw_fn, color, blur, strength=1.0, over=False):
        """A soft light: draw_fn's shape (inside box) in color, blurred.

        Underneath what is drawn so far, like light around a key, or `over`
        it and clipped to it, like a reflection. Only the box plus the blur's
        reach is processed, which keeps a full-face render fast.
        """
        m = blur * 3
        fx0, fy0, fx1, fy1 = self.b((box[0] - m, box[1] - m, box[2] + m, box[3] + m))
        x0, y0 = max(0, int(fx0)), max(0, int(fy0))
        x1 = min(self.img.size[0], int(math.ceil(fx1)))
        y1 = min(self.img.size[1], int(math.ceil(fy1)))
        if x1 <= x0 or y1 <= y0:
            return
        layer = Image.new("RGBA", (x1 - x0, y1 - y0), (0, 0, 0, 0))
        self.off = (x0, y0)
        try:
            draw_fn(ImageDraw.Draw(layer), color + (int(255 * strength),))
        finally:
            self.off = (0, 0)
        layer = layer.filter(ImageFilter.GaussianBlur(self.n(blur)))
        under = self.img.crop((x0, y0, x1, y1))
        if over:
            out = Image.alpha_composite(under, _clip_alpha(layer, under))
        else:
            out = Image.alpha_composite(layer, under)
        self.img.paste(out, (x0, y0))
        self.d = ImageDraw.Draw(self.img)

    def finish(self):
        return self.img.resize(self.size, Image.LANCZOS)


def dock(size, scale, slots):
    """The dock panel: gunmetal like the face's side columns, each deck's
    screen in a gloss bezel under its name.

    size is the panel in face units, drawn at `scale` so its print matches
    the faces beside it. slots: [(title, glass box)] in face units, the glass
    being where the screen image goes.
    """
    w, h = size
    B, pad = L.DOCK_BEZEL, L.DOCK_PAD
    p = Painter((0, 0, w, h), scale)
    p.rrect((0, 0, w, h), 10, fill=(6, 6, 7))
    p.gradient((2, 2, w - 2, h - 2), GUNMETAL_TOP, GUNMETAL_BOTTOM, radius=8)

    def heading(x0, x1, y, text, px):
        """A printed title with a rule after it, as the face prints '— SEARCH'."""
        p.text(x0, y, text, px, PRINT, anchor="lm", bold=True)
        tw = text_width(text, p.n(px), True) / p.k
        p.line([(x0 + tw + 8, y), (x1, y)], PRINT_DIM, 1)

    for title, (x0, y0, x1, y1) in slots:
        heading(x0 - B, x1 + B, y0 - B - L.DOCK_HEADER / 2, title, 11)
        # The bezel: the display panel's gloss black and its chrome front edge.
        p.rrect((x0 - B - 2, y0 - B - 2, x1 + B + 2, y1 + B + 5), 9, fill=(4, 4, 5))
        p.gradient((x0 - B, y0 - B, x1 + B, y1 + B), (26, 26, 28), GLOSS, radius=8)
        p.gradient((x0 - B + 4, y1 + B - 2, x1 + B - 4, y1 + B + 3),
                   (170, 172, 178), (40, 41, 44), radius=2)
        p.glow((x0 - B, y0 - B, x1 + B, y1 + B),
               lambda d, c, x0=x0, y0=y0, y1=y1: d.polygon(
                   [p.p(x0 - B, y1 + B), p.p(x0 - B, y0 - B),
                    p.p(x0 + (y1 - y0) * 0.6, y0 - B), p.p(x0 + (y1 - y0) * 0.2, y1 + B)],
                   fill=c), (255, 255, 255), 30, 0.04, over=True)
        # The glass's own edge; the screen image covers the inside.
        p.rrect((x0 - 2, y0 - 2, x1 + 2, y1 + 2), 3, fill=(2, 2, 3),
                outline=(48, 49, 52), width=1)

    return p.finish().convert("RGB")


class Art:
    """Every image of one deck face at one scale."""

    GLOW = 12                           # margin around a key for its light

    def __init__(self, scale, deck_label):
        self.scale = scale
        self.deck_label = deck_label
        self.size = (round(L.W * scale), round(L.H * scale))
        self._sprites = {}

    # -- the static body --------------------------------------------------------

    def body(self, keys_rest):
        """The whole face at rest. keys_rest: [(Key, look)] to bake in."""
        p = Painter((0, 0, L.W, L.H), self.scale)
        self._chassis(p)
        self._display_panel(p)
        self._left_column(p)
        self._right_column(p)
        self._jog_surround(p)
        self._slider_frame(p)
        for x, y, text in L.PILLS:
            self._pill(p, x, y, text)
        for x, y, text, col in L.BADGES:
            self._badge(p, x, y, text, col)
        for names, margin in L.RECESSES:
            self._recess(p, L.recess_box(names, margin))
        for x, y, text in L.FRAMES:
            self._frame(p, x, y, text)
        for style, cx, cy, r in L.KNOBS:
            self._knob(p, style, cx, cy, r)
        for pts in L.PRINTED_LINES:
            p.line(pts, PRINT, 1.2)
        for t in L.TEXTS:
            p.text(t.x, t.y, t.text, t.size, PRINT, t.anchor, t.bold)
        for key, look in keys_rest:
            self._key(p, key, look)
        p.text(900, 1207, "MULTI PLAYER", 8, PRINT)
        p.text(900, 1222, L.TITLE, 11, PRINT)
        p.text(485, 1196, self.deck_label, 16, (150, 154, 160), bold=True)
        return p.finish().convert("RGB")

    def _chassis(self, p):
        H, W = L.H, L.W
        # The columns and the lower plate first, the raised panel over them,
        # all under the face's one light from above its left edge.
        p.rrect((0, 52, W, H), 10, fill=(4, 4, 5))
        p.lit((2, 54, 135, H - 2), (20, 20, 21), 80, (0, 52, 400, 700), radius=8)
        p.lit((837, 54, W - 2, H - 2), (26, 26, 27), 14, (837, 52, 300, 900), radius=8)
        p.lit((139, 404, 833, H - 2), (6, 6, 7), 150, L.LIGHT, brushed=True)
        p.line([(137, 404), (137, H)], (8, 8, 9), 2)
        p.line([(835, 404), (835, H)], (8, 8, 9), 2)
        # The foot at the front edge.
        p.rrect((225, H - 24, 745, H + 6), 10, fill=(18, 18, 20),
                outline=(60, 62, 66), width=1)

    def _display_panel(self, p):
        x0, y0, x1, y1 = L.TOP_BLOCK
        p.rrect((x0, y0, x1, y1 + 4), 8, fill=(4, 4, 5))
        p.gradient((x0 + 2, y0 + 2, x1 - 2, y1), (22, 22, 24), GLOSS, radius=7)
        # The slanted lip at the back of the panel.
        p.gradient((x0 + 4, 2, x1 - 4, 26), (48, 49, 52), (14, 14, 15), radius=5)
        p.line([(x0 + 6, 27), (x1 - 6, 27)], (70, 72, 76), 1)
        # Its front edge: a chrome bevel onto the brushed plate.
        p.gradient((x0, y1 - 3, x1, y1 + 5), (170, 172, 178), (40, 41, 44))
        # The screen's glass and the dark panel behind the selector.
        lx0, ly0, lx1, ly1 = L.LCD
        p.rrect((lx0 - 10, ly0 - 12, lx1 + 10, ly1 + 10), 4, fill=(2, 2, 3),
                outline=(44, 45, 48), width=1)
        p.rrect(L.SELECT_PANEL, 10, fill=(24, 25, 27), outline=(60, 62, 66), width=1)
        # The source keys hang on one line.
        p.line([(188, 74), (188, 234)], (120, 122, 128), 1)
        # A gloss reflection across the panel.
        p.glow((137, 0, 520, 402), lambda d, c: d.polygon([p.p(137, 250), p.p(430, 0), p.p(520, 0),
                                       p.p(230, 402), p.p(137, 402)], fill=c),
               (255, 255, 255), 30, 0.05, over=True)

    def _left_column(self, p):
        # USB socket: a chrome frame around the port.
        p.rrect((18, 68, 80, 126), 5, fill=(10, 10, 11))
        p.gradient((20, 70, 78, 124), (200, 202, 208), (110, 112, 118), radius=4)
        p.rrect((27, 78, 71, 110), 3, fill=(14, 14, 15))
        p.rrect((33, 84, 65, 94), 1, fill=(62, 64, 68))
        p.line([(40, 150), (56, 150)], PRINT, 1)
        p.rrect((97, 72, 111, 82), 1, outline=PRINT, width=1)
        p.line([(94, 84), (114, 84)], PRINT, 1)
        # SD slot: a recess with its light bar.
        p.rrect((18, 188, 122, 246), 4, fill=(8, 8, 9), outline=(70, 72, 76), width=1)
        p.rrect((26, 196, 114, 228), 2, fill=(20, 20, 22))
        p.glow((30, 237, 110, 241), lambda d, c: d.rectangle(p.b((30, 237, 110, 241)), fill=c),
               (200, 230, 255), 3, 0.9)
        p.rrect((30, 237, 110, 241), 1, fill=(215, 235, 255))
        p.rrect((54, 255, 62, 267), 1, outline=PRINT, width=1)
        # The hot cue ladder.
        for x in (26, 115):
            for y0, y1 in ((330, 366), (404, 440), (478, 514), (554, 596)):
                p.line([(x, y0 + 10), (x, y1 - 4)], PRINT_DIM, 1)

    def _right_column(self, p):
        p.circle(900, 99, 2.5, fill=(200, 20, 20))
        p.d.arc(p.c(882, 70, 6), 300, 240, fill=PRINT, width=p.w(1.3))
        p.line([(882, 62), (882, 70)], PRINT, 1.3)
        # The jog mode panel.
        p.rrect((843, 504, 958, 554), 4, fill=(16, 16, 17), outline=(70, 72, 76),
                width=1)

    def _jog_surround(self, p):
        cx, cy, r = L.JOG
        p.circle(cx, cy, r + 10, fill=(10, 10, 11), outline=(80, 82, 88), width=1)
        for a0, a1, head in ((120, 148, 120), (32, 60, 60)):
            p.d.arc(p.c(cx, cy, r + 24), a0, a1, fill=PRINT, width=p.w(1.2))
            a = math.radians(head)
            tip = (cx + (r + 24) * math.cos(a), cy + (r + 24) * math.sin(a))
            # A small solid wedge, as printed on the panel.
            back = math.radians(head + (7 if head > 90 else -7))
            p.d.polygon([p.p(*tip)] + [
                p.p(cx + (r + 24 + off) * math.cos(back), cy + (r + 24 + off) * math.sin(back))
                for off in (-3.5, 3.5)], fill=PRINT)

    def _slider_frame(self, p):
        x0, y0, x1, y1 = L.SLIDER_FRAME
        p.rrect((x0, y0, x1, y1), 5, fill=(8, 8, 9), outline=(64, 66, 70), width=1)
        p.rrect((x0 + 5, y0 + 6, x1 - 5, y1 - 22), 3, fill=(20, 21, 23))
        for i in range(24):
            y = y0 + 16 + i * (y1 - y0 - 44) / 23
            p.line([(x0 + 8, y), (x1 - 8, y)], (44, 45, 48), 1.2)
        p.rrect((897, y0 + 12, 903, y1 - 28), 3, fill=(0, 0, 0))
        top, bottom = L.SLIDER[1], L.SLIDER[2]
        for i in range(28):
            p.circle(857, top + i * (bottom - top) / 27, 1, fill=PRINT_DIM)
        p.rrect((838, 994, 850, 999), 1, fill=(200, 230, 40))

    def _pill(self, p, x, y, text):
        tw = text_width(text, p.n(7.5), True) / p.k
        p.rrect((x - tw / 2 - 5, y - 6, x + tw / 2 + 5, y + 6), 3, fill=(190, 192, 196))
        p.text(x, y, text, 7.5, (20, 20, 22), bold=True)

    def _badge(self, p, x, y, text, col):
        p.rrect((x - 19, y - 7, x + 19, y + 7), 2, fill=_mix(col, (0, 0, 0), 0.25))
        p.text(x, y, text, 8, (240, 240, 245), bold=True)

    def _recess(self, p, box):
        """A well a key group sits in: a shallow stadium, its top edge in
        shadow and a faint lip of light along its bottom."""
        x0, y0, x1, y1 = box
        r = (y1 - y0) / 2
        p.rrect((x0, y0 + 0.5, x1, y1 + 1), r, fill=(58, 59, 62))
        p.rrect(box, r, fill=(14, 14, 15))
        p.gradient((x0 + 0.8, y0 + 1.2, x1 - 0.8, y1 - 0.4), (30, 30, 32), (44, 44, 47),
                   radius=r - 1)

    def _frame(self, p, x, y, text):
        """A word in a printed outline, as DELETE beside CALL."""
        tw = text_width(text, p.n(7.5), True) / p.k
        p.rrect((x - tw / 2 - 4, y - 6, x + tw / 2 + 4, y + 6), 2, outline=PRINT, width=1)
        p.text(x, y, text, 7.5, PRINT, bold=True)

    def _knob(self, p, style, cx, cy, r):
        if style == "vinyl":
            # Tick lines round the dial, all but its bottom.
            for i in range(11):
                a = math.radians(118 + i * 30.4)
                p.line([(cx + (r + 4) * math.cos(a), cy + (r + 4) * math.sin(a)),
                        (cx + (r + 9) * math.cos(a), cy + (r + 9) * math.sin(a))],
                       PRINT_DIM, 1.1)
        else:
            # Jog adjust: dots over the top, a bigger one at twelve, and a
            # short line at each end, at LIGHT and HEAVY.
            for i in range(11):
                a = math.radians(135 + i * 27)
                big = i == 5
                p.circle(cx + (r + 6) * math.cos(a), cy + (r + 6) * math.sin(a),
                         1.5 if big else 0.9, fill=PRINT if big else PRINT_DIM)
            for deg in (121, 59):
                a = math.radians(deg)
                p.line([(cx + (r + 9) * math.cos(a), cy + (r + 9) * math.sin(a)),
                        (cx + (r + 16) * math.cos(a), cy + (r + 16) * math.sin(a))],
                       PRINT, 1.2)
        p.circle(cx, cy, r, fill=(6, 6, 7))
        for i in range(24):
            a = math.radians(i * 15)
            p.line([(cx + (r - 3) * math.cos(a), cy + (r - 3) * math.sin(a)),
                    (cx + r * math.cos(a), cy + r * math.sin(a))], (60, 62, 66), 1)
        if style == "jog_adjust":
            # A spun silver cap inside the knurled rim, its pointer at twelve.
            p.chrome(cx, cy, r - 5, lobes=3)
            p.line([(cx, cy), (cx, cy - (r - 6))], (250, 250, 252), 2.2)
            return
        p.dome(cx, cy, r - 4, (20, 20, 22))
        # The pointer at rest, as the knob ships: about seven o'clock.
        a = math.radians(120)
        p.line([(cx, cy), (cx + (r - 4) * math.cos(a), cy + (r - 4) * math.sin(a))],
               (235, 236, 240), 2)

    # -- keys -----------------------------------------------------------------

    def key_box(self, key):
        x0, y0, x1, y1 = key.bounds()
        g = self.GLOW
        return (x0 - g, y0 - g, x1 + g, y1 + g)

    def key_sprite(self, key, lit, down, look):
        """(image, pixel position) of one key in one state."""
        k = (key.name, lit, down, look)
        if k not in self._sprites:
            p = Painter(self.key_box(key), self.scale)
            self._key(p, key, look, lit, down)
            self._sprites[k] = (p.finish(), p.px)
        return self._sprites[k]

    def _key(self, p, key, look, lit=False, down=False):
        """look: 'live', 'untested' (decoded, not yet tried) or 'inert'. The
        face draws them alike: a mark on the key read as a lamp the real deck
        does not have, so the status bar says how sure a control is instead."""
        getattr(self, "_k_" + key.kind)(p, key, lit or down, down)

    def _led_outline(self, p, box, radius, color, lit, width=1.6):
        if lit:
            p.glow(box, lambda d, c: d.rounded_rectangle(p.b(box), radius=p.n(radius),
                                                    outline=c, width=p.w(width * 2)),
                   color, 3, 0.9)
        p.rrect(box, radius, outline=color if lit else _dim(color), width=width)

    def _k_src(self, p, key, lit, down):
        x0, y0, x1, y1 = key.box
        p.rrect((x0 - 1, y0 - 1, x1 + 1, y1 + 2), 3, fill=(0, 0, 0))
        p.gradient(key.box, (34, 34, 36) if not down else (16, 16, 17),
                   (10, 10, 11), radius=3)
        self._led_outline(p, key.box, 3, key.color, lit)
        p.text((x0 + x1) / 2, (y0 + y1) / 2 + (1 if down else 0), key.label,
               key.font, (255, 255, 255) if lit else (200, 202, 206), bold=True)

    _k_top = _k_src

    def _k_pad(self, p, key, lit, down):
        x0, y0, x1, y1 = key.box
        p.rrect((x0 - 2, y0 - 2, x1 + 2, y1 + 3), 5, fill=(6, 6, 7))
        p.gradient(key.box, (44, 45, 48), (26, 27, 29), radius=4)
        self._led_outline(p, (x0 + 6, y0 + 6, x1 - 6, y1 - 6), 1, key.color, lit, 1.8)

    def _k_rect(self, p, key, lit, down):
        x0, y0, x1, y1 = key.box
        p.rrect((x0 - 1, y0 - 1, x1 + 1, y1 + 2), 3, fill=(0, 0, 0))
        p.gradient(key.box, (58, 59, 62) if not down else (30, 31, 33),
                   (20, 20, 22), radius=3)
        if lit and key.label:
            self._led_outline(p, key.box, 3, key.color, True, 1.2)
        p.text((x0 + x1) / 2, (y0 + y1) / 2, key.label, key.font, (230, 232, 236),
               bold=True)

    def _k_dome(self, p, key, lit, down):
        cx, cy, r = key.circle
        p.circle(cx, cy, r + 3, fill=(70, 72, 76))
        p.circle(cx, cy, r + 1.5, fill=(4, 4, 5))
        p.dome(cx, cy, r, (8, 8, 9) if down else (18, 18, 20))
        if key.dot:
            rd = max(2.5, r * 0.3)
            if lit:
                p.glow((cx - rd, cy - rd, cx + rd, cy + rd), lambda d, c: d.ellipse(p.c(cx, cy, rd * 1.4), fill=c), key.dot,
                       rd, 1.0)
            p.circle(cx, cy, rd, fill=_mix(key.dot, (255, 255, 255), 0.25) if lit
                     else _dim(key.dot, 0.45))

    def _led(self, p, cx, cy, rd, color, lit):
        """An LED dot in a key's centre."""
        if lit:
            p.glow((cx - rd, cy - rd, cx + rd, cy + rd),
                   lambda d, c: d.ellipse(p.c(cx, cy, rd * 1.5), fill=c), color, rd, 1.0)
        p.circle(cx, cy, rd, fill=_mix(color, (255, 255, 255), 0.25) if lit
                 else _mix((10, 10, 11), color, 0.55))

    # Chrome on the small keys is darker and more contrasty than the big
    # search keys'.
    DARK_CHROME = (-44, -44, -42)

    def _k_cdome(self, p, key, lit, down):
        """A chrome dome: spun metal with a smaller spun cap."""
        cx, cy, r = key.circle
        p.circle(cx, cy, r + 2.5, fill=(6, 6, 7))
        p.chrome(cx, cy, r, tint=self.DARK_CHROME, spin=8 if down else 0)
        p.chrome(cx, cy, r * 0.62, tint=self.DARK_CHROME, lobes=3, spin=30)
        if key.dot:
            self._led(p, cx, cy, max(2.5, r * 0.3), key.dot, lit)

    def _k_ring(self, p, key, lit, down, ring=None):
        """A glossy black dome in a ring: chrome, or `ring` as a colour."""
        cx, cy, r = key.circle
        p.circle(cx, cy, r + 3.5, fill=(4, 4, 5))
        if ring:
            p.circle(cx, cy, r + 2, fill=ring)
        else:
            p.chrome(cx, cy, r + 2, tint=self.DARK_CHROME, spin=20)
        p.circle(cx, cy, r - 0.5, fill=(3, 3, 4))
        p.dome(cx, cy, r - 1.5, (6, 6, 7) if down else (14, 14, 15))
        if key.dot:
            self._led(p, cx, cy, max(2.5, r * 0.3), key.dot, lit)

    def _k_pale(self, p, key, lit, down):
        self._k_ring(p, key, lit, down, ring=(196, 198, 202))

    def _k_plate(self, p, key, lit, down):
        """A chrome key with its name on a small plate, lit in its colour."""
        cx, cy, r = key.circle
        p.circle(cx, cy, r + 3, fill=(4, 4, 5))
        p.chrome(cx, cy, r - 1, spin=8 if down else 0)
        face = key.color if lit else _mix((40, 40, 42), key.color, 0.55)
        tw = text_width(key.label, p.n(7), True) / p.k
        p.rrect((cx - tw / 2 - 4, cy - 6, cx + tw / 2 + 4, cy + 6), 3, fill=face)
        p.text(cx, cy, key.label, 7, (24, 20, 16), bold=True)

    def _k_chrome(self, p, key, lit, down):
        cx, cy, r = key.circle
        if key.symbol and key.symbol != "eject":
            # The search keys: a small spun disc in a thick black bezel.
            p.circle(cx, cy, r + 1, fill=(4, 4, 5))
            p.circle(cx, cy, r - 1, fill=(18, 18, 19))
            p.chrome(cx, cy, r - 4, tint=self.DARK_CHROME, spin=8 if down else 0)
            self._symbol(p, key.symbol, cx, cy, r * 0.3, L.AMBER)
            return
        p.circle(cx, cy, r + 3, fill=(4, 4, 5))
        if key.name == "master" or lit:
            if lit:
                p.glow((cx - r, cy - r, cx + r, cy + r), lambda d, c: d.ellipse(p.c(cx, cy, r + 2), fill=c), key.color,
                       4, 0.8)
            p.circle(cx, cy, r + 2, fill=key.color if lit else _dim(key.color, 0.5))
            p.circle(cx, cy, r - 2, fill=(4, 4, 5))
        p.chrome(cx, cy, r - 3, spin=8 if down else 0)
        if key.symbol:
            self._symbol(p, key.symbol, cx, cy, r * 0.4,
                         (80, 200, 40) if key.symbol == "eject" else L.AMBER)
        p.text(cx, cy, key.label, key.font, (30, 30, 32), bold=True)

    def _k_loop(self, p, key, lit, down):
        cx, cy, r = key.circle
        p.circle(cx, cy, r + 5, fill=(34, 35, 38))
        p.circle(cx, cy, r + 3, fill=(4, 4, 5))
        if lit:
            p.glow((cx - r, cy - r, cx + r, cy + r), lambda d, c: d.ellipse(p.c(cx, cy, r), fill=c), key.color, 5, 0.9)
            p.gradient((cx - r, cy - r, cx + r, cy + r), (255, 205, 130),
                       (255, 110, 20), ellipse=True)
        else:
            p.gradient((cx - r, cy - r, cx + r, cy + r), (150, 96, 52),
                       (84, 44, 16), ellipse=True)
        p.glow((cx - r, cy - r, cx + r, cy + r), lambda d, c: d.ellipse(p.c(cx - r * 0.15, cy - r * 0.45, r * 0.45),
                                      fill=c), (255, 255, 255), r * 0.3, 0.5, over=True)

    def _k_call(self, p, key, lit, down):
        cx, cy, r = key.circle
        p.circle(cx, cy, r + 3, fill=(70, 72, 76))
        p.circle(cx, cy, r + 1.5, fill=(4, 4, 5))
        p.dome(cx, cy, r, (8, 8, 9) if down else (22, 22, 24))
        self._symbol(p, key.symbol, cx, cy, r * 0.45, (255, 200, 90) if lit else L.AMBER)

    def _k_big(self, p, key, lit, down):
        cx, cy, r = key.circle
        p.circle(cx, cy, r + 2, fill=(4, 4, 5))
        if lit:
            p.glow((cx - r, cy - r, cx + r, cy + r), lambda d, c: d.ellipse(p.c(cx, cy, r + 1), fill=c), key.color, 9, 1.0)
        p.circle(cx, cy, r, fill=_mix(key.color, (255, 255, 255), 0.15) if lit
                 else _dim(key.color, 0.42))
        p.circle(cx, cy, r - 8, fill=(4, 4, 5))
        p.chrome(cx, cy, r - 11, spin=10 if down else 0)
        if key.symbol:
            self._symbol(p, key.symbol, cx, cy, r * 0.16, (40, 40, 42))
        p.text(cx, cy, key.label, key.font, (40, 40, 42), bold=True)

    def _k_arc(self, p, key, lit, down):
        x0, y0, x1, y1 = key.box
        p.rrect(key.box, 9, fill=(4, 4, 5))
        p.gradient((x0 + 1.5, y0 + 1.5, x1 - 1.5, y1 - 1.5),
                   (64, 66, 70) if not down else (30, 31, 33), (26, 27, 29), radius=8)
        p.line([(x0 + 8, y0 + 3), (x1 - 8, y0 + 3)], (110, 112, 118), 1)

    def _k_lever(self, p, key, lit, down):
        x0, y0, x1, y1 = key.box
        p.rrect(key.box, 3, fill=(6, 6, 7), outline=(80, 82, 86), width=1)
        p.gradient((x0 + 4, y0 + 4, x1 - 4, y1 - 4), (30, 31, 33), (10, 10, 11),
                   radius=2)
        hy = (y0 + y1) / 2 + (8 if down else 0)
        p.gradient((x0 + 6, hy - 11, x1 - 6, hy + 5), (120, 122, 128), (48, 49, 53),
                   radius=2)
        p.rrect((x0 + 10, hy - 6, x1 - 10, hy + 1), 1, fill=(70, 72, 76))

    def _symbol(self, p, name, cx, cy, s, col):
        """Transport glyphs, drawn rather than typed so every system agrees."""
        def tri(x, y, s, right):
            d = 1 if right else -1
            p.d.polygon([p.p(x - d * s * 0.5, y - s * 0.6), p.p(x + d * s * 0.6, y),
                         p.p(x - d * s * 0.5, y + s * 0.6)], fill=col)

        if name in ("scan_fwd", "scan_rev"):
            right = name == "scan_fwd"
            tri(cx - s * 0.45, cy, s, right)
            tri(cx + s * 0.45, cy, s, right)
        elif name in ("track_fwd", "track_rev"):
            right = name == "track_fwd"
            sgn = 1 if right else -1
            tri(cx - sgn * s * 0.55, cy, s, right)
            tri(cx + sgn * s * 0.25, cy, s, right)
            bx = cx + sgn * s * 0.95
            p.rrect((bx - s * 0.12, cy - s * 0.6, bx + s * 0.12, cy + s * 0.6), 0,
                    fill=col)
        elif name in ("left", "right"):
            tri(cx, cy, s * 1.3, name == "right")
        elif name == "play":
            tri(cx - s * 1.1, cy, s * 1.5, True)
            p.line([(cx - s * 0.2, cy + s * 0.8), (cx + s * 0.15, cy - s * 0.8)], col, 1.2)
            for dx in (0.45, 1.0):
                p.rrect((cx + s * dx, cy - s * 0.8, cx + s * (dx + 0.32), cy + s * 0.8),
                        0, fill=col)
        elif name == "eject":
            p.d.polygon([p.p(cx - s, cy + s * 0.2), p.p(cx, cy - s * 0.9),
                         p.p(cx + s, cy + s * 0.2)], fill=col)
            p.rrect((cx - s, cy + s * 0.45, cx + s, cy + s * 0.75), 0, fill=col)

    # -- the jog --------------------------------------------------------------

    JOG_DIMPLES = 30                     # the rim repeats every 12 degrees
    JOG_PHASES = 12                      # one sprite per degree of that

    def jog_box(self):
        cx, cy, r = L.JOG
        return (cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2)

    def jog_sprite(self, phase):
        """The platter turned by `phase` degrees (mod the rim's period)."""
        phase = int(phase) % self.JOG_PHASES
        k = ("jog", phase)
        if k not in self._sprites:
            self._sprites[k] = self._jog(phase)
        return self._sprites[k]

    def _jog(self, phase):
        cx, cy, r = L.JOG
        p = Painter(self.jog_box(), self.scale)
        # The outer chrome ring, broad and bright, between two dark seams.
        p.circle(cx, cy, r, fill=(6, 6, 7))
        p.chrome(cx, cy, r - 1.5, lobes=2, spin=-10)
        p.circle(cx, cy, r - 13, fill=(8, 8, 9))
        # The rubber rim: mid grey, a bump every 12 degrees and three studs
        # between each pair. The light stays put while it turns.
        rim_out, rim_in = r - 15, L.JOG_RIM
        p.gradient((cx - rim_out, cy - rim_out, cx + rim_out, cy + rim_out),
                   (50, 51, 55), (34, 35, 38), ellipse=True)
        p.circle(cx, cy, rim_in + 1, fill=(12, 12, 13))
        mid = (rim_out + rim_in) / 2
        step = 360 / self.JOG_DIMPLES
        for i in range(self.JOG_DIMPLES):
            a = math.radians(i * step + phase - 90)
            self._bump(p, cx + mid * math.cos(a), cy + mid * math.sin(a), a,
                       (rim_out - rim_in) * 0.34, math.radians(step) * mid * 0.42)
            b = math.radians((i + 0.5) * step + phase - 90)
            for rr in (rim_in + 9, mid, rim_out - 9):
                self._stud(p, cx + rr * math.cos(b), cy + rr * math.sin(b))
        # The inner chrome ring, with the teal cast of the lens under it.
        p.chrome(cx, cy, rim_in, tint=(0, 16, 18), lobes=2, spin=-10)
        p.circle(cx, cy, rim_in - 5, fill=(0, 0, 0))
        # The platter top: black, with a faint sheen and the display's window.
        top = rim_in - 7
        p.gradient((cx - top, cy - top, cx + top, cy + top), (16, 16, 18), (6, 6, 7),
                   ellipse=True)
        p.glow((cx - top, cy - top, cx + top, cy + top),
               lambda d, c: d.pieslice(p.c(cx, cy, top - 6), 200, 250, fill=c),
               (255, 255, 255), 40, 0.06, over=True)
        p.circle(cx, cy, L.JOG_DISPLAY, fill=(0, 0, 0), outline=(96, 98, 104),
                 width=0.8)
        return p.finish(), p.px

    def _bump(self, p, x, y, a, half_len, half_wid):
        """One pillow of the rim: dark at its edge, lit toward the top."""
        steps = 10
        for k in range(steps):
            t = k / (steps - 1)
            f = 1 - 0.72 * t
            # The lit centre drifts up the screen, wherever the bump is.
            ox, oy = 0, -half_len * 0.28 * t
            pts = []
            for deg in range(0, 360, 10):
                tr = math.radians(deg)
                u, v = half_len * f * math.cos(tr), half_wid * f * math.sin(tr)
                pts.append(p.p(x + ox + u * math.cos(a) - v * math.sin(a),
                               y + oy + u * math.sin(a) + v * math.cos(a)))
            p.d.polygon(pts, fill=_mix((30, 31, 34), (62, 64, 68), t ** 1.2))

    def _stud(self, p, x, y):
        p.circle(x, y, 3.4, fill=(22, 22, 24))
        p.circle(x, y - 0.6, 2.4, fill=(64, 66, 70))
        p.circle(x, y - 0.2, 1.6, fill=(40, 41, 44))

    def centre_box(self):
        cx, cy, _ = L.JOG
        r = L.JOG_DISPLAY - 2
        return (cx - r, cy - r, cx + r, cy + r)

    def centre_sprite(self, ring, live):
        """The jog's centre display without its pointer: the tick disc and the
        ring light. The pointer is drawn over it (centre_pointer), so following
        it costs a canvas move, not an image."""
        return self._centre_base(ring, live)

    def centre_pointer(self, step):
        """The pointer at `step` turns, in canvas pixels: the black gap it
        clears in the ticks (a polygon) and its two red marks (lines)."""
        cx, cy, _ = L.JOG
        s = self.scale
        r_in, r_out = [v * s for v in self.CENTRE_R]
        r_in -= 0.5
        r_out += 1
        a0 = step * 360 - 90
        arc = [math.radians(a0 + self.CENTRE_GAP * i / 8) for i in range(9)]
        gap = [(cx * s + r_out * math.cos(a), cy * s + r_out * math.sin(a)) for a in arc]
        gap += [(cx * s + r_in * math.cos(a), cy * s + r_in * math.sin(a))
                for a in reversed(arc)]
        marks = []
        for da in (-1.2, 1.2):
            a = math.radians(a0 + da)
            marks.append([(cx * s + (r_in + 1.5 * s) * math.cos(a),
                           cy * s + (r_in + 1.5 * s) * math.sin(a)),
                          (cx * s + (r_in + 11 * s) * math.cos(a),
                           cy * s + (r_in + 11 * s) * math.sin(a))])
        return gap, marks

    CENTRE_TICKS = 180
    CENTRE_R = (35, 60)                  # the tick band's inner and outer radius
    CENTRE_GAP = 24                      # degrees of ticks the pointer clears

    def _centre_base(self, ring, live):
        k = ("centre-base", ring, live)
        if k in self._sprites:
            return self._sprites[k]
        cx, cy, _ = L.JOG
        p = Painter(self.centre_box(), self.scale)
        p.circle(cx, cy, L.JOG_DISPLAY - 2, fill=(0, 0, 0))
        r_in, r_out = self.CENTRE_R
        if ring:
            p.glow((cx - r_out, cy - r_out, cx + r_out, cy + r_out),
                   lambda d, c: d.ellipse(p.c(cx, cy, r_out + 4), outline=c,
                                          width=p.w(3)), (200, 225, 255), 3, 0.7)
        tick = (228, 230, 236) if live else (70, 72, 76)
        n = self.CENTRE_TICKS
        for i in range(n):
            a = math.radians(i * 360 / n - 90)
            p.line([(cx + r_in * math.cos(a), cy + r_in * math.sin(a)),
                    (cx + r_out * math.cos(a), cy + r_out * math.sin(a))], tick, 0.55)
        # Concentric seams through the ticks: the display's fine mesh.
        for rr in range(r_in + 4, r_out, 4):
            p.circle(cx, cy, rr, outline=(0, 0, 0), width=0.6)
        for i in range(8):
            p.d.arc(p.c(cx, cy, 25), i * 45 + 8, i * 45 + 40, fill=tick, width=p.w(3.4))
        p.gradient((cx - 18, cy - 18, cx + 18, cy + 18), (150, 180, 215), (70, 105, 150),
                   ellipse=True)
        p.circle(cx, cy, 18, outline=(30, 40, 60), width=1)
        self._sprites[k] = (p.finish(), p.px)
        return self._sprites[k]

    # -- the rotary selector and the tempo slider --------------------------------

    SELECT_NOTCHES = 24                  # repeats every 15 degrees
    SELECT_PHASES = 3                    # 5 degrees a detent

    def select_box(self):
        cx, cy, r = L.SELECT
        return (cx - r - 12, cy - r - 12, cx + r + 12, cy + r + 12)

    def select_sprite(self, phase, down):
        phase = int(phase) % self.SELECT_PHASES
        k = ("select", phase, down)
        if k not in self._sprites:
            cx, cy, r = L.SELECT
            p = Painter(self.select_box(), self.scale)
            p.circle(cx, cy, r + 10, fill=(4, 4, 5), outline=(64, 66, 70), width=1)
            p.chrome(cx, cy, r, spin=phase * 5)
            for i in range(self.SELECT_NOTCHES):
                a = math.radians(i * 15 + phase * 5)
                p.line([(cx + (r - 6) * math.cos(a), cy + (r - 6) * math.sin(a)),
                        (cx + r * math.cos(a), cy + r * math.sin(a))], (60, 62, 66), 1.4)
            ri = r * 0.62
            p.circle(cx, cy, ri + 2, fill=(20, 20, 22))
            p.chrome(cx, cy, ri, lobes=3, spin=phase * 5 + (15 if down else 0))
            p.circle(cx, cy, ri * 0.55, fill=(30, 31, 34) if not down else (14, 14, 15))
            self._sprites[k] = (p.finish(), p.px)
        return self._sprites[k]

    SLIDER_CAP = (58, 56)

    def slider_cap(self):
        k = ("cap",)
        if k not in self._sprites:
            w, h = self.SLIDER_CAP
            p = Painter((0, 0, w + 8, h + 8), self.scale)
            p.rrect((2, 5, w + 6, h + 8), 4, fill=(0, 0, 0, 170))
            p.gradient((4, 4, w + 4, h + 4), (78, 80, 84), (26, 27, 29), radius=3)
            for i in range(7):
                y = 8 + i * (h - 8) / 6
                if abs(y - (4 + h / 2)) < 5:
                    continue
                p.line([(8, y), (w, y)], (16, 16, 18), 1.2)
                p.line([(8, y + 1.3), (w, y + 1.3)], (96, 98, 104), 0.6)
            p.rrect((6, 2 + h / 2, w + 2, 6 + h / 2), 1, fill=(245, 246, 250))
            self._sprites[k] = (p.finish(), None)
        return self._sprites[k][0]


# -- every size from one reference render -------------------------------------
#
# Drawing is slow -- supersampled shapes, glows, chrome: about 0.7 s for the
# body and 0.5 s per jog phase at scale 1 -- and a window resize needs every
# image again. So each image is drawn once, at REF_SCALE, and every smaller
# size is a LANCZOS downscale of it (~30 ms for the body), which is what the
# supersampling does anyway. The reference images are kept on disk, keyed by
# this file's and layout.py's contents, so a later start draws nothing at all.

REF_SCALE = 1.3
_REFS = {}                               # deck label -> the reference Art


def _cache_dir():
    """Where the reference images live; CDJ_APP_CACHE overrides."""
    h = hashlib.sha1(f"{REF_SCALE} {SS}".encode())
    here = os.path.dirname(os.path.abspath(__file__))
    for name in ("art.py", "layout.py"):
        with open(os.path.join(here, name), "rb") as fh:
            h.update(fh.read())
    base = os.environ.get("CDJ_APP_CACHE") or os.path.join(tempfile.gettempdir(),
                                                          "cdj-app-art")
    return os.path.join(base, h.hexdigest()[:12])


_CACHE = None
_LOADED = {}                             # name -> reference image, once read


def _stored(name, render):
    """The reference image `name`: from memory, from disk, or rendered and
    then stored."""
    global _CACHE
    if name in _LOADED:
        return _LOADED[name]
    if _CACHE is None:
        _CACHE = _cache_dir()
        os.makedirs(_CACHE, exist_ok=True)
    path = os.path.join(_CACHE, name + ".png")
    try:
        with Image.open(path) as img:
            _LOADED[name] = img.copy()
            return _LOADED[name]
    except OSError:
        pass
    img = render()
    try:
        tmp = f"{path}.{os.getpid()}.tmp"
        img.save(tmp, "PNG")
        os.replace(tmp, path)            # atomic: two decks may store at once
    except OSError:
        pass                             # a read-only cache just costs time
    _LOADED[name] = img
    return img


class ScaledArt(Art):
    """An Art whose images are downscaled from the reference's."""

    def __init__(self, scale, deck_label):
        super().__init__(scale, deck_label)
        if deck_label not in _REFS:
            _REFS[deck_label] = Art(REF_SCALE, deck_label)
        self.ref = _REFS[deck_label]

    def _fit(self, name, box, render):
        """(image, pixel position) of the reference's `name` at this scale,
        placed on `box` (face units) exactly as a Painter would place it."""
        k = ("fit", name)
        if k not in self._sprites:
            img = _stored(name, render)
            s = self.scale
            px = (round(box[0] * s), round(box[1] * s))
            size = (max(1, round(box[2] * s) - px[0]), max(1, round(box[3] * s) - px[1]))
            self._sprites[k] = (img.resize(size, Image.LANCZOS), px)
        return self._sprites[k]

    def body(self, keys_rest):
        looks = hashlib.sha1("".join(look for _, look in keys_rest).encode()).hexdigest()[:8]
        label = self.deck_label.replace(" ", "_")
        return self._fit(f"body-{label}-{looks}", (0, 0, L.W, L.H),
                         lambda: self.ref.body(keys_rest))[0]

    def key_sprite(self, key, lit, down, look):
        return self._fit(f"key-{key.name}-{int(lit)}{int(down)}-{look}", self.key_box(key),
                         lambda: self.ref.key_sprite(key, lit, down, look)[0])

    def jog_sprite(self, phase):
        phase = int(phase) % self.JOG_PHASES
        return self._fit(f"jog-{phase}", self.jog_box(),
                         lambda: self.ref.jog_sprite(phase)[0])

    def centre_sprite(self, ring, live):
        return self._fit(f"centre-{int(ring)}{int(live)}", self.centre_box(),
                         lambda: self.ref.centre_sprite(ring, live)[0])

    def select_sprite(self, phase, down):
        phase = int(phase) % self.SELECT_PHASES
        return self._fit(f"select-{phase}{int(down)}", self.select_box(),
                         lambda: self.ref.select_sprite(phase, down)[0])

    def slider_cap(self):
        w, h = self.SLIDER_CAP
        return self._fit("cap", (0, 0, w + 8, h + 8), self.ref.slider_cap)[0]


def make_art(scale, deck_label):
    """The Art for a face at `scale`: downscaled from the reference when it is
    smaller than that, drawn directly when larger."""
    if scale <= REF_SCALE:
        return ScaledArt(scale, deck_label)
    return Art(scale, deck_label)
