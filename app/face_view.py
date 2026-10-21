# SPDX-License-Identifier: GPL-2.0-or-later
"""One older player on screen: a face from face.py with the live LCD in it.

The same job as deck_view.py, with the same interface to the window, for a
player whose face is data. The LCD arrives over VNC; the keyboard goes to the
display board over VNC as well, and the drawn keys send panel datagrams to the
MAIN emulator (panel_link.py). Nothing is lit: no lamp signal comes back, and
a control with no known report bit is drawn dark and ignored. A player with a
touch screen gets the finger on its LCD: as panel datagrams where the panel
MCU digitises it (report bytes 0x16-0x19), or over VNC where the display
board takes a pointer. The jog turns under the mouse like the NXS2's and, on
a player whose counter has been measured (the face's `counter`), sends the
platter's report fields; so does the tempo slider where the face names its
report byte. On any other player the jog only turns on screen and the slider
stays where the body draws it.
"""

import math
import time

import pygame

import art as ART
import controls as C
import gfx
from face_art import FaceArt

DETENT_DEG = 15


def _angle(x, y, centre):
    return math.degrees(math.atan2(y - centre[1], x - centre[0]))


def _turned(angle, since):
    """The degrees from `since` to `angle`, the short way round."""
    return (angle - since + 180) % 360 - 180


class FaceDeckView:
    """Positions handed in (the mouse) are in points from the face's top left."""

    def __init__(self, face, number, tag, link, screen, on_hover):
        self.face = face
        self.number = number
        self.tag = tag
        self.link = link
        self.screen = screen
        self.on_hover = on_hover
        self.down_key = None
        self.latched = set()            # names of the levers held in
        self.turning = None             # the angle the selector was last at
        self.turn_acc = 0.0
        self.frames_shown = []
        self.jog = C.Jog(self.send, **face.jog_counter) if face.jog_counter else C.Jog(lambda datagram: None)
        if face.jog_counter:
            self.jog.start()
        self.grab_jog = None            # the angle the platter was last at
        self.slider = C.WordSlider(self.send, face.slider_off) if face.slider_off is not None else None
        self.grab_slider = False
        self.finger = False             # the mouse is down on the touch screen
        self.scale = self.ratio = None
        self.surf = None
        self.lcd_size = None
        self.lcd = None
        self.pressed = {}               # key name -> (sprite, position)
        self.select_layer = None
        self.detents = 0
        self.warm = []
        self.dirty = []

    def send(self, datagram):
        self.link.send(self.tag, datagram)

    @property
    def size(self):
        return round(self.face.size[0] * self.scale), round(self.face.size[1] * self.scale)

    @property
    def px(self):
        return self.scale * self.ratio

    def close(self):
        self.jog.stop()

    # -- drawing --------------------------------------------------------------

    def rescale(self, scale, ratio):
        self.scale, self.ratio = scale, ratio
        self.art = FaceArt(self.px, self.face, f"DECK {self.number}")
        self.body = gfx.surface(self.art.body())
        self.surf = self.body.copy()
        x0, y0, x1, y1 = [round(v * self.px) for v in self.face.lcd]
        self.lcd_size = (x1 - x0, y1 - y0)
        self.lcd_pos = (x0, y0)
        self.lcd = None
        self.pressed = {}
        self.screen.set_views({"face": self.lcd_size})
        self._draw_jog()
        self._draw_cap()
        img, pos = self.art.centre_sprite(False, False)
        self.centre = (gfx.surface(img), pos)
        self.warm = list(range(1, ART.Art.JOG_PHASES))
        self._draw_select(False)
        for name in self.latched:
            self._draw_key(self.face.key_named(name), True)
        self.dirty = [self.surf.get_rect()]

    def _draw_jog(self):
        img, pos = self.art.jog_sprite(self.jog.angle % ART.Art.JOG_PHASES)
        self.jog_layer = (gfx.surface(img), pos)
        self.dirty.append(pygame.Rect(pos, self.jog_layer[0].get_size()))

    def _draw_cap(self):
        if not self.slider:
            self.cap_layer = None
            return
        sprite = gfx.surface(self.art.slider_cap())
        self.cap_layer = (sprite, self.art.cap_pos(self.slider.pos))
        self.dirty.append(pygame.Rect(self.cap_layer[1], sprite.get_size()))

    def _move_cap(self, y):
        top, bottom = self.face.slider[1:3]
        old = self.cap_layer[1]
        self.slider.set((y - top) / (bottom - top))
        self.dirty.append(pygame.Rect(old, self.cap_layer[0].get_size()))
        self._draw_cap()

    def _draw_select(self, down):
        img, pos = self.art.select_sprite(self.detents, down)
        self.select_layer = (gfx.surface(img), pos)
        self.dirty.append(pygame.Rect(pos, self.select_layer[0].get_size()))

    def _turn_select(self, detents):
        self.detents += detents
        datagram = self.face.turn_datagram(detents)
        if datagram:
            self.send(datagram)
        self._draw_select(False)

    def _draw_key(self, key, down):
        if key.name == "ROTARY PUSH":
            self._draw_select(down)
            return
        img, pos = self.art.key_sprite(key, False, down, "live")
        sprite = gfx.surface(img)
        if down:
            self.pressed[key.name] = (sprite, pos)
        else:
            self.pressed.pop(key.name, None)
        self.dirty.append(pygame.Rect(pos, sprite.get_size()))

    def _redraw(self, rect):
        s = self.surf
        s.set_clip(rect)
        s.blit(self.body, rect.topleft, area=rect)
        for sprite, pos in filter(None, (self.jog_layer, self.cap_layer, self.centre)):
            s.blit(sprite, pos)
        if self.lcd:
            s.blit(self.lcd, self.lcd_pos)
        for sprite, pos in (*self.pressed.values(), self.select_layer):
            s.blit(sprite, pos)
        s.set_clip(None)

    def compose(self):
        bounds = self.surf.get_rect()
        rects = [r.clip(bounds) for r in self.dirty]
        self.dirty = []
        rects = [r for r in rects if r.w and r.h]
        for r in rects:
            self._redraw(r)
        return rects

    def tick(self, now):
        frame = self.screen.take_frame()
        if frame is None:
            if self.warm:
                self.art.jog_sprite(self.warm.pop(0))
            return
        shown = frame.get("face")
        if shown is not None and shown.size == self.lcd_size:
            self.lcd = gfx.surface(shown, self.surf)
            self.dirty.append(pygame.Rect(self.lcd_pos, self.lcd_size))
        self.frames_shown = [t for t in self.frames_shown if now - t < 2.0] + [now]

    def fps(self):
        return len(self.frames_shown) / 2.0

    def health(self):
        name = f"Deck {self.number}"
        if not self.screen.connected:
            why = self.screen.trouble(time.monotonic())
            return name, f"No screen  ·  {why}" if why else "Waiting for the screen", False
        return name, f"Running  ·  {self.fps():.0f} fps", True

    def status(self):
        scr = self.screen
        screen = (f"screen via {scr.via()} {scr.updates.rate(time.monotonic()):.0f} upd/s, "
                  f"{self.fps():.0f} fps shown" if scr.connected else scr.status)
        return f"deck {self.number} ({self.tag}, {self.face.title}): {screen}"

    # -- the mouse, in points from the face's top left ----------------------------

    def units(self, pos):
        return pos[0] / self.scale, pos[1] / self.scale

    def _key_at(self, x, y):
        return next((k for k in self.face.keys if k.hit(x, y)), None)

    def _in_jog(self, x, y):
        cx, cy, r = self.face.jog
        return math.hypot(x - cx, y - cy) <= r

    def _in_slider(self, x, y):
        sx, top, bottom = self.face.slider[:3]
        cap_w, cap_h = self.art.SLIDER_CAP
        return bool(self.slider) and abs(x - sx) <= cap_w / 2 and top - cap_h / 2 <= y <= bottom + cap_h / 2

    def _in_selector(self, x, y):
        cx, cy = self.face.rotary
        return math.hypot(x - cx, y - cy) <= self.face.rotary_radius

    def _in_touch_screen(self, x, y):
        x0, y0, x1, y1 = self.face.lcd
        return bool(self.face.touch) and x0 <= x < x1 and y0 <= y < y1

    def _touch(self, x, y, down):
        """The finger at (x, y) in points-as-units, in the LCD's own pixels."""
        x0, y0, x1, y1 = self.face.lcd
        w, h = self.face.touch
        px = min(max(int((x - x0) / (x1 - x0) * w), 0), w - 1)
        py = min(max(int((y - y0) / (y1 - y0) * h), 0), h - 1)
        if self.face.touch_via == "vnc":
            self.screen.pointer(px, py, down)
        else:
            self.send(f"{px}:{py}:{int(down)}:touch")

    def down(self, pos):
        x, y = self.units(pos)
        if self._in_touch_screen(x, y):
            self.finger = True
            self._touch(x, y, True)
            return
        key = self._key_at(x, y)
        if key and key.wired and key.latch:
            held = key.name not in self.latched
            self.latched.symmetric_difference_update({key.name})
            self.send(key.datagram("hold" if held else "rel"))
            self._draw_key(key, held)
        elif key and key.wired:
            self.down_key = key
            self.send(key.datagram("hold"))
            self._draw_key(key, True)
        elif self._in_selector(x, y):
            self.turning = _angle(x, y, self.face.rotary)
            self.turn_acc = 0.0
        elif self._in_slider(x, y):
            self.grab_slider = True
            self._move_cap(y)
        elif self._in_jog(x, y):
            self.grab_jog = _angle(x, y, self.face.jog)
            if self.face.jog_counter:
                self.jog.touch(math.hypot(x - self.face.jog[0], y - self.face.jog[1]) <= self.face.platter_radius)

    def drag(self, pos):
        x, y = self.units(pos)
        if self.finger:
            self._touch(x, y, True)
            return
        if self.grab_slider:
            self._move_cap(y)
            return
        if self.grab_jog is not None:
            angle = _angle(x, y, self.face.jog)
            self.jog.turn(_turned(angle, self.grab_jog), time.monotonic())
            self.grab_jog = angle
            self._draw_jog()
            return
        if self.turning is None:
            return
        angle = _angle(x, y, self.face.rotary)
        self.turn_acc += _turned(angle, self.turning)
        self.turning = angle
        detents = int(self.turn_acc / DETENT_DEG)
        if detents:
            self.turn_acc -= detents * DETENT_DEG
            self._turn_select(detents)

    def up(self, pos):
        if self.finger:
            self.finger = False
            self._touch(*self.units(pos), False)
        if self.grab_jog is not None and self.face.jog_counter:
            self.jog.touch(False)
            self.jog.release(time.monotonic())
        self.turning = self.grab_jog = None
        self.grab_slider = False
        key, self.down_key = self.down_key, None
        if key:
            self.send(key.datagram("rel"))
            self._draw_key(key, False)

    def wheel(self, pos, sign):
        """One notch: +1 up, -1 down; wheel up moves up the list, over the
        jog it nudges the platter."""
        x, y = self.units(pos)
        if self._in_jog(x, y):
            self.jog.nudge(4.0 * sign, time.monotonic())
            self._draw_jog()
        else:
            self._turn_select(-sign)

    def hover(self, pos):
        x, y = self.units(pos)
        key = self._key_at(x, y)
        if key:
            parts = [key.name.capitalize()]
            if key.keycap:
                parts.append(f"key {key.keycap}")
            if key.latch:
                parts.append("click to flip it")
            if not key.wired:
                parts.append("no known panel signal, not wired")
            text = "  ·  ".join(parts)
        elif self._in_touch_screen(x, y):
            text = "Touch screen  ·  click and drag to use it"
        elif self._in_selector(x, y):
            text = "Rotary selector  ·  wheel or drag to turn, centre to push"
        elif self._in_slider(x, y):
            text = "Tempo  ·  drag to move the slider"
        elif self._in_jog(x, y) and self.face.jog_counter:
            text = "Jog  ·  drag round to bend or scrub, the top plate is touch-sensitive"
        elif self._in_jog(x, y):
            text = "Jog  ·  drag round to turn it, not wired on this player"
        else:
            text = ""
        self.on_hover(self, text)

    def popup(self, pos):
        pass
