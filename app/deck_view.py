# SPDX-License-Identifier: GPL-2.0-or-later
"""One deck on screen: the drawn face, the live LCD, the mouse on its controls.

A Tk canvas holds the face (art.py) as one image, with sprites on top for
whatever changes. Input takes two routes, one per kind of thing:

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
import tkinter as tk

from PIL import Image, ImageDraw, ImageTk

import art as ART
import controls as C
import layout as L

LCD_SIZE = (800, 480)
# Right-click (Button-2 on macOS) on the screen opens it in its own window.
POPUP_BUTTONS = ("<Button-3>", "<Button-2>")


class DeckView:
    """One deck: canvas, sprites and the controls behind them."""

    def __init__(self, master, number, tag, relay, screen, scale, on_hover,
                 on_popup=None):
        self.number = number
        self.tag = tag
        self.relay = relay
        self.screen = screen
        self.on_hover = on_hover
        self.canvas = tk.Canvas(master, highlightthickness=0, bd=0, bg="#050505",
                                cursor="arrow")
        self.keys = {k.name: k for k in L.KEYS}
        self.controls = {k.name: C.KeyControl(k.action, self.send) for k in L.KEYS}
        self.jog = C.Jog(self.send)
        self.jog.start()
        self.selector = C.Selector(self.send)
        self.slider = C.TempoSlider(self.send)
        self.grab = None                # what the mouse button is holding
        self.lit = {}                   # key name -> (lit, down) now drawn
        self.photos = {}                # sprite key -> PhotoImage, kept alive
        self.items = {}
        self.item_imgs = {}             # canvas item -> the PIL image it shows
        self.frames_shown = []
        self.sinks = {}                 # name -> ScreenSlot showing this screen too
        self.face_shown = 0.0
        self.on_popup = on_popup
        self.lcd_size = None
        self._bind()
        self.rescale(scale)

    def send(self, datagram):
        """One panel datagram to this deck, through the relay."""
        self.relay.send(self.tag, datagram)

    # -- drawing --------------------------------------------------------------

    def rescale(self, scale):
        self.scale = scale
        self.art = ART.make_art(scale, f"DECK {self.number}")
        self.photos.clear()
        self.lit.clear()
        self.canvas.delete("all")
        self.items = {}
        w, h = self.art.size
        self.canvas.config(width=w, height=h)
        body = self.art.body([(k, self.controls[k.name].look) for k in L.KEYS])
        self.items["body"] = self.canvas.create_image(0, 0, anchor="nw",
                                                      image=self._photo("body", body))
        self.item_imgs = {self.items["body"]: body}
        x0, y0, x1, y1 = [round(v * scale) for v in L.LCD]
        self.lcd_size = (x1 - x0, y1 - y0)
        self.views_changed()
        self.lcd_photo = ImageTk.PhotoImage(Image.new("RGB", self.lcd_size))
        self.items["lcd"] = self.canvas.create_image(x0, y0, anchor="nw",
                                                     image=self.lcd_photo)
        self._sprite("jog", *self.art.jog_sprite(0))
        self._sprite("select", *self.art.select_sprite(0, False))
        self.centre_state = None
        self._draw_centre()
        cap = self.art.slider_cap()
        self.items["cap"] = self.canvas.create_image(0, 0, anchor="nw",
                                                     image=self._photo("cap", cap))
        self.item_imgs[self.items["cap"]] = cap
        self._place_cap()
        # Draw the lamps' lit looks ahead, a few per tick, so the first blink
        # of a lamp does not stall the screen while its sprite is made.
        self.warm = [(k, lit, down) for k in L.KEYS if k.lamp or self.controls[k.name].live
                     for lit, down in ((True, False), (False, True), (True, True))]
        # And the jog's rotation phases, drawn the first time it turns otherwise.
        self.warm += [("jog", phase) for phase in range(1, self.art.JOG_PHASES)]

    def _photo(self, key, img):
        if key not in self.photos:
            self.photos[key] = ImageTk.PhotoImage(img)
        return self.photos[key]

    def _sprite(self, name, img, pos, key=None):
        photo = self._photo(key or (name, id(img)), img)
        if name in self.items:
            self.canvas.itemconfig(self.items[name], image=photo)
        else:
            self.items[name] = self.canvas.create_image(pos[0], pos[1], anchor="nw",
                                                        image=photo)
        self.item_imgs[self.items[name]] = img

    def _draw_key(self, name, lit, down):
        if self.lit.get(name, (False, False)) == (lit, down):
            return
        self.lit[name] = (lit, down)
        key = self.keys[name]
        img, pos = self.art.key_sprite(key, lit, down, self.controls[name].look)
        self._sprite("key:" + name, img, pos, key=("key", name, lit, down))
        # The selector sits over the keys around it.
        if "select" in self.items:
            self.canvas.tag_raise(self.items["select"])

    def _draw_jog(self):
        img, pos = self.art.jog_sprite(self.jog.angle % self.art.JOG_PHASES)
        self._sprite("jog", img, pos)
        for name in ("centre", "gap"):
            self.canvas.tag_raise(self.items[name])
        for item in self.items["marks"]:
            self.canvas.tag_raise(item)

    def _draw_centre(self):
        st = self.relay.state(self.tag)
        look = (st.lamp("ring"), st.live)
        if look != self.centre_state:
            self.centre_state = look
            img, pos = self.art.centre_sprite(*look)
            self._sprite("centre", img, pos, key=("centre",) + look)
            if "gap" not in self.items:
                self.items["gap"] = self.canvas.create_polygon(0, 0, 0, 0, fill="#000000",
                                                               outline="")
                self.items["marks"] = [self.canvas.create_line(0, 0, 0, 0, fill="#ff2a20",
                                                               width=max(1, round(1.6 * self.scale)))
                                       for _ in range(2)]
            for item in [self.items["gap"]] + self.items["marks"]:
                self.canvas.tag_raise(item)
            self.pointer_step = -1
        turns = st.pointer_turns()
        step = -1 if turns is None else round(turns * 135)
        if step == self.pointer_step:
            return
        self.pointer_step = step
        state = "hidden" if step < 0 else "normal"
        for item in [self.items["gap"]] + self.items["marks"]:
            self.canvas.itemconfig(item, state=state)
        if step >= 0:
            gap, marks = self.art.centre_pointer(step / 135)
            self.canvas.coords(self.items["gap"], *[v for pt in gap for v in pt])
            for item, line in zip(self.items["marks"], marks):
                self.canvas.coords(item, *[v for pt in line for v in pt])

    def _place_cap(self):
        x, top, bottom = L.SLIDER
        w, h = self.art.SLIDER_CAP
        y = top + (bottom - top) * self.slider.pos
        self.canvas.coords(self.items["cap"], round((x - w / 2 - 4) * self.scale),
                           round((y - h / 2 - 4) * self.scale))

    def add_sink(self, name, view):
        """Show this deck's screen in `view` (a ScreenSlot) as well."""
        self.sinks[name] = view
        self.views_changed()

    def drop_sink(self, name):
        self.sinks.pop(name, None)
        self.views_changed()

    def views_changed(self):
        """Tell the frame source every size the screen is now shown at."""
        if self.lcd_size is None:
            return
        sizes = {"face": self.lcd_size}
        sizes.update({n: v.size for n, v in self.sinks.items() if v.size})
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
            self.lcd_photo.paste(face)
            self.item_imgs[self.items["lcd"]] = face
        for name, view in self.sinks.items():
            view.show(views.get(name))

    def snapshot(self):
        """What the canvas shows, as one image, composed from its layers."""
        out = Image.new("RGBA", self.art.size, (5, 5, 5, 255))
        draw = ImageDraw.Draw(out)
        for item in self.canvas.find_all():
            if self.canvas.itemcget(item, "state") == "hidden":
                continue
            kind = self.canvas.type(item)
            xy = self.canvas.coords(item)
            if kind == "image" and item in self.item_imgs:
                out.alpha_composite(self.item_imgs[item].convert("RGBA"),
                                    (max(0, round(xy[0])), max(0, round(xy[1]))))
            elif kind == "polygon":
                draw.polygon(xy, fill=self.canvas.itemcget(item, "fill"))
            elif kind == "line":
                draw.line(xy, fill=self.canvas.itemcget(item, "fill"),
                          width=round(float(self.canvas.itemcget(item, "width"))))
        return out.convert("RGB")

    # -- the periodic update ------------------------------------------------------

    def _warm_one(self):
        if self.warm:
            job = self.warm.pop(0)
            if job[0] == "jog":
                self.art.jog_sprite(job[1])
                return
            key, lit, down = job
            img, _ = self.art.key_sprite(key, lit, down, self.controls[key.name].look)
            self._photo(("key", key.name, lit, down), img)

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

    # -- the mouse ------------------------------------------------------------

    def _bind(self):
        c = self.canvas
        c.bind("<ButtonPress-1>", self._down)
        c.bind("<B1-Motion>", self._drag)
        c.bind("<ButtonRelease-1>", self._up)
        c.bind("<Motion>", self._hover)
        c.bind("<MouseWheel>", self._wheel)
        c.bind("<Button-4>", lambda e: self._wheel(e, 1))
        c.bind("<Button-5>", lambda e: self._wheel(e, -1))
        for b in POPUP_BUTTONS:
            c.bind(b, self._popup)

    def units(self, e):
        return e.x / self.scale, e.y / self.scale

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

    def _down(self, e):
        x, y = self.units(e)
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

    def _drag(self, e):
        if not self.grab:
            return
        x, y = self.units(e)
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

    def _up(self, e):
        grab, self.grab = self.grab, None
        if not grab:
            return
        kind = grab[0]
        if kind == "lcd":
            x, y = self.units(e)
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

    def _wheel(self, e, sign=None):
        if sign is None:
            sign = 1 if e.delta > 0 else -1
        x, y = self.units(e)
        r_jog, _ = self._polar(x, y, L.JOG)
        if r_jog <= L.JOG[2] and not self._in_lcd(x, y):
            # A wheel notch nudges the platter a few degrees.
            self.jog.nudge(4.0 * sign, time.monotonic())
            self._draw_jog()
        else:
            # Wheel up moves up the list, as the Up arrow does.
            self._turn_select(-sign)

    def _hover(self, e):
        x, y = self.units(e)
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

    def _popup(self, e):
        x, y = self.units(e)
        if self._in_lcd(x, y) and self.on_popup:
            self.on_popup(self)
