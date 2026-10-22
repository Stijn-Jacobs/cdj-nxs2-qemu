"""Where a deck's saved points live.

A point is every board of the deck saved by scripts/run/snapshot_deck.py:
main.vm and gui.vm for the NXS2, main.vm alone for a deck whose display runs
inside MAIN. QEMU only takes a state back into the binary and machine that
wrote it, so the folder is keyed by the QEMU binaries, the firmware images,
the USB medium and the QEMU options that shape the machine: a rebuild or a
new medium starts a fresh cache by itself.
"""

import hashlib
import os

from . import host

# The points the drivers know how to reach: the settled deck screen, and the
# first track loaded and playing.
POINTS = ("idle", "loaded")

# A state that records "paused" comes back paused: the save stops the deck.
SAVE_ARGS = ["-global", "migration.store-global-state=off"]


def root(tmp, files, options=""):
    h = hashlib.sha1()
    for path in files:
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
    h.update(options.encode())
    return os.path.join(tmp, "cdj-snap", h.hexdigest()[:16])


def saved(point_dir, boards=("main.vm", "gui.vm")):
    return all(os.path.isfile(os.path.join(point_dir, n)) for n in boards)


def restore_args(point_dir):
    """QEMU's options that start a one-board deck from its saved point."""
    return ["-incoming", "file:%s/main.vm" % host.native(point_dir)]
