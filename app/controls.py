# SPDX-License-Identifier: GPL-2.0-or-later
"""What the deck face's controls do: panel datagrams, nothing on screen.

Every control here resolves its action in midi/cdj_actions.py, which is the
one table of front-panel report bits, and sends through a `send(datagram)`
callable (the relay, for one deck's tag). The drawing and the mouse live in
deck_view.py; this module is toolkit-free.
"""

import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "midi"))

import cdj_actions as A  # noqa: E402

LIVE, UNTESTED, INERT = "live", "untested", "inert"


def look_for(action_name):
    """How a control with this action is drawn and whether it responds.

    confirmed and partial actions work; decoded ones work too but carry the
    'untested' mark, since nothing has pressed them on a running deck yet;
    guesses, unbound actions and controls with no action are drawn inert.
    """
    if not action_name:
        return INERT
    status = A.resolve(action_name).status
    if status in (A.CONFIRMED, A.PARTIAL):
        return LIVE
    if status == A.DECODED:
        return UNTESTED
    return INERT


def describe_action(action_name):
    """An action's name, evidence status and note, for the status bar."""
    a = A.resolve(action_name)
    note = f" -- {a.note}" if a.note else ""
    return f"{a.name} ({a.status}){note}"


def describe(key):
    """One line for the status bar: what the control is and how sure we are."""
    what = key.name.replace("_", " ")
    if not key.action:
        return f"{what}: not modelled here (no report bit is known for it)"
    host = f"   [keyboard: {key.keycap}]" if key.keycap else ""
    return f"{what}: {describe_action(key.action)}{host}"


class KeyControl:
    """A panel key, down exactly as long as the mouse button is."""

    def __init__(self, action_name, send):
        self.send = send
        self.look = look_for(action_name)
        self.action = A.resolve(action_name) if action_name else None
        self.down = False

    @property
    def live(self):
        return self.look != INERT and isinstance(self.action, A.KeyAction)

    def press(self):
        if not self.live or self.down:
            return False
        self.down = True
        self.send(self.action.hold_datagram())
        return True

    def release(self):
        if not self.down:
            return False
        self.down = False
        self.send(self.action.release_datagram())
        return True


class Jog:
    """The platter: a drag turns it, a hand on the top plate touches it.

    The firmware's jog engine reads a rolling position counter and a pulse
    period (cdj_actions.Platter), so the drag becomes ticks at the platter's
    own scale: a turn at 33 1/3 rpm (200 degrees a second) is nominal speed.

    Mouse events come unevenly, and more so on a busy host; fed straight to
    the Platter, a gap between two of them reads as the platter stopping. So
    the drag sets a velocity and ticks go out at a steady PUMP_S from it, until
    the hand has been still for HOLD_S. The ticks go out from a thread of
    their own (start()), so a busy UI thread cannot stall the platter either.
    """

    NOMINAL_DEG_S = 200.0
    PUMP_S = 0.01
    HOLD_S = 0.25
    FIRST_EVENT_S = 0.03            # the gap assumed before a drag's first move

    def __init__(self, send):
        self.send = send
        self.platter = A.Platter()
        self.ticks_per_degree = self.platter.nominal_tps / self.NOMINAL_DEG_S
        self.carry = 0.0
        self.angle = 0.0                # degrees turned, for the drawing
        self.velocity = 0.0             # degrees a second
        self.touched = False
        self.last_move = float("-inf")
        self.last_pump = 0.0
        self.lock = threading.Lock()
        self.running = False

    def start(self):
        """Pump from a thread of its own until stop()."""
        self.running = True
        threading.Thread(target=self._run, name="jog", daemon=True).start()

    def stop(self):
        self.running = False

    def _run(self):
        while self.running:
            self.pump(time.monotonic())
            time.sleep(self.PUMP_S)

    def touch(self, on):
        if on == self.touched:
            return
        self.touched = on
        key = A.ACTIONS["jog_touch"]
        self.send(key.hold_datagram() if on else key.release_datagram())

    def turn(self, degrees, now):
        """The hand moved the platter by `degrees` since its last move."""
        with self.lock:
            self._turn(degrees, now)
        self.pump(now, force=True)

    def _turn(self, degrees, now):
        self.angle += degrees
        gap = now - self.last_move
        inst = degrees / max(gap if gap < self.HOLD_S else self.FIRST_EVENT_S, 0.004)
        self.velocity = inst if gap >= self.HOLD_S else 0.6 * self.velocity + 0.4 * inst
        self.last_move = now

    def nudge(self, degrees, now):
        """A short push, as from a wheel notch: `degrees` over HOLD_S."""
        with self.lock:
            self.angle += degrees
            self.velocity = degrees / self.HOLD_S
            self.last_move = now

    def release(self, now):
        """The hand let go: a CDJ platter stops with it."""
        with self.lock:
            self.last_move = float("-inf")
        self.pump(now, force=True)

    def pump(self, now, force=False):
        """Send the platter's state; call this often (every UI tick)."""
        with self.lock:
            dt = now - self.last_pump
            if not force and dt < self.PUMP_S * 0.8:
                return
            self.last_pump = now
            if now - self.last_move > self.HOLD_S:
                self.velocity = 0.0
            if self.velocity:
                self.carry += self.velocity * min(dt, 0.05) * self.ticks_per_degree
                whole = int(self.carry)
                if whole:
                    self.carry -= whole
                    self.platter.tick(whole, now)
            out = self.platter.datagrams(now)
        for g in out:
            self.send(g)


class Selector:
    """The rotary selector: turned by detents, pushed to enter or load."""

    def __init__(self, send):
        self.send = send
        self.turn_action = A.ACTIONS["select_turn"]
        self.push = KeyControl("rotary_push", send)
        self.detents = 0

    def turn(self, detents):
        if detents:
            self.detents += detents
            self.send(self.turn_action.datagram(detents))


class TempoSlider:
    """The tempo fader. `pos` runs 0 (top, the '-' end) to 1 (bottom, '+').

    The report byte (0x04) is PARTIAL in cdj_actions: the firmware takes it
    into its tempo word, but the TEMPO readout has not been seen to follow.
    Which end is which byte value is not established either.
    """

    DETENT = 0.015

    def __init__(self, send):
        self.send = send
        self.action = A.ACTIONS["tempo"]
        self.pos = 0.5
        self.sent = None

    def set(self, pos):
        pos = max(0.0, min(1.0, pos))
        if abs(pos - 0.5) < self.DETENT:
            pos = 0.5
        self.pos = pos
        value = round(pos * 127)
        if value != self.sent:
            self.sent = value
            self.send(self.action.datagram(value))
