#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Deck state -> DJ-202 LEDs. The way back through the bridge.

The DJ-202 lights a button when it receives the SAME channel and note that
button sends, on its output port: velocity 127 on, 0 off (verified on PLAY,
CUE, pads, FX 1-3, TAP, SLIP and KEY LOCK). So an LED needs no map of its own;
it is addressed by its control's name in the controller profile
(controllers/<name>.json).

What to light comes from the deck. Each machine publishes its state through
scripts/run/midi_relay.py (see cdj_stateout_* in hw/cdj/):

    <tag> state frm beat=3 bars=13 tempo=0 bpm=1280
    <tag> state pnl <80 hex chars>

`frm` is read off the MAIN->GUI heartbeat after every emulator patch, so the
beat is exactly the box the screen lights. `pnl` is MAIN's frame to the panel
micro, which carries the NXS2's own lamps.

The NXS2's own lamps:

  pnl byte 0 bit 0   PLAY  slot 0x0B54C05C, setter 0x084DC2D8. Steady on while
                     playing; the instant PLAY paused the deck it began to
                     blink (1 s period in firmware time).
  pnl byte 0 bit 1   CUE   slot 0x0B54C060, setter 0x084DC2E0. Off while
                     playing, blinking at twice PLAY's rate once paused -- the
                     NXS2's "paused off the cue point". Named by behaviour: it
                     has not yet been seen go steady on the cue point.
  pnl byte 3 bit 0x02, byte 3 bit 0x20, byte 3 bit 0x80, byte 4 bit 0x80
                     SD, LINK, rekordbox (PC) and DISC source lamps. Each is
                     steady on while its source is selected and clears when
                     another source key is pressed. USB's own signal is not
                     yet measured -- it is not simply "none of the other
                     four" (that was tried and retracted; see the audit doc).
  pnl byte 2 bit 0x10
                     TEMPO RESET indicator, slot/setter not yet traced.
                     Toggles on press.
  pnl byte 5 bits 0x80 | 0x20
                     USB port indicator (separate from the USB source-select
                     key): steady on while the medium is mounted, dark at
                     rest. Confirmed against the real unit's own behaviour
                     (steady while mounted, flashes only while ejecting --
                     not measured here, no key presses it): sampled every
                     ~20 ms this bit makes one transition, then holds
                     rock-steady, matching "steady", not "flashes".

The frame builder is FUN_0844F808 (most of it was undefined in Ghidra); MAIN
toggles the bit itself to blink, so mirroring the bit reproduces the NXS2's
own blink rate with no timer here.

The scheme, per deck (roles bound to controls in the mapping's "leds"):

  play   mirrors the NXS2's PLAY lamp.
  cue    mirrors the NXS2's CUE lamp.
  slip   mirrors the NXS2's SLIP lamp (pnl byte 0 bit 7), on SLIP.
  master_tempo
         mirrors the NXS2's MASTER TEMPO lamp (pnl byte 1 bit 2), on KEY LOCK.
  ring   mirrors the NXS2's jog ring light (pnl byte 2, two 2-bit levels), on
         the headphone CUE buttons: lit at any level, so SLIP's flash shows.
  source_sd, source_disc, source_link, source_pc
         mirror the NXS2's SD/DISC/LINK/rekordbox source-select lamps. USB's
         is not wired here yet -- its signal has not been measured.
  tempo_reset
         mirrors the NXS2's TEMPO RESET indicator, on TEMPO RESET.
  usb_indicator
         mirrors the NXS2's USB port indicator (steady while a medium is
         mounted). Not on the DJ-202.
  beat   four LEDs (FX 1, FX 2, FX 3, TAP) show the firmware's beat in the
         bar, exactly as the screen's beat box does (held while paused). In
         the LAST BAR of a phrase (the deck's own Bars countdown <= 4) they
         FILL instead -- 1, 1-2, 1-2-3, all four -- so the phrase turn is
         visible before it lands.
  pads   a pad with a binding is lit, so the unit shows what it can do.
"""
import time

import mido

# pnl byte, mask: the lamp is lit while any masked bit is set. A role mapped in
# the mapping but missing here stays dark -- SYNC, which needs a second deck
# to lock to before its lamp can be seen.
#
# The jog ring (JOGILM_W / JOGILM_R on the schematic) is two 2-bit levels in
# byte 2: bits 3:2 from slot 0x0B54C09C, bits 1:0 from 0x0B54C0A0 (getters
# 0x084DC640 / 0x084DC64A, packed at 0x0844F990). FUN_084DC768 sets them from
# the JOG BRIGHTNESS setting (*0x0A35F6E0). Bits 3:2 read 3 whenever a deck is
# up and flash on SLIP (the SLIP FLASHING setting); bits 1:0 have never been
# seen lit, so which field is white and which red is by behaviour. The
# headphone CUE LED has one colour, so it shows the ring lit at any level.
LAMPS = {
    "play": (0x00, 0x01),
    "cue": (0x00, 0x02),
    "slip": (0x00, 0x80),           # on at the 1st SLIP press, off at the 2nd
    "master_tempo": (0x01, 0x04),   # on at the 1st MT press, off at the 2nd
    "ring": (0x02, 0x0F),
    "source_sd": (0x03, 0x02),      # dev_sd
    "source_link": (0x03, 0x20),    # dev_link
    "source_pc": (0x03, 0x80),      # dev_rekordbox
    "source_disc": (0x04, 0x80),    # dev_disc
    "tempo_reset": (0x02, 0x10),
    "usb_indicator": (0x05, 0xA0),
}
# source_usb has no LAMPS entry: pressing dev_usb never set a bit anywhere in
# byte 3/4 (unlike dev_sd/dev_disc/dev_link, each its own bit there), and "USB
# is lit whenever none of the other three are" was tried and retracted -- it
# is an inference, not a measured signal, and this project wires only what
# the firmware is measured to send. usb_indicator above is a different lamp
# (the port indicator next to the slot, not the source-select key) and does
# have a measured bit.
ROLE_NAMES = tuple(LAMPS)
# Page-2 (pnl2) lamps reported by the CDJ-850 panel status packet, verified
# against app/faces/cdj850.json. Kept separate from NXS2 LAMPS/ROLE_NAMES so
# NXS2 mappings and byte-0 checks never collide with CDJ-850's frame-1 type byte.
CDJ850_LAMPS = {
    "play": (0x05, 0x10),
    "cue": (0x05, 0x08),
    "master_tempo": (0x06, 0x10),
    "vinyl": (0x06, 0x20),
    "loop_in": (0x05, 0x20),
    "loop_out": (0x05, 0x40),
    "reloop": (0x06, 0x01),
    "rev": (0x06, 0x08),
    "beat_1": (0x07, 0x01),
    "beat_2": (0x07, 0x02),
    "beat_4": (0x07, 0x04),
    "beat_8": (0x07, 0x08),
    "browse": (0x07, 0x10),
    "tag_list": (0x07, 0x20),
    "info": (0x07, 0x40),
    "menu": (0x07, 0x80),
    "eject": (0x06, 0x40),
    "standby": (0x08, 0x01),
    "source_pc": (0x05, 0x04),
    "source_usb": (0x05, 0x80),
    "source_disc": (0x06, 0x02),
    "usb_indicator": (0x05, 0x02),
}
CDJ850_ROLE_NAMES = tuple(CDJ850_LAMPS)
ALL_ROLE_NAMES = ROLE_NAMES + tuple(
    r for r in CDJ850_ROLE_NAMES if r not in LAMPS
)
STALE_S = 3.0           # no state for this long: the deck is gone, go dark


def lamp_lit(frame, name):
    """True while the 40-byte pnl `frame` lights the named lamp."""
    byte, mask = LAMPS[name]
    return bool(frame[byte] & mask)


def lamp_lit_page2(frame, name):
    """True while a CDJ-850 page-2 `pnl2` frame lights the named lamp."""
    byte, mask = CDJ850_LAMPS[name]
    return byte < len(frame) and bool(frame[byte] & mask)


def parse_state(payload):
    """'frm beat=3 bpm=1280' -> ('frm', {'beat': 3, 'bpm': 1280}); 'pnl <hex>' / 'pnl2 <hex>' -> bytes."""
    kind, _, rest = payload.partition(" ")
    if kind in ("pnl", "pnl2"):
        try:
            return kind, bytes.fromhex(rest.strip())
        except ValueError:
            return None, None
    if kind == "frm":
        fields = {}
        for item in rest.split():
            k, _, v = item.partition("=")
            try:
                fields[k] = int(v)
            except ValueError:
                pass
        return kind, fields
    return None, None


class LedSurface:
    """The DJ-202's lamps. Sends only what changed; MIDI out is not free."""

    def __init__(self, controls, port_name, dry_run=False):
        self.controls = controls
        self.port = None if dry_run else mido.open_output(port_name)
        self.lit = {}

    def address(self, name):
        c = self.controls.get(name)
        if c is None or c["type"] != "note":
            return None
        return c["channel"], c["number"]

    def set(self, name, on):
        addr = self.address(name)
        if addr is None:
            return
        vel = 127 if on else 0
        if self.lit.get(addr) == vel:
            return
        self.lit[addr] = vel
        if self.port:
            self.port.send(mido.Message("note_on", channel=addr[0],
                                        note=addr[1], velocity=vel))

    def forget(self, channel, note):
        """The unit drives a button's own LED on press/release, behind our
        back, so the next render must re-send it rather than trust the cache."""
        self.lit.pop((channel, note), None)

    def all_off(self):
        for (ch, note) in self.lit:
            if self.port:
                self.port.send(mido.Message("note_on", channel=ch, note=note,
                                            velocity=0))
        self.lit.clear()

    def close(self):
        if self.port:
            self.port.close()


class DeckLeds:
    """One deck's state and the LEDs it drives."""

    def __init__(self, deck, tag, roles, surface, pads):
        self.deck = deck
        self.tag = tag
        self.roles = roles
        self.surface = surface
        self.pads = pads
        self.frm = {}
        self.pnl = None
        self.pnl2 = None
        self.seen = float("-inf")
        self.announced = False

    def update(self, kind, value, panel_diff=False):
        self.seen = time.time()
        if not self.announced:
            self.announced = True
            print(f"leds: {self.deck} ({self.tag}) is publishing state")
        if kind == "frm":
            self.frm = value
        elif kind == "pnl":
            if panel_diff and self.pnl is not None:
                self._print_panel_diff(value, self.pnl, "pnl")
            self.pnl = value
        elif kind == "pnl2":
            if panel_diff and self.pnl2 is not None:
                self._print_panel_diff(value, self.pnl2, "pnl2")
            self.pnl2 = value

    def lamp(self, name):
        if self.pnl2 is not None:
            return name in CDJ850_LAMPS and lamp_lit_page2(self.pnl2, name)
        return self.pnl is not None and name in LAMPS and lamp_lit(self.pnl, name)

    def _print_panel_diff(self, new, old, label):
        changes = [f"[{i:02x}] {a:02x}->{b:02x}"
                   for i, (a, b) in enumerate(zip(old, new)) if a != b]
        if changes:
            print(f"{label} {self.deck}: {'  '.join(changes)}")

    def render(self, now):
        live = now - self.seen < STALE_S

        # The beat box as the NXS2 screen draws it: no play gate. A gate on
        # "PLAY steadily lit" held the LEDs dark for 1.5 s after every
        # resume, which read on the controller as the bar restarting.
        beat_leds = self.roles.get("beat", [])
        beat = self.frm.get("beat", 0)
        bars = self.frm.get("bars", 0)
        in_bar = live and 1 <= beat <= len(beat_leds)
        fill = in_bar and 0 < bars <= len(beat_leds)
        for i, name in enumerate(beat_leds, start=1):
            self.surface.set(name, in_bar and (i <= beat if fill else i == beat))

        for role in ALL_ROLE_NAMES:
            if role in self.roles:
                self.surface.set(self.roles[role], live and self.lamp(role))
        for name in self.pads:
            self.surface.set(name, True)


class Leds:
    """Every deck's LEDs, fed from relay state lines."""

    def __init__(self, controls, mapping, port_name, dry_run=False,
                 panel_diff=False):
        self.surface = LedSurface(controls, port_name, dry_run)
        self.panel_diff = panel_diff
        decks = {k: v for k, v in mapping["decks"].items()
                 if not k.startswith("_")}
        bound = {n: s for n, s in mapping["bindings"].items()
                 if not n.startswith("_")}
        led_roles = mapping.get("leds", {})
        self.by_tag = {}
        for deck, tag in decks.items():
            pads = sorted(n for n, s in bound.items()
                          if s.get("deck") == deck and "/pad_" in n
                          and controls.get(n, {}).get("led"))
            self.by_tag[tag] = DeckLeds(deck, tag, led_roles.get(deck, {}),
                                        self.surface, pads)

    def feed(self, tag, payload):
        deck = self.by_tag.get(tag)
        if deck is None:
            return
        kind, value = parse_state(payload)
        if kind:
            deck.update(kind, value, self.panel_diff)

    def render(self):
        now = time.time()
        for deck in self.by_tag.values():
            deck.render(now)

    def hello(self):
        """A one-bar sweep across every beat LED: the bridge owns the lamps."""
        rows = [d.roles.get("beat", []) for d in self.by_tag.values()]
        for step in range(max((len(r) for r in rows), default=0)):
            for row in rows:
                for i, name in enumerate(row):
                    self.surface.set(name, i == step)
            time.sleep(0.12)
        self.surface.all_off()

    def close(self):
        self.surface.all_off()
        self.surface.close()
