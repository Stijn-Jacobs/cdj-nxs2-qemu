# SPDX-License-Identifier: GPL-2.0-or-later
"""app/web_deck.py and app/websock.py, the deck in a web page: the WebSocket
framing, the screen's frame encoder, the page's events to panel datagrams and
the token and Origin checks. No emulator: a fake frame file, relay and VNC
line, and a WebSocket client written here."""
import base64
import json
import os
import socket
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
import zlib

import pytest

APP = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "app")
if APP not in sys.path:
    sys.path.insert(0, APP)

from PIL import Image, ImageDraw  # noqa: E402

import cdj_actions as A  # noqa: E402
import web_deck as W  # noqa: E402
import websock as WS  # noqa: E402

LCD = (800, 480)


# -- fakes ---------------------------------------------------------------------

class FakeRelay:
    def __init__(self):
        self.sent = []

    def send(self, tag, datagram):
        self.sent.append((tag, datagram))


class FakeFrames:
    def __init__(self):
        self.lock = threading.Lock()
        self.seq = None
        self.raw = None

    def put(self, seq, raw):
        with self.lock:
            self.seq, self.raw = seq, raw


class FakeRfb:
    def __init__(self):
        self.pointers = []
        self.keys = []

    def pointer(self, x, y, down):
        self.pointers.append((x, y, down))

    def key(self, sym, down):
        self.keys.append((sym, down))


def rgb565(img):
    """A PIL image as the board writes it: big-endian RGB565."""
    px = img.tobytes()
    return b"".join(struct.pack(">H", (px[i] >> 3) << 11 | (px[i + 1] >> 2) << 5 | px[i + 2] >> 3)
                    for i in range(0, len(px), 3))


def play_screen(shift=0, remain="03:21"):
    """Something like a play screen: flat UI colour, a waveform band, digits."""
    img = Image.new("RGB", LCD, (8, 8, 12))
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, 799, 40), fill=(30, 30, 40))
    d.text((20, 12), "TRACK 01  A Song Title", fill=(230, 230, 230))
    for x in range(800):
        h = 20 + ((x + shift) * 37 % 61)
        d.line((x, 200 - h, x, 200 + h), fill=(40, 120, 255))
    d.text((600, 300), remain, fill=(255, 255, 255))
    return rgb565(img)


@pytest.fixture(scope="module")
def screens():
    return play_screen(0), play_screen(3, "03:20")


def unpack(msg, base=None):
    tag, seq, kind = struct.unpack_from("<cIB", msg)
    assert tag == b"F"
    data = zlib.decompress(msg[6:])
    if kind == W.DELTA:
        data = W.xor_frames(base, data)
    return seq, kind, data


# -- the frame encoder -----------------------------------------------------------

def test_a_whole_frame_and_a_delta_decode_to_the_frames(screens):
    a, b = screens
    seq, kind, data = unpack(W.frame_message(7, a))
    assert (seq, kind, data) == (7, W.KEY, a)
    seq, kind, data = unpack(W.frame_message(8, b, a), base=a)
    assert (seq, kind, data) == (8, W.DELTA, b)


def test_a_delta_is_smaller_than_the_whole_frame_and_a_still_one_is_tiny(screens):
    a, b = screens
    whole = W.frame_message(1, b)
    delta = W.frame_message(2, b, a)
    still = W.frame_message(3, b, b)
    assert len(whole) < len(b) / 4
    assert len(delta) < len(whole)
    assert len(still) < len(b) / 100


def test_pages_that_kept_up_share_one_encoded_delta(screens):
    a, b = screens
    frames = FakeFrames()
    deck = W.Deck("show1", FakeRelay(), frames, FakeRfb())
    m1 = deck.message((1, a), 2, b)
    assert deck.message((1, a), 2, b) is m1
    assert deck.message(None, 2, b) is not m1
    deck.message((2, b), 3, a)
    assert deck.cache_to == 3 and list(deck.cache) == [(2, 3)]


# -- the WebSocket framing ---------------------------------------------------------

def test_accept_key_is_the_rfc_example():
    assert WS.accept_key("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="


def test_upgrade_headers_are_checked():
    good = {"Upgrade": "websocket", "Connection": "keep-alive, Upgrade",
            "Sec-WebSocket-Version": "13", "Sec-WebSocket-Key": base64.b64encode(b"0123456789abcdef").decode()}
    assert WS.upgrade_error(good) is None
    assert WS.upgrade_error({**good, "Upgrade": "h2c"})
    assert WS.upgrade_error({**good, "Connection": "keep-alive"})
    assert WS.upgrade_error({**good, "Sec-WebSocket-Version": "8"})
    assert WS.upgrade_error({**good, "Sec-WebSocket-Key": "short"})


@pytest.fixture
def pair():
    """(server WebSocket, the client's raw socket)."""
    a, b = socket.socketpair()
    b.settimeout(2)
    yield WS.WebSocket(a, a.makefile("rb")), b
    a.close()
    b.close()


MASK = b"\x11\x22\x33\x44"


def read_frame(sock):
    """One server frame: (fin, opcode, payload). Server frames are unmasked."""
    def exact(n):
        out = b""
        while len(out) < n:
            chunk = sock.recv(n - len(out))
            if not chunk:
                raise EOFError
            out += chunk
        return out
    b0, b1 = exact(2)
    assert not b1 & 0x80
    n = b1 & 0x7F
    if n == 126:
        (n,) = struct.unpack(">H", exact(2))
    elif n == 127:
        (n,) = struct.unpack(">Q", exact(8))
    return bool(b0 & 0x80), b0 & 0x0F, exact(n)


def close_code(sock):
    fin, op, payload = read_frame(sock)
    assert op == WS.CLOSE
    return struct.unpack(">H", payload)[0]


def test_masked_text_is_unmasked(pair):
    ws, c = pair
    c.sendall(WS.frame(WS.TEXT, b'{"k":"cue","d":1}', mask=MASK))
    assert ws.recv() == (WS.TEXT, b'{"k":"cue","d":1}')


def test_an_unmasked_client_frame_closes_with_a_protocol_error(pair):
    ws, c = pair
    c.sendall(WS.frame(WS.TEXT, b"hi"))
    assert ws.recv() is None
    assert close_code(c) == WS.PROTOCOL_ERROR


def test_fragments_join_and_a_ping_between_them_is_answered(pair):
    ws, c = pair
    c.sendall(WS.frame(WS.TEXT, b"ab", fin=False, mask=MASK)
              + WS.frame(WS.PING, b"p", mask=MASK)
              + WS.frame(WS.CONT, b"cd", fin=False, mask=MASK)
              + WS.frame(WS.CONT, b"ef", mask=MASK))
    assert ws.recv() == (WS.TEXT, b"abcdef")
    assert read_frame(c) == (True, WS.PONG, b"p")


def test_a_continuation_without_a_start_is_refused(pair):
    ws, c = pair
    c.sendall(WS.frame(WS.CONT, b"x", mask=MASK))
    assert ws.recv() is None
    assert close_code(c) == WS.PROTOCOL_ERROR


def test_a_message_past_the_limit_is_refused_even_in_small_fragments(pair):
    ws, c = pair
    part = b"x" * 16384
    n = WS.MAX_MESSAGE // len(part) + 1

    def send():
        try:
            for i in range(n):
                c.sendall(WS.frame(WS.CONT if i else WS.TEXT, part, fin=i == n - 1, mask=MASK))
        except OSError:
            pass
    t = threading.Thread(target=send)
    t.start()
    assert ws.recv() is None
    t.join()
    assert close_code(c) == WS.TOO_BIG


def test_one_frame_past_the_limit_is_refused_from_its_header(pair):
    ws, c = pair
    c.sendall(bytes([0x82, 0xFF]) + struct.pack(">Q", WS.MAX_MESSAGE + 1) + MASK)
    assert ws.recv() is None
    assert close_code(c) == WS.TOO_BIG


def test_a_close_is_answered_and_ends_recv(pair):
    ws, c = pair
    c.sendall(WS.frame(WS.CLOSE, struct.pack(">H", 1001), mask=MASK))
    assert ws.recv() is None
    assert close_code(c) == 1001
    with pytest.raises(OSError):
        ws.send_text("late")


def test_large_server_frames_use_the_long_length():
    data = b"z" * 70000
    f = WS.frame(WS.BINARY, data)
    assert f[:2] == bytes([0x82, 127]) and struct.unpack(">Q", f[2:10])[0] == 70000


# -- the page's events -------------------------------------------------------------

@pytest.fixture
def page():
    relay, rfb = FakeRelay(), FakeRfb()
    deck = W.Deck("show1", relay, FakeFrames(), rfb)
    p = W.Page(None, deck, 30, "test")
    p.relay, p.rfb = relay, rfb
    return p


def sent(p):
    out = [dg for _, dg in p.relay.sent]
    p.relay.sent.clear()
    return out


def test_a_key_press_and_release_send_its_hold_and_release(page):
    play = A.resolve("play_pause")
    page.event({"k": "play", "d": 1})
    page.event({"k": "play", "d": 1})
    page.event({"k": "play", "d": 0})
    assert sent(page) == [play.hold_datagram(), play.release_datagram()]
    assert all(tag == "show1" for tag, _ in page.relay.sent)


def test_inert_and_unknown_keys_send_nothing(page):
    page.event({"k": "quantize", "d": 1})       # no action in layout.py
    page.event({"k": "no_such_key", "d": 1})
    assert sent(page) == []


def test_the_selector_turns_and_pushes(page):
    page.event({"sel": 2})
    page.event({"sel": -1})
    page.event({"k": "rotary_push", "d": 1})
    turn, push = A.ACTIONS["select_turn"], A.resolve("rotary_push")
    assert sent(page) == [turn.datagram(2), turn.datagram(-1), push.hold_datagram()]


def test_lcd_touch_and_keyboard_go_over_vnc(page):
    page.event({"touch": [400.5, 240, 1]})
    page.event({"touch": [410, 240, 0]})
    page.event({"key": 0x20, "d": 1})
    page.event({"key": 0x20, "d": 0})
    page.event({"key": -5, "d": 1})
    assert page.rfb.pointers == [(400.5, 240.0, True), (410.0, 240.0, False)]
    assert page.rfb.keys == [(0x20, True), (0x20, False)]


def test_a_hidden_page_lets_go_of_everything_it_holds(page):
    page.event({"k": "cue", "d": 1})
    page.event({"key": 0xFF52, "d": 1})
    page.event({"touch": [10, 20, 1]})
    sent(page)
    page.event({"hidden": True})
    assert sent(page) == [A.resolve("cue").release_datagram()]
    assert page.rfb.keys[-1] == (0xFF52, False)
    assert page.rfb.pointers[-1] == (10.0, 20.0, False)
    page.event({"hidden": True})
    assert sent(page) == []


# -- the server end to end -------------------------------------------------------------

@pytest.fixture
def server(tmp_path, screens):
    def make(token=None):
        (tmp_path / "face-cdj2000nxs2.json").write_text('{"units": [972, 1252]}')
        (tmp_path / "keys").mkdir(exist_ok=True)
        (tmp_path / "keys" / "cdj2000nxs2-cue-down.png").write_bytes(b"\x89PNG")
        relay, rfb, frames = FakeRelay(), FakeRfb(), FakeFrames()
        frames.put(1, screens[0])
        deck = W.Deck("show1", relay, frames, rfb)
        srv = W.WebServer(("127.0.0.1", 0), [deck], str(tmp_path), token)
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        made.append(srv)
        srv.relay, srv.frames, srv.rfb = relay, frames, rfb
        srv.base = "http://127.0.0.1:%d" % srv.server_address[1]
        return srv
    made = []
    yield make
    for srv in made:
        srv.shutdown()
        srv.server_close()


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **kw):
        return None


def get(url, cookie=None):
    """(status, headers, body) without following redirects."""
    req = urllib.request.Request(url, headers={"Cookie": cookie} if cookie else {})
    try:
        with urllib.request.build_opener(NoRedirect).open(req, timeout=5) as r:
            return r.status, r.headers, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers, e.read()


def ws_connect(srv, origin="same", path="/ws?deck=1", cookie=None):
    """(socket, status line) of a WebSocket upgrade."""
    port = srv.server_address[1]
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    lines = [f"GET {path} HTTP/1.1", f"Host: 127.0.0.1:{port}", "Upgrade: websocket",
             "Connection: Upgrade", "Sec-WebSocket-Version: 13",
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ=="]
    if origin == "same":
        lines.append(f"Origin: http://127.0.0.1:{port}")
    elif origin:
        lines.append(f"Origin: {origin}")
    if cookie:
        lines.append(f"Cookie: {cookie}")
    s.sendall(("\r\n".join(lines) + "\r\n\r\n").encode())
    head = b""
    while b"\r\n\r\n" not in head:
        chunk = s.recv(1)
        if not chunk:
            break
        head += chunk
    status = head.split(b"\r\n")[0].decode()
    if " 101 " in status:
        assert f"Sec-WebSocket-Accept: {WS.accept_key('dGhlIHNhbXBsZSBub25jZQ==')}".encode() in head
    return s, status


def next_message(s):
    while True:
        fin, op, payload = read_frame(s)
        if op != WS.PING:
            return op, payload


def wait_for(cond, timeout=3.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.01)
    return False


def test_the_page_and_the_face_are_served_and_nothing_else(server):
    srv = server()
    status, headers, body = get(srv.base + "/")
    assert status == 200 and b'src="deck.js"' in body
    assert headers["Content-Type"].startswith("text/html")
    assert get(srv.base + "/link.js")[0] == 200
    assert get(srv.base + "/face-cdj2000nxs2.json")[2] == b'{"units": [972, 1252]}'
    assert get(srv.base + "/keys/cdj2000nxs2-cue-down.png")[1]["Content-Type"] == "image/png"
    for path in ("/../web_deck.py", "/web_deck.py", "/%2e%2e/frames.py", "/keys/../x.png", "/nothing.js"):
        assert get(srv.base + path)[0] == 404, path


def test_a_page_gets_a_whole_frame_then_deltas_and_drives_the_deck(server, screens):
    srv = server()
    s, status = ws_connect(srv)
    assert " 101 " in status
    op, msg = next_message(s)
    assert op == WS.BINARY
    seq, kind, frame = unpack(msg)
    assert (seq, kind, frame) == (1, W.KEY, screens[0])
    srv.frames.put(2, screens[1])
    seq, kind, frame = unpack(next_message(s)[1], base=frame)
    assert (seq, kind, frame) == (2, W.DELTA, screens[1])

    s.sendall(WS.frame(WS.TEXT, b'{"k":"cue","d":1}', mask=MASK))
    cue = A.resolve("cue")
    assert wait_for(lambda: ("show1", cue.hold_datagram()) in srv.relay.sent)
    s.close()
    # The page went with CUE held: the server lets go of it.
    assert wait_for(lambda: srv.relay.sent[-1] == ("show1", cue.release_datagram()))


def test_a_page_that_stops_answering_pings_is_dropped_and_let_go(server, monkeypatch):
    monkeypatch.setattr(W.Page, "PING_S", 0.05)
    monkeypatch.setattr(W.Page, "DEAD_S", 0.3)
    srv = server()
    s, _ = ws_connect(srv)
    s.sendall(WS.frame(WS.TEXT, b'{"k":"play","d":1}', mask=MASK))
    play = A.resolve("play_pause")
    assert wait_for(lambda: srv.relay.sent[-1:] == [("show1", play.hold_datagram())])
    # The client never answers the pings (a browser always does).
    assert wait_for(lambda: srv.relay.sent[-1] == ("show1", play.release_datagram()))
    s.close()


def test_no_such_deck_and_a_bad_upgrade_are_refused(server):
    srv = server()
    assert " 404 " in ws_connect(srv, path="/ws?deck=2")[1]
    assert " 404 " in ws_connect(srv, path="/ws?deck=0")[1]
    assert get(srv.base + "/ws")[0] == 400


def test_a_foreign_or_missing_origin_cannot_open_the_websocket(server):
    srv = server()
    assert " 403 " in ws_connect(srv, origin="http://evil.example")[1]
    assert " 403 " in ws_connect(srv, origin=None)[1]


def test_with_a_token_every_request_needs_it_once_then_the_cookie(server):
    srv = server(token="sekrit")
    assert get(srv.base + "/")[0] == 403
    assert get(srv.base + "/?k=wrong")[0] == 403
    assert get(srv.base + "/face-cdj2000nxs2.json")[0] == 403
    status, headers, _ = get(srv.base + "/?k=sekrit&deck=1")
    assert status == 303
    assert headers["Location"] == "/?deck=1"
    cookie = headers["Set-Cookie"].split(";")[0]
    assert cookie == "cdj_deck=sekrit"
    assert "HttpOnly" in headers["Set-Cookie"] and "SameSite=Strict" in headers["Set-Cookie"]
    assert get(srv.base + "/?deck=1", cookie)[0] == 200
    assert get(srv.base + "/", "cdj_deck=other")[0] == 403
    assert " 403 " in ws_connect(srv)[1]
    assert " 403 " in ws_connect(srv, origin="http://evil.example", cookie=cookie)[1]
    s, status = ws_connect(srv, cookie=cookie)
    assert " 101 " in status
    s.close()


def test_only_loopback_and_private_lan_addresses_are_served():
    for ok in ("127.0.0.1", "192.168.1.20", "10.0.0.5", "172.16.3.4"):
        assert W.bind_error(ok) is None
    for bad in ("0.0.0.0", "8.8.8.8", "172.32.0.1", "169.254.1.1", "localhost"):
        assert W.bind_error(bad), bad


def test_parse_args_needs_a_frame_dir_and_refuses_a_public_bind(monkeypatch):
    monkeypatch.delenv("CDJ_APP_FRAME_DIR", raising=False)
    with pytest.raises(SystemExit):
        W.parse_args([])
    with pytest.raises(SystemExit):
        W.parse_args(["--frame-dir", "x", "--bind", "0.0.0.0"])
    assert W.parse_args(["--frame-dir", "x"]).bind == "127.0.0.1"


# -- the face export -----------------------------------------------------------------

def test_the_face_export_carries_layouts_geometry_and_the_lamp_table():
    import controls as C
    import export_face
    import layout as L
    from relay_link import leds
    looks = {k.name: C.look_for(k.action) for k in L.KEYS}
    down = ["keys/cdj2000nxs2-cue-down.png", 10, 20]
    face = json.loads(json.dumps(export_face.face(looks, 1.3, {"cue": {"down": down}})))
    assert face["units"] == [L.W, L.H] and face["lcd"] == list(L.LCD)
    assert face["lcd_pixels"] == [800, 480]
    keys = {k["name"]: k for k in face["keys"]}
    assert len(keys) == len(L.KEYS)
    cue = keys["cue"]
    assert cue["circle"] == [68, 1037, 55] and cue["sprites"] == {"down": down}
    assert cue["look"] == C.LIVE and cue["keycap"] == "C" and cue["color"] == "#ff781e"
    assert keys["browse"]["box"] == [278, 53, 370, 76]
    assert keys["quantize"]["look"] == C.INERT and "sprites" not in keys["quantize"]
    assert face["lamps"] == {r: list(v) for r, v in leds.LAMPS.items()}


def test_an_older_players_face_comes_from_its_face_file():
    import controls as C
    import export_face
    import face as F
    f = F.load("cdj2000")
    sprites = {"CUE": {"down": ["keys/cdj2000-cue-down.png", 1, 2]}}
    face = json.loads(json.dumps(export_face.face_file("cdj2000", 1.3, sprites)))
    assert face["model"] == "cdj2000" and face["title"] == "CDJ-2000"
    assert face["units"] == list(f.size) and face["lcd_pixels"] == [480, 255]
    keys = {k["name"]: k for k in face["keys"]}
    assert len(keys) == len(f.keys)
    assert keys["CUE"]["lamp"] == "CUE" and face["lamps"]["CUE"] == list(f.key_named("CUE").lamp)
    assert keys["CUE"]["sprites"] == sprites["CUE"]
    dark = next(k for k in f.keys if not k.wired)
    assert keys[dark.name]["look"] == C.INERT
