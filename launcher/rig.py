# SPDX-License-Identifier: GPL-2.0-or-later
"""The live rig: one or two decks running the real DSP, with the DSP's own
McBSP0 PCM as audio (CDJ_C6X_AUDIO). The deck starts playing by itself at the
end of the load; PLAY pauses and resumes. The machine is paced to real time
(icount sleep=on,align=on); unpaced it overfills or underruns the PCM ring.

  usage: bash scripts/run/rig.sh [prefix=show] [film-frames=120]
  env:   DJLINK=1 (Pro DJ Link on; 0 = off)   GROUP=<ip:port> (own segment)
         NDECKS=1 (2 for both DJ-202 sides)   GUI_DISPLAY=gtk|cocoa|none   AUDIODEV=<-audio spec, %TAG% ok>
         RING=3000 PREFILL=150 MAXLAT=450 (ms)   NOSOUND=1   WARM=0 (1 = throwaway warm-up wave first)
"""

import os
import subprocess

from . import chain, host
from .chain import export_default, ifset, nonempty
from .layout import Layout

DEFAULT_GROUP = "239.77.77.1:45000"


def default_audiodev():
    k = host.kind()
    if k == host.WINDOWS:
        # QEMU 9.1 has no WASAPI audiodev, but SDL2's Windows backend is WASAPI.
        return "sdl,out.buffer-length=200000,timer-period=5000"
    if k == host.MACOS:
        # Core Audio's buffer-length is per buffer, times buffer-count, not the
        # total as for pa, so pa's 200000 would queue 800 ms; 8 x 23 ms is
        # about pa's 200 ms in all.
        return "coreaudio,out.buffer-length=23220,out.buffer-count=8,timer-period=5000"
    return "pa,server=unix:/mnt/wslg/PulseServer,out.buffer-length=200000,timer-period=5000"


def jit_module(env):
    """The DSP JIT module: the first that exists of C66X_JIT (named by hand),
    ~/c14gen/$MODULE/m.so (MODULE=none: no module), ~/c14gen/curated/m.so
    (built by ./setup.sh --curated-jit), then the maintainers' g20u800 (g18u
    plus MASTER TEMPO's code) and g18u."""
    if env.get("C66X_JIT") or env.get("MODULE") == "none":
        return env.get("C66X_JIT", "")
    cache = Layout().jit_cache
    names = ([env["MODULE"]] if env.get("MODULE") else []) + ["curated", "g20u800", "g18u"]
    cands = [os.path.join(cache, d, "m.so") for d in names]
    if not host.is_windows():
        cands.append("/tmp/c14gen/g18u/m.so")
    for c in cands:
        if os.path.isfile(c):
            return host.native(c)
    return ""


def rig_env(env, tag, ndecks, frames):
    """rig.sh's knobs, applied to `env`. Returns the Pro DJ Link group when
    the rig should run a DHCP server for it."""
    say, warn = chain.say, chain.err
    if env.get("NOSOUND", "0") != "1":
        # Ring/prefill/cap 3000/150/450 ms; a narrower band ping-pongs between
        # underruns and trims. The larger buffer rides over a host sink that
        # drains slower than the DSP produces under load; the model's
        # resampler (CDJ_C6X_AUDIO_ASRC) absorbs the rest.
        env["CDJ_C6X_AUDIO"] = "1:%s:%s:%s" % (nonempty(env, "RING", "3000"), nonempty(env, "PREFILL", "150"),
                                               nonempty(env, "MAXLAT", "450"))
        env["CDJ_AUDIODEV"] = nonempty(env, "AUDIODEV", default_audiodev())
    chain.unset(env, "CDJ_AUDIO_LIVE", "CDJ_AUDIO_OUT")
    say("[%s] %s real-DSP deck(s): tags %s1 .. %s%s (controller mapping: cdjA -> %s1, cdjB -> %s2)"
        % (tag, ndecks, tag, tag, ndecks, tag, tag))
    say("[%s] audio: %s  ring/prefill/cap %s/%s/%s ms" % (
        tag, env.get("CDJ_AUDIODEV") or "off", nonempty(env, "RING", "3000"), nonempty(env, "PREFILL", "150"),
        nonempty(env, "MAXLAT", "450")))
    if nonempty(env, "AUTOLOAD", "1") == "1":
        say("[%s] each deck loads the first track and starts playing by itself (AUTOLOAD=0 leaves that to you)." % tag)
        say("[%s] the screen may stay on the BROWSE list after the auto-load: BROWSE toggles to the waveform view." % tag)

    # A raw FAT16 image instead of vvfat: vvfat commits the whole tree on every
    # guest write with the BQL held, which stalls the machine when the firmware
    # rewrites export.pdb.
    export_default(env, "MEDIA_MODE", "img")
    # The display board's RTOS idle loop at 0x0E510588 becomes a halt, which
    # frees most of a core. The hook checks the opcode first. Empty = no hook.
    env["CDJ_GUI_IDLE_PC"] = ifset(env, "CDJ_GUI_IDLE_PC", "0x0E51058A")
    if host.is_windows():
        export_default(env, "QEMU_BUILD", "/c/qemu-build-mingw")
        export_default(env, "QEMU_EB_BUILD", "/c/qemu-build-mingw-eb")
    # With a module the run-time auto-JIT is off, so no compiler runs
    # mid-session. AUTOJIT=1 keeps it on beside a module, AUTOJIT=0 turns it off.
    env["C66X_JIT"] = jit_module(env)
    export_default(env, "C66X_JIT_AUTO", host.native(Layout().jit_cache))
    autojit = env.get("AUTOJIT", "")
    if autojit == "0" or (autojit != "1" and env["C66X_JIT"]):
        chain.unset(env, "C66X_JIT_AUTO")
    if not env["C66X_JIT"] and env.get("C66X_JIT_AUTO"):
        say("[%s] no curated JIT module: the auto-JIT compiles the DSP's hot code as it plays" % tag)
    export_default(env, "CDJ_NATIVE_LIBC", "1")

    # Pro DJ Link, on by default. DJLINK=0 turns it off.
    #  CDJ_NETDEV        a multicast segment shared by every deck on GROUP;
    #                    QEMU's socket backend carries one frame per datagram.
    #  CDJ_ETHER_PHYADS  0,1,5. The link-up gate is link_status(0) | link_status(1).
    #  CDJ_ETHER_MAC     a distinct MAC per deck (%N% = deck number).
    #  CDJ_PCALL2        wakes the ether worker, which waits in twai_flg, with
    #                    iset_flg(42, 0x50000001). The pattern needs a bit in
    #                    0xFC000000 or the worker goes straight back to waiting.
    # The deck will not announce without a DHCP lease, so a DHCP server runs
    # as long as the rig.
    export_default(env, "DJLINK", "1")
    dhcp = None
    if env["DJLINK"] == "1":
        group = nonempty(env, "GROUP", DEFAULT_GROUP)
        # Windows reserves UDP ranges for Hyper-V/WSL, and a bind inside one
        # fails with only "Unknown error" from QEMU.
        gport = int(group.rsplit(":", 1)[-1])
        if host.in_ranges(gport, host.windows_reserved_ranges("udp")):
            warn("[%s] \u26a0 UDP port %d is in a Windows RESERVED range -- the" % (tag, gport))
            warn("[%s]   netdev cannot bind it and MAIN will not start. Pick another:" % tag)
            warn("[%s]   GROUP=239.77.77.1:45000 bash scripts/run/live_linked.sh %s" % (tag, tag))
            warn("[%s]   (netsh int ipv4 show excludedportrange protocol=udp)" % tag)
        export_default(env, "CDJ_NETDEV", "socket,id=djlink,mcast=" + group)
        export_default(env, "CDJ_ETHER_PHYADS", "0,1,5")
        env["CDJ_ETHER_MAC"] = ifset(env, "CDJ_ETHER_MAC", "02:00:00:00:00:0%N%")
        export_default(env, "CDJ_PCALL2", "0x08345574:0x08516448:0x2a:0x50000001:0x0")
        if nonempty(env, "DJLINK_DHCP", "1") == "1":
            dhcp = group
        say("[%s] Pro DJ Link ON: %s, MAC %s, leases -> /tmp/cdj-%s-dhcpd.log" % (tag, group, env["CDJ_ETHER_MAC"], tag))
        say("[%s]   watch it: python3 scripts/net/capture_link.py %s /tmp/cdj-%s.pcap  (score with score_link.py)"
            % (tag, group, tag))
    else:
        say("[%s] Pro DJ Link off (DJLINK=0)" % tag)
    # MAIN/DSP lockstep quantum. It sets how coarsely MAIN sees the DSP's play
    # position; at 16 ms the Pro DJ Link beat sender misses a few beats. One
    # deck can afford 4 ms, two decks cannot.
    export_default(env, "CDJ_C6X_QUANTUM_US", "4000" if str(ndecks) == "1" else "16000")
    # A new heartbeat supersedes the queued ones on the GUI side, so keys reach
    # the screen quickly. 16, not 2: a queue of 2 starves the GUI link.
    export_default(env, "CDJ_SPILINK_FRESH", "16")
    # A receive that finds the queue empty gets the newest heartbeat again
    # after 20 ms; a starved GUI link driver gives up and the display freezes.
    export_default(env, "CDJ_GUI_LINK_IDLE_MS", "20")
    # The display firmware repaints REMAIN once its frames' measured drawing
    # time adds up to 42 ms; the emulated board draws so fast that this was ~3
    # times a second. Count each frame as 43 ms: a repaint every frame. 0 = firmware.
    export_default(env, "CDJ_GUI_CLOCK_DT", "43")
    # Diagnostic re-read and scan of every DMA'd display frame; off.
    export_default(env, "CDJ_GUI_FRAME_SCAN", "0")
    # TOUCH=1 (default): a click/drag in the display window, or a 'touch'/'tap'
    # on the panel key socket, becomes the report's touch X/Y (bytes 0x16..0x19).
    env["CDJ_TOUCH"] = nonempty(env, "TOUCH", "1")
    # MAXLAG=<ms>: cap how far a deck may fall behind real time. Without a cap a
    # deck that falls behind catches up by running fast, and the audio
    # resampler then plays up to 4 % sharp; with two decks and MASTER TEMPO the
    # worst lateness went from 20-54 s to 1 s at 250. 0 = QEMU's catch-up.
    env["CDJ_ICOUNT_MAXLAG_MS"] = nonempty(env, "MAXLAG", "250")
    # TBFAST=1 (default): seven QEMU fast paths (TB lookup, FPSCR exits,
    # code-page store checks, getenv cache, MMIO splitting) that remove most of
    # MAIN's emulation overhead. TBFAST=0 runs the plain QEMU paths.
    if nonempty(env, "TBFAST", "1") == "1":
        for k, v in (("CDJ_TB_FPSCR", "1"), ("CDJ_TB_XPAGE", "1"), ("CDJ_TB_CALLPRED", "1"), ("CDJ_SMC", "2"),
                     ("CDJ_ENVCACHE", "1"), ("CDJ_JC_HASH", "1"), ("CDJ_IOSPLIT", "1")):
            export_default(env, k, v)
    say("[%s] icount %s  DSP thread %s  module %s  native libc %s" % (
        tag, nonempty(env, "ICOUNT", "shift=2,sleep=on,align=on"), nonempty(env, "DSPTHREAD", "2"),
        env["C66X_JIT"] or "<none: auto-JIT into %s>" % (env.get("C66X_JIT_AUTO") or "off"), env["CDJ_NATIVE_LIBC"]))
    # SNAP=none: no RAM snapshots at the play key. Each 29 MB memsave stalls the
    # machine just as the track starts (0.5x for ~10 s), and the rig never
    # scores them.
    env.update({"LADDER": "0", "W1": "0", "THIN": nonempty(env, "THIN", "0"), "HOLDS": "0", "C6X": "1",
                "KEYBITS": "0x00", "SNAP": nonempty(env, "SNAP", "none"),
                "ICOUNT": nonempty(env, "ICOUNT", "shift=2,sleep=on,align=on"),
                "DSPTHREAD": nonempty(env, "DSPTHREAD", "2"), "GUI_DISPLAY": nonempty(env, "GUI_DISPLAY", "gtk"),
                "JOBS": str(ndecks), "WARMUP": nonempty(env, "WARM", "0"), "FILMN": str(frames),
                "MOTION_MS": nonempty(env, "MOTION_MS", "5000")})
    return dhcp


def main(argv):
    tag = argv[0] if argv else "show"
    frames = argv[1] if len(argv) > 1 else "120"
    lay = Layout()
    env = dict(os.environ)
    ndecks = nonempty(env, "NDECKS", "1")
    for i in range(1, int(ndecks) + 1):
        chain.remove(os.path.join(lay.tmp, "bridge-main-%s%d.log" % (tag, i)))
        chain.remove(os.path.join(lay.tmp, "bridge-gui-%s%d.log" % (tag, i)))
    helpers = []
    group = rig_env(env, tag, ndecks, frames)
    if group:
        with open(os.path.join(lay.tmp, "cdj-%s-dhcpd.log" % tag), "wb") as log:
            helpers.append(subprocess.Popen(host.python_argv() + [
                os.path.join(lay.scripts, "net", "dhcp_server.py"), group], stdout=log, stderr=subprocess.STDOUT))
    # PRIO=AboveNormal|High|Normal: Windows priority class for the rig's
    # QEMUs. A busy desktop otherwise keeps MAIN's vCPU off the CPU.
    prio = nonempty(env, "PRIO", "AboveNormal")
    if host.is_windows() and prio != "Normal":
        ps = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "WindowsPowerShell", "v1.0",
                          "powershell.exe")
        with open(os.path.join(lay.tmp, "%s-prio.txt" % tag), "wb") as log:
            subprocess.Popen([ps, "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                              os.path.join(lay.run, "raise_priority.ps1"), "-Seconds", "0", "-Class", prio],
                             stdout=log, stderr=subprocess.STDOUT)
        chain.say("[%s] QEMU priority %s (PRIO=Normal to disable; log /tmp/%s-prio.txt)" % (tag, prio, tag))
    try:
        return chain.run_script("play_real_dsp", [tag, ndecks], env)
    finally:
        for p in helpers:
            p.terminate()
