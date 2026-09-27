# SPDX-License-Identifier: GPL-2.0-or-later
"""cdj.conf: the answers setup gave, which start reads.

The file stays a list of shell assignments (KEY=value, shell-quoted), so it
reads the same as before and is still valid to source from bash. Only plain
assignments are understood; anything else is reported and skipped.
"""

import shlex
import time

# The keys setup writes, in the order it writes them.
KEYS = ("CDJ_DECKS", "CDJ_NAME", "CDJ_DJLINK", "CDJ_AUDIO", "CDJ_CONTROLLER",
        "CDJ_RELAY_PORT", "CDJ_GROUP", "CDJ_MIDI_PYTHON", "CDJ_TOOLS_PYTHON",
        "QEMU_BUILD", "QEMU_EB_BUILD")

# What start assumes for a key the file does not set.
START_DEFAULTS = {
    "CDJ_DECKS": "1", "CDJ_NAME": "show", "CDJ_DJLINK": "0", "CDJ_AUDIO": "1",
    "CDJ_CONTROLLER": "none", "CDJ_RELAY_PORT": "7202",
    "CDJ_GROUP": "239.77.77.1:45000", "CDJ_MIDI_PYTHON": "",
    "QEMU_BUILD": "", "QEMU_EB_BUILD": "",
    # CDJ_APP=1 makes the virtual deck app the default window (--no-app
    # overrides); CDJ_APP_PYTHON is the Python that runs it (pygame-ce + Pillow).
    "CDJ_APP": "0", "CDJ_APP_PYTHON": "",
    # CDJ_SERVICE=1 makes start.sh boot into the service manual's SERVICE MODE
    # by default (--service/--no-service overrides).
    "CDJ_SERVICE": "0",
}


def parse(text, warn=None):
    """{key: value} from the text of a cdj.conf."""
    out = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("export "):
            line = line[len("export "):].lstrip()
        key, eq, rest = line.partition("=")
        if not eq or not key.replace("_", "").isalnum() or key[0].isdigit():
            if warn:
                warn("cdj.conf line %d is not a KEY=value assignment; ignored: %s" % (n, raw))
            continue
        try:
            words = shlex.split(rest, comments=True, posix=True)
        except ValueError:
            if warn:
                warn("cdj.conf line %d has unbalanced quotes; ignored: %s" % (n, raw))
            continue
        out[key] = " ".join(words)
    return out


def load(path, warn=None):
    try:
        with open(path, encoding="utf-8") as f:
            return parse(f.read(), warn)
    except FileNotFoundError:
        return {}


def with_start_defaults(values):
    merged = dict(START_DEFAULTS)
    merged.update(values)
    return merged


def render(values):
    """The file's text: the known keys first, in setup's order, then any other
    key the file already had, so a hand-added one (CDJ_APP=1) survives a
    re-run of setup."""
    lines = ["# Written by ./setup.sh on %s. ./start.sh reads it." % time.strftime("%Y-%m-%d %H:%M"),
             "# Edit it, or run ./setup.sh --reconfigure."]
    for k in KEYS:
        lines.append("%s=%s" % (k, shlex.quote(values.get(k, ""))))
    for k, v in values.items():
        if k not in KEYS:
            lines.append("%s=%s" % (k, shlex.quote(v)))
    return "\n".join(lines) + "\n"


def save(path, values):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(values))
