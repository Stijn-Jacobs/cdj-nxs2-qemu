#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Load a track: wait for the MY SETTINGS modal to come and go, then walk.

After boot the deck screen sits unchanged for about 8 s, the MY SETTINGS modal
appears at about 20.5 s and dismisses itself at about 25.6 s. The modal
swallows keys, so a walk on a fixed timer loses most of its presses. This
driver waits for the settled deck screen, then for the modal to appear and
clear, and only then walks. It does not press MENU to dismiss the modal: that
reaches an inert LOAD PREVIOUS TRACK view.

usage: load_track.py <tag>
env:   WALK DWELL POSTLOAD KEYBYTE KEYBITS FILMN MOTION_MS MAXWAIT POLL SHOTDIR
"""

import os
import socket
import struct
import subprocess
import sys
import time
import cdj_monsock

HERE = os.path.dirname(os.path.abspath(__file__))
TAG = sys.argv[1]
MON = "/tmp/cdj-%s-gui-mon.sock" % TAG
KEYSOCK = "/tmp/cdj-panel-keys-%s.sock" % TAG
# Needed early: the readiness wait reads guest RAM.
MAINMON = "/tmp/cdj-%s-main-mon.sock" % TAG
OUT = os.environ.get("SHOTDIR", "/tmp")

DWELL = float(os.environ.get("DWELL", "1.2"))
WALK = os.environ.get("WALK", "push,rot+1,push,rot+1,push,push")
POSTLOAD = float(os.environ.get("POSTLOAD", "4"))
MOTION_MS = float(os.environ.get("MOTION_MS", "1200"))
MAXWAIT = float(os.environ.get("MAXWAIT", "45"))
POLL = float(os.environ.get("POLL", "0.3"))
FILMN = int(os.environ.get("FILMN", "1"))
KEYBYTE = int(os.environ.get("KEYBYTE", "0x12"), 0)
KEYBITS = [int(b, 0) for b in
           os.environ.get("KEYBITS", "0x10").split(",")]

W, H = 800, 480
R_MODAL = (190, 135, 610, 200)

# All waits are machine time. MAIN publishes its virtual ms to CDJ_VCLOCK_FILE;
# when readable, the driver follows it (an -icount MAIN runs slower than the
# wall). Otherwise TIMESCALE scales the wall clock.
VCLOCK = os.environ.get("CDJ_VCLOCK_FILE", "/tmp/cdj-%s-vclock" % TAG)
TIMESCALE = float(os.environ.get("TIMESCALE", "1"))
_WALL0 = time.time()


_vlast = [None, 0.0]            # last virtual reading, wall time it changed
VSTALE = float(os.environ.get("VSTALE", "30"))


def vnow():
    """Seconds of machine time: MAIN's virtual clock when published, else scaled wall.

    A clock that stops (a dead or wedged MAIN) must not hang every wait in this
    driver forever, so after VSTALE wall seconds without change the wall takes
    over from the last reading."""
    try:
        with open(VCLOCK, "rb") as f:
            v = int(f.read(20)) / 1000.0
    except (OSError, ValueError):
        return (time.time() - _WALL0) / TIMESCALE
    now = time.time()
    if v != _vlast[0]:
        _vlast[0], _vlast[1] = v, now
        return v
    stale = now - _vlast[1]
    return v + (stale - VSTALE) / TIMESCALE if stale > VSTALE else v


def vsleep(dt):
    """Sleep dt seconds of machine time."""
    end = vnow() + dt
    while True:
        left = end - vnow()
        if left <= 0:
            return
        time.sleep(min(max(left * 0.5, 0.01), 0.2))

# Frames worth a filename of their own; see grab().
KEEP = {"verdict", "pre", "w-start"}

# The modal measures about 0.857 in R_MODAL and the plain deck about 0.129.
MODAL_ON = 0.60
MODAL_OFF = 0.30


def key(spec):
    env = dict(os.environ, CDJ_PANEL_KEYSOCK=KEYSOCK)
    arg = {"push": "0x11:0x01", "rot+1": "rot:0x0e:+1",
           "rot-1": "rot:0x0e:-1"}.get(spec, spec)
    subprocess.run([sys.executable, os.path.join(HERE, "panel_key.py"), arg],
                   env=env, check=False)


def grab(name, timeout=3.0):
    # Polling grabs reuse one file (an 800x480 PPM is 1.15 MB and a run takes
    # hundreds); only frames in KEEP or named b* get their own name.
    if name not in KEEP and not name.startswith("b"):
        name = "scratch"
    path = os.path.join(OUT, "z89-%s-%s.ppm" % (TAG, name))
    try:
        os.unlink(path)
    except OSError:
        pass
    # Read the monitor's answer before closing. Closing with unread bytes is an
    # abortive (RST) close on Windows and can leave the TCP monitor dead for the
    # rest of the run.
    why = "ok"
    try:
        s = cdj_monsock.connect(MON)
        try:
            if not _read_to_prompt(s, 3.0):
                why = "no greeting"
            s.sendall(b"screendump " + path.encode() + b"\n")
            if not _read_to_prompt(s, max(timeout, 10.0)):
                why = "no prompt after screendump"
            try:
                s.shutdown(socket.SHUT_WR)
            except OSError:
                pass
        finally:
            s.close()
    except OSError as e:
        why = "connect: %s" % e
    if why == "ok" and not os.path.exists(path):
        why = "prompt came back, no file"
    _film_health(name, why)
    return path if why == "ok" or os.path.exists(path) else None


_PROMPT = b"(qemu) "
_film = {"good": 0, "streak": 0, "lost": False}


def _read_to_prompt(s, timeout):
    """Drain the monitor until its prompt ends the stream. True if it did.

    The HMP monitor echoes the command, prints the answer, then prints the
    prompt again; the prompt is the only sign the command has finished."""
    s.settimeout(0.25)
    buf, end = b"", time.time() + timeout
    while time.time() < end:
        try:
            part = s.recv(65536)
        except socket.timeout:
            continue
        if not part:
            return False
        buf += part
        if buf.rstrip(b"\r\n").endswith(_PROMPT.rstrip()):
            return True
    return False


def _film_health(name, why):
    """Say so, once, when the film dies -- 0 frames must not read as a result."""
    if why == "ok":
        _film["good"] += 1
        _film["streak"] = 0
        return
    _film["streak"] += 1
    if _film["streak"] == 5 and not _film["lost"]:
        _film["lost"] = True
        print("[%s] FILM LOST at %.1f s after %d good grabs (%s, last %s) --"
              " instrument failure, not a negative"
              % (TAG, vnow(), _film["good"], why, name))


def guidump(name):
    """Save a slice of the GUI processor's RAM through the monitor we already own.

    Whether the overview waveform is drawn is decided in GUI memory, not in
    MAIN's RAM or the frame it publishes. pmemsave goes through the same
    monitor as screendump.

    GUIDUMP="<addr>:<size>[,<addr>:<size>...]" -- e.g. 0x1C000000:0x280000 for the
    on-chip RAM the display structures live in. Off by default; each region costs
    its own size on disk per call.
    """
    spec = os.environ.get("GUIDUMP", "")
    if not spec:
        return []
    out = []
    for i, part in enumerate(s for s in spec.split(",") if s):
        addr, _, size = part.partition(":")
        addr, size = int(addr, 0), int(size or "0x1000", 0)
        path = os.path.join(OUT, "gui-%s-%s-%d.bin" % (TAG, name, i))
        try:
            os.unlink(path)
        except OSError:
            pass
        try:
            sk = cdj_monsock.connect(MON)
            sk.settimeout(0.25)
            try:
                sk.recv(65536)
            except OSError:
                pass
            # Quote the path: HMP parses arguments as expressions.
            cmd = 'pmemsave 0x%x %d \"%s\"\n' % (addr, size, path)
            sk.sendall(cmd.encode())
            # The monitor returns before the file is complete; wait for a stable
            # size.
            deadline, last, stable = time.time() + 20.0, -1, 0
            while time.time() < deadline:
                try:
                    got = os.path.getsize(path)
                except OSError:
                    got = -1
                if got == size and got == last:
                    stable += 1
                    if stable >= 2:
                        break
                else:
                    stable = 0
                last = got
                time.sleep(0.05)
            # Read the answer on the same socket; a second connection wedged the
            # monitor. HMP echoes the command a character at a time, so drain
            # until quiet.
            chunks = []
            for _ in range(20):
                try:
                    part = sk.recv(65536)
                except OSError:
                    break
                if not part:
                    break
                chunks.append(part)
            raw = b"".join(chunks).decode("latin-1", "replace")
            reply = "".join(c for c in raw if c == " " or c.isprintable())
            sk.close()
        except OSError:
            continue
        if os.path.exists(path) and os.path.getsize(path) == size:
            out.append(path)
        else:
            got = os.path.getsize(path) if os.path.exists(path) else -1
            print("[%s]   guidump %s SHORT/MISSING (got %d of %d) monitor said: %s"
                  % (TAG, name, got, size,
                     " | ".join(reply.split())[-400:]))
    if out:
        print("[%s]   guidump %s: %s" % (TAG, name, " ".join(out)))
    return out


def pixels(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except (OSError, TypeError):
        return None
    off, fields = 0, 0
    while fields < 4 and off < len(data):
        while off < len(data) and data[off:off + 1].isspace():
            off += 1
        while off < len(data) and not data[off:off + 1].isspace():
            off += 1
        fields += 1
    return data[off + 1:]


def density(px, rect):
    x0, y0, x1, y1 = rect
    hit = tot = 0
    for y in range(y0, y1, 3):
        base = (y * W + x0) * 3
        row = px[base:base + (x1 - x0) * 3]
        tot += len(row)
        hit += sum(1 for b in row if b)
    return (hit / tot) if tot else 0.0


def nonzero(px):
    return sum(1 for b in px if b) if px else 0


# Right half of the title bar: the load verdict. A real title spans the bar and
# puts the key (e.g. "Em") at the far right in white; "Not Loaded." is short and
# leaves this rectangle empty. Whole-frame counts do not separate the two.
R_TITLE_R = (430, 8, 780, 42)

# The REMAIN readout, a second axis: the title bar can show a track the deck
# has no position in. It reads `--:--.---` (about 161 bright px) without a
# position and e.g. `02:26.400` (about 815) with one; the E-8302 modal blanks it
# (about 22). REMAIN_ON sits between the two states.
R_REMAIN = (285, 315, 495, 370)

REMAIN_ON = 400


def bright(px, rect, thr=150):
    """Count near-white pixels in rect -- text on a filled bar."""
    x0, y0, x1, y1 = rect
    n = 0
    for y in range(y0, y1, 2):
        base = (y * W + x0) * 3
        row = px[base:base + (x1 - x0) * 3]
        for i in range(0, len(row) - 2, 3):
            if row[i] > thr and row[i + 1] > thr and row[i + 2] > thr:
                n += 1
    return n


def loaded(name):
    """(is_loaded, bright_px). Retries the grab: a FAILED screendump must not be
    scored as 'not loaded'.

    A monitor screendump occasionally yields no file at all; treating that as
    "not loaded" would trigger a spurious retry push on a deck that had
    already loaded.
    """
    for _ in range(3):
        px = pixels(grab(name))
        if px and len(px) >= W * H * 3:
            b = bright(px, R_TITLE_R)
            # Report the REMAIN box too; see R_REMAIN.
            globals()["LAST_REMAIN"] = bright(px, R_REMAIN)
            return b >= 40, b
        time.sleep(0.2)
    return None, -1            # -1 marks "could not read", not "empty"


def modal_density(name):
    px = pixels(grab(name))
    if not px or len(px) < W * H * 3:
        return None
    return density(px, R_MODAL)


def wait_modal(want_on, deadline, what):
    """Poll until the modal is present (want_on) or absent. True if it happened."""
    n = 0
    while vnow() < deadline:
        d = modal_density("%s%02d" % (what, n))
        n += 1
        if d is not None:
            if want_on and d > MODAL_ON:
                return True
            if not want_on and d < MODAL_OFF:
                return True
        time.sleep(POLL)
    return False


def wait_deck(deadline):
    """Poll until the quiet, settled deck screen is up.

    Its signature: whole-frame non-zero count about 185200, modal-rectangle
    density about 0.129. The band below is deliberately wide; it only has to
    exclude the bright boot panel and the modal.
    """
    n = 0
    while vnow() < deadline:
        px = pixels(grab("k%02d" % n))
        n += 1
        if px and len(px) >= W * H * 3:
            if density(px, R_MODAL) < MODAL_OFF and 150000 < nonzero(px) < 220000:
                return True
        time.sleep(POLL)
    return False


def shot(name):
    return nonzero(pixels(grab(name)))


t_end = vnow() + MAXWAIT

# Wait for the settled deck frame before watching for the modal: during early
# boot the panel is bright enough to pass the modal threshold.
if wait_deck(t_end):
    print("[%s] deck settled at %.2f s" % (TAG, vnow() - (t_end - MAXWAIT)))
else:
    print("[%s] DECK NEVER SETTLED -- anchor did not fire" % TAG)


# Keys pressed on the settled deck before the modal. MENU here is what starts
# the load; pressed after the modal, loads often hang in "NOW LOADING...".
# WALK_PRE_DELAY (default 0) delays it after deck-settle.
vsleep(float(os.environ.get("WALK_PRE_DELAY", "0")))
for step in (s.strip() for s in os.environ.get("WALK_PRE", "menu").split(",")):
    if step:
        key(step)
        vsleep(DWELL)

# NOMODAL=1: the medium has no MYSETTING*.DAT, so no modal is coming. Without
# it the wait costs the full MAXWAIT.
if os.environ.get("NOMODAL"):
    print("[%s] NOMODAL: settings modal not expected, skipping the anchor" % TAG)
elif wait_modal(True, t_end, "m"):
    print("[%s] modal up at %.2f s" % (TAG, vnow() - (t_end - MAXWAIT)))
    # Deliberately NOT pressing MENU -- see the module docstring.
    if wait_modal(False, t_end, "d"):
        print("[%s] modal self-dismissed at %.2f s"
              % (TAG, vnow() - (t_end - MAXWAIT)))
    else:
        print("[%s] modal never cleared -- run is suspect" % TAG)
else:
    # Not fatal, but the run should be discarded.
    print("[%s] MODAL NEVER APPEARED -- anchor did not fire" % TAG)


def wait_settled(prev, timeout, name):
    """After a key: wait until the frame has CHANGED and then stopped changing.

    A fixed dwell is sometimes too short, and the next key then lands
    mid-repaint. Returns the settled count, or the last one seen if nothing
    changed within `timeout`, since a key that does nothing must not stall
    the walk.
    """
    deadline = vnow() + timeout
    last, stable = None, 0
    while vnow() < deadline:
        c = shot(name)
        if c is not None and c == last:
            stable += 1
            if stable >= 2 and c != prev:
                return c
        else:
            stable = 0
        last = c
    return last


cur = shot("w-start")
for i, step in enumerate(s.strip() for s in WALK.split(",")):
    if step:
        key(step)
        cur = wait_settled(cur, DWELL * 2, "w%d" % i)


# Wait for the frame to stop changing rather than a fixed POSTLOAD; the load is
# asynchronous and a fixed wait photographs "NOW LOADING...". The whole-frame
# count does not classify the outcome, so it is not used as a verdict.
LOADWAIT = float(os.environ.get("LOADWAIT", "30"))
STABLE_N = int(os.environ.get("STABLE_N", "6"))
RETRIES = int(os.environ.get("RETRIES", "4"))

deadline = vnow() + LOADWAIT
last, stable, t0 = None, 0, vnow()
while vnow() < deadline:
    c = shot("L")
    if c is not None and c == last:
        stable += 1
        if stable >= STABLE_N:
            break
    else:
        stable = 0
    last = c
pre = last
print("[%s] load settled after %.1f s (stable=%d)"
      % (TAG, vnow() - t0, stable))

# A settled frame is not a ready deck: PLAY is ignored until the deck-state
# word reaches 0x11. READY_ADDR is read over the MAIN monitor (memsave does not
# halt the CPU). READY_WAIT=0 skips the wait.
READY_ADDR = int(os.environ.get("READY_ADDR", "0x0B5125B8"), 0)
READY_VAL = int(os.environ.get("READY_VAL", "0x11"), 0)
READY_WAIT = float(os.environ.get("READY_WAIT", "40"))


def peek32(addr):
    """One little-endian word out of guest RAM, or None if unreadable."""
    # On Windows the monitor is a TCP port, so ask the endpoint, not the
    # filesystem.
    if not cdj_monsock.is_up(MAINMON):
        return None
    path = os.path.join(OUT, "peek-%s.bin" % TAG)
    try:
        os.unlink(path)
    except OSError:
        pass
    try:
        s = cdj_monsock.connect(MAINMON, 5.0)
        time.sleep(0.1)
        try:
            s.recv(65536)
        except socket.timeout:
            pass
        s.sendall(b"memsave 0x%x 4 \"%s\"\n" % (addr, path.encode()))
        time.sleep(0.3)
        s.close()
    except OSError:
        return None
    try:
        with open(path, "rb") as f:
            return struct.unpack("<I", f.read(4))[0]
    except (OSError, struct.error):
        return None


if READY_WAIT > 0:
    end, t1, seen = vnow() + READY_WAIT, vnow(), None
    while vnow() < end:
        seen = peek32(READY_ADDR)
        if seen is None or seen == READY_VAL:
            break
        vsleep(0.5)
    # A monitor that was never up, a deck already ready, and a deck that never
    # got there are reported differently.
    if seen is None:
        verdict = "NO MONITOR -- not waited"
    elif seen == READY_VAL:
        verdict = "ready (*0x%08x = 0x%x)" % (READY_ADDR, seen)
    else:
        verdict = "TIMED OUT -- *0x%08x = 0x%x, wanted 0x%x" % (
            READY_ADDR, seen, READY_VAL)
    print("[%s] deck %s after %.1f s" % (TAG, verdict, vnow() - t1))


def settle_load(tag_name):
    """Wait until the frame stops changing, then return its count."""
    end = vnow() + LOADWAIT
    l, s = None, 0
    while vnow() < end:
        c = shot(tag_name)
        if c is not None and c == l:
            s += 1
            if s >= STABLE_N:
                break
        else:
            s = 0
        l = c
    return l


# Retry only when the title bar is empty and the frame has settled; pressing
# into a load still in progress breaks it. The title arrives after the frame
# settles, so poll for it rather than taking one shot.
TITLEWAIT = float(os.environ.get("TITLEWAIT", "12"))

t_title = vnow()
ok, bpx = loaded("verdict")
while not ok and vnow() - t_title < TITLEWAIT:
    vsleep(0.4)
    ok, bpx = loaded("verdict")
if ok:
    print("[%s] title arrived %.1f s after settle" % (TAG, vnow() - t_title))

# After a failed load the deck screen is up and the encoder does nothing, so
# retry with BROWSE (a toggle back to the list), then push.
RETRY_KEYS = os.environ.get("RETRY_KEYS", "0x14:0x01,push").split(",")

tries = 0
while not ok and tries < RETRIES:
    tries += 1
    for rk in (s.strip() for s in RETRY_KEYS):
        if rk:
            key(rk)
            vsleep(DWELL)
    pre = settle_load("r%d" % tries)
    ok, bpx = loaded("verdict%d" % tries)
    print("[%s]   retry %d -> %s (bright %d, count %s)"
          % (TAG, tries, "LOADED" if ok else "not-loaded", bpx, pre))
rem = globals().get("LAST_REMAIN", -1)
print("[%s] VERDICT %s  POSITION %s (title-right bright px = %d, "
      "REMAIN px = %s, count = %s, retries = %d)"
      % (TAG, "LOADED" if ok else "not-loaded",
         "yes" if rem >= REMAIN_ON else "no", bpx, rem, pre, tries))
print("[%s] NOTE: these are TWO DISPLAY axes and neither is an audio test. "
      "VERDICT is the title bar; POSITION is the REMAIN readout, which reads "
      "no on a freshly loaded deck and yes only after a search key. A deck "
      "can pass VERDICT without having read any audio." % TAG)
print("byte 0x%02x  pre=%s%s"
      % (KEYBYTE, pre, "" if (pre or 0) > 190000 else "   <-- NO TRACK LOADED"))

# KEYDUR: how long the key is held, in ms. panel_key.py defaults to a 150 ms
# tap; on a CDJ a hold is a different gesture (holding CUE previews).
KEYDUR = os.environ.get("KEYDUR", "")

# RAMSNAP="<addr>:<len>": memsave a slice of MAIN's RAM around each keypress via
# MAIN's monitor (needs MAIN_MON=1). Off by default.
RAMSNAP = os.environ.get("RAMSNAP", "")
def ramsnap(name):
    """memsave a region. Returns the path, or None if the monitor is not up."""
    if not RAMSNAP or not cdj_monsock.is_up(MAINMON):
        return None
    addr, length = (int(x, 0) for x in RAMSNAP.split(":"))
    path = os.path.join(OUT, "ram-%s-%s.bin" % (TAG, name))
    try:
        s = cdj_monsock.connect(MAINMON, 10.0)
        time.sleep(0.3)
        try:
            s.recv(65536)
        except socket.timeout:
            pass
        s.sendall(b"memsave 0x%x %d \"%s\"\n" % (addr, length, path.encode()))
        time.sleep(1.0)
        s.close()
    except OSError as e:
        print("[%s]   ramsnap %s failed: %s" % (TAG, name, e))
        return None
    if not os.path.exists(path) or os.path.getsize(path) != length:
        print("[%s]   ramsnap %s SHORT/MISSING" % (TAG, name))
        return None
    return path


# KEYSEQ="0x12:0x10,0x10:0x01": a key sequence across different report bytes,
# filmed after each step. Overrides KEYBYTE/KEYBITS.
KEYSEQ = [s for s in os.environ.get("KEYSEQ", "").split(",") if s]
STEPS = KEYSEQ or ["0x%02x:0x%02x" % (KEYBYTE, b) for b in KEYBITS]

# The step index is in every artefact name so repeated keys do not overwrite
# each other's films.
for si, step in enumerate(STEPS):
    spec = step
    kb = int(spec.split(":")[0], 0)
    bit = int(spec.split(":")[1], 0)
    # Two snapshots before the press give the idle churn baseline; only words
    # that move across the press and not across the idle interval are the key's.
    guidump("pre%d" % si)
    before_a = ramsnap("idle0-%02x" % bit if len(STEPS) == 1 else "idle0-%d-%02x" % (si, bit))
    vsleep(0.6)
    before_b = ramsnap("idle1-%02x" % bit if len(STEPS) == 1 else "idle1-%d-%02x" % (si, bit))
    key(spec + (":" + KEYDUR if KEYDUR else ""))
    vsleep(0.6)
    after = ramsnap("after-%02x" % bit if len(STEPS) == 1 else "after-%d-%02x" % (si, bit))
    # A fourth snapshot one idle interval later tells a one-shot change from a
    # counter that keeps running.
    vsleep(0.6)
    after2 = ramsnap("after2-%02x" % bit if len(STEPS) == 1 else "after2-%d-%02x" % (si, bit))
    if before_a and before_b and after:
        print("[%s]   ramsnap ok: %s %s %s %s"
              % (TAG, before_a, before_b, after, after2 or "(no after2)"))
    frames = []
    for n in range(FILMN + 1):
        # The name must start with "b" or grab() routes it to the scratch file.
        frames.append(shot("b%02x-%02x-s%d-f%d" % (kb, bit, si, n)))
        if n < FILMN:
            vsleep(MOTION_MS / 1000.0)
    guidump("s%d" % si)
    uniq = len({f for f in frames if f is not None})
    if uniq > 2:
        flag = "  *** SUSTAINED MOTION (%d distinct) ***" % uniq
    elif uniq == 2:
        flag = "  changed once"
    elif frames[0] is not None and pre is not None and frames[0] != pre:
        flag = "  (static change)"
    else:
        flag = ""
    print("  key 0x%02x:0x%02x  %s%s" % (kb, bit, " ".join(str(f) for f in frames), flag))
