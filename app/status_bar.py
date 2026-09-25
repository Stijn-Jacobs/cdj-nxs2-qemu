# SPDX-License-Identifier: GPL-2.0-or-later
"""The strip under the decks: each deck's state in a few plain words, and
what the control under the mouse does.

Quiet when all is well: a running deck is one grey line, and only a deck
that is waiting or offline says so in a warmer tone. Drawn in the face's own
type (art.py's Helvetica-style print) so the window reads as one product.
"""

import tkinter as tk
import tkinter.font as tkfont

BG = "#0b0b0c"
RULE = "#1d1e21"
NAME = "#b9bdc4"
TEXT = "#7d828a"
WARN = "#c89a4a"

_FAMILIES = ["Helvetica", "Arial", "Liberation Sans", "Nimbus Sans", "DejaVu Sans"]


def ui_font(root, size):
    """The panel print's family, as Tk names it, at `size` points."""
    have = set(tkfont.families(root))
    family = next((f for f in _FAMILIES if f in have), "TkDefaultFont")
    return tkfont.Font(root=root, family=family, size=size)


class StatusBar:
    H = 26
    PAD = 16

    def __init__(self, master):
        self.canvas = tk.Canvas(master, height=self.H, bg=BG, highlightthickness=0, bd=0)
        self.font = ui_font(master, 9)
        self.shown = None

    def show(self, decks, help_text):
        """decks: [(name, state, ok)] -- a deck's name, its state in words and
        whether that state is the normal one; help_text: shown on the right."""
        state = (tuple(decks), help_text, self.canvas.winfo_width())
        if state == self.shown:
            return
        self.shown = state
        c = self.canvas
        c.delete("all")
        w, mid = c.winfo_width(), self.H / 2
        c.create_line(0, 0, w, 0, fill=RULE)
        x = self.PAD
        for name, words, ok in decks:
            x = self._text(x, mid, name, NAME) + 10
            x = self._text(x, mid, words, TEXT if ok else WARN) + 28
        room = w - x - self.PAD
        if help_text and room > 40:
            c.create_text(w - self.PAD, mid, text=self._fit(help_text, room),
                          font=self.font, fill=TEXT, anchor="e")

    def _text(self, x, y, text, fill):
        self.canvas.create_text(x, y, text=text, font=self.font, fill=fill, anchor="w")
        return x + self.font.measure(text)

    def _fit(self, text, room):
        """text, cut with an ellipsis to fit `room` pixels."""
        if self.font.measure(text) <= room:
            return text
        while text and self.font.measure(text + "…") > room:
            text = text[:-1]
        return text.rstrip() + "…"
