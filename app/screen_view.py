# SPDX-License-Identifier: GPL-2.0-or-later
"""The deck's screen on its own, larger than the drawn face can hold it.

The NXS2's 7-inch screen is a small part of a tall deck, so a face that fits
a monitor shows it small. A ScreenSlot shows one deck's screen at any size
(keeping its 5:3) at a place in a view, and it is a touch screen like the one
on the face. The dock (DockView) holds a slot per deck in a panel drawn like
the deck, beside the faces; a ScreenWindow holds one in a window of its own.

Sizes and places are in points, as the window lays itself out; each is drawn
at the display's pixel density (ratio pixels a point), so a docked screen at
its own 800 x 480 points is 1600 x 960 pixels on a Retina display: every deck
pixel exactly two by two.
"""

import pygame

import art as ART
import gfx
import layout as L

LCD_W, LCD_H = 800, 480
BG = (5, 5, 5)


def fit(w, h):
    """The largest 5:3 size inside w x h."""
    s = max(0.1, min(w / LCD_W, h / LCD_H))
    return max(1, round(LCD_W * s)), max(1, round(LCD_H * s))


class ScreenSlot:
    """One deck's screen at `origin`, `size` big (points) in a view drawn at
    `ratio` pixels a point.

    It is a sink of the deck (DeckView.add_sink): the deck hands it frames at
    px_size through show()."""

    def __init__(self, deck, origin, size, ratio):
        self.deck = deck
        self.origin = self.size = self.px_size = None
        self.ratio = ratio
        self.surf = None
        self.changed = False
        self.place(origin, size, ratio)

    def place(self, origin, size, ratio):
        self.origin = tuple(origin)
        size = tuple(size)
        px_size = (max(1, round(size[0] * ratio)), max(1, round(size[1] * ratio)))
        self.size, self.ratio = size, ratio
        if px_size != self.px_size:
            self.px_size = px_size
            self.surf = pygame.Surface(px_size)
            self.surf.fill((0, 0, 0))
            self.changed = True
            self.deck.views_changed()

    @property
    def px_origin(self):
        return round(self.origin[0] * self.ratio), round(self.origin[1] * self.ratio)

    def show(self, img):
        if img is not None and img.size == self.px_size:
            self.surf = gfx.surface(img, self.surf)
            self.changed = True

    def contains(self, pos):
        x, y = pos[0] - self.origin[0], pos[1] - self.origin[1]
        return 0 <= x < self.size[0] and 0 <= y < self.size[1]

    def touch(self, pos, down):
        """The mouse at pos (points in the slot's view): a touch on the screen."""
        x = (pos[0] - self.origin[0]) * LCD_W / self.size[0]
        y = (pos[1] - self.origin[1]) * LCD_H / self.size[1]
        if down and not (0 <= x < LCD_W and 0 <= y < LCD_H):
            return
        self.deck.screen.pointer(x, y, down)

    def hover(self):
        self.deck.on_hover(self.deck, f"deck {self.deck.number} screen: click to touch")


class DockView:
    """Every docked screen in one panel beside the faces, drawn like the deck:
    gunmetal, each screen in a gloss bezel under its deck's name. The panel is
    only as tall as its screens and sits level with the faces' tops."""

    def __init__(self):
        self.surf = None
        self.bg = None
        self.drawn = None               # what the background was drawn for
        self.slots = {}                 # deck number -> ScreenSlot
        self.size = (0, 0)              # points
        self.full = True                # redraw the background too
        self.grab = None

    @staticmethod
    def overhead():
        """(extra width, extra height per screen, extra height once) around the
        screens, in face units: what the panel adds to z * LCD size."""
        B, pad = L.DOCK_BEZEL, L.DOCK_PAD
        return 2 * (pad + B), L.DOCK_HEADER + 2 * B + 5, pad

    def place(self, decks, z, s, ratio):
        """Lay out every deck's screen at z (points per LCD pixel) in a panel
        drawn at face scale s. Returns the panel's width in points."""
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
        self.size = (width, height)
        key = (width, height, round(s, 4), gw, ratio, tuple(d.number for d in decks))
        if key != self.drawn:
            self.drawn = key
            img = ART.dock((width / s, height / s), s * ratio,
                           [(f"DECK {d.number}", box) for d, box in boxes])
            self.surf = pygame.Surface((round(width * ratio), round(height * ratio)))
            self.bg = gfx.surface(img, self.surf)
            self.full = True
        for d, (x0, y0, _, _) in boxes:
            origin = (round(x0 * s), round(y0 * s))
            slot = self.slots.get(d.number)
            if slot is None:
                slot = self.slots[d.number] = ScreenSlot(d, origin, (gw, gh), ratio)
                d.add_sink("dock", slot)
            else:
                slot.place(origin, (gw, gh), ratio)
        return width

    def clear(self, decks):
        for d in decks:
            if self.slots.pop(d.number, None):
                d.drop_sink("dock")
        self.grab = None

    def compose(self):
        """Bring the surface up to date; the pixel rects that changed."""
        rects = []
        if self.full:
            self.full = False
            self.surf.blit(self.bg, (0, 0))
            for slot in self.slots.values():
                slot.changed = True
            rects.append(self.surf.get_rect())
        for slot in self.slots.values():
            if slot.changed:
                slot.changed = False
                self.surf.blit(slot.surf, slot.px_origin)
                rects.append(pygame.Rect(slot.px_origin, slot.px_size))
        return rects

    def snapshot(self):
        self.compose()
        return gfx.image(self.surf)

    # -- the mouse, in points from the panel's top left --------------------------

    def _slot_at(self, pos):
        return next((s for s in self.slots.values() if s.contains(pos)), None)

    def down(self, pos):
        self.grab = self._slot_at(pos)
        if self.grab:
            self.grab.touch(pos, True)

    def drag(self, pos):
        if self.grab:
            self.grab.touch(pos, True)

    def up(self, pos):
        grab, self.grab = self.grab, None
        if grab:
            grab.touch(pos, False)

    def hover(self, pos):
        slot = self._slot_at(pos)
        if slot:
            slot.hover()

    def wheel(self, pos, sign):
        pass

    def popup(self, pos):
        pass


class ScreenWindow:
    """A deck's screen in a window of its own, resizable, touchable; F11 there
    for full screen."""

    def __init__(self, deck, on_close):
        self.deck = deck
        self.on_close = on_close
        self.win = pygame.Window(f"Deck {deck.number} screen", (LCD_W, LCD_H),
                                 allow_high_dpi=True, resizable=True)
        self.win.minimum_size = (200, 120)
        self.view = ScreenSlot(deck, (0, 0), (LCD_W, LCD_H), self._ratio())
        self.full = False
        self.redraw = True
        self.grab = False

    @property
    def id(self):
        return self.win.id

    def _ratio(self):
        return self.win.get_surface().get_width() / max(1, self.win.size[0])

    def resized(self):
        w, h = self.win.size
        size = fit(w, h)
        self.view.place(((w - size[0]) // 2, (h - size[1]) // 2), size, self._ratio())
        self.redraw = True

    def fullscreen(self):
        self.full = not self.full
        if self.full:
            self.win.set_fullscreen(desktop=True)
        else:
            self.win.set_windowed()

    def present(self):
        if not (self.redraw or self.view.changed):
            return
        surf = self.win.get_surface()
        if self.redraw:
            surf.fill(BG)
            self.redraw = False
        self.view.changed = False
        surf.blit(self.view.surf, self.view.px_origin)
        self.win.flip()

    def down(self, pos):
        self.grab = self.view.contains(pos)
        if self.grab:
            self.view.touch(pos, True)

    def drag(self, pos):
        if self.grab:
            self.view.touch(pos, True)

    def up(self, pos):
        if self.grab:
            self.grab = False
            self.view.touch(pos, False)

    def hover(self, pos):
        self.view.hover()

    def close(self):
        self.win.destroy()
        self.on_close(self)
