# SPDX-License-Identifier: GPL-2.0-or-later
"""Where the deck's screen comes from, and how the window gets it.

Two sources, one interface (FrameSource):

  * FrameFile reads the frame file the display board writes when
    CDJ_GUI_FRAME_FILE is set (hw/cdj/sh7269gui.c): every frame the firmware
    scans out, as soon as it changes, at up to 120 a second;
  * the VNC client (rfb.py), which works with any build but is held to 33
    updates a second by QEMU's VNC server.

Screen puts them together: frames from the file while it is live, from VNC
otherwise, and the touch screen and the keyboard always over VNC.

A source scales each new frame to the window's size in its own thread, so
the UI thread only pastes.
"""

import struct
import threading
import time
from array import array

from PIL import Image

LCD_SIZE = (800, 480)


class FrameStats:
    """Screen updates per second, over a sliding window."""

    WINDOW_S = 2.0

    def __init__(self):
        self.stamps = []
        self.lock = threading.Lock()

    def add(self, now):
        with self.lock:
            self.stamps.append(now)
            cut = now - self.WINDOW_S
            while self.stamps and self.stamps[0] < cut:
                self.stamps.pop(0)

    def rate(self, now):
        with self.lock:
            return sum(1 for t in self.stamps if t >= now - self.WINDOW_S) / self.WINDOW_S

    def last(self):
        with self.lock:
            return self.stamps[-1] if self.stamps else float("-inf")


class FrameSource:
    """The latest screen, whole and scaled to the window, and whether it moved."""

    def __init__(self):
        self.lock = threading.Lock()
        self.frame = None           # PIL RGB image, the whole screen
        self.sizes = {}             # view name -> the size it shows the screen at
        self.views = {}             # view name -> frame scaled to that size
        self.dirty = False
        self.updates = FrameStats()

    def set_views(self, sizes):
        """The sizes the window shows this screen at, by view name."""
        with self.lock:
            self.sizes = {k: tuple(v) for k, v in sizes.items()}
            self.views = {}
            self.dirty = self.frame is not None

    def take_frame(self):
        """{view name: scaled screen} when it changed since the last call,
        else None."""
        with self.lock:
            if not self.dirty or self.frame is None:
                return None
            self.dirty = False
            if set(self.views) != set(self.sizes):
                self.views = self._scaled()
            return self.views

    def _scaled(self):
        out, by_size = {}, {}
        for name, size in self.sizes.items():
            if size not in by_size:
                by_size[size] = self._resize(size)
            out[name] = by_size[size]
        return out

    def _resize(self, size):
        w, h = self.frame.size
        if size == (w, h):
            return self.frame.copy()
        if size[0] % w == 0 and size[1] % h == 0:
            return self.frame.resize(size, Image.NEAREST)   # whole pixels stay sharp
        return self.frame.resize(size, Image.BILINEAR)

    def _publish(self):
        """Call with the lock held after self.frame changed."""
        self.views = self._scaled()
        self.dirty = True
        self.updates.add(time.monotonic())


class FrameFile(FrameSource):
    """The display board's frame file (layout in sh7269gui.c, gui_frame_*)."""

    MAGIC = b"CDJLCD1\0"
    HDR = 32
    POLL_S = 0.002
    RETRY_S = 0.5

    def __init__(self, path):
        super().__init__()
        self.path = path
        self.seq = None
        self.following = False
        self.closed = False
        self.status = f"frames: waiting for {path}"
        self.thread = threading.Thread(target=self._run, name="frames", daemon=True)

    def start(self):
        self.thread.start()

    def close(self):
        self.closed = True

    def live(self, now):
        """True while the file is being followed. A still screen (a paused
        deck) writes nothing new, and must not count as the file gone."""
        return self.following

    def _run(self):
        while not self.closed:
            try:
                with open(self.path, "rb", buffering=0) as f:
                    self._follow(f)
            except (OSError, ValueError) as e:
                self.following = False
                self.status = f"frames: {self.path} ({e})"
            time.sleep(self.RETRY_S)

    def _follow(self, f):
        hdr = f.read(self.HDR)
        if len(hdr) < self.HDR or hdr[:8] != self.MAGIC:
            raise ValueError("not a frame file yet")
        w, h, fmt = struct.unpack_from("<HHI", hdr, 12)
        if fmt != 1:
            raise ValueError(f"unknown pixel format {fmt}")
        size = w * h * 2
        total = self.HDR + size + 4
        self.status = f"frames: {self.path}"
        idle_since = time.monotonic()
        while not self.closed:
            f.seek(8)
            (done,) = struct.unpack("<I", f.read(4))
            if done == self.seq or done == 0:
                # A new QEMU truncates and restarts the file: reopen after a
                # long silence, in case this handle is on the old one.
                if time.monotonic() - idle_since > 5:
                    return
                time.sleep(self.POLL_S)
                continue
            f.seek(0)
            data = f.read(total)
            if len(data) < total:
                time.sleep(self.POLL_S)
                continue
            (done,) = struct.unpack_from("<I", data, 8)
            (started,) = struct.unpack_from("<I", data, total - 4)
            if done != started:
                continue            # a frame was being written: read again
            px = array("H")
            px.frombytes(data[self.HDR:self.HDR + size])
            px.byteswap()           # big-endian RGB565 to the host's order
            img = Image.frombuffer("RGB", (w, h), px.tobytes(), "raw", "BGR;16", 0, 1)
            with self.lock:
                self.frame = img
                self._publish()
            self.seq = done
            self.following = True
            idle_since = time.monotonic()


class Screen:
    """One deck's screen: frames from the file when it is live, else VNC;
    touch and keys over VNC."""

    def __init__(self, rfb, frame_file=None):
        self.rfb = rfb
        self.file = frame_file

    def start(self):
        self.rfb.start()
        if self.file:
            self.file.start()

    def close(self):
        self.rfb.close()
        if self.file:
            self.file.close()

    def source(self):
        if self.file and self.file.live(time.monotonic()):
            return self.file
        return self.rfb

    @property
    def connected(self):
        return self.rfb.connected or (self.file is not None
                                      and self.file.live(time.monotonic()))

    @property
    def status(self):
        return self.rfb.status

    @property
    def updates(self):
        return self.source().updates

    def via(self):
        return "frame file" if self.source() is self.file else "VNC"

    def set_views(self, sizes):
        self.rfb.set_views(sizes)
        if self.file:
            self.file.set_views(sizes)

    def take_frame(self):
        src = self.source()
        # The idle source's pending frame is stale by the time it is shown.
        other = self.file if src is self.rfb else self.rfb
        if other is not None:
            with other.lock:
                other.dirty = False
        self.rfb.want_frames = src is self.rfb
        return src.take_frame()

    def invalidate(self):
        src = self.source()
        with src.lock:
            src.dirty = src.frame is not None

    def pointer(self, x, y, down):
        self.rfb.pointer(x, y, down)

    def key(self, keysym, down):
        self.rfb.key(keysym, down)
