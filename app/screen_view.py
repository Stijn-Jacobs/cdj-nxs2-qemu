# SPDX-License-Identifier: GPL-2.0-or-later
"""The deck's screen on its own, larger than the drawn face can hold it.

The NXS2's 7-inch screen is a small part of a tall deck, so a face that fits
a monitor shows it small. A ScreenSlot shows one deck's screen at any size
(keeping its 5:3) at a place on a canvas, and it is a touch screen like the
one on the face. The dock (DockView) holds a slot per deck in a panel drawn
like the deck, beside the faces; a ScreenWindow holds one in a window of its
own.
"""

import tkinter as tk

from PIL import Image, ImageTk

import art as ART
import layout as L

LCD_W, LCD_H = 800, 480
BG = "#050505"


def fit(w, h):
    """The largest 5:3 size inside w x h."""
    s = max(0.1, min(w / LCD_W, h / LCD_H))
    return max(1, round(LCD_W * s)), max(1, round(LCD_H * s))


class ScreenSlot:
    """One deck's screen as an image on a canvas, at `origin`, `size` big.

    It is a sink of the deck (DeckView.add_sink): the deck hands it frames at
    its size through show()."""

    def __init__(self, canvas, deck, origin, size):
        self.canvas = canvas
        self.deck = deck
        self.origin = None
        self.size = None
        self.photo = None
        self.last = None
        self.item = canvas.create_image(0, 0, anchor="nw")
        canvas.tag_bind(self.item, "<ButtonPress-1>", lambda e: self._touch(e, True))
        canvas.tag_bind(self.item, "<B1-Motion>", lambda e: self._touch(e, True))
        canvas.tag_bind(self.item, "<ButtonRelease-1>", lambda e: self._touch(e, False))
        canvas.tag_bind(self.item, "<Enter>", lambda e: deck.on_hover(
            deck, f"deck {deck.number} screen: click to touch"))
        self.place(origin, size)

    def place(self, origin, size):
        size = tuple(size)
        self.origin = tuple(origin)
        self.canvas.coords(self.item, *self.origin)
        if size != self.size:
            self.size = size
            self.photo = ImageTk.PhotoImage(Image.new("RGB", size))
            self.canvas.itemconfig(self.item, image=self.photo)
            self.last = None
            self.deck.views_changed()

    def show(self, img):
        if img is not None and img.size == self.size:
            self.photo.paste(img)
            self.last = img

    def paste_into(self, img):
        """Draw what this slot shows onto `img` (a snapshot of its canvas)."""
        if self.last is not None:
            img.paste(self.last, self.origin)

    def destroy(self):
        self.canvas.delete(self.item)

    def _touch(self, e, down):
        x = (e.x - self.origin[0]) * LCD_W / self.size[0]
        y = (e.y - self.origin[1]) * LCD_H / self.size[1]
        if down and not (0 <= x < LCD_W and 0 <= y < LCD_H):
            return
        self.deck.screen.pointer(x, y, down)


class DockView:
    """Every docked screen in one panel beside the faces, drawn like the deck:
    gunmetal, each screen in a gloss bezel under its deck's name. The panel is
    only as tall as its screens and sits level with the faces' tops."""

    def __init__(self, master):
        self.canvas = tk.Canvas(master, highlightthickness=0, bd=0, bg=BG)
        self.bg_item = self.canvas.create_image(0, 0, anchor="nw")
        self.bg_photo = None
        self.bg_img = None
        self.drawn = None               # what the background was drawn for
        self.slots = {}                 # deck number -> ScreenSlot

    @staticmethod
    def overhead():
        """(extra width, extra height per screen, extra height once) around the
        screens, in face units: what the panel adds to z * LCD size."""
        B, pad = L.DOCK_BEZEL, L.DOCK_PAD
        return 2 * (pad + B), L.DOCK_HEADER + 2 * B + 5, pad

    def place(self, decks, z, s):
        """Lay out every deck's screen at z (pixels per LCD pixel) in a panel
        drawn at face scale s. Returns the panel's width."""
        B, pad = L.DOCK_BEZEL, L.DOCK_PAD
        gw, gh = round(LCD_W * z), round(LCD_H * z)
        width = round(gw + 2 * (pad + B) * s)
        # The glass boxes in face units, top down.
        boxes, y = [], pad
        for d in decks:
            y += L.DOCK_HEADER + B
            x0 = pad + B
            boxes.append((d, (x0, y, x0 + gw / s, y + gh / s)))
            y += gh / s + B + 5
        height = round((y + pad) * s)
        key = (width, height, round(s, 4), gw, tuple(d.number for d in decks))
        if key != self.drawn:
            self.drawn = key
            self.canvas.config(width=width, height=height)
            self.bg_img = ART.dock((width / s, height / s), s,
                                   [(f"DECK {d.number}", box) for d, box in boxes])
            self.bg_photo = ImageTk.PhotoImage(self.bg_img)
            self.canvas.itemconfig(self.bg_item, image=self.bg_photo)
        for d, (x0, y0, _, _) in boxes:
            origin = (round(x0 * s), round(y0 * s))
            slot = self.slots.get(d.number)
            if slot is None:
                slot = self.slots[d.number] = ScreenSlot(self.canvas, d, origin, (gw, gh))
                d.add_sink("dock", slot)
            else:
                slot.place(origin, (gw, gh))
        return width

    def clear(self, decks):
        for d in decks:
            slot = self.slots.pop(d.number, None)
            if slot:
                d.drop_sink("dock")
                slot.destroy()

    def snapshot(self):
        img = self.bg_img.copy() if self.bg_img else Image.new("RGB", (1, 1))
        for slot in self.slots.values():
            slot.paste_into(img)
        return img


class ScreenWindow:
    """A deck's screen in a window of its own, resizable, touchable."""

    def __init__(self, master, deck, on_close):
        self.deck = deck
        self.on_close = on_close
        self.win = tk.Toplevel(master)
        self.win.title(f"Deck {deck.number} screen")
        self.win.configure(bg=BG)
        self.win.minsize(200, 120)
        self.canvas = tk.Canvas(self.win, highlightthickness=0, bd=0, bg=BG,
                                width=LCD_W, height=LCD_H)
        self.canvas.pack(expand=True)
        self.view = ScreenSlot(self.canvas, deck, (0, 0), (LCD_W, LCD_H))
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        self.win.bind("<Configure>", self._resized)
        self.win.bind("<F11>", self._fullscreen)
        self.full = False

    def _resized(self, e):
        if e.widget is self.win:
            size = fit(e.width, e.height)
            self.canvas.config(width=size[0], height=size[1])
            self.view.place((0, 0), size)

    def _fullscreen(self, e=None):
        self.full = not self.full
        self.win.attributes("-fullscreen", self.full)

    def close(self):
        self.win.destroy()
        self.on_close(self)
