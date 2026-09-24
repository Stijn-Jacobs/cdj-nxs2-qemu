#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Any MIDI controller -> two virtual CDJs. Talks to scripts/run/midi_relay.py.

    python midi\\bridge.py --relay 127.0.0.1:7202
    python midi\\bridge.py --controller my-controller --prefix show

Which controller: --controller names a profile in controllers/ (or a path);
without it the bridge picks the profile whose "match" appears in a connected
MIDI port's name, and falls back to the Roland DJ-202. The mapping is
mappings/<controller>.json unless --mapping says otherwise. profiles.py says
what each file holds; learn.py makes both for a new controller.

The bridge reads MIDI and decides what it means; midi_relay.py turns each
decision into a panel key datagram for the machine (hw/cdj/cdj_panelkeys.h).
The seam is one line of text per press:

    <tag> <off>:<val>:<dur_ms>:<op>        e.g.  show1 0x14:1:150:or

so the relay needs no map knowledge, and a sweep can be driven by hand with
netcat against the same port. The link is TCP because a dropped key press is
worse than a late one: a bit that is set and never cleared leaves a key stuck
down.

Holds: a CDJ key is a bit that is set while held, and dur_ms expires it. The
bridge resends the datagram while the note is down and stops on note_off, so
the bit stays set for as long as the button is held. That is why the loop
polls instead of using mido's callback.

Every binding carries a status from cdj_actions.py. An unbound control prints
why the first time it is touched, then stays quiet; guessed bindings are
marked on every send. Nothing is dropped silently.
"""
import argparse
import json
import os
import select
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdj_actions  # noqa: E402
import leds  # noqa: E402
import profiles  # noqa: E402

try:
    import mido
except ImportError:
    sys.exit("mido is missing:  python -m pip install mido python-rtmidi")

HERE = os.path.dirname(os.path.abspath(__file__))


class Relay:
    """The line to the relay. Reconnects, because the machines outlive the bridge."""

    def __init__(self, host, port, dry_run=False):
        self.addr = (host, port)
        self.dry_run = dry_run
        self.sock = None
        self.inbuf = b""

    def connect(self):
        if self.dry_run or self.sock:
            return True
        try:
            self.sock = socket.create_connection(self.addr, timeout=2)
            self.sock.settimeout(None)
            print(f"relay connected: {self.addr[0]}:{self.addr[1]}")
            return True
        except OSError as e:
            self.sock = None
            print(f"relay unreachable ({e}); is scripts/run/midi_relay.py running?")
            return False

    def send(self, tag, datagram):
        line = f"{tag} {datagram}\n".encode()
        if self.dry_run:
            return
        if not self.sock and not self.connect():
            return
        try:
            self.sock.sendall(line)
        except OSError:
            self.sock = None
            self.connect()

    def lines(self):
        """Whatever the relay sent back: deck state, one line each."""
        if not self.sock:
            return []
        try:
            while select.select([self.sock], [], [], 0)[0]:
                chunk = self.sock.recv(65536)
                if not chunk:
                    self.sock = None
                    break
                self.inbuf += chunk
        except OSError:
            self.sock = None
        *done, self.inbuf = self.inbuf.split(b"\n")
        return [d.decode("ascii", "replace") for d in done]


class Binding:
    """One captured control joined to one CDJ action."""

    def __init__(self, name, capture, spec):
        self.name = name
        self.capture = capture
        self.spec = spec
        self.deck = spec.get("deck")
        self.scale = spec.get("scale", 1)
        self.action_name = spec["action"]
        self.is_focus_switch = self.action_name == "set_focus"
        self.action = (None if self.is_focus_switch
                       else cdj_actions.resolve(self.action_name, spec))
        self.warned = False
        self.last_sent = 0.0
        self.last_value = 0

    @property
    def behaviour(self):
        return self.capture["behaviour"]


class Bridge:
    def __init__(self, capture, mapping, relay, dur_ms, verbose):
        self.relay = relay
        self.dur_ms = dur_ms
        self.verbose = verbose
        self.decks = {k: v for k, v in mapping["decks"].items()
                      if not k.startswith("_")}
        self.focus = mapping.get("focus") or next(iter(self.decks))
        self.index = {}
        self.held = {}
        self.platters = {}          # deck -> cdj_actions.Platter
        self.platter_args = {}
        self.jog_debug = False
        self.next_jog_print = 0.0
        self._build_index(capture, mapping)

    def _build_index(self, capture, mapping):
        controls = capture["controls"]
        for name, spec in mapping["bindings"].items():
            if name.startswith("_"):
                continue
            if name not in controls:
                print(f"  {name}: in the map but never learned -- ignored")
                continue
            binding = Binding(name, controls[name], spec)
            c = binding.capture
            self.index[(c["port"], c["type"], c["channel"], c["number"])] = binding

        bound = {n for n in mapping["bindings"] if not n.startswith("_")}
        extra = sorted(set(controls) - bound)
        if extra:
            print(f"  {len(extra)} learned control(s) not in the map "
                  f"(they do nothing): {', '.join(extra[:8])}"
                  f"{' ...' if len(extra) > 8 else ''}")

    # -- resolving ---------------------------------------------------------

    def _tag(self, binding):
        deck = self.focus if binding.deck == "focus" else binding.deck
        tag = self.decks.get(deck)
        if tag is None:
            raise KeyError(f"{binding.name} names deck {deck!r}, "
                           f"which is not in 'decks'")
        return deck, tag

    def _refuse(self, binding):
        """An unbound action: say why once, then stay out of the way."""
        if not binding.warned:
            binding.warned = True
            print(f"  {binding.name} -> {binding.action_name}: NOT SENT. "
                  f"{binding.action.note}")
        return True

    def _fire(self, binding, value=1, dur_ms=None, datagram=None):
        if not binding.action.sendable:
            return self._refuse(binding)
        deck, tag = self._tag(binding)
        datagram = datagram or binding.action.datagram(value, dur_ms or self.dur_ms)
        self.relay.send(tag, datagram)
        if self.verbose:
            # Print the action's own caveat, once per binding (a fader sends
            # hundreds of messages).
            caveat = ""
            if binding.action.status == cdj_actions.GUESS:
                caveat = "  [UNVERIFIED map]"
            elif binding.action.status == cdj_actions.DECODED and not binding.warned:
                binding.warned = True
                caveat = f"  [decoder name only, never pressed here: {binding.action.note}]"
            elif binding.action.status == cdj_actions.PARTIAL and not binding.warned:
                binding.warned = True
                caveat = f"  [{binding.action.note}]"
            print(f"{binding.name:<20} -> {deck}/{binding.action_name:<12} "
                  f"{datagram}{caveat}")
        return True

    # -- MIDI --------------------------------------------------------------

    def handle(self, port_name, msg):
        if msg.type in ("note_on", "note_off"):
            key = (port_name, "note", msg.channel, msg.note)
            down = msg.type == "note_on" and msg.velocity > 0
        elif msg.type == "control_change":
            key = (port_name, "cc", msg.channel, msg.control)
            down = None
        elif msg.type == "pitchwheel":
            # A 14-bit fader (many controllers send their tempo slider this
            # way). Scaled to the 7 bits every other absolute control uses.
            key = (port_name, "pitch", msg.channel, 0)
            binding = self.index.get(key)
            if binding is not None:
                self._absolute(binding, (msg.pitch + 8192) >> 7)
            return
        else:
            return

        binding = self.index.get(key)
        if binding is None:
            return

        if down is not None:
            self._button(binding, key, down)
        elif binding.behaviour in ("relative", "relative_offset", "relative_signbit"):
            self._relative(binding, msg.value)
        else:
            self._absolute(binding, msg.value)

    def _button(self, binding, key, down):
        if binding.is_focus_switch:
            if down:
                self.focus = binding.deck
                print(f"focus -> {self.focus} ({self.decks[self.focus]})")
            return
        # Keys that can be held for real go down on note_on and up on note_off,
        # so a tap is exactly as long as the finger; the rest keep the re-armed
        # timed press (refresh_holds).
        if hasattr(binding.action, "hold_datagram"):
            if down:
                self._fire(binding, datagram=binding.action.hold_datagram())
            else:
                self._fire(binding, datagram=binding.action.release_datagram())
            return
        if down:
            if self._fire(binding):
                self.held[key] = binding
        else:
            self.held.pop(key, None)

    def _relative(self, binding, value):
        """Decode a detent, in whichever of the two encodings this control uses.

        The browse encoder is signed 7-bit and the platters are offset-64, so
        the capture's own `behaviour` decides. Reading a jog's 65 as signed
        would hand the firmware a 65-row jump per tick.
        """
        if binding.behaviour == "relative_offset":
            delta = value - 64
        elif binding.behaviour == "relative_signbit":
            # sign and magnitude: 1 = +1, 65 = -1 (another common encoding)
            delta = -(value - 64) if value >= 64 else value
        else:
            delta = value if value < 64 else value - 128
        if not delta:
            return

        if isinstance(binding.action, cdj_actions.JogAction):
            deck, _ = self._tag(binding)
            platter = self.platters.get(deck)
            if platter is None:
                platter = self.platters[deck] = cdj_actions.Platter(**self.platter_args)
            platter.tick(delta * binding.scale, time.time())
            return

        # A platter sends ~160 ticks a second and its bits are levels, so
        # coalesce; a direction change goes through immediately.
        interval = getattr(binding.action, "min_interval_ms", 0)
        if interval:
            now = time.time()
            same_way = (delta > 0) == (binding.last_value > 0)
            if same_way and (now - binding.last_sent) * 1000 < interval:
                return
            binding.last_sent, binding.last_value = now, delta

        self._fire(binding, value=delta * binding.scale)

    def _absolute(self, binding, value):
        """A fader or pot: assign the field, and only when it actually moves.

        The DJ-202 sends its faders as an MSB/LSB pair, so the same 7-bit
        position arrives repeatedly; sending each one would be a datagram per
        message for no change in what the deck sees.
        """
        if not binding.action.sendable:
            return self._refuse(binding)
        if value == binding.last_value:
            return
        binding.last_value = value
        self._fire(binding, value=value, dur_ms=0)

    def refresh_platters(self, now):
        """Report every turning platter's counter, period and level bits."""
        for deck, platter in self.platters.items():
            grams = platter.datagrams(now, self.dur_ms)
            if not grams:
                continue
            tag = self.decks[deck]
            for g in grams:
                self.relay.send(tag, g)
            if self.jog_debug and platter.moving and now >= self.next_jog_print:
                self.next_jog_print = now + 0.5
                print(f"jog {deck}: {platter.last_rate:6.0f} ticks/s  "
                      f"P={platter.last_period}  platter "
                      f"{2777800 / platter.last_period / 100:5.0f} %")

    def refresh_holds(self):
        """Re-arm every held key before its dur_ms runs out."""
        for binding in list(self.held.values()):
            if binding.action.sendable:
                deck, tag = self._tag(binding)
                self.relay.send(tag, binding.action.datagram(1, self.dur_ms))


def load(path, what):
    return profiles.load_json(path, what)


def choose_controller(args, input_names):
    """(profile path, profile name) from --capture / --controller / the ports."""
    if args.capture:
        return args.capture, profiles.name_of(args.capture)
    if args.controller:
        return profiles.controller_path(args.controller), profiles.name_of(args.controller)
    found, hits = profiles.detect(input_names)
    if len(hits) > 1:
        print(f"  several known controllers are connected ({', '.join(hits)}); "
              f"using {found} -- pick one with --controller")
    name = found or profiles.DEFAULT
    return profiles.controller_path(name), name


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--relay", default="127.0.0.1:7202", help="host:port of midi_relay.py")
    ap.add_argument("--controller", default=None,
                    help="profile name in controllers/ (or a path); default: "
                         "the one whose ports are connected, else "
                         + profiles.DEFAULT)
    ap.add_argument("--mapping", "--map", dest="mapfile", default=None,
                    help="mapping name in mappings/ (or a path); default: "
                         "mappings/<controller>.json")
    ap.add_argument("--capture", default=None,
                    help="the controller profile by path (the older spelling "
                         "of --controller)")
    ap.add_argument("--prefix", default=None,
                    help="rig name the decks run under (tags <prefix>1, <prefix>2); "
                         "default: the map's own tags")
    ap.add_argument("--match", default=None,
                    help="substring of the MIDI port names to open; default: "
                         "the profile's own \"match\"")
    ap.add_argument("--dur", type=int, default=150,
                    help="ms a key bit stays set per datagram")
    ap.add_argument("--dry-run", action="store_true",
                    help="decode and print, send nothing")
    ap.add_argument("--seconds", type=float, default=0,
                    help="stop after this long; 0 runs until Ctrl-C")
    ap.add_argument("--jog-nominal", type=float, default=160.0,
                    help="DJ-202 jog ticks/s that count as the platter at "
                         "nominal speed (a 1.06x bend in CDJ mode)")
    ap.add_argument("--jog-debug", action="store_true",
                    help="print each turning platter's tick rate and period "
                         "twice a second, to calibrate --jog-nominal")
    ap.add_argument("--jog-pulses", type=int, default=48,
                    help="position-counter pulses per DJ-202 jog tick")
    ap.add_argument("-q", "--quiet", action="store_true")
    ap.add_argument("--no-leds", action="store_true",
                    help="leave the controller's LEDs alone")
    ap.add_argument("--panel-diff", action="store_true",
                    help="print every change in MAIN's frames to the panel "
                         "micro -- for decoding the NXS2's own lamps")
    args = ap.parse_args()

    capture_path, controller = choose_controller(args, mido.get_input_names())
    capture = load(capture_path, "controller profile")
    mapping = load(profiles.mapping_path(args.mapfile or controller), "mapping")
    match = args.match or capture.get("match") or "DJ-202"
    print(f"controller: {capture.get('name', controller)} ({capture_path})")
    if args.prefix:
        mapping["decks"] = {"cdjA": f"{args.prefix}1", "cdjB": f"{args.prefix}2"}

    host, _, port = args.relay.partition(":")
    relay = Relay(host, int(port), args.dry_run)

    print("binding:")
    bridge = Bridge(capture, mapping, relay, args.dur, not args.quiet)
    bridge.platter_args = {"nominal_tps": args.jog_nominal,
                           "pulses_per_tick": args.jog_pulses}
    bridge.jog_debug = args.jog_debug
    print(f"  {len(bridge.index)} control(s) live, "
          f"decks: {', '.join(f'{k}={v}' for k, v in bridge.decks.items())}, "
          f"focus {bridge.focus}")

    names = [n for n in mido.get_input_names() if match.lower() in n.lower()]
    if not names and args.dry_run:
        # With nothing plugged in, a dry run still proves the profile and the
        # mapping load and every binding resolves -- which is what it is for.
        print(f"no MIDI input matching {match!r}; dry run ends after the binding check")
        return
    if not names:
        sys.exit(f"no MIDI input matching {match!r} -- "
                 "is the controller plugged in and on?")
    ports = [(n, mido.open_input(n)) for n in names]
    print(f"  reading {len(ports)} MIDI port(s)")
    if not args.dry_run:
        relay.connect()

    lamps = None
    if not args.no_leds:
        led_port = capture.get("led_output_port", "")
        if args.dry_run or led_port in mido.get_output_names():
            lamps = leds.Leds(capture["controls"], mapping, led_port,
                              args.dry_run, args.panel_diff)
            lamps.hello()
            print(f"  LEDs on {led_port}")
        else:
            print(f"  no MIDI output {led_port!r}: LEDs stay off")
    print("\nrunning; Ctrl-C to stop\n")

    # Re-arm holds at 60% of dur so a bit never lapses between datagrams.
    refresh_every = max(args.dur / 1000.0 * 0.6, 0.02)
    next_refresh = time.time() + refresh_every
    next_platter = 0.0
    next_reconnect = 0.0
    stop_at = time.time() + args.seconds if args.seconds else None
    try:
        while stop_at is None or time.time() < stop_at:
            for name, port in ports:
                for msg in port.iter_pending():
                    bridge.handle(name, msg)
                    if lamps and msg.type in ("note_on", "note_off"):
                        lamps.surface.forget(msg.channel, msg.note)
            now = time.time()
            if bridge.platters and now >= next_platter:
                bridge.refresh_platters(now)
                next_platter = now + 0.01
            if bridge.held and now >= next_refresh:
                bridge.refresh_holds()
                next_refresh = now + refresh_every
            if lamps:
                # The relay only talks back on a live connection, so an LED
                # bridge cannot wait for the next key press to reconnect.
                if not relay.sock and not args.dry_run and now >= next_reconnect:
                    next_reconnect = now + 2.0
                    relay.connect()
                for line in relay.lines():
                    tag, _, state = line.partition(" state ")
                    if state:
                        lamps.feed(tag, state)
                lamps.render()
            time.sleep(0.002)
    except KeyboardInterrupt:
        print("\nstopped")
    finally:
        if lamps:
            lamps.close()


if __name__ == "__main__":
    main()
