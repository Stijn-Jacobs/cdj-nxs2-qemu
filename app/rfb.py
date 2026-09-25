# SPDX-License-Identifier: GPL-2.0-or-later
"""A small RFB (VNC) client: the deck's screen, its touch panel and its keys.

The display board's QEMU runs a VNC server (`-display vnc=...`), which is the
one screen transport QEMU offers on every host. Over it the app gets:

  * the 800x480 LCD, as the rectangles QEMU found changed since the last
    request (QEMU compares against its own copy, so a still screen costs
    nothing);
  * the touch screen: the board registers an absolute pointer while CDJ_TOUCH
    is on, so a pointer event is a finger at an LCD pixel, and the board
    forwards it to MAIN exactly as a click in the QEMU window would be;
  * the keyboard: key events reach the board's own key map (sh7269gui.c,
    "Host keyboard to front panel"), so the app's keys are the deck window's.

Only what QEMU needs is implemented: protocol 3.8 (3.3 accepted), security
"None", the Raw and CopyRect encodings and the DesktopSize and pointer-type
pseudo-encodings. Raw is the fastest on loopback: there is nothing to
decompress, and Pillow unpacks each rectangle in C.

The connection runs in its own thread; the UI polls `take_frame()`.
"""

import socket
import struct
import threading
import time

from PIL import Image

from frames import FrameSource

ENC_RAW = 0
ENC_COPYRECT = 1
ENC_DESKTOP_SIZE = -223
ENC_POINTER_TYPE = -257          # QEMU: absolute (1) or relative (0) pointer

# 32 bpp, depth 24, little-endian, true colour, 8 bits per channel with red
# in bits 16-23: each pixel arrives as the bytes B, G, R, X.
PIXEL_FORMAT = struct.pack(">BBBBHHHBBB3x", 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
RAW_MODE = "BGRX"


class RfbError(Exception):
    pass


class RfbClient(FrameSource):
    """One VNC connection, reconnecting until closed."""

    RETRY_S = 1.0

    def __init__(self, host, port, name="deck"):
        super().__init__()
        self.addr = (host, port)
        self.name = name
        self.status = "waiting for the screen"
        self.absolute = True
        self._want = True           # ask for screen updates
        self._screen_size = (800, 480)
        self.sock = None
        self.send_lock = threading.Lock()
        self.closed = False
        self.thread = threading.Thread(target=self._run, name=f"rfb-{name}",
                                       daemon=True)

    # -- public, any thread ---------------------------------------------------

    def start(self):
        self.thread.start()

    def close(self):
        self.closed = True
        s = self.sock
        if s:
            try:
                s.close()
            except OSError:
                pass

    @property
    def connected(self):
        return self.sock is not None

    @property
    def want_frames(self):
        return self._want

    @want_frames.setter
    def want_frames(self, on):
        """False while another source shows the screen: VNC then only carries
        input. Turning it back on asks for a whole frame."""
        if on and not self._want and self.sock is not None:
            self._request(*self._screen_size, incremental=False)
        self._want = on

    def size(self):
        with self.lock:
            return self.frame.size if self.frame else (800, 480)

    def pointer(self, x, y, down):
        """A finger at LCD pixel (x, y); down is the left button."""
        w, h = self.size()
        x = max(0, min(w - 1, int(x)))
        y = max(0, min(h - 1, int(y)))
        self._send(struct.pack(">BBHH", 5, 1 if down else 0, x, y))

    def key(self, keysym, down):
        """An X11 keysym (Tk's keysym_num is one on every platform)."""
        self._send(struct.pack(">BBxxI", 4, 1 if down else 0, keysym))

    # -- the connection thread -------------------------------------------------

    def _send(self, data):
        s = self.sock
        if s is None:
            return
        with self.send_lock:
            try:
                s.sendall(data)
            except OSError:
                pass

    def _run(self):
        while not self.closed:
            try:
                self._session()
            except (OSError, RfbError, struct.error) as e:
                if not self.closed:
                    self.status = f"screen: {self.addr[0]}:{self.addr[1]} ({e})"
            finally:
                s, self.sock = self.sock, None
                if s:
                    try:
                        s.close()
                    except OSError:
                        pass
            if not self.closed:
                time.sleep(self.RETRY_S)

    def _session(self):
        s = socket.create_connection(self.addr, timeout=2)
        s.settimeout(None)
        s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        rd = s.makefile("rb", buffering=1 << 20)
        read = _exact_reader(rd)

        version = read(12)
        if not version.startswith(b"RFB "):
            raise RfbError(f"not a VNC server ({version!r})")
        minor = int(version[8:11])
        s.sendall(b"RFB 003.008\n" if minor >= 8 else b"RFB 003.003\n")
        if minor >= 7:
            n = read(1)[0]
            if n == 0:
                raise RfbError(_reason(read))
            types = read(n)
            if 1 not in types:
                raise RfbError("the server wants a password; start it without one")
            s.sendall(b"\x01")
        else:
            (sec,) = struct.unpack(">I", read(4))
            if sec != 1:
                raise RfbError("the server wants a password; start it without one")
        if minor >= 8:
            (result,) = struct.unpack(">I", read(4))
            if result != 0:
                raise RfbError(_reason(read))

        s.sendall(b"\x01")                              # shared
        w, h = struct.unpack(">HH", read(4))
        read(16)                                        # its pixel format
        (nlen,) = struct.unpack(">I", read(4))
        read(nlen)

        s.sendall(struct.pack(">B3x", 0) + PIXEL_FORMAT)
        encs = [ENC_RAW, ENC_COPYRECT, ENC_DESKTOP_SIZE, ENC_POINTER_TYPE]
        s.sendall(struct.pack(f">BxH{len(encs)}i", 2, len(encs), *encs))
        with self.lock:
            self.frame = Image.new("RGB", (w, h))
            self.dirty = True
        self.sock = s
        self.status = f"screen: {self.addr[0]}:{self.addr[1]}"
        self._request(w, h, incremental=False)

        while not self.closed:
            kind = read(1)[0]
            if kind == 0:
                w, h = self._update(read, w, h)
                self._screen_size = (w, h)
                if self._want:
                    self._request(w, h, incremental=True)
            elif kind == 1:                             # colour map: unused
                _, _, n = struct.unpack(">xHH", read(5))
                read(6 * n)
            elif kind == 2:                             # bell
                pass
            elif kind == 3:                             # cut text
                (n,) = struct.unpack(">3xI", read(7))
                read(n)
            else:
                raise RfbError(f"unknown server message {kind}")

    def _request(self, w, h, incremental):
        self._send(struct.pack(">BBHHHH", 3, 1 if incremental else 0, 0, 0, w, h))

    def _update(self, read, w, h):
        (nrects,) = struct.unpack(">xH", read(3))
        changed = False
        for _ in range(nrects):
            x, y, rw, rh, enc = struct.unpack(">HHHHi", read(12))
            if enc == ENC_RAW:
                data = read(rw * rh * 4)
                if rw and rh:
                    tile = Image.frombuffer("RGB", (rw, rh), data, "raw",
                                            RAW_MODE, 0, 1)
                    with self.lock:
                        self.frame.paste(tile, (x, y))
                    changed = True
            elif enc == ENC_COPYRECT:
                sx, sy = struct.unpack(">HH", read(4))
                with self.lock:
                    tile = self.frame.crop((sx, sy, sx + rw, sy + rh))
                    self.frame.paste(tile, (x, y))
                changed = True
            elif enc == ENC_DESKTOP_SIZE:
                w, h = rw, rh
                with self.lock:
                    self.frame = Image.new("RGB", (w, h))
                changed = True
            elif enc == ENC_POINTER_TYPE:
                self.absolute = bool(x)
            else:
                raise RfbError(f"unexpected encoding {enc}")
        if changed:
            with self.lock:
                self._publish()
        return w, h


def _exact_reader(rd):
    def read(n):
        data = rd.read(n)
        if data is None or len(data) < n:
            raise RfbError("connection closed")
        return data
    return read


def _reason(read):
    (n,) = struct.unpack(">I", read(4))
    return read(n).decode("utf-8", "replace") or "refused"
