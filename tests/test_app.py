# SPDX-License-Identifier: GPL-2.0-or-later
"""app/, the virtual deck: its window drawn off screen (SDL's dummy video
driver), the mouse and keys driven by posted events, nothing on the network --
the relay and VNC ports are ones nothing listens on."""
import os
import sys

import pytest

os.environ.setdefault("SDL_VIDEODRIVER", "dummy")
os.environ.setdefault("PYGAME_HIDE_SUPPORT_PROMPT", "1")
pygame = pytest.importorskip("pygame")

APP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "app")
if APP not in sys.path:
    sys.path.insert(0, APP)

import gfx  # noqa: E402
import layout as L  # noqa: E402
import virtual_deck as V  # noqa: E402


def test_keysym_maps_the_decks_keys():
    assert gfx.keysym(pygame.K_SPACE) == 0x20
    assert gfx.keysym(pygame.K_a) == 0x61
    assert gfx.keysym(pygame.K_MINUS) == 0x2D
    assert gfx.keysym(pygame.K_RETURN) == 0xFF0D
    assert gfx.keysym(pygame.K_UP) == 0xFF52
    assert gfx.keysym(pygame.K_LSHIFT) == 0xFFE1
    assert gfx.keysym(pygame.K_F1) == 0xFFBE
    assert gfx.keysym(pygame.K_F12) == 0xFFC9
    assert gfx.keysym(pygame.K_KP5) == 0xFFB5


@pytest.fixture
def app(tmp_path, monkeypatch):
    monkeypatch.setenv("CDJ_APP_CACHE", str(tmp_path / "art"))
    a = V.App(V.parse_args(["--decks", "1", "--relay", "127.0.0.1:1", "--vnc-base", "1",
                            "--hidden"]))
    deck = a.decks[0]
    deck.sent, deck.keys_sent = [], []
    deck.relay.send = lambda tag, dg: deck.sent.append(dg)
    deck.screen.key = lambda sym, down: deck.keys_sent.append((sym, down))
    yield a
    a.close()


def post(app, kind, **kw):
    kw.setdefault("window", app.window)
    app._event(pygame.event.Event(kind, **kw))


def at(app, x, y):
    """Face units to window points."""
    s = app.decks[0].scale
    return app._left() + x * s, y * s


def test_window_draws_the_face_at_its_pixel_ratio(app):
    app._present(True)
    img = app.snapshot()
    w, h = app.window.size
    assert img.size == (round(w * app.ratio), round(h * app.ratio))
    # The face is drawn, not left at the background.
    assert len(img.crop((0, 0, img.width, img.height // 2)).getcolors(1 << 20)) > 50


def test_a_panel_key_click_sends_its_datagrams(app):
    deck = app.decks[0]
    play = next(k for k in L.KEYS if "play" in k.name.lower())
    x0, y0, x1, y1 = play.bounds()
    pos = at(app, (x0 + x1) / 2, (y0 + y1) / 2)
    post(app, pygame.MOUSEBUTTONDOWN, pos=pos, button=1)
    post(app, pygame.MOUSEBUTTONUP, pos=pos, button=1)
    assert len(deck.sent) == 2              # down and up


def test_keys_go_to_the_deck_and_f2_docks_the_screen(app):
    deck = app.decks[0]
    for kind in (pygame.KEYDOWN, pygame.KEYUP):
        post(app, kind, key=pygame.K_SPACE, mod=0, unicode="", scancode=0)
    assert deck.keys_sent == [(0x20, True), (0x20, False)]
    post(app, pygame.KEYDOWN, key=pygame.K_F2, mod=0, unicode="", scancode=0)
    assert app.mode == "dock"
    assert [type(v).__name__ for v, _ in app.views] == ["DeckView", "DockView"]
    assert app.window.size[0] >= app.content_size[0]
