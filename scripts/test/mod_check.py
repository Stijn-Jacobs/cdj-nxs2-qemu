#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check a modified MAIN image against the stock one, screen by screen.

One-window decks (MODEL_LAUNCH=deck) only. The image under test runs once and
the installed stock image twice, side by side, on the same stick. Every run
goes through the same steps and takes its pictures where the deck stands
still (the idle screen, the browser, the track cued, the menus), so two stock
runs agree with each other. Where they still disagree (a blinking digit, the
stick's access lamp) is noise and is not judged. The image under test passes
when every picture matches stock outside that noise and outside the regions
the change is meant to touch (--allow).

    mod_check.py <main image> <out dir> [--allow X0,Y0,X1,Y1 ...]
                 [--ignore X0,Y0,X1,Y1 ...] [--model ID] [--media IMG]
                 [--max-noise PCT] [--fresh]

Each run is the deck ./start.sh starts (launcher/deck.py), headless and
without sound, with the image under test in place of the installed MAIN and
the sites the launcher pokes looked up in that image (launcher/sites.py).
Knobs the image under test runs without (the pinned-only ones, when not all
its sites are found) are left out of the stock runs too. Stock is booted
once to the model's idle point (MODEL_IDLE_S after its monitor comes up) and
saved there; both stock runs restore that save. It is the point
SNAPSHOT=idle NOSOUND=1 ./start.sh keeps (launcher/deck.py point_dir), so
either reuses the other's while the QEMU binary, the stock image, the stick
and the DSP module stay the same. --fresh boots and saves it again. The
image under test always boots.

The steps: the idle screen; the profile's MODEL_LOAD_STEPS, with a picture
after every wait but the last (the track then plays); PLAY/PAUSE, then
BROWSE, one detent down and ROTARY PUSH, which load the next track of the
list while the deck is paused, so it stands at its cue point and nothing of it
has played; TIME MODE and back; MENU held for UTILITY, the select knob three
detents down, MENU again; INFO; TAG LIST. Keys are pressed by name through
the model's face (app/faces/<MODEL_APP_FACE>.json, the key table the virtual
deck sends from). A point whose key the face does not wire is skipped and
listed as skipped; without the paused track every later point is skipped too,
as a playing deck never shows the same screen twice.

Every point is taken as FRAMES pictures, FRAME_GAP_S apart, because a stopped
deck blinks; an image-under-test frame counts as equal when it matches any
stock frame of the point. Per point:

  same          equal to stock outside the stock noise
  expected      differs from stock only inside the --allow regions
  DIFFERS       differs from stock elsewhere
  unstable      the stock runs disagree over more than --max-noise percent of
                the screen (an error message in one run): not judged
  missing       a run has no picture for the point

RESULT is FAIL when a point DIFFERS or the image under test is missing a
picture or its QEMU stopped; INCONCLUSIVE when a point is unstable or a stock
run failed (run the check again); PASS otherwise. Exit status 0 PASS, 1 FAIL,
3 INCONCLUSIVE, 2 when the check cannot run.

<out dir> gets a folder of PNGs per run (image, stock1, stock2) with each
run's QEMU and serial log, diff/<point>.png (stock in grey, differences red,
differences inside the stock noise yellow, --allow regions outlined green,
--ignore regions blue) and summary.txt, which is also printed.

QEMU comes from MAIN_QEMU or QEMU_BUILD, as for ./start.sh; MODULE=none runs
without the DSP module.
"""

import argparse
import os
import socket
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EMU = os.path.dirname(os.path.dirname(HERE))
for _p in (EMU, os.path.join(EMU, "scripts", "run")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import snapshot_deck  # noqa: E402
from launcher import deck, firmware, host, model, sites, snapshot  # noqa: E402
from launcher.layout import Layout  # noqa: E402

FRAMES = 4
FRAME_GAP_S = 0.5
# A restored deck needs a moment before its screen is drawn again; a booted
# one gets the same pause, so both start their steps alike.
SETTLE_S = 5
# One detent per datagram: several within one panel frame are misread.
DETENT_GAP_S = 0.4
PRESS_MS = 300
RESTORE_TIMEOUT_S = 90
MONITOR_TIMEOUT_S = 120
SAVE_WAIT_S = 600
MAX_NOISE_PCT = 2.0
# Stock noise is widened by this many pixels each way, so an anti-aliased
# edge next to a blinking digit is not counted against the image under test.
NOISE_GROW = 2

RUNS = ("image", "stock1", "stock2")

# The keys a step presses, by the names the faces give them.
PLAY = ("PLAY/PAUSE",)
BROWSE = ("BROWSE",)
ROTARY_PUSH = ("ROTARY PUSH",)
# Seconds from the push that loads the paused track to its picture.
PAUSED_LOAD_S = 15
TIME_MODE = ("TIME MODE", "TIME MODE/AUTO CUE")
MENU = ("MENU", "MENU/UTILITY")
INFO = ("INFO",)
TAG_LIST = ("TAG LIST",)
TURN = "select knob"


class CheckError(Exception):
    """The check cannot run: the reason, for the user."""


# -- the steps -----------------------------------------------------------------

def parse_box(text):
    """'x0,y0,x1,y1' (pixels, x1 and y1 exclusive) as a tuple."""
    try:
        box = tuple(int(v) for v in text.split(","))
    except ValueError:
        box = ()
    if len(box) != 4 or box[0] >= box[2] or box[1] >= box[3] or min(box) < 0:
        raise argparse.ArgumentTypeError("%r is not x0,y0,x1,y1 with x0 < x1 and y0 < y1" % text)
    return box


def load_steps(text):
    """MODEL_LOAD_STEPS as steps, with a picture after every wait but the
    last: until then the deck waits on a still screen (the browser, a list),
    after the last one the loaded track plays (the 'loaded' point of
    launcher/snapshot.py)."""
    try:
        parsed = snapshot_deck.parse_steps(text)
    except ValueError as e:
        raise CheckError(str(e))
    last_wait = max([i for i, s in enumerate(parsed) if s[0] == "wait"], default=-1)
    steps = []
    for i, s in enumerate(parsed):
        steps.append(s)
        if s[0] == "wait" and i != last_wait:
            steps.append(("shot", "load"))
    return steps


def find_key(face, names):
    """The face's wired key with one of `names`, else None."""
    if face is None:
        return None
    for k in face.keys:
        if k.name in names and k.wired:
            return k
    return None


def press(key, ms=PRESS_MS):
    return ("key", "0x%02x:%d:%d:or" % (key.off, key.mask, ms))


def turn(face, detents):
    one = face.turn_datagram(1 if detents > 0 else -1)
    steps = []
    for i in range(abs(detents)):
        if i:
            steps.append(("wait", DETENT_GAP_S))
        steps.append(("key", one))
    return steps


def plan(m, face):
    """(steps, skipped) for model `m` with its face (None when it has none):
    the steps every run goes through, pictures named in order, and the
    points left out for a key the face does not wire."""
    if not m.load_steps:
        raise CheckError("models/%s.conf sets no MODEL_LOAD_STEPS to load a track with" % m.id)
    keys = {"PLAY/PAUSE": find_key(face, PLAY), "BROWSE": find_key(face, BROWSE),
            "ROTARY PUSH": find_key(face, ROTARY_PUSH),
            "TIME MODE": find_key(face, TIME_MODE),
            "MENU": find_key(face, MENU), "INFO": find_key(face, INFO),
            "TAG LIST": find_key(face, TAG_LIST)}
    knob = face is not None and face.rotary_off is not None

    def block(name, needs, build):
        missing = [n for n in needs if (n == TURN and not knob) or (n != TURN and keys[n] is None)]
        if not paused and name != "paused":
            missing = missing or ["a paused track"]
        if missing:
            skipped.append((name, missing))
            return []
        return build()

    skipped = []
    paused = True
    steps = [("wait", SETTLE_S), ("shot", "idle")]
    steps += load_steps(m.load_steps)
    # The first track plays for as long as the profile waits, and how far it
    # gets differs from run to run; the overview waveform shows how far (its
    # highlight markers go out over the played part and stay out, after a
    # jump back and after reloading the same track). So the deck is paused and
    # the next track of the list is loaded: it stands at its cue point with
    # nothing played, the same in every run. (CUE does not do: on the
    # CDJ-900NXS it changed nothing while the track played, and pressed while
    # paused it sets a new cue point wherever the pause fell.)
    paused_steps = block("paused", ["PLAY/PAUSE", "BROWSE", TURN, "ROTARY PUSH"], lambda: [
        press(keys["PLAY/PAUSE"]), ("wait", 2), press(keys["BROWSE"]), ("wait", 3)] + turn(face, 1) + [
        ("wait", 2), press(keys["ROTARY PUSH"]), ("wait", PAUSED_LOAD_S), ("shot", "paused")])
    paused = bool(paused_steps)
    steps += paused_steps
    steps += block("time-mode", ["TIME MODE"], lambda: [
        press(keys["TIME MODE"]), ("wait", 3), ("shot", "time-mode"), press(keys["TIME MODE"]), ("wait", 3)])
    steps += block("utility", ["MENU", TURN], lambda: [
        ("key", keys["MENU"].datagram("hold")), ("wait", 3), ("key", keys["MENU"].datagram("rel")), ("wait", 4),
        ("shot", "utility")] + turn(face, 3) + [("wait", 2), ("shot", "utility-down"),
                                                press(keys["MENU"]), ("wait", 3)])
    steps += block("info", ["INFO"], lambda: [press(keys["INFO"]), ("wait", 4), ("shot", "info")])
    steps += block("tag-list", ["TAG LIST"], lambda: [press(keys["TAG LIST"]), ("wait", 4), ("shot", "tag-list")])
    return number_shots(steps), skipped


def number_shots(steps):
    """Pictures named 01-idle, 02-load, 03-load ... in the order taken."""
    out, n = [], 0
    for s in steps:
        if s[0] == "shot":
            n += 1
            s = ("shot", "%02d-%s" % (n, s[1]))
        out.append(s)
    return out


def shots(steps):
    return [s[1] for s in steps if s[0] == "shot"]


# -- judging -------------------------------------------------------------------

def diff_mask(a, b):
    from PIL import ImageChops
    return ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v else 0)


def blank(mask, boxes):
    """`mask` with the boxes cleared."""
    from PIL import ImageDraw
    m = mask.copy()
    draw = ImageDraw.Draw(m)
    for x0, y0, x1, y1 in boxes:
        draw.rectangle((x0, y0, x1 - 1, y1 - 1), fill=0)
    return m


def count(mask):
    return mask.histogram()[255]


def noise_mask(stock_frames, ignore=()):
    """Every pixel on which two stock frames of a point differ (within a run:
    blinking; between runs: whatever the two runs did differently), widened
    by NOISE_GROW. None when there is nothing to compare."""
    from PIL import ImageChops, ImageFilter
    if not stock_frames:
        return None
    noise = None
    for i, a in enumerate(stock_frames):
        for b in stock_frames[i + 1:]:
            d = diff_mask(a, b)
            noise = d if noise is None else ImageChops.lighter(noise, d)
    if noise is None:
        from PIL import Image
        noise = Image.new("L", stock_frames[0].size, 0)
    if NOISE_GROW and count(noise):
        noise = noise.filter(ImageFilter.MaxFilter(2 * NOISE_GROW + 1))
    return blank(noise, ignore)


def judge_point(image_frames, stock_runs, allow=(), ignore=(), max_noise_pct=MAX_NOISE_PCT):
    """The verdict on one point: a dict with verdict, the pixels the image
    under test differs by outside the stock noise (differ) and inside it
    (in_noise), the box of the judged differences, the stock noise in pixels,
    and the masks for the picture. stock_runs is one list of frames per stock
    run."""
    from PIL import ImageChops
    if not image_frames or any(len(run) == 0 for run in stock_runs) or len(stock_runs) < 2:
        return {"verdict": "missing"}
    stock = [f for run in stock_runs for f in run]
    size = stock[0].size
    if any(f.size != size for f in image_frames + stock):
        return {"verdict": "DIFFERS", "differ": size[0] * size[1], "box": (0, 0) + size,
                "in_noise": 0, "noise": 0, "note": "the screen sizes differ"}
    noise = noise_mask(stock, ignore)
    worst = None
    for a in image_frames:
        best = None
        for b in stock:
            raw = blank(diff_mask(a, b), ignore)
            judged = ImageChops.subtract(raw, noise)
            n = count(blank(judged, allow))
            if best is None or n < best[0]:
                best = (n, raw, judged, b)
        if worst is None or best[0] > worst[0]:
            worst = best
    n_out, raw, judged, frame = worst
    noise_px = count(noise)
    result = {"differ": n_out, "in_allow": count(judged) - n_out, "in_noise": count(raw) - count(judged),
              "noise": noise_px, "box": None, "masks": (raw, judged, noise), "stock_frame": frame}
    if noise_px * 100.0 > max_noise_pct * size[0] * size[1]:
        result["verdict"] = "unstable"
    elif n_out:
        result["verdict"] = "DIFFERS"
        result["box"] = blank(judged, allow).getbbox()
    elif result["in_allow"]:
        result["verdict"] = "expected"
        result["box"] = judged.getbbox()
    else:
        result["verdict"] = "same"
    return result


def draw_diff(result, allow, ignore, path):
    """The point's picture: stock in grey, judged differences red,
    differences inside the stock noise yellow, regions outlined."""
    from PIL import Image, ImageChops, ImageDraw
    raw, judged, _noise = result["masks"]
    base = result["stock_frame"].convert("L").point(lambda v: v // 2).convert("RGB")
    out = Image.composite(Image.new("RGB", base.size, (255, 220, 0)), base, ImageChops.subtract(raw, judged))
    out = Image.composite(Image.new("RGB", base.size, (255, 0, 0)), out, judged)
    draw = ImageDraw.Draw(out)
    for boxes, colour in ((allow, (0, 255, 0)), (ignore, (0, 128, 255))):
        for x0, y0, x1, y1 in boxes:
            draw.rectangle((x0, y0, x1 - 1, y1 - 1), outline=colour)
    out.save(path)


def result_of(rows, failed_runs):
    """PASS, FAIL or INCONCLUSIVE, and the exit status. failed_runs names the
    runs whose QEMU stopped or never reached their steps."""
    if "image" in failed_runs or any(r["verdict"] == "DIFFERS" for _, r in rows):
        return "FAIL", 1
    if any(r["verdict"] == "missing" for _, r in rows):
        # A picture the stock runs lack cannot be judged; one the image under
        # test lacks is its own failure.
        return ("INCONCLUSIVE", 3) if failed_runs else ("FAIL", 1)
    if failed_runs or any(r["verdict"] == "unstable" for _, r in rows):
        return "INCONCLUSIVE", 3
    return "PASS", 0


# -- running the deck ----------------------------------------------------------

class RunLayout(Layout):
    """The checkout's layout with the stick and the serial log of one run."""

    def __init__(self, media, logs):
        super().__init__()
        self.media = media
        self.logs = logs

    @property
    def usb_image(self):
        return self.media


def base_env(environ):
    """The caller's environment without what would make the deck something
    else than the one under test: a window, sound, Pro DJ Link, extra QEMU
    options, a snapshot point of start's own, mods start would patch in."""
    env = {k: v for k, v in environ.items() if not k.startswith("CDJ_MAIN_")}
    for k in ("DJLINK", "MAIN_ARGS", "SNAPSHOT", "SNAPSHOT_IN", "CDJ_PANEL_KEYSOCK"):
        env.pop(k, None)
    env.update(GUI_DISPLAY="none", NOSOUND="1")
    return env


def knobs(env, before):
    """What deck.command set for the deck: the entries of env that differ
    from `before`, less the per-run key socket."""
    return {k: v for k, v in env.items() if before.get(k) != v and k != "CDJ_PANEL_KEYSOCK"}


def run_argv(argv, mon_spec, restore=None, save=False):
    """The deck's argv with its monitor, and the restore from the point
    folder `restore` or the setting a save needs."""
    a = list(argv) + ["-monitor", mon_spec]
    if restore:
        a += snapshot.restore_args(restore)
    elif save:
        a += snapshot.SAVE_ARGS
    return a


def free_udp_ports(n, start=deck.PANEL_KEY_PORT - 1):
    """n UDP ports below start that nothing is bound to now."""
    ports, p = [], start
    while len(ports) < n and p > 20000:
        p = host.pick_udp_port(p)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            s.bind(("127.0.0.1", p))
            ports.append(p)
        except OSError:
            pass
        finally:
            s.close()
        p -= 1
    if len(ports) < n:
        raise CheckError("no free UDP port for the panel key sockets")
    return ports


class Run:
    """One deck: its QEMU, its steps, its pictures."""

    def __init__(self, name, argv, env, outdir, mon, keyport, steps, idle_s,
                 restore=None, save_to=None, saved=None, wait_for=None):
        self.name, self.argv, self.env = name, argv, dict(env)
        self.env["CDJ_PANEL_KEYSOCK"] = str(keyport)
        self.outdir, self.mon, self.keyport = outdir, mon, keyport
        self.steps, self.idle_s = steps, idle_s
        self.restore, self.save_to, self.saved, self.wait_for = restore, save_to, saved, wait_for
        self.error = ""
        self.started = "booted"
        self.seconds = 0.0

    def go(self):
        t0 = time.time()
        try:
            self._go()
        except Exception as e:      # the summary reports it; the other runs go on
            self.error = self.error or "%s: %s" % (type(e).__name__, e)
        finally:
            if self.saved is not None and self.save_to:
                self.saved.set()
            self.seconds = time.time() - t0

    def _go(self):
        import cdj_monsock
        restore = self.restore
        if self.wait_for is not None:
            if not self.wait_for.wait(SAVE_WAIT_S):
                self.error = "the stock idle point was not saved in time"
                return
            restore = restore if snapshot.saved(restore, ("main.vm",)) else None
        os.makedirs(self.outdir, exist_ok=True)
        argv = run_argv(self.argv, cdj_monsock.spec(self.mon), restore, bool(self.save_to))
        with open(os.path.join(self.outdir, "qemu.log"), "wb") as log:
            self.q = subprocess.Popen(argv, env=self.env, stdout=log, stderr=subprocess.STDOUT)
        try:
            if not cdj_monsock.wait_up(self.mon, MONITOR_TIMEOUT_S):
                self.error = "the monitor never came up"
                return
            if restore:
                self.started = "restored"
                if not self._wait_running():
                    return
            else:
                time.sleep(self.idle_s)
                if self.save_to:
                    if snapshot_deck.save(self.mon, None, self.save_to):
                        self.started = "booted, saved the idle point"
                    else:
                        self.started = "booted, the save FAILED"
                    self.saved.set()
            self._steps()
        finally:
            self._stop(cdj_monsock)

    def _wait_running(self):
        import cdj_monsock
        deadline = time.time() + RESTORE_TIMEOUT_S
        while time.time() < deadline:
            if self.q.poll() is not None:
                self.error = "QEMU exited %d while restoring" % self.q.returncode
                return False
            try:
                if b"running" in cdj_monsock.command(self.mon, "info status"):
                    return True
            except OSError:
                pass
            time.sleep(1.0)
        self.error = "not running %d s after the restore" % RESTORE_TIMEOUT_S
        return False

    def _steps(self):
        for step in self.steps:
            if self.q.poll() is not None:
                self.error = "QEMU exited %d" % self.q.returncode
                return
            if step[0] == "shot":
                self._shoot(step[1])
            else:
                snapshot_deck.do_step(self.mon, step, self.keyport)

    def _shoot(self, name):
        from PIL import Image
        for i in range(FRAMES):
            if i:
                time.sleep(FRAME_GAP_S)
            ppm = os.path.join(self.outdir, "%s-f%d.ppm" % (name, i))
            if snapshot_deck.screendump(self.mon, ppm):
                Image.open(ppm).save(ppm[:-4] + ".png")
                os.remove(ppm)

    def _stop(self, cdj_monsock):
        try:
            cdj_monsock.command(self.mon, "quit")
        except OSError:
            pass
        try:
            self.q.wait(10)
        except subprocess.TimeoutExpired:
            self.q.kill()
            self.q.wait()


def frames(outdir, run, shot):
    from PIL import Image
    out = []
    for i in range(FRAMES):
        path = os.path.join(outdir, run, "%s-f%d.png" % (shot, i))
        if os.path.exists(path):
            with Image.open(path) as im:
                out.append(im.convert("RGB"))
    return out


def load_face(name):
    """app/faces/<name>.json through app/face.py, which is loaded by path:
    app/ on sys.path would put its layout.py before any other."""
    if "face" not in sys.modules:
        import importlib.util
        spec = importlib.util.spec_from_file_location("face", os.path.join(EMU, "app", "face.py"))
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        sys.modules["face"] = mod
    return sys.modules["face"].load(name)


def prepare(args):
    """Everything the runs need, or CheckError."""
    m = model.load(args.model)
    if m.is_rig:
        raise CheckError("the %s starts as the two-board rig; mod_check.py runs one-window decks "
                         "(MODEL_LAUNCH=deck)" % m.title)
    if not m.idle_s:
        raise CheckError("models/%s.conf sets no MODEL_IDLE_S, the seconds to its idle screen" % m.id)
    lay = Layout()
    missing = deck.missing_images(lay, m)
    if missing:
        raise CheckError("missing firmware images: %s -- run ./setup.sh --model %s --firmware <the update file>"
                         % (" ".join(missing), m.id))
    image = os.path.abspath(args.image)
    if not os.path.isfile(image):
        raise CheckError("no image at %s" % image)
    media = os.path.abspath(args.media or lay.usb_image)
    if not os.path.isfile(media):
        raise CheckError("no stick image at %s -- run ./setup.sh --music <folder>, or name one with --media" % media)
    face = load_face(m.app_face) if m.app_face else None
    steps, skipped = plan(m, face)
    stock = os.path.join(model.extract_dir(lay, m), "main_unpacked.bin")
    return m, lay, image, stock, media, face, steps, skipped


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="the MAIN image under test (an unpacked main_unpacked.bin)")
    ap.add_argument("outdir")
    ap.add_argument("--allow", action="append", default=[], type=parse_box, metavar="X0,Y0,X1,Y1",
                    help="a screen region the change is meant to touch (pixels, x1/y1 exclusive); repeatable")
    ap.add_argument("--ignore", action="append", default=[], type=parse_box, metavar="X0,Y0,X1,Y1",
                    help="a screen region never compared; repeatable")
    ap.add_argument("--model", default=None, help="the player (default $CDJ_MODEL)")
    ap.add_argument("--media", default="", help="the stick image (default the one ./start.sh attaches)")
    ap.add_argument("--max-noise", type=float, default=MAX_NOISE_PCT, metavar="PCT",
                    help="share of the screen the stock runs may disagree on before a point is unstable "
                         "(default %(default)s)")
    ap.add_argument("--fresh", action="store_true", help="boot stock to its idle point and save it again")
    args = ap.parse_args(argv)
    try:
        m, lay, image, stock, media, face, steps, skipped = prepare(args)
    except (CheckError, model.ModelError) as e:
        print("mod_check: %s" % e, file=sys.stderr)
        return 2

    outdir = os.path.abspath(args.outdir)
    os.makedirs(outdir, exist_ok=True)
    before = base_env(os.environ)
    ports = free_udp_ports(len(RUNS))
    commands = {}
    for name in RUNS:
        commands[name] = deck.command(RunLayout(media, os.path.join(outdir, name)), m, before,
                                      main=image if name == "image" else stock)
    image_knobs = knobs(commands["image"][1], before)
    # The knobs the image under test runs without (the pinned-only ones when
    # not all its sites are found) are left out of stock too, so the image is
    # the only difference between the runs.
    left_out = sorted(set(knobs(commands["stock1"][1], before)) - set(image_knobs))
    for name in RUNS[1:]:
        for k in left_out:
            del commands[name][1][k]
    stock_knobs = knobs(commands["stock1"][1], before)
    point = deck.point_dir(lay.tmp, commands["stock1"][0], commands["stock1"][1], media, "idle", left_out)
    have_point = not args.fresh and snapshot.saved(point, ("main.vm",))
    saved = threading.Event()
    runs = []
    for i, name in enumerate(RUNS):
        kw = {}
        if name == "stock1" and not have_point:
            kw = {"save_to": point, "saved": saved}
        elif name != "image":
            kw = {"restore": point, "wait_for": None if have_point else saved}
        mon = os.path.join(lay.tmp, "cdj-modcheck-%d-%s-mon.sock" % (os.getpid(), name))
        argv_q, env = commands[name]
        runs.append(Run(name, argv_q, env, os.path.join(outdir, name), mon, ports[i], steps, float(m.idle_s), **kw))

    t0 = time.time()
    threads = [threading.Thread(target=r.go) for r in runs]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    wall = time.time() - t0

    names = shots(steps)
    rows = []
    os.makedirs(os.path.join(outdir, "diff"), exist_ok=True)
    for shot in names:
        r = judge_point(frames(outdir, "image", shot), [frames(outdir, run, shot) for run in RUNS[1:]],
                        args.allow, args.ignore, args.max_noise)
        if "masks" in r:
            draw_diff(r, args.allow, args.ignore, os.path.join(outdir, "diff", shot + ".png"))
        rows.append((shot, r))
    failed = [r.name for r in runs if r.error]
    result, status = result_of(rows, failed)

    lines = ["model: %s (v%s)" % (m.title, m.fw_version),
             "image under test: %s%s" % (image, "  (the stock image itself)"
                                          if firmware.sha256(image) == firmware.sha256(stock) else ""),
             "stock: %s" % stock, "stick: %s" % media, "stock idle point: %s" % point,
             "allowed regions: %s" % (args.allow or "none"), "ignored regions: %s" % (args.ignore or "none")]
    for name, found in (("image under test", sites.find(lay, m, image)), ("stock", sites.find(lay, m))):
        for line in found.switched_off(m):
            lines.append("left out for the %s: %s" % (name, line))
    if left_out:
        lines.append("left out for stock too, as the image under test runs without them: %s"
                     % ", ".join(left_out))
    differ = sorted(k for k in set(image_knobs) | set(stock_knobs) if image_knobs.get(k) != stock_knobs.get(k))
    if differ:
        lines.append("knobs that differ between the image under test and stock: %s" % ", ".join(differ))
    for name, missing in skipped:
        lines.append("skipped %s: the %s face wires no %s" % (name, m.app_face or "(missing)", " / ".join(missing)))
    lines.append("")
    for r in runs:
        lines.append("run %-7s %s, %.0f s%s" % (r.name, r.started, r.seconds, ("  ERROR: " + r.error) if r.error else ""))
    lines += ["", "%-18s %-9s %9s %9s %9s %9s  %s" % ("point", "verdict", "px differ", "in allow", "in noise",
                                                        "noise px", "box of the judged differences")]
    for shot, r in rows:
        if r["verdict"] == "missing":
            lines.append("%-18s %-9s" % (shot, "missing"))
            continue
        lines.append("%-18s %-9s %9d %9d %9d %9d  %s%s" % (shot, r["verdict"], r["differ"], r.get("in_allow", 0),
                                                          r["in_noise"], r["noise"], r["box"] or "-",
                                                      ("  (%s)" % r["note"]) if r.get("note") else ""))
    lines += ["", "RESULT: %s%s" % (result, "  (run the check again)" if result == "INCONCLUSIVE" else ""),
              "wall time: %.0f s" % wall]
    text = "\n".join(lines) + "\n"
    with open(os.path.join(outdir, "summary.txt"), "w", encoding="utf-8") as f:
        f.write(text)
    print(text, end="")
    return status


if __name__ == "__main__":
    sys.exit(main())
