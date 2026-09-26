# SPDX-License-Identifier: GPL-2.0-or-later
"""The strip under the decks: each deck's state in a few plain words, and
what the control under the mouse does.

Quiet when all is well: a running deck is one grey line, and only a deck
that is waiting or offline says so in a warmer tone. Drawn in the face's own
type (art.py's Helvetica-style print) so the window reads as one product, at
the display's pixel density like the rest of the window.
"""

from PIL import Image, ImageDraw

import art as ART
import gfx

BG = (11, 11, 12)
RULE = (29, 30, 33)
NAME = (185, 189, 196)
TEXT = (125, 130, 138)
WARN = (200, 154, 74)


class StatusBar:
    H = 26                              # points
    PAD = 16
    TEXT_PT = 12

    def __init__(self):
        self.shown = None
        self.surf = None

    def show(self, width, ratio, decks, help_text):
        """decks: [(name, state, ok)] -- a deck's name, its state in words and
        whether that state is the normal one; help_text: shown on the right.
        True when the strip was drawn anew (self.surf)."""
        state = (tuple(decks), help_text, width, ratio)
        if state == self.shown:
            return False
        self.shown = state
        r = ratio
        w, h = max(1, round(width * r)), round(self.H * r)
        img = Image.new("RGB", (w, h), BG)
        d = ImageDraw.Draw(img)
        d.line([(0, 0), (w, 0)], fill=RULE, width=max(1, round(r)))
        font = ART.font(self.TEXT_PT * r)
        mid = h / 2
        x = self.PAD * r
        for name, words, ok in decks:
            x = self._text(d, font, x, mid, name, NAME) + 10 * r
            x = self._text(d, font, x, mid, words, TEXT if ok else WARN) + 28 * r
        room = w - x - self.PAD * r
        if help_text and room > 40 * r:
            d.text((w - self.PAD * r, mid), self._fit(font, help_text, room),
                   font=font, fill=TEXT, anchor="rm")
        self.surf = gfx.surface(img)
        return True

    @staticmethod
    def _text(d, font, x, y, text, fill):
        d.text((x, y), text, font=font, fill=fill, anchor="lm")
        return x + font.getlength(text)

    @staticmethod
    def _fit(font, text, room):
        """text, cut with an ellipsis to fit `room` pixels."""
        if font.getlength(text) <= room:
            return text
        while text and font.getlength(text + "…") > room:
            text = text[:-1]
        return text.rstrip() + "…"
