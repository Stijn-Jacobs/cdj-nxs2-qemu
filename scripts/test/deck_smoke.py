# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot every listed deck headless at once and check each reaches its first
stable screen.

Each deck is one QEMU (its display board runs inside the MAIN emulator), so a
deck is checked by its own console: the monitor's screendump is polled and
compared with a reference frame. The comparison is on 8x8-pixel block means,
so a changed clock digit or a moved cursor still passes while a black,
frozen-at-logo or error screen does not. A deck passes as soon as a frame
matches and fails as soon as its QEMU exits; otherwise it fails at its
timeout with the last frame kept beside the log.

    deck_smoke.py <decks.ini> [--tier2] [--record] [--only a,b] [--lock-wait S] [--out DIR]

decks.ini, one section per deck:

    [cdj2000]
    machine = cdj2000                  ; -M
    qemu    = /path/qemu-system-sh4    ; skipped when missing
    kernel  = extract/cdj2000/main_unpacked.bin
    env     = CDJ_ATA=1 CDJ_BF531_UPD=extract/cdj2000/display.upd
    stick   = /path/media.img          ; optional, attached as a snapshot
    lock    = /tmp/cdj-emu-old.lock    ; optional mkdir lock, held for the run
    golden  = golden/cdj2000.ppm       ; the reference frame
    timeout = 60                       ; wall seconds to reach it
    port    = 41201                    ; monitor TCP port
    fb      = 0x14600000 800 480       ; optional: read an RGB565 frame buffer
                                       ; from guest memory instead of the console
    args    = -icount shift=3          ; optional extra QEMU options
    settle  = 10                       ; --record: seconds the screen must hold
    steps   =                          ; --tier2: what to do once the first screen is up,
        at 45                          ;   one per line: at S (wait until S seconds after
        key 0x13:0x04:300              ;   launch), wait S, key <payload> (a UDP datagram
        wait 8                         ;   to CDJ_PANEL_KEYSOCK), sendkey <name> <ms>
        sendkey ret 400                ;   (the monitor's sendkey)
    golden2 = golden/xdj1000-load.ppm  ; the screen the steps lead to
    timeout2 = 200                     ; wall seconds after the last step to reach it
    region2 = 96 72 688 40             ; optional x y w h: compare only this part of
                                       ; the screen (a playing deck's waveform never
                                       ; repeats, its title bar does)
    motion  = xdj1000                  ; optional: once golden2 matches, film the
                                       ; playhead; the deck passes only if
                                       ; scripts/run/score_playhead.py --model <this>
                                       ; calls it MOTION (the older decks play on load)

Relative paths are taken from the ini's [DEFAULT] root (or the ini's folder);
the golden frame's from the ini's folder.
--record boots each deck until its screen has held for `settle` seconds and
writes that frame as the golden; every distinct frame on the way is kept in
--out, which is how a deck's first stable screen is found.

--tier2 runs only the decks that have steps: first screen, then the steps,
then the second golden. With --record the first screen is not checked and the
frame the steps lead to is recorded as golden2.
"""

import argparse
import configparser
import os
import shlex
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCORER = os.path.join(HERE, "..", "run", "score_playhead.py")

POLL = 2.0
BLOCK = 8
BLOCK_TOLERANCE = 24        # mean grey-level difference a block may have
MATCH = 0.90                # share of lit blocks that must agree
MOTION_FRAMES = 3
MOTION_EVERY = 20.0         # wall seconds apart: the older decks play at 0.1-0.25x


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    magic, dims, maxval, pixels = data.split(b"\n", 3)
    if magic != b"P6" or maxval != b"255":
        raise ValueError("%s is not an 8-bit P6 image" % path)
    w, h = (int(v) for v in dims.split())
    return w, h, pixels[:w * h * 3]


def block_means(w, h, pixels):
    """Mean grey level of each BLOCK x BLOCK tile, row by row."""
    cols, rows = w // BLOCK, h // BLOCK
    sums = [0] * (cols * rows)
    for y in range(rows * BLOCK):
        row = pixels[y * w * 3:(y * w + cols * BLOCK) * 3]
        base = (y // BLOCK) * cols
        grey = [row[i] + row[i + 1] + row[i + 2] for i in range(0, len(row), 3)]
        for c in range(cols):
            sums[base + c] += sum(grey[c * BLOCK:(c + 1) * BLOCK])
    n = BLOCK * BLOCK * 3
    return (w, h), [s // n for s in sums]


def rgb565(raw, w, h):
    out = bytearray(w * h * 3)
    for i in range(w * h):
        v = raw[2 * i] | raw[2 * i + 1] << 8
        out[3 * i] = (v >> 11) * 255 // 31
        out[3 * i + 1] = (v >> 5 & 63) * 255 // 63
        out[3 * i + 2] = (v & 31) * 255 // 31
    return bytes(out)


def write_ppm(path, w, h, pixels):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h) + pixels)


def grab(d):
    """The deck's screen as (w, h, rgb), also left in d.frame as a PPM."""
    target = d.frame.replace("\\", "/")
    if not d.fb:
        monitor(d.port, "screendump " + target)
        return read_ppm(d.frame)
    addr, w, h = int(d.fb[0], 0), int(d.fb[1]), int(d.fb[2])
    raw_path = d.frame[:-4] + ".fb"
    monitor(d.port, "pmemsave 0x%x %d %s" % (addr, w * h * 2, raw_path.replace("\\", "/")))
    with open(raw_path, "rb") as f:
        raw = f.read()
    if len(raw) != w * h * 2:
        raise ValueError("short frame buffer read")
    pixels = rgb565(raw, w, h)
    write_ppm(d.frame, w, h, pixels)
    return w, h, pixels


def crop(frame, rect):
    """The blocks of frame inside rect (x y w h in pixels), as a frame of their own."""
    (w, h), means = frame
    cols = w // BLOCK
    x0, y0, x1, y1 = (v // BLOCK for v in (rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3]))
    return (w, h, rect), [means[r * cols + c] for r in range(y0, y1) for c in range(x0, x1)]


def similarity(a, b):
    """The share of lit blocks that agree. Blocks dark in both frames are left
    out, or a mostly black screen would match a black one."""
    if a[0] != b[0]:
        return 0.0
    lit = [(x, y) for x, y in zip(a[1], b[1]) if max(x, y) > BLOCK_TOLERANCE]
    if not lit:
        return 1.0
    return sum(1 for x, y in lit if abs(x - y) <= BLOCK_TOLERANCE) / len(lit)


def lit_share(frame):
    return sum(1 for v in frame[1] if v > BLOCK_TOLERANCE) / len(frame[1])


def monitor(port, line, timeout=5.0):
    with socket.create_connection(("127.0.0.1", port), timeout) as s:
        s.settimeout(0.3)
        try:
            s.recv(65536)
        except OSError:
            pass
        s.sendall(line.encode() + b"\n")
        out = b""
        try:
            while True:
                b = s.recv(65536)
                if not b:
                    break
                out += b
        except OSError:
            pass
        return out


def native(path):
    """QEMU on Windows cannot open MSYS2 paths (/c/...)."""
    if sys.platform == "win32" and path.startswith("/") and len(path) > 2 and path[2] == "/":
        return path[1].upper() + ":" + path[2:]
    return path


class Lock:
    """The lane lock: a directory created with mkdir, as the lane scripts take it."""

    def __init__(self, path, owner):
        self.path, self.owner, self.held = native(path) if path else "", owner, False

    def take(self, wait):
        if not self.path:
            return True
        deadline = time.time() + wait
        while True:
            try:
                os.mkdir(self.path)
                with open(os.path.join(self.path, "owner"), "w") as f:
                    f.write(self.owner + "\n")
                self.held = True
                return True
            except FileExistsError:
                if time.time() >= deadline:
                    return False
                time.sleep(3)

    def release(self):
        if self.held:
            try:
                os.remove(os.path.join(self.path, "owner"))
            except OSError:
                pass
            os.rmdir(self.path)
            self.held = False

    def holder(self):
        try:
            with open(os.path.join(self.path, "owner")) as f:
                return f.read().strip()
        except OSError:
            return "?"


class Deck:
    def __init__(self, name, sec, root, here, out):
        def path(key):
            v = sec.get(key, "")
            return native(v if not v or os.path.isabs(v) or v.startswith("/") else os.path.join(root, v))
        self.name = name
        self.machine = sec["machine"]
        self.qemu = path("qemu")
        self.kernel = path("kernel")
        self.stick = path("stick")
        self.golden = native(os.path.join(here, sec["golden"]))
        self.golden2 = native(os.path.join(here, sec["golden2"])) if sec.get("golden2") else ""
        self.steps = [line.split() for line in sec.get("steps", "").splitlines() if line.strip()]
        self.timeout2 = float(sec.get("timeout2", "200"))
        self.region2 = [int(v) for v in sec.get("region2", "").split()]
        self.motion = sec.get("motion", "")
        self.env = {}
        for kv in shlex.split(sec.get("env", "")):
            k, v = kv.split("=", 1)
            self.env[k] = path_value(v, root)
        self.lock = Lock(sec.get("lock", ""), "deck_smoke-%d" % os.getpid())
        self.timeout = float(sec.get("timeout", "60"))
        self.settle = float(sec.get("settle", "10"))
        self.port = int(sec["port"])
        self.args = shlex.split(sec.get("args", ""))
        self.fb = sec.get("fb", "").split()
        self.log = os.path.join(out, name + ".log")
        self.frame = os.path.join(out, name + ".ppm")
        self.result, self.detail, self.seconds = "SKIP", "", 0.0

    def missing(self):
        for what in (self.qemu, self.kernel):
            if not os.path.isfile(what):
                return what
        return None

    def argv(self):
        a = [self.qemu, "-M", self.machine, "-kernel", self.kernel, "-display", "none",
             "-serial", "null", "-monitor", "tcp:127.0.0.1:%d,server=on,wait=off" % self.port]
        if self.stick:
            a += ["-drive", "if=none,id=usbstk,file=%s,format=raw,snapshot=on" % self.stick,
                  "-device", "usb-storage,drive=usbstk,port=1"]
        return a + self.args


def path_value(v, root):
    """An env value that names a file relative to the root becomes absolute."""
    if v and not os.path.isabs(v) and not v.startswith("/") and os.sep in v.replace("/", os.sep):
        cand = os.path.join(root, v)
        if os.path.exists(cand):
            return native(cand)
    return native(v)


def run_deck(d, record, tier2, lock_wait):
    gone = d.missing()
    if gone:
        d.detail = "no " + gone
        return
    target = d.golden2 if tier2 else d.golden
    if not record and not os.path.isfile(target):
        d.detail = "no golden frame (run with --record)"
        return
    if not d.lock.take(lock_wait):
        d.detail = "lock %s held by %s" % (d.lock.path, d.lock.holder())
        return
    env = dict(os.environ)
    env.update(d.env)
    start = time.time()
    with open(d.log, "wb") as log:
        q = subprocess.Popen(d.argv(), env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        d.result, d.detail = run_phases(d, q, start, record, tier2)
    finally:
        d.seconds = time.time() - start
        try:
            monitor(d.port, "quit")
        except OSError:
            pass
        try:
            q.wait(10)
        except subprocess.TimeoutExpired:
            q.kill()
            q.wait()
        d.lock.release()


def run_phases(d, q, start, record, tier2):
    if not (record and tier2):
        golden = None if record else block_means(*read_ppm(d.golden))
        result, detail = watch(d, q, golden, start, d.golden, d.timeout)
        if not tier2 or result not in ("PASS", "REC"):
            return result, detail
    play_steps(d, q, start)
    if q.poll() is not None:
        return "FAIL", "qemu exited %d during the steps" % q.returncode
    golden = None if record else block_means(*read_ppm(d.golden2))
    if golden and d.region2:
        golden = crop(golden, d.region2)
    result, detail = watch(d, q, golden, time.time(), d.golden2, d.timeout2, d.region2)
    if result != "PASS" or not d.motion:
        return result, detail
    return film_motion(d, q, detail)


def film_motion(d, q, detail):
    """Film the loaded deck MOTION_FRAMES times and let the playhead scorer
    say whether it plays."""
    stem = d.frame[:-4] + "-motion-"
    for i in range(MOTION_FRAMES):
        if i:
            time.sleep(MOTION_EVERY)
        for _ in range(5):
            if q.poll() is not None:
                return "FAIL", "qemu exited %d while filming the playhead" % q.returncode
            try:
                grab(d)
                break
            except (OSError, ValueError):
                time.sleep(POLL)
        else:
            return "FAIL", "no screen to film the playhead"
        os.replace(d.frame, "%s%d.ppm" % (stem, i))
    r = subprocess.run([sys.executable, SCORER, "--model", d.motion, stem + "*.ppm"],
                       capture_output=True, text=True)
    verdict = next((" ".join(line.split("VERDICT:", 1)[1].replace("*", "").split())
                    for line in r.stdout.splitlines()
                    if "VERDICT:" in line), (r.stderr.strip().splitlines() or ["no verdict"])[-1])
    return ("PASS" if verdict.startswith("MOTION") else "FAIL"), "%s; playhead %s" % (detail, verdict)


def play_steps(d, q, start):
    keysock = int(d.env.get("CDJ_PANEL_KEYSOCK", "0"))
    for step in d.steps:
        if q.poll() is not None:
            return
        if step[0] == "at":
            time.sleep(max(0.0, start + float(step[1]) - time.time()))
        elif step[0] == "wait":
            time.sleep(float(step[1]))
        elif step[0] == "key":
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.sendto(step[1].encode(), ("127.0.0.1", keysock))
        elif step[0] == "sendkey":
            monitor(d.port, "sendkey %s %s" % (step[1], step[2] if len(step) > 2 else "100"))
        else:
            raise ValueError("%s: unknown step '%s'" % (d.name, step[0]))


def watch(d, q, golden, start, out, timeout, region=None):
    """Poll the screen until it matches golden or, with no golden, until it has
    held still for d.settle seconds and is written to out. Times count from
    start."""
    last, still_since, best = None, None, 0.0
    deadline = start + (timeout if golden else timeout + d.settle)
    while time.time() < deadline:
        time.sleep(POLL)
        if q.poll() is not None:
            return "FAIL", "qemu exited %d" % q.returncode
        try:
            frame = block_means(*grab(d))
        except (OSError, ValueError):
            continue
        if region:
            frame = crop(frame, region)
        now = time.time() - start
        if golden is not None:
            best = max(best, similarity(frame, golden))
            if best >= MATCH:
                return "PASS", "screen matched at %.0f s" % now
            continue
        if last is not None and similarity(frame, last) >= MATCH and lit_share(frame) > 0.004:
            still_since = still_since or now
            if now - still_since >= d.settle:
                os.replace(d.frame, out)
                return "REC", "screen still from %.0f s" % still_since
        else:
            still_since = None
            with open(d.frame, "rb") as f, open("%s-%03d.ppm" % (d.frame[:-4], now), "wb") as g:
                g.write(f.read())
        last = frame
    if golden is not None:
        return "FAIL", "best match %.0f%% after %.0f s, frame kept" % (best * 100, timeout)
    return "FAIL", "screen never settled"


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("decks")
    ap.add_argument("--record", action="store_true")
    ap.add_argument("--tier2", action="store_true", help="run each deck's steps and check the screen they lead to")
    ap.add_argument("--only", default="")
    ap.add_argument("--lock-wait", type=float, default=20.0,
                    help="seconds to wait for a busy deck lock before skipping the deck")
    ap.add_argument("--out", default=os.path.join(os.environ.get("TMP", "/tmp"), "deck_smoke"))
    args = ap.parse_args()

    ini = configparser.ConfigParser()
    ini.read(args.decks)
    root = ini.defaults().get("root", os.path.dirname(os.path.abspath(args.decks)))
    os.makedirs(args.out, exist_ok=True)
    only = set(filter(None, args.only.split(",")))
    here = os.path.dirname(os.path.abspath(args.decks))
    decks = [Deck(n, ini[n], root, here, args.out) for n in ini.sections() if not only or n in only]
    if args.tier2:
        decks = [d for d in decks if d.steps and d.golden2]
    if args.record:
        for d in decks:
            os.makedirs(os.path.dirname(d.golden2 if args.tier2 else d.golden), exist_ok=True)

    start = time.time()
    threads = [threading.Thread(target=run_deck, args=(d, args.record, args.tier2, args.lock_wait)) for d in decks]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    for d in decks:
        print("%-10s %-4s %5.0f s  %s" % (d.name, d.result, d.seconds, d.detail))
    print("decks: %.0f s wall, logs in %s" % (time.time() - start, args.out))
    return 1 if any(d.result == "FAIL" for d in decks) else 0


if __name__ == "__main__":
    sys.exit(main())
