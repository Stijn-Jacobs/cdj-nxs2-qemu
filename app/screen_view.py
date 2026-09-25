# SPDX-License-Identifier: GPL-2.0-or-later
"""The deck's screen on its own, larger than the drawn face can hold it.

The NXS2's 7-inch screen is a small part of a tall deck, so a face that fits
a monitor shows it small. A ScreenView shows one deck's screen at any size
(keeping its 5:3), docked beside the faces in the main window or in a window
of its own, and it is a touch screen like the one on the face.
"""

import tkinter as tk

from PIL import Image, ImageTk

LCD_W, LCD_H = 800, 480
BG = "#050505"


def fit(w, h):
    """The largest 5:3 size inside w x h."""
    s = max(0.1, min(w / LCD_W, h / LCD_H))
    return max(1, round(LCD_W * s)), max(1, round(LCD_H * s))


class ScreenView:
    """One deck's screen in a canvas, scaled to `size`."""

    def __init__(self, master, deck, size, title=""):
        self.deck = deck
        self.canvas = tk.Canvas(master, highlightthickness=0, bd=0, bg=BG)
        self.title = title
        self.size = None
        self.item = None
        self.photo = None
        self.last = None
        self.resize(size)
        c = self.canvas
        c.bind("<ButtonPress-1>", lambda e: self._touch(e, True))
        c.bind("<B1-Motion>", lambda e: self._touch(e, True))
        c.bind("<ButtonRelease-1>", lambda e: self._touch(e, False))
        c.bind("<Enter>", lambda e: deck.on_hover(deck, f"deck {deck.number} screen: "
                                                  "click to touch"), add="+")

    LABEL_H = 18

    def resize(self, size):
        size = tuple(size)
        if size == self.size:
            return
        self.size = size
        top = self.LABEL_H if self.title else 0
        self.canvas.config(width=size[0], height=size[1] + top)
        self.canvas.delete("all")
        if self.title:
            self.canvas.create_text(4, top // 2, anchor="w", text=self.title,
                                    fill="#8a8e96", font=("TkDefaultFont", 9))
        self.photo = ImageTk.PhotoImage(Image.new("RGB", size))
        self.item = self.canvas.create_image(0, top, anchor="nw", image=self.photo)
        self.deck.views_changed()

    def snapshot(self):
        """What this view shows, as an image (for the app's snapshot)."""
        top = self.LABEL_H if self.title else 0
        img = Image.new("RGB", (self.size[0], self.size[1] + top), (5, 5, 5))
        if self.last is not None:
            img.paste(self.last, (0, top))
        return img

    def show(self, img):
        if img is not None and img.size == self.size:
            self.photo.paste(img)
            self.last = img

    def _touch(self, e, down):
        top = self.LABEL_H if self.title else 0
        x = e.x * LCD_W / self.size[0]
        y = (e.y - top) * LCD_H / self.size[1]
        if down and not (0 <= x < LCD_W and 0 <= y < LCD_H):
            return
        self.deck.screen.pointer(x, y, down)


class ScreenWindow:
    """A deck's screen in a window of its own, resizable, touchable."""

    def __init__(self, master, deck, on_close):
        self.deck = deck
        self.on_close = on_close
        self.win = tk.Toplevel(master)
        self.win.title(f"Deck {deck.number} screen")
        self.win.configure(bg=BG)
        self.win.minsize(200, 120)
        self.view = ScreenView(self.win, deck, (LCD_W, LCD_H))
        self.view.canvas.pack(expand=True)
        self.win.protocol("WM_DELETE_WINDOW", self.close)
        self.win.bind("<Configure>", self._resized)
        self.win.bind("<F11>", self._fullscreen)
        self.full = False

    def _resized(self, e):
        if e.widget is self.win:
            self.view.resize(fit(e.width, e.height))

    def _fullscreen(self, e=None):
        self.full = not self.full
        self.win.attributes("-fullscreen", self.full)

    def close(self):
        self.win.destroy()
        self.on_close(self)
