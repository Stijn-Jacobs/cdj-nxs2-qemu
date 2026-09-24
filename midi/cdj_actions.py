#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The CDJ side of the controller map: what a virtual deck can be told.

This is the set of front panel report bits there is evidence for, and each
entry carries how strong that evidence is. A binding to an invented report bit
would silently do nothing, so every action names its status, bridge.py refuses
to send `unbound` ones, and the learn/bridge tools print the status next to
the binding.

Transport is the panel key channel (hw/cdj/cdj_panelkeys.h): one UDP datagram
per press, `<off>:<val>:<dur_ms>[:<op>]`. The main ops:

    or    report[off] |= val   for as long as dur_ms lasts   (a held key)
    rot   report[off] += val   permanently, in every frame   (a counter)
    lvl   report[off]  = val   and held                      (an analogue byte)

The select knob is a counter the firmware differences between frames, not a
held bit, which is why it needs its own op.

Evidence status:

    confirmed   seen to move the firmware's own state
    partial     does something, but not the thing its name implies
    decoded     the firmware's panel decoder reads this bit into a setter the
                firmware itself names, but it has not been seen to act on a deck
    guess       ordered from the front panel's layout, not verified at the
                decoder; also every raw key_0xOFF_0xMASK bit with no name yet
    unbound     no report bit is known; the control has nowhere to go

Every bit is mappable. Besides the named actions, `key_0xOFF_0xMASK` names any
single bit of the report, `level_0xOFF` any whole analogue byte and
`rot_0xOFF` any counter byte. `catalogue()` lists every named action plus a raw
key for each unnamed bit of the bytes the decoder tests (KEY_BYTES).
"""

import re

CONFIRMED = "confirmed"
PARTIAL = "partial"
GUESS = "guess"
UNBOUND = "unbound"
DECODED = "decoded"


class Action:
    """One thing a deck can be told to do, and how sure we are it works."""

    def __init__(self, name, status, note=""):
        self.name = name
        self.status = status
        self.note = note

    @property
    def sendable(self):
        return self.status != UNBOUND

    def datagram(self, value=1, dur_ms=150):
        """The wire string for the panel key channel. Subclasses define it."""
        raise NotImplementedError


class KeyAction(Action):
    """A report bit held for dur_ms -- an ordinary front panel button."""

    def __init__(self, name, off, mask, status, note=""):
        super().__init__(name, status, note)
        self.off = off
        self.mask = mask

    def datagram(self, value=1, dur_ms=150):
        return f"0x{self.off:02x}:{self.mask}:{dur_ms}:or"

    # A real button is down exactly as long as the finger is. The timed press
    # above merges taps closer than dur_ms and releases up to dur_ms late, so
    # the bridge sends these.
    def hold_datagram(self):
        return f"0x{self.off:02x}:{self.mask}:0:hold"

    def release_datagram(self):
        return f"0x{self.off:02x}:{self.mask}:0:rel"


class RotaryAction(Action):
    """A persistent counter -- the select knob. `value` is the detent delta."""

    def __init__(self, name, off, status, note=""):
        super().__init__(name, status, note)
        self.off = off

    def datagram(self, value=1, dur_ms=0):
        return f"0x{self.off:02x}:{int(value)}:0:rot"


class JogAction(Action):
    """The platter: several bits at one offset, held while the jog moves.

    The CDJ jog is not a position or a delta. It is three level bits the
    decoder polls -- JogMov says the platter is moving, JogRoundFwd says which
    way, JogTouch says a hand is on it -- and all three live at offset 0x0F,
    so one datagram carries the whole state.

    That fits the transport exactly as it is. Datagrams only ever OR bits in
    and dur_ms expires them, so "moving forward" is simply re-sending
    JogMov|JogRoundFwd while the ticks keep coming, and "stopped" is not
    sending anything and letting it lapse. Reverse is JogMov alone -- the
    direction bit is a level, and a level that is never set is the reverse
    direction, which is why an OR-only channel can express it at all.

    A DJ-202 platter emits about 160 ticks a second, so `min_interval_ms`
    coalesces them: the bits only need re-arming faster than they expire.

    JogTouch is deliberately NOT part of this. It is its own key bound to the
    platter's own touch note, so a hand resting on a stationary platter holds
    the touch bit without claiming the deck is moving. Both OR into the same
    byte, which is exactly what the transport is for.
    """

    OFF = 0x0F
    TOUCH = 0x20
    ROUND_FWD = 0x40
    MOV = 0x80

    def __init__(self, name, status, note="", min_interval_ms=40):
        super().__init__(name, status, note)
        self.min_interval_ms = min_interval_ms

    def datagram(self, value=1, dur_ms=150):
        mask = self.MOV | (self.ROUND_FWD if value > 0 else 0)
        return f"0x{self.OFF:02x}:{mask}:{dur_ms}:or"


class Platter:
    """One deck's platter as the NXS2 panel CPU reports it: position + period.

    The level bits alone never bend the track. The firmware's jog decoder
    (FUN_08443E5C) takes motion and direction from a 16-bit ROLLING POSITION
    COUNTER in report bytes 8-9, and its jog engine (FUN_08444318) takes the
    speed from a PULSE PERIOD P in bytes 10-11: platter speed % = 27778 / P, so
    P = 278 is a platter at nominal speed and 0 means stopped. Both are
    big-endian and both are needed. Measured on the real DSP: 1.060x speed at
    P=278, 1.180x at P=139.

    The DJ-202 reports ticks, so this keeps a counter of its own and turns the
    recent tick rate into a period. `nominal_tps` is the DJ-202 tick rate that
    counts as the platter turning at nominal speed; `pulses_per_tick` scales the
    counter, which must move by >= 42 per 30 firmware passes before the bend
    engages (2000 pulses/s was measured to be enough).
    """

    POS = 0x08
    PERIOD = 0x0A
    NOMINAL_PERIOD = 27778 / 100
    WINDOW_S = 0.08
    STOP_S = 0.1

    def __init__(self, nominal_tps=160.0, pulses_per_tick=48):
        self.nominal_tps = nominal_tps
        self.pulses_per_tick = pulses_per_tick
        self.count = 0
        self.ticks = []            # (time, delta) inside WINDOW_S
        self.moving = False
        self.last_rate = self.last_period = 0

    def tick(self, delta, now):
        self.count = (self.count + delta * self.pulses_per_tick) & 0xFFFF
        self.ticks.append((now, delta))

    def datagrams(self, now, dur_ms=150):
        """The report fields to (re)send now, or [] when nothing changed."""
        self.ticks = [(t, d) for t, d in self.ticks if now - t <= self.WINDOW_S]
        if not self.ticks or now - self.ticks[-1][0] > self.STOP_S:
            if not self.moving:
                return []
            self.moving = False
            return [f"0x{self.PERIOD:02x}:0:0:lvl", f"0x{self.PERIOD + 1:02x}:0:0:lvl"]
        self.moving = True
        net = sum(d for _, d in self.ticks)
        rate = sum(abs(d) for _, d in self.ticks) / self.WINDOW_S
        period = int(round(self.NOMINAL_PERIOD * self.nominal_tps / max(rate, 1e-3)))
        period = max(1, min(0xFFFE, period))
        self.last_rate, self.last_period = rate, period
        bits = JogAction.MOV | (JogAction.ROUND_FWD if net >= 0 else 0)
        return [f"0x{self.POS:02x}:{self.count >> 8}:0:lvl",
                f"0x{self.POS + 1:02x}:{self.count & 0xFF}:0:lvl",
                f"0x{self.PERIOD:02x}:{period >> 8}:0:lvl",
                f"0x{self.PERIOD + 1:02x}:{period & 0xFF}:0:lvl",
                f"0x{JogAction.OFF:02x}:{bits}:{dur_ms}:or"]


class LevelAction(Action):
    """An analogue field: one whole report byte, assigned and held.

    The panel decoder reads two bytes as values rather than bits and hands
    them to their setters zero-extended -- byte 0x02 is the release/start pot
    and byte 0x04 the tempo slider. A key op cannot carry either: "or" only
    ever sets bits and then expires, so it can neither lower a value nor hold
    a midpoint. The machine's `lvl` op assigns the byte and keeps it.

    MIDI gives 7 bits and the field is 8, so the value is doubled. Whether the
    firmware's usable range really is the full 0..254 is not established --
    what IS established is which byte carries it and that the byte arrives as
    a value.
    """

    def __init__(self, name, off, status, note="", full_scale=254):
        super().__init__(name, status, note)
        self.off = off
        self.full_scale = full_scale

    def datagram(self, value=0, dur_ms=0):
        scaled = int(round(max(0, min(127, value)) * self.full_scale / 127))
        return f"0x{self.off:02x}:{scaled}:0:lvl"


class TouchTapAction(Action):
    """A tap on the deck's touch screen at one pixel, for a pad or button.

    The panel key socket also carries the touch screen (cdj_panelkeys.h:
    `<x>:<y>:<ms>:tap`, x 0..799, y 0..479). x goes out in hex because the
    relay's line format wants the first field as 0x..; the machine reads it
    with %i, which takes either. The pixel comes from the mapping entry:
    {"action": "touch_tap", "x": 400, "y": 240}.
    """

    def __init__(self, name, status, note="", x=400, y=240):
        super().__init__(name, status, note)
        self.x = max(0, min(799, int(x)))
        self.y = max(0, min(479, int(y)))

    def datagram(self, value=1, dur_ms=150):
        return f"0x{self.x:x}:{self.y}:{max(int(dur_ms), 1)}:tap"


class UnboundAction(Action):
    """A DJ-202 control with no known CDJ target. Kept so the map is honest."""

    def __init__(self, name, note):
        super().__init__(name, UNBOUND, note)


# Key bits and names come from the firmware: the service console's key table
# at 0x0809137C, inject_key's jump table at 0x08201316 and the panel decoder at
# 0x0844D400. The names are the firmware's, not the panel silkscreen's; e.g.
# 0x12:0x10 is ScanRev and the real Cue is 0x10:0x02.
ACTIONS = {a.name: a for a in [
    RotaryAction("select_turn", 0x0E, CONFIRMED,
                 "one row per detent, two boots, against an idle control"),

    KeyAction("play_pause", 0x10, 0x01, PARTIAL,
              "PAUSES but does not RESUME: one press stops a playing deck "
              "within 5 s, a second press leaves it stopped. Playback starts "
              "from the load, not from this key. Do not test this with your "
              "hand on the jog: jog_touch freezes the position by itself"),
    KeyAction("cue", 0x10, 0x02, CONFIRMED, "the real Cue"),
    KeyAction("reloop_exit", 0x10, 0x04, CONFIRMED, ""),
    KeyAction("loop_out", 0x10, 0x08, CONFIRMED, ""),
    KeyAction("loop_in", 0x10, 0x10, CONFIRMED, ""),

    KeyAction("rotary_push", 0x11, 0x01, CONFIRMED,
              "enters a browse row; this is what loading a track needs"),
    KeyAction("slip_mode", 0x11, 0x02, CONFIRMED, ""),
    KeyAction("direction_rev", 0x11, 0x04, CONFIRMED, ""),
    KeyAction("slip_reverse", 0x11, 0x08, CONFIRMED, ""),

    KeyAction("track_rev", 0x12, 0x04, CONFIRMED, ""),
    KeyAction("track_fwd", 0x12, 0x08, CONFIRMED, ""),
    KeyAction("scan_rev", 0x12, 0x10, CONFIRMED,
              "search backwards; holding it moves and releasing reverts"),
    KeyAction("scan_fwd", 0x12, 0x20, CONFIRMED, ""),

    KeyAction("dev_usb", 0x13, 0x04, CONFIRMED, ""),

    KeyAction("browse", 0x14, 0x01, CONFIRMED, ""),
    KeyAction("taglist", 0x14, 0x02, CONFIRMED, ""),
    KeyAction("information", 0x14, 0x04, CONFIRMED, ""),
    KeyAction("menu_utility", 0x14, 0x08, CONFIRMED, ""),
    KeyAction("back", 0x14, 0x10, CONFIRMED, ""),
    KeyAction("tagtrack", 0x14, 0x20, CONFIRMED, ""),

    KeyAction("sync", 0x15, 0x02, CONFIRMED, ""),
    KeyAction("tempo_range", 0x15, 0x08, CONFIRMED, ""),
    KeyAction("master_tempo", 0x15, 0x10, CONFIRMED, ""),
    # Jog mode (VINYL/CDJ): report 0x15 bit 0x01, read at 0x0844D968 into the
    # setter 0x084E16E0 (JogModeVinyl). Only in VINYL mode does a touched
    # platter hold/scratch; in CDJ mode the top bends like the rim.
    KeyAction("jog_mode", 0x15, 0x01, PARTIAL,
              "the decoder site and setter are read off the firmware; the "
              "toggle itself is not yet seen on the glass"),

    KeyAction("memory", 0x0C, 0x08, CONFIRMED, ""),

    # The jog, as its three separate bits, for anything that wants them raw.
    KeyAction("jog_touch", 0x0F, 0x20, CONFIRMED, ""),
    KeyAction("jog_round_fwd", 0x0F, 0x40, CONFIRMED, ""),
    KeyAction("jog_mov", 0x0F, 0x80, CONFIRMED, ""),

    # ...and as one thing a platter can be bound to.
    JogAction("jog", CONFIRMED,
              "bits are firmware-table facts; whether a stopped deck reacts "
              "to a platter is a separate question"),

    # The two analogue pots, from the decoder's value-read sites. The tempo
    # slider field moves a real word (setter TempoSliderMax), but the tempo
    # readout has not been seen to follow it. The tempo keys do work:
    # TempoRange 0x15:0x08 cycles the range badge, MasterTempo 0x15:0x10
    # lights the MT badge.
    LevelAction("tempo", 0x04, PARTIAL,
                "moves the firmware's word at 0x0B54CAE8 (+deck*716) as "
                "byte<<8, measured with MWATCH -- but the TEMPO readout does "
                "not follow it, on a deck with a track loaded"),
    LevelAction("release_start", 0x02, PARTIAL,
                "the other analogue pot, ReleaseStartMax (0x084E1654); same "
                "standing as tempo"),

    # The rest of the panel decoder (0x0844D400), named after the firmware's
    # key table. Decoded, not verified on a running deck. "(2000 only)" is the
    # firmware's own label: the NXS2 decodes those keys anyway.
    KeyAction("sd_door_open", 0x10, 0x40, DECODED,
              "firmware name 'SDdoorOpen( 2000 only )'"),
    KeyAction("rec_mode", 0x12, 0x02, DECODED, "firmware name 'RecMode'"),
    KeyAction("dev_rekordbox", 0x13, 0x01, DECODED, "firmware name 'DevRekordbox'"),
    KeyAction("dev_link", 0x13, 0x02, DECODED, "firmware name 'DevLink'"),
    KeyAction("dev_sd", 0x13, 0x08, DECODED, "firmware name 'DevSD( 2000 only )'"),
    KeyAction("dev_disc", 0x13, 0x10, DECODED, "firmware name 'DevDisc'"),
    KeyAction("time_a_cue", 0x13, 0x20, DECODED, "firmware name 'TimeAcue'"),
    KeyAction("master", 0x15, 0x04, DECODED, "firmware name 'Master'"),
    KeyAction("tempo_reset", 0x15, 0x20, DECODED,
              "firmware name 'TempoReset( 2000 only )'"),
    KeyAction("call_rev", 0x0C, 0x01, DECODED,
              "firmware name 'Call_Rev'; the decoder hands the same bit to "
              "LoopCut on another branch"),
    KeyAction("call_fwd", 0x0C, 0x02, DECODED,
              "firmware name 'Call_Fwd'; the decoder hands the same bit to "
              "LoopDouble on another branch"),
    KeyAction("delete", 0x0C, 0x04, DECODED, "firmware name 'Delete'"),
    KeyAction("eject", 0x0C, 0x10, DECODED, "firmware name 'Eject'"),
    KeyAction("four_beat_loop", 0x1A, 0x80, DECODED,
              "firmware name 'fourBeatLoop( 2000 only )'"),

    # The touch screen, one pixel per binding (see TouchTapAction).
    TouchTapAction("touch_tap", PARTIAL,
                   "the touch path reaches the firmware's own UI handler; "
                   "driving it from a controller is untested"),

    # Named by the firmware, with no known report field.
    UnboundAction("needle_search",
                  "the firmware names NeedleTouch and NeedlePositionRev/Fwd, "
                  "but its panel decoder reads no report byte for them; the "
                  "needle strip is not modelled"),
    UnboundAction("hot_cue",
                  "the decoder hands report byte 0x1A to the HotCueA..H "
                  "setter, but which bit is which cue is not established -- "
                  "bind key_0x1a_0x01 .. key_0x1a_0x40 raw to find out"),
]}

# The bytes the panel decoder tests bit by bit. Every bit of these is listed
# by catalogue() as a mappable key_0xOFF_0xMASK action, named or not.
KEY_BYTES = (0x0C, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x1A)

# The three names the older notes used, kept working so nothing silently
# stops firing when a map file predates the firmware-table rewrite.
for _old, _new in (("select_push", "rotary_push"), ("menu", "menu_utility"),
                   ("usb", "dev_usb")):
    ACTIONS[_old] = ACTIONS[_new]


_RAW = re.compile(r"^(?:key|raw)_0x([0-9a-f]{1,2})_0x([0-9a-f]{1,2})$", re.I)
_LEVEL = re.compile(r"^level_0x([0-9a-f]{1,2})$", re.I)
_ROT = re.compile(r"^rot_0x([0-9a-f]{1,2})$", re.I)


def named_bit(off, mask):
    """The named key action on exactly this bit, if there is one."""
    for name, a in ACTIONS.items():
        if type(a) is KeyAction and a.name == name and a.off == off and a.mask == mask:
            return a
    return None


def resolve(name, spec=None):
    """Look an action up by mapping-file name, or raise with a usable message.

    `spec` is the mapping entry, for the actions that take a parameter from it
    (touch_tap's pixel). The raw names build an action for any bit or byte.
    """
    spec = spec or {}
    if name == "touch_tap":
        base = ACTIONS[name]
        return TouchTapAction(name, base.status, base.note,
                              spec.get("x", 400), spec.get("y", 240))
    if name in ACTIONS:
        return ACTIONS[name]
    m = _RAW.match(name)
    if m:
        off, mask = int(m.group(1), 16), int(m.group(2), 16)
        known = named_bit(off, mask)
        note = (f"the same bit as '{known.name}'" if known
                else "raw report bit, no name yet: unverified")
        return KeyAction(name, off, mask, known.status if known else GUESS, note)
    m = _LEVEL.match(name)
    if m:
        return LevelAction(name, int(m.group(1), 16), GUESS,
                           "raw analogue byte, unverified")
    m = _ROT.match(name)
    if m:
        return RotaryAction(name, int(m.group(1), 16), GUESS,
                            "raw counter byte, unverified")
    raise KeyError(
        f"unknown action {name!r}; known: {', '.join(sorted(ACTIONS))}, "
        "or key_0xOFF_0xMASK / level_0xOFF / rot_0xOFF for a raw field")


def catalogue():
    """Every mappable action: [(name, kind, where, status, note)].

    The named ones first, then a key_0xOFF_0xMASK for every bit of KEY_BYTES
    that no named action covers.
    """
    rows, taken = [], set()
    for name, a in ACTIONS.items():
        if a.name != name:          # an old alias
            continue
        if isinstance(a, KeyAction):
            where = f"0x{a.off:02x}:0x{a.mask:02x}"
            taken.add((a.off, a.mask))
        elif isinstance(a, (LevelAction, RotaryAction)):
            where = f"0x{a.off:02x}"
        elif isinstance(a, JogAction):
            where = f"0x{a.OFF:02x} + 0x08..0x0b"
        elif isinstance(a, TouchTapAction):
            where = "x,y"
        else:
            where = "-"
        rows.append((name, type(a).__name__.replace("Action", "").lower(),
                     where, a.status, a.note))
    for off in KEY_BYTES:
        for bit in range(8):
            mask = 1 << bit
            if (off, mask) not in taken:
                rows.append((f"key_0x{off:02x}_0x{mask:02x}", "key",
                             f"0x{off:02x}:0x{mask:02x}", GUESS,
                             "no name yet; bind it to find out what it does"))
    return rows


def raw_action(off, mask, dur_ms=150):
    """An escape hatch for sweeping: bind a DJ-202 control to any report bit.

    Marked `guess` on purpose -- a raw binding is by definition unverified,
    and the bridge should say so every time it fires one.
    """
    return KeyAction(f"raw_0x{off:02x}_0x{mask:02x}", off, mask, GUESS,
                     "raw report bit from the map file, unverified")
