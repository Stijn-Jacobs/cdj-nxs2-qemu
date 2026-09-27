# SPDX-License-Identifier: GPL-2.0-or-later
"""python -m launcher setup|start|stop|firmware|run [args]: what setup.sh,
start.sh and the scripts under scripts/run/ do, and what the packaged program
runs."""

import sys

USAGE = """usage: python -m launcher <command> [options]
  start [--app|--no-app|--dry-run|stop]   start the decks (./start.sh)
  setup [options]                          set everything up (./setup.sh)
  stop                                     stop running decks
  firmware [--install [--force]] <UPD> [outdir]   prepare the firmware images
  run <script> [args]                      one of scripts/run/*.sh (rig, boot_deck, ...)
  packages                                 the missing build prerequisites, as package names
"""


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    # Line endings as the shell scripts wrote them, also on Windows, where a
    # redirected stream would otherwise get CRLF.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(newline="\n", line_buffering=True)
    if not argv:
        from .layout import Layout
        if Layout().packaged:
            # The packaged program double-clicked: set up what is missing
            # (setup offers to start at the end), else start.
            from . import setup, start
            return start.main([]) if setup.ready(Layout()) else setup.main([])
    cmd = argv[0] if argv else ""
    rest = argv[1:]
    if cmd == "start":
        from . import start
        return start.main(rest)
    if cmd == "stop":
        from . import start
        from .layout import Layout
        return start.stop(Layout())
    if cmd == "run" and rest:
        from . import chain
        if rest[0] in chain.SCRIPTS:
            return chain.run_main(rest[0], rest[1:])
    if cmd == "firmware":
        from . import firmware
        return firmware.main(rest)
    if cmd == "packages":
        from . import host, setup
        print(" ".join(setup.packages_for(host.kind(), setup.missing_prerequisites(host.kind()))))
        return 0
    if cmd == "setup":
        from . import setup
        return setup.main(rest)
    sys.stdout.write(USAGE)
    return 0 if cmd in ("-h", "--help") else 2


if __name__ == "__main__":
    sys.exit(main())
