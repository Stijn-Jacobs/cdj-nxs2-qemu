#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Teach the bridge a MIDI controller: capture what it sends, then map it.

Runs where the controller enumerates (Windows, with a python.org Python that
has mido + python-rtmidi). Two files come out, both plain JSON (profiles.py):

    controllers/<name>.json   the profile: what each control sends
    mappings/<name>.json      the mapping: which CDJ action each control drives

A NEW CONTROLLER, start to finish:

    python midi/learn.py ports                      what is plugged in?
    python midi/learn.py new                        name it, then move every
                                                    control and name each one
    python midi/learn.py map --controller <name>    bind controls to CDJ actions
    python midi/bridge.py --controller <name>       play

Other modes, for any --controller (default: the Roland DJ-202):

    watch      decode everything, live -- a sanity tap
    learn      walk the profile's checklist (controllers/<name>.checklist.txt)
    free       capture controls the checklist never mentions
    session    non-interactive learn: --names a,b,c, one burst each, in order
    actions    every CDJ action a control can be mapped to
    show       the mapping as a table

Why a learn pass at all, when makers publish MIDI charts: because a chart
describes a mode, and a unit has several. The DJ-202's pads alone send
different notes per pad mode, and its two ports carry different halves of the
surface. A captured profile is a fact about the device in front of you; a
transcribed chart is a fact about a PDF.

WHAT A CONTROL LOOKS LIKE

Buttons send note_on then note_off. Faders and knobs send a stream of CC on
one number, or a 14-bit pitchwheel. Jogs and encoders send CC too, but
relative, in one of three encodings; `classify` records which, plus the value
range seen, so the bridge knows whether it is holding a button, following a
fader or differencing a counter.
"""
import argparse
import collections
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdj_actions  # noqa: E402
import profiles  # noqa: E402

try:
    import mido
except ImportError:
    mido = None

# The DJ-202 (and many units) free-run MIDI clock at ~24 ppqn. It would swamp
# captures and fill the silences `session` mode uses to separate controls.
REALTIME = {"clock", "start", "stop", "continue", "active_sensing",
            "reset", "songpos", "song_select", "sysex", "quarter_frame"}

BUTTON = "button"
ABSOLUTE = "absolute"
# Three relative encodings exist in the wild, and the DJ-202 alone uses two --
# decoding one as another turns a single detent into a jump of 64, so they are
# separate behaviours:
#   RELATIVE          two's complement: 1 = +1, 127 = -1   (DJ-202 browse)
#   RELATIVE_OFFSET   offset-64:       65 = +1,  63 = -1   (DJ-202 platters)
#   RELATIVE_SIGNBIT  sign-magnitude:   1 = +1,  65 = -1
RELATIVE = "relative"
RELATIVE_OFFSET = "relative_offset"
RELATIVE_SIGNBIT = "relative_signbit"


def need_mido():
    if mido is None:
        sys.exit("mido is missing:  python -m pip install mido python-rtmidi")


def open_ports(match):
    """Every matching input, opened. Many units present more than one."""
    need_mido()
    names = [n for n in mido.get_input_names() if match.lower() in n.lower()]
    if not names:
        available = mido.get_input_names()
        sys.exit(
            f"no MIDI input matching {match!r}.\n"
            f"  available: {available if available else '(none)'}\n"
            "  Is the controller plugged in and switched on? "
            "(--match picks the port name)")
    return [(n, mido.open_input(n)) for n in names]


def drain(ports, seconds, on_message=None):
    """Collect every message arriving in a window. Returns [(port, msg)]."""
    got = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        for name, port in ports:
            for msg in port.iter_pending():
                if msg.type in REALTIME:
                    continue
                got.append((name, msg))
                if on_message:
                    on_message(name, msg)
        time.sleep(0.005)
    return got


def flush(ports):
    for _, port in ports:
        list(port.iter_pending())


def describe(name, msg):
    if msg.type in ("note_on", "note_off"):
        return f"{name:<28} {msg.type:<9} ch{msg.channel:<2} note {msg.note:>3} vel {msg.velocity:>3}"
    if msg.type == "control_change":
        return f"{name:<28} cc        ch{msg.channel:<2} cc   {msg.control:>3} val {msg.value:>3}"
    if msg.type == "pitchwheel":
        return f"{name:<28} pitch     ch{msg.channel:<2} {msg.pitch}"
    return f"{name:<28} {msg}"


def classify(captured):
    """Turn a burst of messages into one control definition.

    Picks the dominant (port, type, channel, number) -- a jog touch can leak a
    note into a turn capture, and the busiest stream is the control you moved.
    A pitchwheel only counts when nothing else arrived: the DJ-202's platters
    send one alongside their CC, and the CC is the one the bridge decodes.
    """
    counts = collections.Counter()
    values = collections.defaultdict(list)
    pitch = collections.Counter()
    pitch_values = collections.defaultdict(list)
    for name, msg in captured:
        if msg.type in ("note_on", "note_off"):
            key = (name, "note", msg.channel, msg.note)
            values[key].append(msg.velocity)
        elif msg.type == "control_change":
            key = (name, "cc", msg.channel, msg.control)
            values[key].append(msg.value)
        elif msg.type == "pitchwheel":
            key = (name, "pitch", msg.channel, 0)
            pitch[key] += 1
            pitch_values[key].append((msg.pitch + 8192) >> 7)
            continue
        else:
            continue
        counts[key] += 1
    if not counts and pitch:
        counts, values = pitch, pitch_values
    if not counts:
        return None

    (port, kind, channel, number), hits = counts.most_common(1)[0]
    seen = values[(port, kind, channel, number)]
    lo, hi = min(seen), max(seen)

    if kind == "note":
        behaviour = BUTTON
    elif kind == "pitch":
        behaviour = ABSOLUTE
    elif hits >= 4 and 56 <= lo and hi <= 72:
        # Everything hugging 64 is an offset-64 encoder. A fader would have to
        # be swept only across its middle eighth to look like this.
        behaviour = RELATIVE_OFFSET
    elif hits >= 4 and lo <= 16 and 65 <= hi <= 80 and \
            all(v <= 16 or 65 <= v <= 80 for v in seen):
        # Small values one way, 65.. the other: sign-magnitude.
        behaviour = RELATIVE_SIGNBIT
    elif hits >= 4 and (lo <= 8 or hi >= 120) and not (lo < 30 and hi > 100):
        # A two's-complement encoder sits at the ends: small positives one way,
        # near-127 the other. A fader swept end to end covers the whole range,
        # which is what the last clause excludes.
        behaviour = RELATIVE
    else:
        behaviour = ABSOLUTE

    return {
        "port": port,
        "type": kind,
        "channel": channel,
        "number": number,
        "behaviour": behaviour,
        "value_min": lo,
        "value_max": hi,
        "messages": hits,
        "also_seen": sorted(
            {f"{k[1]}:{k[2]}:{k[3]}" for k in counts
             if (k[1], k[2], k[3]) != (kind, channel, number)}),
    }


def capture_bursts(ports, seconds, gap):
    """Split a capture window into one burst per control, on the silences.

    `learn` asks you to press ENTER between controls. That needs a terminal
    on stdin, which an agent driving this from a tool call does not have. So
    this mode segments on the pauses instead: touch a control, pause, touch
    the next. Anything separated by more than `gap` seconds of silence is a
    different control.
    """
    events = []
    deadline = time.time() + seconds
    while time.time() < deadline:
        for name, port in ports:
            for msg in port.iter_pending():
                if msg.type in REALTIME:
                    continue
                events.append((time.time(), name, msg))
        time.sleep(0.002)

    bursts, current, last = [], [], None
    for when, name, msg in events:
        if last is not None and when - last > gap:
            bursts.append(current)
            current = []
        current.append((name, msg))
        last = when
    if current:
        bursts.append(current)
    return bursts


# -- the profile on disk -----------------------------------------------------

class Profile:
    """controllers/<name>.json, loaded or new, and saved in one place."""

    def __init__(self, args):
        self.name = args.controller
        self.path = profiles.controller_path(args.controller)
        self.data = {}
        if os.path.exists(self.path) and not getattr(args, "restart", False):
            self.data = profiles.load_json(self.path, "profile")
        self.data.setdefault("name", args.controller)
        self.data.setdefault("controls", {})
        self.match = args.match or self.data.get("match") or self.data["name"]

    @property
    def controls(self):
        return self.data["controls"]

    def save(self):
        self.data["match"] = self.match
        self.data.setdefault("note", "captured from the device by learn.py; "
                                     "do not hand-edit numbers, re-learn instead")
        profiles.save_json(self.path, self.data)
        print(f"\nwrote {len(self.controls)} control(s) to {self.path}")
        report_collisions(self.controls)

    def find(self, result):
        """The control name already recorded at this MIDI address, if any."""
        for name, c in self.controls.items():
            if (c["port"], c["type"], c["channel"], c["number"]) == \
                    (result["port"], result["type"], result["channel"], result["number"]):
                return name
        return None


def print_result(result):
    print(f"    {result['type']} ch{result['channel']} "
          f"#{result['number']} {result['behaviour']} "
          f"({result['value_min']}..{result['value_max']}, "
          f"{result['messages']} msgs) on {result['port']}")
    if result["also_seen"]:
        print(f"    also saw: {', '.join(result['also_seen'])}")


def read_checklist(path):
    entries = []
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, _, hint = line.partition("--")
            entries.append((name.strip(), hint.strip()))
    return entries


def report_collisions(controls):
    """Two names on one MIDI address means one capture caught the wrong thing."""
    seen = collections.defaultdict(list)
    for name, c in controls.items():
        seen[(c["port"], c["type"], c["channel"], c["number"])].append(name)
    clashes = {k: v for k, v in seen.items() if len(v) > 1}
    if not clashes:
        return
    print("\nWARNING -- these controls share a MIDI address, so at least one")
    print("capture caught a neighbouring control. Re-learn them:")
    for (port, kind, channel, number), names in clashes.items():
        print(f"  {kind} ch{channel} #{number}: {', '.join(names)}")


# -- modes -------------------------------------------------------------------

def cmd_ports(args):
    need_mido()
    names = mido.get_input_names()
    if not names:
        print("no MIDI inputs at all. Plug the controller in and switch it on.")
        return
    known = {n: p.get("match", "") for n, p in profiles.available()}
    for n in names:
        mark = [c for c, m in known.items() if m and m.lower() in n.lower()]
        print(f"  {n}{'  <-- ' + ', '.join(mark) if mark else ''}")
    print("\noutputs (LEDs):")
    for n in mido.get_output_names():
        print(f"  {n}")


def cmd_watch(args):
    prof = Profile(args)
    ports = open_ports(prof.match)
    print(f"watching {len(ports)} port(s) for {args.seconds:.0f}s\n", flush=True)
    try:
        drain(ports, args.seconds,
              lambda n, m: print(describe(n, m), flush=True))
    except KeyboardInterrupt:
        pass
    print("stopped")


def cmd_learn(args):
    prof = Profile(args)
    checklist_file = args.checklist or profiles.checklist_path(prof.name)
    if not os.path.exists(checklist_file):
        sys.exit(f"no checklist at {checklist_file} -- use `free` (or `new`) to "
                 "capture controls one by one and name them as you go")
    ports = open_ports(prof.match)
    checklist = read_checklist(checklist_file)
    captured = prof.controls
    if captured and not args.restart:
        print(f"resuming: {len(captured)} control(s) already captured "
              f"(--restart to start over)\n")

    print("For each control: move it, then wait. ENTER alone skips it,")
    print("'r' redoes the previous one, 'q' saves and quits.\n")

    index = 0
    while index < len(checklist):
        name, hint = checklist[index]
        if name in captured and not args.restart:
            index += 1
            continue

        prompt = f"[{index + 1}/{len(checklist)}] {name}"
        if hint:
            prompt += f"   ({hint})"
        print(prompt)
        answer = input("    move it now, then ENTER > ").strip().lower()
        if answer == "q":
            break
        if answer == "r" and index > 0:
            index -= 1
            captured.pop(checklist[index][0], None)
            continue

        result = classify(drain(ports, args.settle))
        if result is None:
            print("    nothing arrived -- skipped\n")
            index += 1
            continue
        captured[name] = result
        print_result(result)
        print()
        flush(ports)
        index += 1
    prof.save()


def free_capture(prof, ports, settle):
    print("Move a control, then ENTER, then type a name for it (e.g. deck1/play).")
    print("An empty name drops the capture; 'q' as a name saves and stops.\n")
    try:
        while True:
            input("move a control, then ENTER > ")
            result = classify(drain(ports, settle))
            if result is None:
                print("    nothing arrived\n")
                continue
            print_result(result)
            old = prof.find(result)
            name = input(f"    name it{f' [{old}]' if old else ''} > ").strip() or old
            if name == "q":
                break
            if name:
                prof.controls[name] = result
            flush(ports)
            print()
    except (KeyboardInterrupt, EOFError):
        pass


def cmd_free(args):
    prof = Profile(args)
    ports = open_ports(prof.match)
    free_capture(prof, ports, args.settle)
    prof.save()


def cmd_new(args):
    """A new controller: name it, find its ports, capture every control."""
    need_mido()
    inputs = mido.get_input_names()
    print("MIDI inputs:", ", ".join(inputs) if inputs else "(none -- plug it in first)")
    title = args.name or input("controller name (e.g. 'Pioneer DDJ-400') > ").strip()
    if not title:
        sys.exit("a name is needed")
    args.controller = args.controller if args.controller != profiles.DEFAULT \
        else profiles.slug(title)
    guess = args.match or next((n.split(":")[0].rsplit(" ", 1)[0] for n in inputs
                                if title.split()[-1].lower() in n.lower()), "")
    match = input(f"a word in its MIDI port name [{guess}] > ").strip() or guess
    if not match:
        sys.exit("need a word from the port name, so the bridge can find it")
    args.match = match
    prof = Profile(args)
    prof.data["name"] = title
    outs = [n for n in mido.get_output_names() if match.lower() in n.lower()]
    if outs:
        print(f"LED output port: {outs[-1]} (edit led_output_port in the profile to change)")
        prof.data.setdefault("led_output_port", outs[-1])
    ports = open_ports(match)
    free_capture(prof, ports, args.settle)
    prof.save()
    print(f"\nnext:  python midi/learn.py map --controller {args.controller}")


def print_actions():
    rows = cdj_actions.catalogue()
    order = {cdj_actions.CONFIRMED: 0, cdj_actions.PARTIAL: 1,
             cdj_actions.DECODED: 2, cdj_actions.GUESS: 3, cdj_actions.UNBOUND: 4}
    print(f"{'action':<18} {'kind':<9} {'report':<16} {'status':<10} note")
    for name, kind, where, status, note in sorted(rows, key=lambda r: (order[r[3]], r[0])):
        print(f"{name:<18} {kind:<9} {where:<16} {status:<10} {note[:60]}")
    print("\nconfirmed = seen working here; partial = does something; decoded = the "
          "firmware reads it, never pressed here;\nguess = unverified raw bit; "
          "unbound = nothing to send. Any key_0xOFF_0xMASK, level_0xOFF or "
          "rot_0xOFF works too.")


def cmd_actions(_args):
    print_actions()


def load_mapping(args):
    name = profiles.name_of(args.controller)
    path = profiles.mapping_path(args.mapping or name)
    if os.path.exists(path):
        return path, profiles.load_json(path, "mapping")
    return path, profiles.empty_mapping(name, args.prefix)


def cmd_show(args):
    path, mapping = load_mapping(args)
    decks = {k: v for k, v in mapping["decks"].items() if not k.startswith("_")}
    print(f"{path}\n  decks: {decks}  focus: {mapping.get('focus')}\n")
    for name, spec in sorted(mapping["bindings"].items()):
        if name.startswith("_"):
            continue
        extra = {k: v for k, v in spec.items() if k not in ("action", "deck")}
        print(f"  {name:<24} -> {spec.get('deck', ''):<6} {spec['action']:<16}"
              f"{' ' + str(extra) if extra else ''}")


def ask_binding(control, spec, decks):
    """Prompt for one control's action and deck. None = leave it as it is."""
    current = f"{spec['action']} on {spec.get('deck')}" if spec else "unbound"
    while True:
        a = input(f"    action [{current}] (? list, - unbind, ENTER keep) > ").strip()
        if a == "":
            return None
        if a == "?":
            print_actions()
            continue
        if a == "-":
            return {}
        if a != "set_focus":
            try:
                cdj_actions.resolve(a)
            except KeyError as e:
                print(f"    {e.args[0][:200]}")
                continue
        break
    deck_choices = list(decks) + ([] if a == "set_focus" else ["focus"])
    default = (spec or {}).get("deck", deck_choices[0])
    d = input(f"    deck {'/'.join(deck_choices)} [{default}] > ").strip() or default
    if d not in deck_choices:
        print(f"    unknown deck {d!r}; kept {default}")
        d = default
    out = {"action": a, "deck": d}
    if control.get("behaviour", "").startswith("relative"):
        sc = input("    scale (detents per step, - to reverse) [1] > ").strip()
        out["scale"] = int(sc) if sc.lstrip("-").isdigit() else 1
    if a == "touch_tap":
        out["x"] = int(input("    x pixel 0..799 [400] > ").strip() or 400)
        out["y"] = int(input("    y pixel 0..479 [240] > ").strip() or 240)
    return out


def cmd_map(args):
    """Walk the controller's controls and bind each to a CDJ action."""
    prof = Profile(args)
    if not prof.controls:
        sys.exit(f"{prof.path} has no controls yet -- run `learn.py new` first")
    path, mapping = load_mapping(args)
    mapping["controller"] = profiles.name_of(args.controller)
    decks = {k: v for k, v in mapping["decks"].items() if not k.startswith("_")}
    bindings = mapping["bindings"]
    ports = None
    if mido is not None:
        try:
            ports = open_ports(prof.match)
        except SystemExit as e:
            print(f"{e}\n(continuing without the device: type control names instead)")

    print(f"mapping {prof.data.get('name')} -> {path}")
    print(f"decks: {', '.join(f'{k}={v}' for k, v in decks.items())}; 'focus' = the "
          "deck the last set_focus control chose\n")
    print("Move a control (then ENTER), or type its name, 'l' to list them, "
          "'all' to walk every one, 'q' to save.\n")
    try:
        while True:
            q = input("control > ").strip()
            if q == "q":
                break
            if q == "l":
                for n in sorted(prof.controls):
                    s = bindings.get(n)
                    print(f"  {n:<24} {s['action'] + ' / ' + s.get('deck', '') if s else '-'}")
                continue
            names = sorted(prof.controls) if q == "all" else [q] if q else []
            if not names:
                if not ports:
                    continue
                result = classify(drain(ports, args.settle))
                name = prof.find(result) if result else None
                if not name:
                    print("    nothing known arrived (learn it with `free` first)")
                    continue
                names = [name]
                flush(ports)
            for name in names:
                if name not in prof.controls:
                    print(f"    no control {name!r} in the profile")
                    continue
                print(f"  {name}  ({prof.controls[name]['type']}, "
                      f"{prof.controls[name]['behaviour']})")
                new = ask_binding(prof.controls[name], bindings.get(name), decks)
                if new is None:
                    continue
                if new:
                    bindings[name] = new
                else:
                    bindings.pop(name, None)
    except (KeyboardInterrupt, EOFError):
        print()
    profiles.save_json(path, mapping)
    print(f"wrote {sum(1 for n in bindings if not n.startswith('_'))} binding(s) to {path}")
    print(f"try it:  python midi/bridge.py --controller {args.controller} --dry-run")


def cmd_session(args):
    """Non-interactive learn: a list of names, one burst each, in order."""
    names = [n.strip() for n in args.names.split(",") if n.strip()]
    if not names:
        sys.exit("--names is required for session mode")
    prof = Profile(args)
    ports = open_ports(prof.match)
    flush(ports)

    print(f"capturing {args.seconds:.0f}s -- touch these in order, "
          f"pausing about {args.gap}s between:")
    for i, n in enumerate(names, 1):
        print(f"  {i}. {n}")
    print(flush=True)

    bursts = capture_bursts(ports, args.seconds, args.gap)

    # The DJ-202's pad section emits a lone `note 0 vel 3` when first touched.
    # A real press is note_on then note_off, so a one-message burst is noise.
    noise = [b for b in bursts if len(b) < 2]
    bursts = [b for b in bursts if len(b) >= 2]
    for b in noise:
        print(f"  ignored a 1-message burst (noise): {b[0][1]}")

    print(f"{len(bursts)} burst(s) for {len(names)} name(s)"
          f"{'' if len(bursts) == len(names) else '   <-- MISMATCH'}\n")

    for i, burst in enumerate(bursts):
        result = classify(burst)
        label = names[i] if i < len(names) else f"UNCLAIMED_{i + 1}"
        if result is None:
            print(f"  {label}: empty burst")
            continue
        print(f"  {label:<22} {result['type']} ch{result['channel']} "
              f"#{result['number']:<4} {result['behaviour']:<9} "
              f"{result['value_min']}..{result['value_max']}  "
              f"{result['messages']} msgs"
              + (f"   also: {', '.join(result['also_seen'])}"
                 if result["also_seen"] else ""))
        if i < len(names):
            prof.controls[label] = result

    if len(bursts) != len(names):
        print("\nBurst count does not match the name list, so the bindings")
        print("above may be off by one. Re-run the ones that look wrong.")
    prof.save()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["ports", "watch", "learn", "free", "session",
                                     "new", "map", "actions", "show"])
    ap.add_argument("--controller", default=profiles.DEFAULT,
                    help="profile name in controllers/ (default %(default)s)")
    ap.add_argument("--name", default="", help="new: the controller's display name")
    ap.add_argument("--mapping", default=None,
                    help="map/show: mapping name or path (default: the controller's)")
    ap.add_argument("--prefix", default="show",
                    help="map: rig name for a NEW mapping's decks (<prefix>1, <prefix>2)")
    ap.add_argument("--names", default="",
                    help="session mode: control names, in the order you will "
                         "touch them")
    ap.add_argument("--seconds", type=float, default=45.0,
                    help="session/watch: how long to capture")
    ap.add_argument("--gap", type=float, default=1.2,
                    help="session mode: silence that separates two controls")
    ap.add_argument("--match", default=None,
                    help="substring of the MIDI port names to open "
                         "(default: the profile's own)")
    ap.add_argument("--checklist", default=None,
                    help="learn: the checklist (default controllers/<name>.checklist.txt)")
    ap.add_argument("--settle", type=float, default=1.2,
                    help="seconds of MIDI collected per control")
    ap.add_argument("--restart", action="store_true",
                    help="ignore an existing capture and start from scratch")
    args = ap.parse_args()
    {"ports": cmd_ports, "watch": cmd_watch, "learn": cmd_learn,
     "free": cmd_free, "session": cmd_session, "new": cmd_new, "map": cmd_map,
     "actions": cmd_actions, "show": cmd_show}[args.mode](args)


if __name__ == "__main__":
    main()
