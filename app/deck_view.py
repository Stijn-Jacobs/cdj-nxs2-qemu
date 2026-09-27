# SPDX-License-Identifier: GPL-2.0-or-later
"""One deck on screen: the drawn face, the live LCD, the mouse on its controls.

The face (art.py) is one image with sprites on top for whatever changes,
composed into a surface of the deck's own at the display's pixel density: a
face laid out in points is drawn with ratio pixels to the point (2 on a Retina
screen), so it is as sharp as the display. Input takes two routes, one per kind
of thing:

  * the LCD and the keyboard go over VNC (rfb.py) to the display board, whose
    touch handler and key map (sh7269gui.c) forward them to MAIN, exactly as
    they do for the QEMU window;
  * the drawn panel keys, the jog, the selector and the tempo fader go to
    the relay (relay_link.py) as panel datagrams, the path a MIDI controller
    takes.

The lamps come back from the relay: MAIN's panel-lamp frame lights PLAY, CUE,
SLIP, MASTER TEMPO and the jog ring, and its jog pointer drives the centre
display.
"""

import math
import time

import pygame

import art as ART
import controls as C
import gfx
import layout as L

LCD_SIZE = (800, 480)
BG = (5, 5, 5)
POINTER_RED = (255, 42, 32)


class DeckView:
    """One deck: its surface, sprites and the controls behind them. Positions
    handed in (the mouse) are in points relative to the face's top left."""

    def __init__(self, number, tag, relay, screen, on_hover, on_popup=None):
        self.number = number
        self.tag = tag
        self.relay = relay
        self.screen = screen
        self.on_hover = on_hover
        self.on_popup = on_popup
        self.keys = {k.name: k for k in L.KEYS}
        self.controls = {k.name: C.KeyControl(k.action, self.send) for k in L.KEYS}
        self.jog = C.Jog(self.send)
        self.jog.start()
        self.selector = C.Selector(self.send)
        self.slider = C.TempoSlider(self.send)
        self.grab = None                # what the mouse button is holding
        self.lit = {}                   # key name -> (lit, down) now drawn
        self.surfaces = {}              # sprite key -> Surface, made once a scale
        self.layers = {}                # layer name -> (Surface, (x, y) pixels)
        self.key_order = []             # key layers, in the order first drawn
        self.pointer = None             # (gap polygon, marks) or None, pixels
        self.frames_shown = []
        self.sinks = {}                 # name -> ScreenSlot showing this screen too
        self.face_shown = 0.0
        self.lcd_size = None
        self.scale = self.ratio = None
        self.surf = None
        self.dirty = []                 # pixel rects of self.surf to redraw

    def send(self, datagram):
        """One panel datagram to this deck, through the relay."""
        self.relay.send(self.tag, datagram)

    @property
    def size(self):
        """The face's size in points."""
        return round(L.W * self.scale), round(L.H * self.scale)

    # -- drawing --------------------------------------------------------------

    def rescale(self, scale, ratio):
        """Lay the face out at `scale` points a face unit, drawn at `ratio`
        pixels a point."""
        self.scale, self.ratio = scale, ratio
        self.art = ART.make_art(scale * ratio, f"DECK {self.number}")
        self.surf = pygame.Surface(self.art.size)
        self.surfaces.clear()
        self.lit.clear()
        self.layers = {}
        self.key_order = []
        self.pointer = None
        body = self.art.body([(k, self.controls[k.name].look) for k in L.KEYS])
        self.layers["body"] = (self._surface("body", body), (0, 0))
        x0, y0, x1, y1 = [round(v * self.px) for v in L.LCD]
        self.lcd_size = (x1 - x0, y1 - y0)
        self.layers["lcd"] = (pygame.Surface(self.lcd_size), (x0, y0))
        self.views_changed()
        self._sprite("jog", *self.art.jog_sprite(0))
        self._sprite("select", *self.art.select_sprite(0, False))
        self.centre_state = None
        self.pointer_step = None
        self._draw_centre()
        self._sprite("cap", self.art.slider_cap(), (0, 0), key="cap")
        self._place_cap()
        # Draw the lamps' lit looks ahead, a few per tick, so the first blink
        # of a lamp does not stall the screen while its sprite is made.
        self.warm = [(k, lit, down) for k in L.KEYS if k.lamp or self.controls[k.name].live
                     for lit, down in ((True, False), (False, True), (True, True))]
        # And the jog's rotation phases, drawn the first time it turns otherwise.
        self.warm += [("jog", phase) for phase in range(1, self.art.JOG_PHASES)]
        self.dirty = [self.surf.get_rect()]

    @property
    def px(self):
        """Pixels a face unit."""
        return self.scale * self.ratio

    def _surface(self, key, img):
        if key not in self.surfaces:
            self.surfaces[key] = gfx.surface(img, self.surf)
        return self.surfaces[key]

    def _layer_rect(self, name):
        surf, pos = self.layers[name]
        return pygame.Rect(pos, surf.get_size())

    def _sprite(self, name, img, pos, key=None):
        surf = self._surface(key or (name, id(img)), img)
        if name in self.layers:
            self.dirty.append(self._layer_rect(name))
        elif name.startswith("key:"):
            self.key_order.append(name)
        self.layers[name] = (surf, tuple(pos))
        self.dirty.append(self._layer_rect(name))

    def _order(self):
        """Bottom to top: the keys over the jog, the selector over the keys
        around it, the centre display over the jog."""
        return ["body", "lcd", "jog", "cap"] + self.key_order + ["select", "centre"]

    def _redraw(self, rect):
        s = self.surf
        s.set_clip(rect)
        for name in self._order():
            if name in self.layers:
                surf, pos = self.layers[name]
                if rect.colliderect(pygame.Rect(pos, surf.get_size())):
                    s.blit(surf, pos)
        if self.pointer:
            gap, marks = self.pointer
            pygame.draw.polygon(s, (0, 0, 0), gap)
            width = max(1, round(1.6 * self.px))
            for line in marks:
                pygame.draw.line(s, POINTER_RED, line[0], line[1], width)
        s.set_clip(None)

    def compose(self):
        """Bring the surface up to date; the pixel rects that changed."""
        if not self.dirty:
            return []
        bounds = self.surf.get_rect()
        rects = [r.clip(bounds) for r in self.dirty]
        rects = [r for r in rects if r.w and r.h]
        self.dirty = []
        for r in rects:
            self._redraw(r)
        return rects

    def _draw_key(self, name, lit, down):
        if self.lit.get(name, (False, False)) == (lit, down):
            return
        self.lit[name] = (lit, down)
        key = self.keys[name]
        img, pos = self.art.key_sprite(key, lit, down, self.controls[name].look)
        self._sprite("key:" + name, img, pos, key=("key", name, lit, down))

    def _draw_jog(self):
        img, pos = self.art.jog_sprite(self.jog.angle % self.art.JOG_PHASES)
        self._sprite("jog", img, pos)

    def _pointer_rect(self):
        if not self.pointer:
            return None
        gap, marks = self.pointer
        pts = gap + [p for line in marks for p in line]
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        pad = max(2, round(2 * self.px))
        return pygame.Rect(math.floor(min(xs)) - pad, math.floor(min(ys)) - pad,
                           math.ceil(max(xs) - min(xs)) + 2 * pad,
                           math.ceil(max(ys) - min(ys)) + 2 * pad)

    def _draw_centre(self):
        st = self.relay.state(self.tag)
        look = (st.lamp("ring"), st.live)
        if look != self.centre_state:
            self.centre_state = look
            img, pos = self.art.centre_sprite(*look)
            self._sprite("centre", img, pos, key=("centre",) + look)
            self.pointer_step = None
        turns = st.pointer_turns()
        step = -1 if turns is None else round(turns * 135)
        if step == self.pointer_step:
            return
        self.pointer_step = step
        old = self._pointer_rect()
        if old:
            self.dirty.append(old)
        self.pointer = self.art.centre_pointer(step / 135) if step >= 0 else None
        new = self._pointer_rect()
        if new:
            self.dirty.append(new)

    def _place_cap(self):
        x, top, bottom = L.SLIDER
        w, h = self.art.SLIDER_CAP
        y = top + (bottom - top) * self.slider.pos
        surf, _ = self.layers["cap"]
        self.dirty.append(self._layer_rect("cap"))
        self.layers["cap"] = (surf, (round((x - w / 2 - 4) * self.px),
                                     round((y - h / 2 - 4) * self.px)))
        self.dirty.append(self._layer_rect("cap"))

    def add_sink(self, name, view):
        """Show this deck's screen in `view` (a ScreenSlot) as well."""
        self.sinks[name] = view
        self.views_changed()

    def drop_sink(self, name):
        self.sinks.pop(name, None)
        self.views_changed()

    def views_changed(self):
        """Tell the frame source every size, in pixels, the screen is now shown at."""
        if self.lcd_size is None:
            return
        sizes = {"face": self.lcd_size}
        sizes.update({n: v.px_size for n, v in self.sinks.items() if v.px_size})
        self.screen.set_views(sizes)

    # With the screen shown larger elsewhere, the face's small copy of it is
    # refreshed at most this often, leaving the UI thread to the large one.
    FACE_MIRROR_S = 1 / 30

    def _show_lcd(self, views, now):
        face = views.get("face")
        if self.sinks:
            if now - self.face_shown < self.FACE_MIRROR_S:
                face = None
            else:
                self.face_shown = now
        if face is not None and face.size == self.lcd_size:
            _, pos = self.layers["lcd"]
            self.layers["lcd"] = (gfx.surface(face, self.surf), pos)
            self.dirty.append(self._layer_rect("lcd"))
        for name, view in self.sinks.items():
            view.show(views.get(name))

    def snapshot(self):
        """What the face shows now, as a PIL image at the drawn pixel size."""
        self.compose()
        return gfx.image(self.surf)

    # -- the periodic update ------------------------------------------------------

    def _warm_one(self):
        if self.warm:
            job = self.warm.pop(0)
            if job[0] == "jog":
                self.art.jog_sprite(job[1])
                return
            key, lit, down = job
            img, _ = self.art.key_sprite(key, lit, down, self.controls[key.name].look)
            self._surface(("key", key.name, lit, down), img)

    def tick(self, now):
        frame = self.screen.take_frame()
        if frame is not None:
            self._show_lcd(frame, now)
            self.frames_shown.append(now)
        self.frames_shown = [t for t in self.frames_shown if now - t < 2.0]
        st = self.relay.state(self.tag)
        for name, key in self.keys.items():
            lamp = bool(key.lamp) and st.lamp(key.lamp)
            down = self.controls[name].down
            if lamp or down or name in self.lit:
                self._draw_key(name, lamp, down)
        self._draw_centre()
        if frame is None:
            self._warm_one()

    def fps(self):
        return len(self.frames_shown) / 2.0

    def health(self):
        """(name, state in words, whether that is the normal state) for the
        status strip."""
        name = f"Deck {self.number}"
        if not self.screen.connected:
            return name, "Waiting for the screen", False
        if not self.relay.connected:
            return name, "Panel offline", False
        if not self.relay.state(self.tag).live:
            return name, "Starting up", False
        return name, f"Running  ·  {self.fps():.0f} fps", True

    def status(self):
        st = self.relay.state(self.tag)
        scr = self.screen
        screen = (f"screen via {scr.via()} {scr.updates.rate(time.monotonic()):.0f} "
                  f"upd/s, {self.fps():.0f} fps shown" if scr.connected else scr.status)
        lamps = "lamps live" if st.live else "no lamps yet"
        return f"deck {self.number} ({self.tag}): {screen}, {lamps}"

    # -- the mouse, in points from the face's top left ------------------------------

    def units(self, pos):
        return pos[0] / self.scale, pos[1] / self.scale

    def _in_lcd(self, x, y):
        x0, y0, x1, y1 = L.LCD
        return x0 <= x < x1 and y0 <= y < y1

    def _lcd_pixel(self, x, y):
        x0, y0, x1, y1 = L.LCD
        return ((x - x0) * LCD_SIZE[0] / (x1 - x0), (y - y0) * LCD_SIZE[1] / (y1 - y0))

    @staticmethod
    def _polar(x, y, centre):
        cx, cy = centre[0], centre[1]
        return math.hypot(x - cx, y - cy), math.degrees(math.atan2(y - cy, x - cx))

    def down(self, pos):
        x, y = self.units(pos)
        r_jog, a_jog = self._polar(x, y, L.JOG)
        r_sel, a_sel = self._polar(x, y, L.SELECT)
        sx0, sy0, sx1, sy1 = L.SLIDER_FRAME
        if self._in_lcd(x, y):
            self.grab = ("lcd",)
            self.screen.pointer(*self._lcd_pixel(x, y), down=True)
        elif r_sel <= L.SELECT[2] * 0.62:
            self.grab = ("push",)
            self.selector.push.press()
            self._sprite("select", *self.art.select_sprite(self.selector.detents, True))
        elif r_sel <= L.SELECT[2] + 8:
            self.grab = ("select", a_sel, 0.0)
        elif r_jog <= L.JOG[2]:
            self.grab = ("jog", a_jog, time.monotonic())
            self.jog.touch(r_jog <= L.JOG_TOP)
        elif sx0 <= x <= sx1 and sy0 <= y <= sy1:
            self.grab = ("slider",)
            self._slide(y)
        else:
            key = next((k for k in L.KEYS if k.hit(x, y)), None)
            if key and self.controls[key.name].press():
                self.grab = ("key", key.name)
                self._draw_key(key.name, False, True)

    def drag(self, pos):
        if not self.grab:
            return
        x, y = self.units(pos)
        kind = self.grab[0]
        if kind == "lcd":
            self.screen.pointer(*self._lcd_pixel(x, y), down=True)
        elif kind == "jog":
            _, a = self._polar(x, y, L.JOG)
            delta = (a - self.grab[1] + 180) % 360 - 180
            self.grab = ("jog", a, time.monotonic())
            self.jog.turn(delta, time.monotonic())
            self._draw_jog()
        elif kind == "select":
            _, a = self._polar(x, y, L.SELECT)
            acc = self.grab[2] + (a - self.grab[1] + 180) % 360 - 180
            # One detent per 15 degrees of drag, clockwise is down the list.
            n = int(acc / 15)
            self.grab = ("select", a, acc - n * 15)
            if n:
                self._turn_select(n)
        elif kind == "slider":
            self._slide(y)

    def up(self, pos):
        grab, self.grab = self.grab, None
        if not grab:
            return
        kind = grab[0]
        if kind == "lcd":
            x, y = self.units(pos)
            self.screen.pointer(*self._lcd_pixel(x, y), down=False)
        elif kind == "push":
            self.selector.push.release()
            self._sprite("select", *self.art.select_sprite(self.selector.detents, False))
        elif kind == "jog":
            self.jog.touch(False)
            self.jog.release(time.monotonic())
        elif kind == "key":
            self.controls[grab[1]].release()
            self._draw_key(grab[1], self.lit.get(grab[1], (False,))[0], False)

    def _slide(self, y):
        _, top, bottom = L.SLIDER
        self.slider.set((y - top) / (bottom - top))
        self._place_cap()

    def _turn_select(self, n):
        self.selector.turn(n)
        self._sprite("select", *self.art.select_sprite(self.selector.detents, False))

    def wheel(self, pos, sign):
        """One wheel notch at pos: +1 up (away from the user), -1 down."""
        x, y = self.units(pos)
        r_jog, _ = self._polar(x, y, L.JOG)
        if r_jog <= L.JOG[2] and not self._in_lcd(x, y):
            # A wheel notch nudges the platter a few degrees.
            self.jog.nudge(4.0 * sign, time.monotonic())
            self._draw_jog()
        else:
            # Wheel up moves up the list, as the Up arrow does.
            self._turn_select(-sign)

    def hover(self, pos):
        x, y = self.units(pos)
        if self._in_lcd(x, y):
            text = "screen: click to touch; right-click (or F3) opens it in a window"
        elif self._polar(x, y, L.SELECT)[0] <= L.SELECT[2] + 8:
            text = "Rotary selector  ·  wheel or drag to turn, click the centre to push"
        elif self._polar(x, y, L.JOG)[0] <= L.JOG[2]:
            text = ("Jog  ·  drag round to bend, the top plate is touch-sensitive  "
                    "·  wheel to nudge")
        elif L.SLIDER_FRAME[0] <= x <= L.SLIDER_FRAME[2] and \
                L.SLIDER_FRAME[1] <= y <= L.SLIDER_FRAME[3]:
            text = "Tempo  ·  drag to move the slider"
        else:
            key = next((k for k in L.KEYS if k.hit(x, y)), None)
            text = C.describe(key) if key else ""
        self.on_hover(self, text)

    def popup(self, pos):
        x, y = self.units(pos)
        if self._in_lcd(x, y) and self.on_popup:
            self.on_popup(self)

