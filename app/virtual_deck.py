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
"""

import argparse
import os
import sys
import time
import tkinter as tk

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import layout as L  # noqa: E402
from deck_view import DeckView  # noqa: E402
from frames import FrameFile, Screen  # noqa: E402
from screen_view import LCD_H, LCD_W, ScreenView, ScreenWindow  # noqa: E402
from relay_link import RelayLink  # noqa: E402
from rfb import RfbClient  # noqa: E402

# A new frame is on screen within one tick. On Windows Tk's timer runs at the
# system tick (15.6 ms) unless the process asks for 1 ms (fine_timer below).
TICK_MS = 4
BG = "#050505"
BG_RGB = (5, 5, 5)
STATUS_MS = 500
RESCALE_MS = 350
# The key release Tk reports for host auto-repeat comes a moment before the
# next press; a release only goes out if no press follows within this.
RELEASE_GRACE_MS = 40


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
    GAP = 6                             # pixels between faces and screens

    def __init__(self, args):
        fine_timer()
        self.args = args
        self.root = tk.Tk()
        self.root.title("NXS2 virtual deck")
        self.root.configure(bg=BG)
        host, _, port = args.relay.partition(":")
        self.relay = RelayLink(host, int(port))
        self.decks = []
        self.focus = None
        self.hover_text = ""
        self.pending_release = {}
        self.windows = {}               # deck number -> ScreenWindow
        self.stats_next = time.monotonic() + args.stats if args.stats else None

        self.row = tk.Frame(self.root, bg=BG)
        self.row.pack(side="top")
        self.dock = tk.Frame(self.row, bg=BG)
        self.docked = {}                # deck number -> ScreenView in the dock
        for n in range(1, args.decks + 1):
            tag = f"{args.prefix}{n}"
            frames = (FrameFile(os.path.join(args.frame_dir, f"cdj-lcd-{tag}.bin"))
                      if args.frame_dir else None)
            screen = Screen(RfbClient(args.vnc_host, args.vnc_base + n, f"deck{n}"),
                            frames)
            deck = DeckView(self.row, n, tag, self.relay, screen, 0.5, self._hover,
                            self.toggle_window)
            deck.canvas.bind("<Enter>", lambda e, d=deck: self._set_focus(d), add="+")
            self.decks.append(deck)
        self.focus = self.decks[0]
        self.status = tk.Label(self.root, anchor="w", justify="left", bg="#111214",
                               fg="#b8bcc4", font=("TkDefaultFont", 9), padx=8)
        self.status.pack(side="bottom", fill="x")

        sw, sh = self.root.winfo_screenwidth(), self.root.winfo_screenheight()
        self.room = (sw - 40, sh - 120)     # the window's content, at first
        self.mode = args.screen
        if self.mode == "auto":
            self.mode = "dock" if self._dock_fits(*self.room) else "face"
        self.arrange()
        if args.screen == "window":
            for d in self.decks:
                self.toggle_window(d)

        # bind_all: a screen's own window types into its deck too.
        self.root.bind_all("<KeyPress>", self._key_down)
        self.root.bind_all("<KeyRelease>", self._key_up)
        self.root.bind("<Configure>", self._resized)
        self.root.protocol("WM_DELETE_WINDOW", self.close)
        self.size = None
        self.rescale_job = None

        self.relay.start()
        for d in self.decks:
            d.screen.start()
        self.root.after(TICK_MS, self._tick)
        self.root.after(STATUS_MS, self._status)

    # -- layout ---------------------------------------------------------------
    #
    # The NXS2's screen is small on a tall deck, so a face that fits the
    # monitor's height shows it small. "dock" puts each deck's screen beside
    # the faces as well, as large as the height allows; "face" is the faces
    # alone; either way a screen can also have a window of its own (F3).

    # The docked screen at its own 800 x 480 at most: a pixel for a pixel is
    # both the sharpest and the cheapest (no scaling per frame).
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
        label = ScreenView.LABEL_H
        z = min(self.SCREEN_MAX, (h - n * label - self.GAP * (n - 1)) / (n * LCD_H))
        s = min(h / L.H, (w - LCD_W * z - self.GAP * n) / (n * L.W))
        if s < self.FACE_MIN:
            s = self.FACE_MIN
            z = (w - n * L.W * s - self.GAP * n) / LCD_W
        if self.args.scale:
            s = self.args.scale
        return max(0.3, s), max(0.25, z)

    def _dock_fits(self, w, h):
        mode, self.mode = self.mode, "dock"
        s, z = self._scales(w, h)
        self.mode = mode
        return z >= 0.8 and s >= self.FACE_MIN

    def arrange(self, room=None):
        """Lay the faces (and the dock) out to fill `room` (w, h)."""
        w, h = room or self.room
        s, z = self._scales(w, h)
        for widget in self.row.winfo_children():
            widget.pack_forget()
        faces = [d.canvas for d in self.decks]
        if self.mode == "dock":
            size = (round(LCD_W * z), round(LCD_H * z))
            for d in self.decks:
                view = self.docked.get(d.number)
                if view is None:
                    view = ScreenView(self.dock, d, size, f"DECK {d.number}")
                    self.docked[d.number] = view
                    view.canvas.pack(side="top", pady=(0 if d.number == 1 else self.GAP, 0))
                    d.add_sink("dock", view)
                view.resize(size)
            # One deck: face | screen. Two: face | screens | face.
            order = [faces[0], self.dock] + faces[1:]
        else:
            for d in self.decks:
                if d.number in self.docked:
                    d.drop_sink("dock")
                    self.docked.pop(d.number).canvas.destroy()
            order = faces
        for i, widget in enumerate(order):
            widget.pack(side="left", anchor="n", padx=(0 if i == 0 else self.GAP, 0))
        for d in self.decks:
            if abs(d.scale - s) > 0.005:
                d.rescale(s)

    def toggle_mode(self):
        self.mode = "face" if self.mode == "dock" else "dock"
        self.arrange(self._content())

    def toggle_window(self, deck):
        """Open this deck's screen in a window of its own, or close it."""
        win = self.windows.pop(deck.number, None)
        if win:
            win.close()
            return
        win = ScreenWindow(self.root, deck, self._window_closed)
        self.windows[deck.number] = win
        deck.add_sink("window", win.view)

    def _window_closed(self, win):
        self.windows.pop(win.deck.number, None)
        win.deck.drop_sink("window")

    def _content(self):
        return (self.root.winfo_width(),
                self.root.winfo_height() - self.status.winfo_height())

    def _resized(self, e):
        if e.widget is not self.root:
            return
        size = (e.width, e.height)
        if self.size is None:
            self.size = size
            return
        if size == self.size:
            return
        self.size = size
        if self.rescale_job:
            self.root.after_cancel(self.rescale_job)
        self.rescale_job = self.root.after(RESCALE_MS, self._rescale)

    def _rescale(self):
        self.rescale_job = None
        self.arrange(self._content())

    # -- the loop ---------------------------------------------------------------

    def _tick(self):
        now = time.monotonic()
        for d in self.decks:
            d.tick(now)
        if self.stats_next and now >= self.stats_next:
            self.stats_next = now + self.args.stats
            print(" | ".join(d.status() for d in self.decks), flush=True)
        self.root.after(TICK_MS, self._tick)

    def _status(self):
        parts = [d.status() for d in self.decks] + [self.relay.status]
        line = "   ·   ".join(parts)
        if self.hover_text:
            line += "\n" + self.hover_text
        else:
            line += (f"\nkeys go to deck {self.focus.number} · hover a control for what "
                     "it does · F2: screen beside the deck on/off · F3 or right-click "
                     "the screen: screen in its own window")
        self.status.config(text=line)
        self.root.after(STATUS_MS, self._status)

    def _hover(self, deck, text):
        self._set_focus(deck)
        self.hover_text = text

    def _set_focus(self, deck):
        self.focus = deck

    # -- the keyboard, over VNC to the focused deck ------------------------------

    # The app's own keys; everything else goes to the deck.
    APP_KEYS = {"F2": "toggle_mode", "F3": "window_for_focus"}

    def window_for_focus(self):
        self.toggle_window(self.focus)

    def _key_down(self, e):
        if e.keysym in self.APP_KEYS:
            getattr(self, self.APP_KEYS[e.keysym])()
            return
        job = self.pending_release.pop(e.keysym_num, None)
        if job:
            self.root.after_cancel(job)
            return                      # auto-repeat: the key never went up
        self.focus.screen.key(e.keysym_num, True)

    def _key_up(self, e):
        if e.keysym in self.APP_KEYS:
            return
        deck, sym = self.focus, e.keysym_num

        def send():
            self.pending_release.pop(sym, None)
            deck.screen.key(sym, False)
        self.pending_release[sym] = self.root.after(RELEASE_GRACE_MS, send)

    def snapshot(self):
        """The window's faces (and docked screens) side by side, composed from
        their layers rather than grabbed off the desktop."""
        from PIL import Image
        parts = []
        for widget in self.row.pack_slaves():
            deck = next((d for d in self.decks if d.canvas is widget), None)
            if deck:
                parts.append(deck.snapshot())
            elif widget is self.dock:
                col = [self.docked[d.number].snapshot() for d in self.decks
                       if d.number in self.docked]
                img = Image.new("RGB", (max(c.width for c in col),
                                        sum(c.height for c in col)
                                        + self.GAP * (len(col) - 1)), BG_RGB)
                y = 0
                for c in col:
                    img.paste(c, (0, y))
                    y += c.height + self.GAP
                parts.append(img)
        w = sum(p.width for p in parts) + self.GAP * (len(parts) - 1)
        out = Image.new("RGB", (w, max(p.height for p in parts)), BG_RGB)
        x = 0
        for p in parts:
            out.paste(p, (x, 0))
            x += p.width + self.GAP
        return out

    def close(self):
        for d in self.decks:
            d.jog.stop()
            d.screen.close()
        self.relay.close()
        self.root.destroy()

    def run(self):
        self.root.mainloop()


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
                         "(app/gui_vnc.sh; env CDJ_APP_FRAME_DIR); without it "
                         "the screen comes over VNC, at up to 33 frames a second")
    ap.add_argument("--screen", default=os.environ.get("CDJ_APP_SCREEN", "auto"),
                    choices=("auto", "dock", "face", "window"),
                    help="dock: each deck's screen large beside the faces; face: "
                         "the faces alone; window: the screens in windows of their "
                         "own; auto: dock when the monitor has room (env "
                         "CDJ_APP_SCREEN; F2 and F3 switch while running)")
    ap.add_argument("--scale", type=float, default=0,
                    help="face scale, 1.0 = 972 x 1252 px a deck (default: fit)")
    ap.add_argument("--stats", type=float, default=0, metavar="S",
                    help="print each deck's frame rate every S seconds")
    return ap.parse_args(argv)


def main(argv=None):
    App(parse_args(argv)).run()


if __name__ == "__main__":
    main()
