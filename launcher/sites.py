# SPDX-License-Identifier: GPL-2.0-or-later
"""The firmware addresses the launcher pokes while it runs a player.

Each player's module (mods/players/<model>.py) names them in LAUNCHER, as
anchors: the instructions around each place. They are looked up in the MAIN
image installed for the model, the supported update or any other (another
version, a modded update installed with --any-firmware), and a site that is not
found exactly once stays unset, so the launcher leaves out the poke that needed
it and start says so. A player whose module names none is poked nowhere.
"""

import functools
import importlib
import os
import sys

from . import firmware, model

# What is lost when the site (or one of the sites, for a poke that needs two)
# is missing.
OFF = (
    (("load_poke_pc", "load_poke_addr"), "the load poke: the loader may stall waiting for the file layer"),
    (("own_mac",), "the firmware's own MAC hold: two decks may not settle their device numbers"),
    (("colour_capability",), "the colour capability byte: the display may stay in its monochrome mode"),
    (("deck_ready",), "the wait for the deck-ready word before the first key"),
    (("sr_save_slot",), "the status-register seed: the firmware may fault in its first interrupt"),
    (("native_memcmp", "native_strlen"), "the native memcmp/strlen speed-up (the machine runs the firmware's own code: slower)"),
)

# Switched off on the rig whenever MAIN is not the CDJ-2000NXS2 1.87 image
# (Sites.board_image): they are tied to addresses in it with nothing to look up
# or to check them by. Another player built on the same machine has other
# code at those addresses.
UNPINNED = (
    "the firmware log and trace taps, and the play-key holds and edge pokes of the lab runs",
)
UNPINNED_DSP_IDLE = "the DSP idle-loop skip (the DSP program is run in full: slower)"
UNPINNED_GUI = "the display board's idle-loop halt (it keeps a host core busy)"


class Sites:
    """`values` holds every site the model's launcher looks up, None for one
    that is not found. `pinned`: the installed MAIN is the update the model's
    profile pins. `board_image`: it is the pinned MAIN of the model's machine,
    the image the board's own defaults and the lab arms' fixed addresses were
    measured on (for every rig model the CDJ-2000NXS2 1.87)."""

    def __init__(self, values, pinned, board_image, gui_pinned=True):
        self.values = values
        self.pinned = pinned
        self.board_image = board_image
        self.gui_pinned = gui_pinned

    def get(self, name):
        return self.values.get(name)

    def has(self, *names):
        return all(self.values.get(n) is not None for n in names)

    def sr_seed_slot(self):
        """CDJ_SR_SEED_SLOT for this image: the slot found in it, else None to
        keep the board's on the image the board was built around, else nothing."""
        if self.has("sr_save_slot"):
            return "0x%08X" % self.get("sr_save_slot")
        return None if self.board_image else "0"

    def native_pcs(self, *names):
        """The colon-separated addresses of the routines `names`, the value of a
        CDJ_NATIVE_* knob, or None when one of them is not found."""
        if not self.has(*names):
            return None
        return ":".join("0x%08X" % self.get(n) for n in names)

    def switched_off(self, m):
        """What the launcher leaves out of a start of model `m` on this image,
        one line each."""
        lines = [] if self.gui_pinned else [UNPINNED_GUI]
        lines += [what for names, what in OFF if all(n in self.values for n in names) and not self.has(*names)]
        if m.is_rig and not self.board_image:
            lines += list(UNPINNED)
        if m.dsp_idle and not self.pinned:
            lines.append(UNPINNED_DSP_IDLE)
        return lines


def _player(emu, m):
    """The module of the player whose anchors fit model `m`: its own, else that
    of the player whose machine it runs on (a player built on the CDJ-2000NXS2
    platform); None for a player with neither."""
    folder = os.path.join(emu, "mods")
    for name in (m.id, m.main_machine):
        if os.path.isfile(os.path.join(folder, "players", name + ".py")):
            if folder not in sys.path:
                sys.path.insert(0, folder)
            return importlib.import_module("players." + name)
    return None


@functools.lru_cache(maxsize=None)
def _found(path, mtime_ns, size, emu, player_name):
    """The launcher anchors of mods/players/<player_name>.py looked up in the
    MAIN image at `path`; None for a site that is not found exactly once."""
    folder = os.path.join(emu, "mods")
    if folder not in sys.path:
        sys.path.insert(0, folder)
    import sigpatch
    player = importlib.import_module("players." + player_name)
    with open(path, "rb") as f:
        img = sigpatch.Image(player.MAIN_PROFILE, f.read())
    values = {}
    for name, anchor in player.LAUNCHER.items():
        try:
            values[name] = sigpatch.anchor_value(img, anchor)
        except (sigpatch.NotApplied, SystemExit):
            values[name] = None
    return values


def _is_stock_main(path, m):
    """`path` is the MAIN image the supported update of `m` unpacks to (a
    profile that pins no image counts every image as that one)."""
    want = dict((os.path.basename(rel), sha) for rel, sha in m.expected).get("main_unpacked.bin")
    return want is None or firmware.installed_sha256(path) == want


def find(lay, m=None, main=None):
    """The sites in the MAIN image installed for model `m` (the selected one
    when None), or in the image at `main`, a MAIN run in the installed one's
    place (a modified one under test); none while no image is installed."""
    m = m or model.load()
    folder = model.extract_dir(lay, m)
    differ = firmware.differing_images(folder, m)
    gui_pinned = "gui_unpacked.bin" not in differ
    if main is None:
        path = os.path.join(folder, "main_unpacked.bin")
        pinned = "main_unpacked.bin" not in differ
    else:
        path = main
        pinned = _is_stock_main(path, m)
    board_image = pinned
    if m.main_machine != m.id:
        machine_sha = firmware.stock_main_sha256(model.load(m.main_machine))
        board_image = machine_sha == (firmware.installed_sha256(path) if os.path.isfile(path)
                                      else firmware.stock_main_sha256(m))
    player = _player(lay.emu, m)
    if player is None or not getattr(player, "LAUNCHER", None) or not os.path.isfile(path):
        return Sites({}, pinned, board_image, gui_pinned)
    st = os.stat(path)
    name = player.__name__.split(".")[-1]
    return Sites(dict(_found(path, st.st_mtime_ns, st.st_size, lay.emu, name)), pinned, board_image, gui_pinned)
