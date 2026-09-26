# SPDX-License-Identifier: GPL-2.0-or-later
"""live.sh and live_linked.sh: the MIDI relay beside the real-DSP rig, which
both live exactly as long as the script; Ctrl-C stops both.

  usage: bash scripts/run/live.sh [prefix=show] [decks=2]
         bash scripts/run/live_linked.sh [prefix=show] [decks=2]
  live_linked.sh adds Pro DJ Link on GROUP (default 239.77.77.1:45000) and
  prints the DHCP leases at the end; SNIFF=1 also captures the segment to
  /tmp/j2-<tag>.pcap. The rig lives FRAMES (default 240) x 5 s = ~20 min.
"""

import os
import subprocess

from . import chain, host
from .chain import nonempty
from .layout import Layout
from .rig import DEFAULT_GROUP


def main(argv, linked=False):
    tag = argv[0] if argv else "show"
    n = int(argv[1]) if len(argv) > 1 else 2
    lay = Layout()
    env = dict(os.environ)
    port = nonempty(env, "RELAY_PORT", "7202")
    tags = ",".join("%s%d" % (tag, i) for i in range(1, n + 1))
    if linked:
        env["GROUP"] = nonempty(env, "GROUP", DEFAULT_GROUP)
        chain.say("[%s] %d deck(s); DJ-202 relay on 127.0.0.1:%s; Pro DJ Link on %s" % (tag, n, port, env["GROUP"]))
        chain.say("[%s] on WINDOWS, once the deck windows are up:" % tag)
        chain.say("[%s]     python -u midi\\bridge.py --relay 127.0.0.1:%s" % (tag, port))
    helpers = [subprocess.Popen(host.python_argv() + [os.path.join(lay.run, "midi_relay.py"), "--tags", tags,
                                                      "--port", port])]
    pcap = os.path.join(lay.tmp, "j2-%s.pcap" % tag)
    if linked and env.get("SNIFF") == "1":
        with open(os.path.join(lay.tmp, "j2-%s.sniff" % tag), "wb") as log:
            helpers.append(subprocess.Popen(host.python_argv() + [
                os.path.join(lay.scripts, "net", "capture_link.py"), env["GROUP"], pcap],
                stdout=log, stderr=subprocess.STDOUT))
        chain.say("[%s] capturing the segment -> /tmp/j2-%s.pcap" % (tag, tag))
    env["NDECKS"] = str(n)
    try:
        rc = chain.run_script("rig", [tag, nonempty(env, "FRAMES", "240")], env)
    finally:
        for p in helpers:
            p.terminate()
    if not linked:
        return rc
    if env.get("SNIFF") == "1" and os.path.isfile(pcap):
        chain.say("")
        chain.say("===== what reached the wire =====")
        subprocess.run(host.python_argv() + [os.path.join(lay.scripts, "net", "score_link.py"), pcap])
    chain.say("")
    chain.say("===== leases =====")
    try:
        with open(os.path.join(lay.tmp, "cdj-%s-dhcpd.log" % tag), encoding="utf-8", errors="replace") as f:
            chain.say(f.read().rstrip("\n"))
    except OSError:
        chain.say("  (no DHCP log -- DJLINK=0?)")
    return 0
