#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The deck in a web page: the virtual deck app's sibling, for a browser.

    python app/web_deck.py --frame-dir DIR [--decks 1] [--port 7210] [--bind ADDR]

`./start.sh --web` starts the decks with their screens on VNC and a frame
file, as for --app, and runs this. Open http://127.0.0.1:<port>/ (deck N:
/?deck=N). The page (web/) is the NXS2 face that export_face.py writes at
start, the deck's LCD drawn live over it, and the face's keys, selector, LCD
touch and keyboard sent back here, where the app's own controls (controls.py)
turn them into panel datagrams for the relay, and the LCD touch and keys go
to the display board over VNC.

One WebSocket per page (/ws?deck=N): binary messages carry LCD frames, text
messages JSON events. A frame is `b"F" + u32 seq + u8 kind` (little-endian)
and then zlib: kind 0 the whole frame, as the board wrote it (big-endian
RGB565), kind 1 that XORed with the last frame this page was sent. Each page
gets the newest frame when it is ready for one, at most --fps (?fps=) a
second, and a whole one every KEY_S; a slow page skips frames rather than
falling behind.

On loopback anyone on this PC can open it, as they can the relay. With
--bind on a LAN address (`./start.sh --web lan`) every request needs the
token in CDJ_WEB_TOKEN: the URL's ?k=<token> once, a cookie after that. The
WebSocket upgrade also needs an Origin of this server, so another site open
in the same browser cannot drive the deck.
"""

import argparse
import http.cookies
import http.server
import ipaddress
import json
import logging
import os
import re
import secrets
import socket
import struct
import sys
import tempfile
import threading
import time
import urllib.parse
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import controls as C  # noqa: E402
import layout as L  # noqa: E402
import websock as WS  # noqa: E402
from frames import FrameFile  # noqa: E402
from relay_link import RelayLink  # noqa: E402
from rfb import RfbClient  # noqa: E402

WEB = os.path.join(HERE, "web")
COOKIE = "cdj_deck"
# A LAN bind must be one of these: the server never listens on a public
# address, nor on every address at once.
PRIVATE = [ipaddress.ip_network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")]
STATIC = re.compile(r"^(keys/)?[a-z0-9_-]+\.(html|js|json|png)$")
TYPES = {"html": "text/html; charset=utf-8", "js": "text/javascript; charset=utf-8",
         "json": "application/json", "png": "image/png"}

KEY, DELTA = 0, 1

log = logging.getLogger("web")


def xor_frames(a, b):
    return (int.from_bytes(a, "little") ^ int.from_bytes(b, "little")).to_bytes(len(b), "little")


def frame_message(seq, raw, base=None):
    """One screen message: the whole frame, or its delta from `base`."""
    if base is None:
        return struct.pack("<cIB", b"F", seq & 0xFFFFFFFF, KEY) + zlib.compress(raw, 1)
    return struct.pack("<cIB", b"F", seq & 0xFFFFFFFF, DELTA) + zlib.compress(xor_frames(base, raw), 1)


class Deck:
    """One deck's frame file, its VNC line for input, and its encoded frames.

    Pages that keep up all want the same delta, from the frame before to the
    newest one, so each (from, to) pair is encoded once and kept until a
    newer frame arrives."""

    def __init__(self, tag, relay, frames, rfb):
        self.tag = tag
        self.relay = relay
        self.frames = frames
        self.rfb = rfb
        self.lock = threading.Lock()
        self.cache_to = None
        self.cache = {}

    def send(self, datagram):
        self.relay.send(self.tag, datagram)

    def latest(self):
        """(seq, raw) of the newest frame, or (None, None) before the first."""
        with self.frames.lock:
            return self.frames.seq, self.frames.raw

    def message(self, base, seq, raw):
        """The message taking a page from `base` ((seq, raw) it was last sent,
        or None for a whole frame) to frame `seq`."""
        k = (base[0] if base else None, seq)
        with self.lock:
            if self.cache_to != seq:
                self.cache_to, self.cache = seq, {}
            msg = self.cache.get(k)
            if msg is None:
                msg = self.cache[k] = frame_message(seq, raw, base[1] if base else None)
            return msg


class Page:
    """One connected page: the controls it holds, and its frame sender.

    The page's controls are its own, so a page that goes away (closed, out of
    Wi-Fi, hidden) releases exactly what it held: `hold` has no timeout on the
    board, and a dropped finger on CUE would otherwise keep CUE held."""

    PING_S = 2.5
    DEAD_S = 5.0                     # two pings unanswered: the read times out
    KEY_S = 10.0
    POLL_S = 0.004

    def __init__(self, ws, deck, fps, name):
        self.ws = ws
        self.deck = deck
        self.period = 1.0 / fps
        self.name = name
        self.keys = {k.name: C.KeyControl(k.action, deck.send) for k in L.KEYS}
        self.selector = C.Selector(deck.send)
        self.keys["rotary_push"] = self.selector.push
        self.syms = set()               # keysyms held down over VNC
        self.touch = None               # (x, y) while a finger is on the LCD

    def event(self, msg):
        """One JSON event from the page (the table in web/deck.js)."""
        if "k" in msg:
            ctl = self.keys.get(msg["k"])
            if ctl and msg.get("d"):
                ctl.press()
            elif ctl:
                ctl.release()
        elif "sel" in msg:
            self.selector.turn(max(-8, min(8, int(msg["sel"]))))
        elif "touch" in msg:
            x, y, down = msg["touch"]
            x, y = float(x), float(y)
            self.touch = (x, y) if down else None
            self.deck.rfb.pointer(x, y, bool(down))
        elif "key" in msg:
            sym, down = int(msg["key"]), bool(msg.get("d"))
            if not 0 < sym <= 0xFFFFFF:
                return
            if down:
                self.syms.add(sym)
            else:
                self.syms.discard(sym)
            self.deck.rfb.key(sym, down)
        elif msg.get("hidden"):
            self.release_all()

    def release_all(self):
        for ctl in self.keys.values():
            ctl.release()
        for sym in self.syms:
            self.deck.rfb.key(sym, False)
        self.syms.clear()
        if self.touch:
            self.deck.rfb.pointer(*self.touch, False)
            self.touch = None

    def run(self):
        """Read events until the page goes; then let go of everything."""
        sender = threading.Thread(target=self._send_frames, name=f"frames-{self.name}", daemon=True)
        sender.start()
        try:
            while True:
                got = self.ws.recv()
                if got is None:
                    break
                op, data = got
                if op != WS.TEXT:
                    continue
                try:
                    msg = json.loads(data)
                    if isinstance(msg, dict):
                        self.event(msg)
                except (ValueError, TypeError, KeyError):
                    log.info("%s: ignored %r", self.name, data[:80])
        finally:
            self.ws.close()
            self.release_all()
            log.info("%s: gone", self.name)

    def _send_frames(self):
        ws, deck = self.ws, self.deck
        sent = None                     # (seq, raw) this page has now
        due = next_ping = time.monotonic()
        next_key, frames, nbytes, since = 0.0, 0, 0, due
        try:
            while not ws.closed:
                now = time.monotonic()
                if now >= next_ping:
                    ws.ping()
                    next_ping = now + self.PING_S
                seq, raw = deck.latest()
                if raw is None or now < due or (sent and seq == sent[0]):
                    time.sleep(self.POLL_S)
                    continue
                whole = sent is None or now >= next_key
                msg = deck.message(None if whole else sent, seq, raw)
                ws.send_binary(msg)
                sent = (seq, raw)
                due = max(due + self.period, now)
                frames, nbytes = frames + 1, nbytes + len(msg)
                if whole:
                    if next_key:
                        log.info("%s: %.1f frames/s, %.0f KB/s", self.name,
                                 frames / (now - since), nbytes / 1024 / (now - since))
                    next_key, frames, nbytes, since = now + self.KEY_S, 0, 0, now
        except OSError:
            pass
        ws.close()


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "cdj-web"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        log.info("%s %s", self.address_string(), fmt % args)

    def _fail(self, code, text):
        body = text.encode()
        self.send_response(code)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _allowed(self, query):
        """True when no token is needed or the request has it. A token in the
        URL becomes a cookie, and the URL is reloaded without it."""
        token = self.server.token
        if not token:
            return True
        given = query.get("k", [""])[0]
        if given and secrets.compare_digest(given, token):
            path = urllib.parse.urlsplit(self.path).path
            rest = urllib.parse.urlencode({k: v for k, v in query.items() if k != "k"}, doseq=True)
            self.send_response(303)
            self.send_header("Set-Cookie", f"{COOKIE}={token}; HttpOnly; SameSite=Strict; Path=/")
            self.send_header("Location", path + ("?" + rest if rest else ""))
            self.send_header("Content-Length", "0")
            self.end_headers()
            return False
        jar = http.cookies.SimpleCookie(self.headers.get("Cookie", ""))
        if COOKIE in jar and secrets.compare_digest(jar[COOKIE].value, token):
            return True
        self._fail(403, "open the address the launcher printed, with its ?k=")
        return False

    def do_GET(self):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        if self.server.token and not self._allowed(query):
            return
        name = url.path.lstrip("/") or "index.html"
        if name == "ws":
            self._websocket(query)
        elif STATIC.match(name):
            self._file(name)
        else:
            self._fail(404, "not found")

    def _file(self, name):
        for root in (WEB, self.server.face_dir):
            path = os.path.join(root, *name.split("/"))
            if os.path.isfile(path):
                with open(path, "rb") as f:
                    body = f.read()
                self.send_response(200)
                self.send_header("Content-Type", TYPES[name.rsplit(".", 1)[1]])
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-cache")
                self.end_headers()
                self.wfile.write(body)
                return
        self._fail(404, "not found")

    def _websocket(self, query):
        why = WS.upgrade_error(self.headers)
        if why:
            self._fail(400, why)
            return
        origin = self.headers.get("Origin", "")
        if origin != f"http://{self.headers.get('Host', '')}":
            log.info("%s: refused WebSocket from origin %r", self.address_string(), origin)
            self._fail(403, "wrong origin")
            return
        try:
            n = int(query.get("deck", ["1"])[0])
            fps = max(1, min(60, int(query.get("fps", [str(self.server.fps)])[0])))
        except ValueError:
            n = 0
        if not 1 <= n <= len(self.server.decks):
            self._fail(404, "no such deck")
            return
        deck = self.server.decks[n - 1]
        self.wfile.write(WS.handshake_response(self.headers["Sec-WebSocket-Key"]))
        self.wfile.flush()
        self.close_connection = True
        self.connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # A page that drops off the network sends nothing more, not even a
        # close; the pings make a live one answer well inside this.
        self.connection.settimeout(Page.DEAD_S)
        name = f"{deck.tag} {self.address_string()}:{self.client_address[1]}"
        log.info("%s: connected, %d fps", name, fps)
        Page(WS.WebSocket(self.connection, self.rfile), deck, fps, name).run()


class WebServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, addr, decks, face_dir, token=None, fps=30):
        self.decks = decks
        self.face_dir = face_dir
        self.token = token
        self.fps = fps
        super().__init__(addr, Handler)

    def handle_error(self, request, client_address):
        # Browsers drop idle keep-alive connections without a goodbye.
        if not isinstance(sys.exc_info()[1], ConnectionError):
            super().handle_error(request, client_address)


def bind_error(addr):
    """Why the server must not listen on `addr`, or None."""
    try:
        ip = ipaddress.ip_address(addr)
    except ValueError:
        return f"{addr} is not an IPv4 address"
    if ip.is_loopback or any(ip in n for n in PRIVATE):
        return None
    return f"{addr} is not a loopback or private LAN address"


def face_dir():
    """Where the exported face goes; CDJ_APP_CACHE moves it with art.py's cache."""
    base = os.environ.get("CDJ_APP_CACHE") or tempfile.gettempdir()
    return os.path.join(base, "cdj-web-face")


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
                    help="deck N's screen is on VNC port <base>+N (env CDJ_APP_VNC_BASE)")
    ap.add_argument("--frame-dir", default=os.environ.get("CDJ_APP_FRAME_DIR", ""),
                    help="where the display boards write cdj-lcd-<tag>.bin (env CDJ_APP_FRAME_DIR)")
    ap.add_argument("--bind", default="127.0.0.1",
                    help="the address to listen on: loopback, or this PC's private LAN address")
    ap.add_argument("--port", type=int, default=7210)
    ap.add_argument("--fps", type=int, default=30, help="frames a second to each page (?fps= overrides)")
    args = ap.parse_args(argv)
    if not args.frame_dir:
        ap.error("--frame-dir is needed (GUI_DISPLAY=vnc writes the frame files there)")
    why = bind_error(args.bind)
    if why:
        ap.error(why)
    return args


def main(argv=None):
    import export_face
    args = parse_args(argv)
    logging.basicConfig(level=logging.INFO,
                        format="%(asctime)s %(name)s: %(message)s")
    token = os.environ.get("CDJ_WEB_TOKEN") or None
    if not ipaddress.ip_address(args.bind).is_loopback and not token:
        sys.exit("a LAN address needs a token in CDJ_WEB_TOKEN (./start.sh --web lan makes one)")
    faces = face_dir()
    export_face.export(faces)
    host, _, port = args.relay.partition(":")
    relay = RelayLink(host, int(port))
    decks = []
    for n in range(1, args.decks + 1):
        tag = f"{args.prefix}{n}"
        frames = FrameFile(os.path.join(args.frame_dir, f"cdj-lcd-{tag}.bin"))
        rfb = RfbClient(args.vnc_host, args.vnc_base + n, f"deck{n}",
                        os.path.join(args.frame_dir, f"cdj-vnc-{tag}.port"))
        rfb.want_frames = False         # the frame file carries the screen
        decks.append(Deck(tag, relay, frames, rfb))
    server = WebServer((args.bind, args.port), decks, faces, token, args.fps)
    relay.start()
    for d in decks:
        d.frames.start()
        d.rfb.start()
    log.info("serving %d deck(s) on http://%s:%d/%s", len(decks), args.bind, args.port,
             " (token required)" if token else "")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
