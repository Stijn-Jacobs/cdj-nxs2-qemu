#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The virtual deck: the emulated player drawn as a player, in one window.

    python app/virtual_deck.py [--decks 2] [--relay 127.0.0.1:7202]

`./start.sh --app` starts the decks with their screens on VNC and runs this.
Each deck N is the machine tagged <prefix>N (show1, show2), with its screen on
VNC port <vnc-base>+N and its panel through the relay. The decks need not be
up yet: the window shows them as they arrive.

Keys typed into the window go to the deck under the mouse (the last one it
was over), through the same key map as the QEMU window: Space play/pause,
arrows browse, Enter load, - = nudge ... (emulator/README.md, "Keyboard").

The window is pygame's (SDL's). It is laid out in points, as the desktop
measures windows, and drawn in the display's own pixels -- two a point on a
Retina screen -- so the face is as sharp as the display.
"""

import argparse
import os
import sys
import time

# Before pygame loads SDL: Windows then reports sizes in points too, and
# gives the window the display's real pixels instead of stretching it.
os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
os.environ.setdefault("SDL_WINDOWS_DPI_AWARENESS", "permonitorv2")
os.environ.setdefault("SDL_WINDOWS_DPI_SCALING", "1")

import pygame  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gfx  # noqa: E402
import layout as L  # noqa: E402
from deck_view import DeckView  # noqa: E402
from frames import FrameFile, Screen  # noqa: E402
from screen_view import LCD_H, LCD_W, DockView, ScreenWindow  # noqa: E402
from relay_link import RelayLink  # noqa: E402
from rfb import RfbClient  # noqa: E402
from status_bar import StatusBar  # noqa: E402

# A new frame is on screen within one tick. On Windows the sleep runs at the
# system tick (15.6 ms) unless the process asks for 1 ms (fine_timer below).
TICK_MS = 4
BG = (5, 5, 5)
STATUS_S = 0.5
# A resize redraws once the window has held still this long. Redrawing is a
# downscale of cached images (art.py, ~50 ms), so this can be short.
RESCALE_S = 0.06
# What the window's frame takes, for the first layout.
TITLE_BAR = 32
TITLE = "NXS2 Virtual Deck"


def work_area():
    """(x, y, w, h) in points: the part of the screen a window may use,
    without the taskbar where the system says where it is."""
    if sys.platform == "win32":
        try:
            import ctypes
            from ctypes import wintypes
            r = wintypes.RECT()
            if ctypes.windll.user32.SystemParametersInfoW(0x0030, 0, ctypes.byref(r), 0):
                # In pixels; SDL's window sizes are in points (the DPI scaling hint).
                scale = ctypes.windll.user32.GetDpiForSystem() / 96.0
                return (round(r.left / scale), round(r.top / scale),
                        round((r.right - r.left) / scale), round((r.bottom - r.top) / scale))
        except (OSError, AttributeError):
            pass
    # Elsewhere SDL cannot see the menu bar or the dock; leave room for them.
    w, h = pygame.display.get_desktop_sizes()[0]
    return 0, 0, w, h - 90


def fine_timer():
    """Ask Windows for 1 ms timers while the window is up, or every frame
    waits for the next 15.6 ms tick and the screen tops out near 60 fps."""
    if sys.platform == "win32":
        try:
            import ctypes
            ctypes.windll.winmm.timeBeginPeriod(1)
        except (OSError, AttributeError):
            pass


class App:
    GAP = 6                             # points between faces and screens

    def __init__(self, args):
        fine_timer()
        pygame.display.init()
        self.args = args
        self.window = pygame.Window(TITLE, (640, 480), allow_high_dpi=True,
                                    resizable=True, hidden=True)
        host, _, port = args.relay.partition(":")
        self.relay = RelayLink(host, int(port))
        self.decks = []
        self.focus = None
        self.hover_text = ""
        self.windows = {}               # deck number -> ScreenWindow
        self.stats_next = time.monotonic() + args.stats if args.stats else None
        self.running = True
        self.grab = None                # the view holding the mouse button
        self.mouse = (0, 0)             # the last mouse position, points
        self.wheel_acc = 0.0
        self.rescale_at = None
        self.set_size = None            # the size this code last gave the window
        self.redraw = True

        self.dock = DockView()
        for n in range(1, args.decks + 1):
            tag = f"{args.prefix}{n}"
            frames = (FrameFile(os.path.join(args.frame_dir, f"cdj-lcd-{tag}.bin"))
                      if args.frame_dir else None)
            screen = Screen(RfbClient(args.vnc_host, args.vnc_base + n, f"deck{n}"),
                            frames)
            self.decks.append(DeckView(n, tag, self.relay, screen, self._hover,
                                       self.toggle_window))
        self.focus = self.decks[0]
        self.status = StatusBar()
        self.views = []                 # (view, origin in points), left to right

        # The first size: most of the work area, the frame and status included.
        wx, wy, ww, wh = work_area()
        self.room = (int(ww * 0.94), int(wh * 0.94) - TITLE_BAR - StatusBar.H)
        self.mode = args.screen
        if self.mode == "auto":
            self.mode = "dock" if self._dock_fits(*self.room) else "face"
        self.ratio = self._ratio()
        self.arrange()
        w, h = self.content_size
        self._resize_window((w, h + StatusBar.H))
        self.window.position = (wx + max(0, (ww - w) // 2),
                                wy + max(0, (wh - TITLE_BAR - h - StatusBar.H) // 2))
        if not args.hidden:
            self.window.show()
        # Keys are keys here, not text: no input method, no accent pop-up.
        pygame.key.stop_text_input()
        if args.screen == "window":
            for d in self.decks:
                self.toggle_window(d)

        self.relay.start()
        for d in self.decks:
            d.screen.start()

    # -- layout ---------------------------------------------------------------
    #
    # The NXS2's screen is small on a tall deck, so a face that fits the
    # monitor's height shows it small. "dock" puts each deck's screen beside
    # the faces as well, as large as the height allows; "face" is the faces
    # alone; either way a screen can also have a window of its own (F3).

    # The docked screen at its own 800 x 480 points at most: sharp (each deck
    # pixel is whole display pixels) and no larger than the deck means it.
    SCREEN_MAX = 1.0
    FACE_MIN = 0.42                     # below this the face's print is unreadable

    def _scales(self, w, h):
        """(face scale, docked screen scale) for a w x h content area."""
        n = len(self.decks)
        gaps = self.GAP * (n - 1)
        if self.args.scale and self.mode != "dock":
            return self.args.scale, 0
        if self.mode != "dock":
            s = min(h / L.H, (w - gaps) / (n * L.W))
            return max(0.3, min(1.6, s)), 0
        # The dock's bezels and padding are in face units, so they scale with
        # s; its screens get whatever height the faces leave them.
        ew, eh, e1 = DockView.overhead()
        s = h / L.H
        z = min(self.SCREEN_MAX, (h - s * (n * eh + e1)) / (n * LCD_H))
        if n * L.W * s + LCD_W * z + ew * s + self.GAP * n > w:
            s = (w - LCD_W * z - self.GAP * n) / (n * L.W + ew)
        if s < self.FACE_MIN:
            s = self.FACE_MIN
            z = (w - (n * L.W + ew) * s - self.GAP * n) / LCD_W
        if self.args.scale:
            s = self.args.scale
        return max(0.3, s), max(0.25, z)

    def _dock_fits(self, w, h):
        mode, self.mode = self.mode, "dock"
        s, z = self._scales(w, h)
        self.mode = mode
        return z >= 0.8 and s >= self.FACE_MIN

    def _ratio(self):
        """Display pixels a point: 2 on a Retina screen, 1 on most others."""
        return self.window.get_surface().get_width() / max(1, self.window.size[0])

    def arrange(self, room=None):
        """Lay the faces (and the dock) out to fill `room` (w, h points)."""
        w, h = room or self.room
        s, z = self._scales(w, h)
        for d in self.decks:
            if d.surf is None or abs(d.scale - s) > 0.005 or d.ratio != self.ratio:
                d.rescale(s, self.ratio)
        if self.mode == "dock":
            self.dock.place(self.decks, z, s, self.ratio)
            # One deck: face | screen. Two: face | screens | face.
            order = [self.decks[0], self.dock] + self.decks[1:]
        else:
            self.dock.clear(self.decks)
            order = self.decks
        self.views, x = [], 0
        for i, view in enumerate(order):
            if i:
                x += self.GAP
            self.views.append((view, (x, 0)))
            x += view.size[0]
        self.content_size = (x, max(v.size[1] for v, _ in self.views))
        self.redraw = True

    def _resize_window(self, size):
        self.set_size = tuple(size)
        self.window.size = size

    def toggle_mode(self):
        """Dock or undock the screens, the window growing or shrinking to the
        new layout: laid out for the room the first window had."""
        self.mode = "face" if self.mode == "dock" else "dock"
        self.arrange(self.room)
        w, h = self.content_size
        self._resize_window((w, h + StatusBar.H))

    def toggle_window(self, deck):
        """Open this deck's screen in a window of its own, or close it."""
        win = self.windows.pop(deck.number, None)
        if win:
            win.close()
            return
        win = ScreenWindow(deck, self._window_closed)
        self.windows[deck.number] = win
        deck.add_sink("window", win.view)

    def _window_closed(self, win):
        self.windows.pop(win.deck.number, None)
        win.deck.drop_sink("window")

    def _content(self):
        w, h = self.window.size
        return w, h - StatusBar.H

    def _left(self):
        """Where the row of views starts: centred in the window."""
        return max(0, (self.window.size[0] - self.content_size[0]) // 2)

    # -- drawing ------------------------------------------------------------------

    def _present(self, status_due=False):
        """Put what changed on screen: the views' changed parts, and the status
        strip when it is due (every STATUS_S) and says something new."""
        surf = self.window.get_surface()
        r = self.ratio
        left = self._left()
        full, self.redraw = self.redraw, False
        flip = full
        if full:
            surf.fill(BG)
        for view, (x, y) in self.views:
            ox, oy = round((left + x) * r), round(y * r)
            rects = view.compose()
            if full:
                surf.blit(view.surf, (ox, oy))
            for rect in [] if full else rects:
                surf.blit(view.surf, rect.move(ox, oy), area=rect)
                flip = True
        if status_due or full:
            w, h = self.window.size
            new = self.status.show(w, r, [d.health() for d in self.decks], self.hover_text)
            if (new or full) and self.status.surf is not None:
                surf.blit(self.status.surf, (0, round((h - StatusBar.H) * r)))
                flip = True
        if flip:
            self.window.flip()

    # -- the loop ---------------------------------------------------------------

    def run(self):
        status_next = 0.0
        while self.running:
            for e in pygame.event.get():
                self._event(e)
            if not self.running:
                break
            now = time.monotonic()
            if self.rescale_at and now >= self.rescale_at:
                self.rescale_at = None
                self.arrange(self._content())
            ratio = self._ratio()
            if ratio != self.ratio:     # moved to a display of another density
                self.ratio = ratio
                self.arrange(self._content())
            for d in self.decks:
                d.tick(now)
            if self.stats_next and now >= self.stats_next:
                self.stats_next = now + self.args.stats
                print(" | ".join(d.status() for d in self.decks), flush=True)
            status_due = now >= status_next
            if status_due:
                status_next = now + STATUS_S
            self._present(status_due)
            for win in list(self.windows.values()):
                win.present()
            pygame.time.wait(TICK_MS)
        self.close()

    # -- events -------------------------------------------------------------------

    def _screen_window(self, e):
        wid = getattr(getattr(e, "window", None), "id", None)
        return next((w for w in self.windows.values() if w.id == wid), None)

    def _is_main(self, e):
        win = getattr(e, "window", None)
        return win is None or win.id == self.window.id

    def _view_at(self, pos):
        """(view, pos relative to it) under pos (window points), or (None, None)."""
        left = self._left()
        for view, (x, y) in self.views:
            vx, vy = pos[0] - left - x, pos[1] - y
            if 0 <= vx < view.size[0] and 0 <= vy < view.size[1]:
                return view, (vx, vy)
        return None, None

    def _local(self, view, pos):
        left = self._left()
        for v, (x, y) in self.views:
            if v is view:
                return pos[0] - left - x, pos[1] - y
        return pos

    def _event(self, e):
        t = e.type
        if t == pygame.QUIT:
            self.running = False
        elif t == pygame.WINDOWCLOSE:
            win = self._screen_window(e)
            if win:
                win.close()
            elif self._is_main(e):
                self.running = False
        elif t in (pygame.WINDOWSIZECHANGED, pygame.WINDOWRESIZED):
            win = self._screen_window(e)
            if win:
                win.resized()
            elif self._is_main(e):
                self.redraw = True
                if tuple(self.window.size) != self.set_size:
                    self.set_size = None
                    self.rescale_at = time.monotonic() + RESCALE_S
        elif t in (pygame.WINDOWEXPOSED, pygame.WINDOWDISPLAYCHANGED):
            win = self._screen_window(e)
            if win:
                win.resized()
            else:
                self.redraw = True
        elif t in (pygame.MOUSEBUTTONDOWN, pygame.MOUSEBUTTONUP, pygame.MOUSEMOTION):
            self._mouse(e)
        elif t == pygame.MOUSEWHEEL:
            self._wheel(e)
        elif t in (pygame.KEYDOWN, pygame.KEYUP):
            self._key(e, t == pygame.KEYDOWN)

    def _mouse(self, e):
        win = self._screen_window(e)
        if win:
            self.focus = win.deck
            if e.type == pygame.MOUSEBUTTONDOWN and e.button == 1:
                win.down(e.pos)
            elif e.type == pygame.MOUSEBUTTONUP and e.button == 1:
                win.up(e.pos)
            elif e.type == pygame.MOUSEMOTION:
                if e.buttons[0]:
                    win.drag(e.pos)
                else:
                    win.hover(e.pos)
            return
        if not self._is_main(e):
            return
        self.mouse = e.pos
        if e.type == pygame.MOUSEBUTTONDOWN:
            view, pos = self._view_at(e.pos)
            if view is None:
                return
            if e.button == 1:
                self.grab = view
                view.down(pos)
            elif e.button in (2, 3):
                view.popup(pos)
        elif e.type == pygame.MOUSEBUTTONUP:
            if e.button == 1 and self.grab:
                grab, self.grab = self.grab, None
                grab.up(self._local(grab, e.pos))
        elif self.grab:
            self.grab.drag(self._local(self.grab, e.pos))
        else:
            view, pos = self._view_at(e.pos)
            if view is not None:
                view.hover(pos)
            elif self.hover_text:
                self.hover_text = ""

    def _wheel(self, e):
        # A mouse wheel gives whole notches; a trackpad gives fractions of one,
        # which add up to a notch as they come.
        self.wheel_acc += getattr(e, "precise_y", e.y) or e.y
        steps = int(self.wheel_acc)
        if not steps:
            return
        self.wheel_acc -= steps
        if getattr(e, "flipped", False):
            steps = -steps
        view, pos = self._view_at(self.mouse)
        if view is None:
            return
        sign = 1 if steps > 0 else -1
        for _ in range(abs(steps)):
            view.wheel(pos, sign)

    def _hover(self, deck, text):
        self.focus = deck
        self.hover_text = text

    # -- the keyboard, over VNC to the focused deck ------------------------------

    # The app's own keys; everything else goes to the deck.
    APP_KEYS = {pygame.K_F2: "toggle_mode", pygame.K_F3: "window_for_focus"}

    def window_for_focus(self):
        self.toggle_window(self.focus)

    def _key(self, e, down):
        win = self._screen_window(e)
        if win and e.key == pygame.K_F11:
            if down:
                win.fullscreen()
            return
        if e.key in self.APP_KEYS:
            if down:
                getattr(self, self.APP_KEYS[e.key])()
            return
        sym = gfx.keysym(e.key)
        if sym is not None:
            self.focus.screen.key(sym, down)

    def snapshot(self):
        """The window as it is drawn, faces, docked screens and status strip,
        at the display's pixels."""
        self.redraw = True
        self._present()
        return gfx.image(self.window.get_surface())

    def close(self):
        for d in self.decks:
            d.jog.stop()
            d.screen.close()
        self.relay.close()
        for win in list(self.windows.values()):
            win.close()
        pygame.quit()


def parse_args(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--decks", type=int, default=int(os.environ.get("CDJ_DECKS", "1")),
                    choices=(1, 2), help="how many decks (env CDJ_DECKS)")
    ap.add_argument("--prefix", default=os.environ.get("CDJ_NAME", "show"),
                    help="machine tag prefix; deck N is <prefix>N (env CDJ_NAME)")
    ap.add_argument("--relay",
                    default=f"127.0.0.1:{os.environ.get('RELAY_PORT', '7202')}",
                    help="host:port of scripts/run/midi_relay.py (env RELAY_PORT)")
    ap.add_argument("--vnc-host", default="127.0.0.1")
    ap.add_argument("--vnc-base", type=int,
                    default=int(os.environ.get("CDJ_APP_VNC_BASE", "5920")),
                    help="deck N's screen is on VNC port <base>+N "
                         "(env CDJ_APP_VNC_BASE)")
    ap.add_argument("--frame-dir", default=os.environ.get("CDJ_APP_FRAME_DIR", ""),
                    help="where the display boards write cdj-lcd-<tag>.bin "
                         "(GUI_DISPLAY=vnc; env CDJ_APP_FRAME_DIR); without it "
                         "the screen comes over VNC, at up to 33 frames a second")
    ap.add_argument("--screen", default=os.environ.get("CDJ_APP_SCREEN", "face"),
                    choices=("face", "dock", "window", "auto"),
                    help="face (default): the decks alone, each with its screen in "
                         "it; dock: each deck's screen large beside the faces as "
                         "well; window: the screens in windows of their own; auto: "
                         "dock when the monitor has room (env CDJ_APP_SCREEN; F2 "
                         "and F3 switch while running)")
    ap.add_argument("--scale", type=float, default=0,
                    help="face scale in points a face unit, 1.0 = 972 x 1252 points "
                         "a deck (default: fit)")
    ap.add_argument("--stats", type=float, default=0, metavar="S",
                    help="print each deck's frame rate every S seconds")
    # Draw without showing the window: tests render it and read snapshot().
    ap.add_argument("--hidden", action="store_true", help=argparse.SUPPRESS)
    return ap.parse_args(argv)


def main(argv=None):
    App(parse_args(argv)).run()


if __name__ == "__main__":
    main()
