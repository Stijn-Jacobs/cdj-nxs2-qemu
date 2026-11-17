#!/usr/bin/env python3
"""Local qualification driver (adapted from the read-only eval harness): one cdj2000nxs deck in their emulator, headless, driven through
the HMP monitor (sendkey / screendump / xp). Mirrors the mods repo's
gui_drivers_boot.py steps: ROOT -> auto-cue on -> 3rd ROOT item -> first track
-> load -> PLAY 20 s; records wall times, MAIN position (0x0491b278, 150 Hz),
host CPU of the qemu process, and the exit reports.

usage: qualify_sd.py OUT_DIR --qemu CANDIDATE --main MAIN --display UPD
                     [--usb RAW | --source sd --sd RAW] [--play 20] [--keys]
Default USB steps match xcheck's boot/root/browse/load/play/info/menu sequence.
SD uses a closed lid, settles before source selection and disconnects USB.
All run artifacts are written under OUT_DIR in this worktree.
"""
# SPDX-License-Identifier: GPL-2.0-or-later
import signal
# SIGTERM -> SystemExit so finally: blocks quit QEMU
signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(SystemExit(143)))
import argparse, hashlib, json, os, re, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, ROOT)
from launcher import deck, model  # noqa: E402
from launcher.layout import Layout  # noqa: E402

POS, STATE, ACUE = 0x0491b278, 0x04e34e48, 0x04e34e5f


class Mon:
    def __init__(self, path):
        self.path = path
        for _ in range(300):
            try:
                self.s = socket.socket(socket.AF_UNIX); self.s.connect(path); break
            except OSError:
                time.sleep(0.2)
        else:
            raise SystemExit("monitor never came up")
        self.s.settimeout(30); self._read()

    def _read(self):
        buf = b""
        while not buf.endswith(b"(qemu) "):
            chunk = self.s.recv(65536)
            if not chunk:
                break
            buf += chunk
        return buf.decode("utf-8", "replace")

    def cmd(self, c):
        self.s.sendall(c.encode() + b"\n"); return self._read()

    def word(self, a):
        m = re.search(r":\s*(0x[0-9a-f]+)", self.cmd("xp /1wx 0x%x" % a))
        return int(m.group(1), 16) if m else None

    def key(self, k, ms=150):
        self.cmd("sendkey %s %d" % (k, ms)); time.sleep(ms / 1000 + 0.25)


def ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    w, h = map(int, parts[1].split()); return w, h, parts[3]


def white_count(path, box):
    w, h, px = ppm(path); x0, y0, x1, y1 = box; n = 0
    for y in range(y0, min(y1, h)):
        for x in range(x0, min(x1, w)):
            i = 3 * (y * w + x)
            if min(px[i], px[i + 1], px[i + 2]) > 200:
                n += 1
    return n


def cpu_seconds(pid):
    t = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True).stdout.strip()
    if not t:
        return None
    parts = t.split(":"); s = 0.0
    for p in parts:
        s = s * 60 + float(p)
    return s


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out"); ap.add_argument("--main"); ap.add_argument("--display")
    ap.add_argument("--qemu", help="local candidate QEMU path")
    ap.add_argument("--source", choices=("usb", "sd"), default="usb")
    ap.add_argument("--sd", help="raw SD image (always a snapshot overlay)")
    ap.add_argument("--settle", type=float, help="seconds before browsing; SD default 30")
    ap.add_argument("--key-ms", type=int, help="encoder pulse length; SD default 400")
    ap.add_argument("--module", default="auto"); ap.add_argument("--explore", type=float)
    ap.add_argument("--play", type=float, default=20); ap.add_argument("--root-timeout", type=float, default=240)
    ap.add_argument("--hold", type=float, help="just keep QEMU up this many seconds (for a debugger)")
    ap.add_argument("--nor", help="boot MAIN through the bootloader in this NOR image (padded to 4 MiB, per-run copy) instead of -kernel")
    ap.add_argument("--usb", help="USB image (default: their usbmedia3)")
    ap.add_argument("--load-timeout", type=float, default=0, help="give up this many s after ENTER on the track")
    ap.add_argument("--keys", action="store_true", help="after play: INFO press, then MENU hold, each with a shot")
    ap.add_argument("--save-up", help="save the whole machine here once the deck is up, then stop")
    ap.add_argument("--steps", default="", help="incoming mode: ';'-separated 'key K MS' / 'wait S' / 'shot NAME'")
    ap.add_argument("--save", help="save the whole machine here once playing")
    ap.add_argument("--incoming", help="start from a saved main.vm dir; only sample play")
    a = ap.parse_args()
    os.makedirs(a.out)
    os.environ["CDJ_MODEL"] = "cdj2000nxs"
    if a.qemu:
        os.environ["MAIN_QEMU"] = os.path.abspath(a.qemu)
    if a.sd:
        os.environ["CDJ_SD_IMAGE"] = os.path.abspath(a.sd)
    if a.source == "sd":
        if not os.environ.get("CDJ_SD_IMAGE"):
            ap.error("--source sd needs --sd or CDJ_SD_IMAGE")
        os.environ.update(DRIVE_SOURCE_KEY="f", DRIVE_NO_USB="1")
        os.environ.setdefault("DRIVE_SETTLE", "30")
        os.environ.setdefault("DRIVE_KEY_MS", "400")
    if a.settle is not None:
        os.environ["DRIVE_SETTLE"] = str(a.settle)
    if a.key_ms is not None:
        os.environ["DRIVE_KEY_MS"] = str(a.key_ms)
    lay, m = Layout(), model.load()
    env = dict(os.environ, GUI_DISPLAY="none", AUDIODEV="none", CDJ_REPORT="1")
    if a.module == "none":
        env["MODULE"] = "none"
    # Own panel key port per run: a second deck on the shared default port
    # cannot bind it and silently ignores every key.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as u:
        u.bind(("127.0.0.1", 0)); env["CDJ_PANEL_KEYSOCK"] = str(u.getsockname()[1])
    argv, env = deck.command(lay, m, env)
    if a.main:
        argv[argv.index("-kernel") + 1] = os.path.abspath(a.main)
    if os.environ.get("DRIVE_QEMU_EXTRA"):  # e.g. "-icount shift=auto,align=off"
        argv += os.environ["DRIVE_QEMU_EXTRA"].split()
    if a.nor:  # as the deck boots: reset vector in NOR, bootloader, then MAIN
        img = open(a.nor, "rb").read()
        if len(img) > 4 << 20:
            raise SystemExit("NOR image larger than 4 MiB")
        nor = os.path.join(os.path.abspath(a.out), "nor.bin")
        with open(nor, "wb") as f:
            f.write(img + b"\xff" * ((4 << 20) - len(img)))
        k = argv.index("-kernel"); del argv[k:k + 2]
        argv += ["-drive", "if=pflash,format=raw,file=%s" % nor]
    if a.display:
        env["CDJ_BF531_UPD"] = os.path.abspath(a.display)
    if env.get("CDJ_BF531_FLASH"):  # flash boot: the GUI comes from the flash, not an UPD
        env.pop("CDJ_BF531_UPD", None)
    if a.usb:
        i = next(i for i, x in enumerate(argv) if x.startswith("if=none,id=usbstk"))
        argv[i] = "if=none,id=usbstk,file=%s,format=raw,snapshot=on" % os.path.abspath(a.usb)
    if a.incoming:
        argv += ["-incoming", "file:%s/main.vm" % os.path.abspath(a.incoming)]
    if os.environ.get("DRIVE_NO_USB"):
        i = next(i for i,x in enumerate(argv) if x.startswith("if=none,id=usbstk"))
        del argv[i-1:i+1]
        i = argv.index("-device"); del argv[i:i+2]
    mon = os.path.join(a.out, "mon.sock")
    argv += ["-monitor", "unix:%s,server,nowait" % mon,
             "-monitor", "unix:%s,server,nowait" % os.path.join(a.out, "mon2.sock")]
    i = argv.index("-serial"); argv[i + 1] = "file:" + os.path.join(a.out, "serial.log")
    rep = {"argv": argv, "env": {k: v for k, v in env.items() if k.startswith(("CDJ_", "C66X_"))}, "t": {},
           "scenario": {"source": a.source, "sd": env.get("CDJ_SD_IMAGE"),
                        "settle": os.environ.get("DRIVE_SETTLE", "0.5"),
                        "key_ms": os.environ.get("DRIVE_KEY_MS", "80"), "play": a.play}}
    def sha_file(path):
        h = hashlib.sha256()
        with open(path, "rb") as f:
            while chunk := f.read(1024 * 1024):
                h.update(chunk)
        return h.hexdigest()
    inputs = [argv[0], a.main, a.display, a.sd or env.get("CDJ_SD_IMAGE"),
              a.usb if not os.environ.get("DRIVE_NO_USB") else None]
    rep["inputs_sha256"] = {os.path.abspath(p): sha_file(p) for p in inputs if p}
    log = open(os.path.join(a.out, "qemu.log"), "wb")
    t0 = time.time()
    q = subprocess.Popen(argv, env=env, stdout=log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    rep["pid"] = q.pid
    shot_n = [0]

    def shot(label):
        p = os.path.join(a.out, "%03d-%s.ppm" % (shot_n[0], label)); shot_n[0] += 1
        M.cmd("screendump " + p); time.sleep(0.2); return p

    def mark(label):
        rep["t"][label] = round(time.time() - t0, 2); print(label, rep["t"][label], flush=True)

    try:
        rep["t0_wall"] = t0
        M = Mon(mon); mark("monitor")
        time.sleep(float(os.environ.get("DRIVE_SETTLE", "0.5")))
        if b"could NOT bind" in open(os.path.join(a.out, "qemu.log"), "rb").read():
            raise RuntimeError("panel key port did not bind: this deck would ignore every key")
        if a.incoming:
            while "running" not in (st := M.cmd("info status")):
                if "inmigrate" not in st and "paused" in st:
                    M.cmd("cont")  # the save stopped the deck, so it comes back paused
                if time.time() - t0 > 120:
                    raise RuntimeError("incoming never ran")
                time.sleep(0.2)
            mark("restored"); shot("restored")
            for st in (x.split() for x in a.steps.split(";") if x.strip()):
                if st[0] == "key": M.cmd("sendkey %s %s" % (st[1], st[2])); mark("key_%s_%s" % (st[1], st[2]))
                elif st[0] == "wait": time.sleep(float(st[1]))
                elif st[0] == "shot": shot(st[1])
                elif st[0] == "xp":  # xp ADDR NWORDS: one read into the report
                    out = M.cmd("xp /%sxw 0x%x" % (st[2], int(st[1], 16)))
                    rep.setdefault("xp", []).append([round(time.time() - t0, 2), st[1], out.split("\n", 1)[-1]])
                    print(out.split("\n", 1)[-1].strip(), flush=True)
                elif st[0] == "poll":  # poll ADDR SECS: log the word every 2 s
                    ad, end, last = int(st[1], 16), time.time() + float(st[2]), None
                    while time.time() < end:
                        v = M.word(ad)
                        if v != last: mark("w%x=%x" % (ad, v)); last = v
                        time.sleep(2)
            p0, w0 = M.word(POS), time.time(); time.sleep(a.play); p1, w1 = M.word(POS), time.time()
            shot("playing"); rep.update(pos_before=p0, pos_after=p1, state_after=M.word(STATE),
                realtime_ratio=round((p1 - p0) / 150.0 / (w1 - w0), 3)); mark("done"); rep["pass"] = rep.get("state_after") == 3 and (rep.get("realtime_ratio") or 0) > 0.95
            return
        if a.hold:  # keep the deck up for a debugger: no monitor traffic at all
            time.sleep(a.hold); return
        if a.explore:
            end = t0 + a.explore
            while time.time() < end:
                shot("t%03d" % int(time.time() - t0)); time.sleep(3)
            return
        # ROOT: the browser's light preview pane on the right (absent on the
        # "Not Loaded." player screen). USB is pressed once the deck is up and
        # again every 8 s until the browser opens (ours: --source-key-when-ready).
        presses, last_press = 0, -99.0
        while True:
            el = time.time() - t0
            if el > a.root_timeout:
                raise RuntimeError("ROOT timeout")
            p = shot("root-poll")
            if white_count(p, (280, 40, 440, 120)) > 3000:
                break
            up = white_count(p, (0, 2, 120, 20)) > 40
            if up and el - last_press > 8:
                if presses == 0:
                    mark("deck_up")
                    if a.save_up:
                        sys.path.insert(0, os.path.join(ROOT, "scripts/run")); import snapshot_deck
                        rep["saved"] = snapshot_deck.save(os.path.join(a.out, "mon2.sock"), None, a.save_up)
                        mark("saved_up"); rep["pass"] = True; return
                M.key(os.environ.get("DRIVE_SOURCE_KEY", "u")); presses += 1; last_press = el; mark("%s_press_%d" % (a.source, presses))
            else:
                os.unlink(p)
            time.sleep(1)
        mark("root"); time.sleep(2); shot("root")
        rep["acue"] = M.word(ACUE)
        # 3rd ROOT item (TRACK), then its first entry (gui_drivers_boot: rotary -20, +2, enter; -20, enter)
        for _ in range(6): M.key("up", int(os.environ.get("DRIVE_KEY_MS", "80")))
        M.key("down", int(os.environ.get("DRIVE_KEY_MS", "80"))); M.key("down", int(os.environ.get("DRIVE_KEY_MS", "80"))); time.sleep(2); shot("cursor")
        M.key("ret", int(os.environ.get("DRIVE_KEY_MS", "150"))); mark("enter_category"); time.sleep(15); shot("browse")
        for _ in range(6): M.key("up", int(os.environ.get("DRIVE_KEY_MS", "80")))
        time.sleep(2); M.key("ret", int(os.environ.get("DRIVE_KEY_MS", "150"))); mark("enter_track")
        # Auto-cue is off here (no TIME/A.CUE key in their keymap), so the load
        # plays by itself (state 3); at a cue (7) PLAY is pressed.
        while os.environ.get("DRIVE_SCREEN_LOAD") is None and M.word(STATE) not in (3, 4, 7):
            if time.time() - t0 > a.root_timeout + 400 or (a.load_timeout and time.time() - t0 - rep["t"]["enter_track"] > a.load_timeout):
                raise RuntimeError("load timeout (state %r)" % M.word(STATE))
            time.sleep(0.5)
        if os.environ.get("DRIVE_SCREEN_LOAD"):
            for i in range(12):  # owned MAIN: no stock state word; watch the screen
                time.sleep(5); shot("load-%02d" % i)
        mark("loaded_state%d" % M.word(STATE))
        if M.word(STATE) == 7:
            M.key("spc", 100); mark("play")
        time.sleep(5); shot("loaded")
        p0, c0, w0 = M.word(POS), cpu_seconds(q.pid), time.time()
        samples = []
        while time.time() - w0 < a.play:
            time.sleep(2); samples.append((round(time.time() - w0, 2), M.word(POS), M.word(STATE)) + ((M.word(int(os.environ['DRIVE_TICK'], 0)),) if os.environ.get('DRIVE_TICK') else ()))
        p1, c1, w1 = M.word(POS), cpu_seconds(q.pid), time.time()
        shot("playing")
        rep.update(pos_before=p0, pos_after=p1, play_wall=round(w1 - w0, 2), samples=samples,
                   cpu_play=round((c1 - c0) / (w1 - w0), 3) if c0 is not None else None,
                   state_after=M.word(STATE))
        if p0 is not None and p1 is not None:
            rep["realtime_ratio"] = round((p1 - p0) / 150.0 / (w1 - w0), 3)
        for spec in filter(None, os.environ.get("DRIVE_XP", "").split(",")):  # ADDR:NWORDS of MAIN
            ad, n = spec.split(":")
            rep.setdefault("xp", []).append([ad, M.cmd("xp /%sxw %s" % (n, ad)).split("\n", 1)[-1].strip()])
        for c in range(int(os.environ.get("DRIVE_BROWSE_CYCLES", "0"))):  # 5e's slice3 trigger: BROWSE, BROWSE back, INFO on/off
            M.key("b", 150); time.sleep(4); shot("b%d-browse" % c); mark("b%d_browse" % c)
            M.key("b", 150); time.sleep(4); shot("b%d-player" % c); mark("b%d_player" % c)
            M.key("i", 150); time.sleep(4); shot("b%d-info" % c); mark("b%d_info" % c)
            M.key("i", 150); time.sleep(3); shot("b%d-info-off" % c); mark("b%d_info_off" % c)
        if a.keys:  # a Player key after the load, then MENU held for UTILITY
            shot("before-keys"); M.key("i", 150); time.sleep(3); shot("info"); mark("info")
            M.key("i", 150); time.sleep(3); M.key("m", 2500); time.sleep(4); shot("menu-hold"); mark("menu_hold")
            if int(os.environ.get("DRIVE_INFO_CYCLES", "0")):  # leave the UTILITY the MENU hold opened
                M.key("m", 150); time.sleep(3); shot("left-utility")
            for c in range(int(os.environ.get("DRIVE_INFO_CYCLES", "0"))):  # EC's stall trigger: INFO, 8 s, INFO
                M.key("i", 1500 if c % 2 else 150); time.sleep(8); shot("i%d-info" % c); mark("i%d_info" % c)  # odd: LINK INFO (held)
                M.key("i", 150); time.sleep(3); shot("i%d-back" % c)
            for c in range(1, int(os.environ.get("DRIVE_KEY_CYCLES", "1"))):  # repeat: leave UTILITY, INFO on/off, MENU hold
                M.key("m", 150); time.sleep(3); shot("c%d-player" % c)
                M.key("i", 150); time.sleep(3); shot("c%d-info" % c); mark("c%d_info" % c)
                M.key("i", 150); time.sleep(3); M.key("m", 2500); time.sleep(4); shot("c%d-menu" % c); mark("c%d_menu" % c)
        if a.save:
            sys.path.insert(0, os.path.join(ROOT, "scripts/run")); import snapshot_deck
            ts = time.time(); rep["saved"] = snapshot_deck.save(os.path.join(a.out, "mon2.sock"), None, a.save)
            rep["save_s"] = round(time.time() - ts, 2); shot("after-save")
        mark("done"); rep["pass"] = rep.get("state_after") == 3 and (rep.get("realtime_ratio") or 0) > 0.95
    except Exception as e:  # keep the report
        rep["pass"] = False; rep["error"] = str(e); print("ERROR", e, flush=True)
        try: shot("error")
        except Exception: pass
    finally:
        rep["cpu_total"] = cpu_seconds(q.pid); rep["wall_total"] = round(time.time() - t0, 2)
        if os.environ.get("CDJ_BF531_GDB"):  # let an attached GUI debugger detach first, or quit can hang
            open(os.path.join(a.out, "teardown"), "w").close(); time.sleep(3)
        try: M.cmd("quit")
        except Exception: q.terminate()
        try: q.wait(60)
        except subprocess.TimeoutExpired: q.kill(); q.wait()
        log.close()
        txt = open(os.path.join(a.out, "qemu.log"), "rb").read().decode("utf-8", "replace")
        rep["report"] = [l for l in txt.splitlines() if re.search(
            r"c6747: (running|stopped|trap)|behind the virtual|MAIN waited|dsp audio: \d|jit|regions|verify", l)][-12:]
        behind = [int(n) for n in re.findall(r"at most (\d+) ms behind the virtual clock", txt)]
        rep["dsp_behind_ms"] = max(behind) if behind else None
        rep["verdict"] = ("INCONCLUSIVE" if behind and max(behind) > 1000 else
                          "PASS" if rep.get("pass") else "FAIL")
        json.dump(rep, open(os.path.join(a.out, "report.json"), "w"), indent=1)
        print("VERDICT", rep["verdict"], "DSP behind", rep["dsp_behind_ms"], flush=True)
    return rep.get("verdict")


if __name__ == "__main__":
    verdict = main()
    sys.exit(0 if verdict == "PASS" else 2 if verdict == "INCONCLUSIVE" else 1)
