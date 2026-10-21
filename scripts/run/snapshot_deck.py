#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Save a running deck, every board, so a later run starts from this moment.

    usage: snapshot_deck.py <tag> <dir>
           snapshot_deck.py --reach <point> <monitor> <dir>

MAIN needs its monitor (MAIN_MON=1). Both boards are stopped, the link
between them is given a moment to drain into their receive buffers, each is
written to <dir>/main.vm and <dir>/gui.vm, and both run on. Start a deck from
the pair with SNAPSHOT_IN=<dir>: the same build, firmware, medium and knobs as
the run that saved it, or QEMU refuses the state.

--reach drives a one-board deck (launcher/deck.py) to a point and saves it
into <dir>/main.vm: 'idle' MODEL_IDLE_S seconds after its monitor comes up,
'loaded' after MODEL_LOAD_STEPS from there, saving 'idle' beside it on the
way. The steps are ';'-separated: 'sendkey <key> <ms>' (the monitor's
sendkey), 'key <payload>' (one datagram to the panel key socket,
CDJ_PANEL_KEYSOCK) and 'wait <seconds>'.
"""

import contextlib
import hashlib
import os
import re
import socket
import sys
import time

import cdj_monsock

LINK_DRAIN_S = 1.0
SAVE_TIMEOUT_S = 120

# A booting deck redraws its screen in steps and sits on each for a while (the
# CDJ-2000NXS, with its host slowed to a 125 s boot: up to 25 s); the idle
# screen it ends on never changes again.
SETTLE_S = 45
SETTLE_POLL_S = 2
SETTLE_TIMEOUT_S = 300
SCREENDUMP_TIMEOUT_S = 10


def _command(mon, line, deadline):
    """A monitor command that waits out refused connects: while a save holds
    QEMU's main loop the monitor accepts nobody, and on Windows a full listen
    backlog refuses rather than queues."""
    while True:
        try:
            return cdj_monsock.command(mon, line)
        except OSError:
            if time.time() >= deadline:
                raise
            time.sleep(0.5)


def _status(mon, deadline):
    out = _command(mon, "info migrate", deadline).decode("utf-8", "replace")
    m = re.search(r"Migration status:\s*(\S+)", out)
    return m.group(1) if m else ""


def _read_to_prompt(sock):
    """Reads the monitor up to its next prompt. A socket closed with a reply
    still unread is reset on Windows, which can leave the monitor dead."""
    sock.settimeout(0.25)
    seen, end = b"", time.time() + SCREENDUMP_TIMEOUT_S
    while not seen.rstrip().endswith(b"(qemu)") and time.time() < end:
        try:
            part = sock.recv(65536)
        except socket.timeout:
            continue
        if not part:
            return
        seen += part


def screendump(mon, path):
    """Writes the deck's screen to path and returns its digest, None if the
    monitor gave no picture."""
    try:
        os.unlink(path)
    except OSError:
        pass
    try:
        with contextlib.closing(cdj_monsock.connect(mon)) as sock:
            _read_to_prompt(sock)
            sock.sendall(("screendump %s\n" % path.replace("\\", "/")).encode())
            _read_to_prompt(sock)
        with open(path, "rb") as f:
            return hashlib.md5(f.read()).hexdigest()
    except OSError:
        return None


def wait_settled(mon, idle_s, screen_path):
    """Waits for the deck to finish booting: at least idle_s seconds, and the
    screen unchanged for SETTLE_S. False once SETTLE_TIMEOUT_S has passed
    without that. screen_path keeps the last picture taken."""
    start = time.time()
    shown, since = None, start
    while True:
        now = time.time()
        digest = screendump(mon, screen_path)
        if digest != shown:
            shown, since = digest, now
        if now - start >= idle_s and now - since >= SETTLE_S:
            return True
        if now - start >= SETTLE_TIMEOUT_S:
            return False
        time.sleep(SETTLE_POLL_S)


def save(main_mon, gui_mon, out_dir):
    """True once every board is saved; they are running again either way. A
    deck whose display runs inside MAIN has no gui_mon."""
    os.makedirs(out_dir, exist_ok=True)
    boards = [(main_mon, os.path.join(out_dir, "main.vm"))]
    if gui_mon:
        boards.append((gui_mon, os.path.join(out_dir, "gui.vm")))
    for mon, _ in boards:
        _command(mon, "stop", time.time() + SAVE_TIMEOUT_S)
    if gui_mon:
        time.sleep(LINK_DRAIN_S)
    ok = True
    try:
        for mon, path in boards:
            part = path + ".part"
            deadline = time.time() + SAVE_TIMEOUT_S
            _command(mon, 'migrate "file:%s"' % part.replace("\\", "/"), deadline)
            status = _status(mon, deadline)
            while status not in ("completed", "failed", "cancelled") and time.time() < deadline:
                time.sleep(0.5)
                status = _status(mon, deadline)
            if status != "completed":
                print("snapshot: %s not saved (migration %s)" % (path, status or "never started"))
                ok = False
                break
            os.replace(part, path)
    finally:
        for mon, _ in boards:
            _command(mon, "cont", time.time() + SAVE_TIMEOUT_S)
    return ok


def run_steps(mon, steps, keysock):
    for step in (s.split() for s in steps.split(";") if s.strip()):
        if step[0] == "wait":
            time.sleep(float(step[1]))
        elif step[0] == "sendkey":
            _command(mon, "sendkey %s %s" % (step[1], step[2] if len(step) > 2 else "100"), time.time() + 10)
        elif step[0] == "key":
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.sendto(step[1].encode(), ("127.0.0.1", keysock))
        else:
            raise ValueError("MODEL_LOAD_STEPS: unknown step '%s'" % step[0])


def reach(point, mon, out_dir):
    """Drive a one-board deck from power-on to point and save it there."""
    if not cdj_monsock.wait_up(mon):
        print("snapshot: the deck's monitor never came up")
        return False
    time.sleep(float(os.environ.get("MODEL_IDLE_S", "60")))
    root = os.path.dirname(out_dir)
    if point == "loaded":
        if not os.path.isfile(os.path.join(root, "idle", "main.vm")):
            save(mon, None, os.path.join(root, "idle"))
        run_steps(mon, os.environ.get("MODEL_LOAD_STEPS", ""), int(os.environ.get("CDJ_PANEL_KEYSOCK", "0")))
    t0 = time.time()
    ok = save(mon, None, out_dir)
    print("snapshot: '%s' %s in %.1f s -> %s" % (point, "saved" if ok else "FAILED", time.time() - t0, out_dir))
    return ok


def main(argv):
    if len(argv) == 4 and argv[0] == "--reach":
        return 0 if reach(*argv[1:]) else 1
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2
    tag, out_dir = argv
    t0 = time.time()
    ok = save("/tmp/cdj-%s-main-mon.sock" % tag, "/tmp/cdj-%s-gui-mon.sock" % tag, out_dir)
    print("snapshot: %s in %.1f s -> %s" % ("saved" if ok else "FAILED", time.time() - t0, out_dir))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
