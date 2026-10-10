# SPDX-License-Identifier: GPL-2.0-or-later
"""The live rig: one or two decks running the real DSP, with the DSP's own
McBSP0 PCM as audio (CDJ_C6X_AUDIO). The deck starts playing by itself at the
end of the load; PLAY pauses and resumes. The machine is paced to real time
(icount sleep=on,align=on); unpaced it overfills or underruns the PCM ring.

  usage: bash scripts/run/rig.sh [prefix=show] [film-frames=120]
  env:   DJLINK=1 (Pro DJ Link on; 0 = off)   GROUP=<ip:port> (own segment)
         DJLINK=tap:<adapter> puts one deck on a TAP-Windows6 adapter instead (see scripts/run/rig.sh)
         NDECKS=1 (2 for both DJ-202 sides)   GUI_DISPLAY=gtk|cocoa|none   AUDIODEV=<-audio spec, %TAG% ok>
         RING=3000 PREFILL=150 MAXLAT=450 (ms)   NOSOUND=1   WARM=0 (1 = throwaway warm-up wave first)
         SERVICE=1 boots into SERVICE MODE instead of playing a track
"""

import os
import subprocess

from . import chain, dsp_module, host, mods
from . import model as cdj_model
from .chain import export_default, ifset, nonempty, warn_banner
from .layout import Layout

DEFAULT_GROUP = "239.77.77.1:45000"
TAP_HOST_IP = "192.168.50.1"


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


def jit_module(env, m):
    """The DSP JIT module: the first that exists of C66X_JIT (named by hand),
    ~/c14gen/$MODULE/m.so (MODULE=none: no module), the model's own module
    (built by ./setup.sh, and only while its stamp fits) and, for the default
    player only, the maintainers' g23n (compiles MASTER TEMPO at a non-zero
    tempo too), g20u800 (g18u plus MASTER TEMPO's code) and g18u. Another
    player runs a different DSP program, which those modules do not fit."""
    if env.get("C66X_JIT") or env.get("MODULE") == "none":
        return env.get("C66X_JIT", "")
    lay = Layout()
    cands = [os.path.join(lay.jit_cache, env["MODULE"], "m.so")] if env.get("MODULE") else []
    cands.append(dsp_module.check(lay, m).path)
    if m.id == cdj_model.DEFAULT:
        cands += [os.path.join(lay.jit_cache, d, "m.so") for d in ("g23n", "g20u800", "g18u")]
        if not host.is_windows():
            cands.append("/tmp/c14gen/g18u/m.so")
    for c in cands:
        if os.path.isfile(c):
            return host.native(c)
    return ""


def tap_adapter(ifname, ndecks):
    """DJLINK=tap:<ifname>: the adapter name, once it is fit to carry the deck.
    The deck and the host share the adapter's segment, and the host end of it
    is the rig's DHCP server."""
    if not host.is_windows():
        raise SystemExit("DJLINK=tap: needs Windows (TAP-Windows6)")
    if int(ndecks) > 1:
        raise SystemExit("DJLINK=tap:%s: a TAP-Windows6 adapter carries one deck, NDECKS=%s" % (ifname, ndecks))
    setup = ("one-time setup: install TAP-Windows6, rename the adapter to %s, then: "
             "netsh interface ip set address \"%s\" static %s 255.255.255.0 (details in scripts/run/rig.sh)"
             % (ifname, ifname, TAP_HOST_IP))
    has_ip = host.adapter_has_address(ifname, TAP_HOST_IP)
    if has_ip is None:
        raise SystemExit("DJLINK=tap:%s: no adapter of that name; %s" % (ifname, setup))
    if not has_ip:
        raise SystemExit("DJLINK=tap:%s: the adapter has no %s; %s" % (ifname, TAP_HOST_IP, setup))
    return ifname


def rig_env(env, tag, ndecks, frames):
    """rig.sh's knobs, applied to `env`. Returns the argument for the rig's
    DHCP server (the Pro DJ Link group, or tap:<host ip>) when the rig should
    run one."""
    say, warn = chain.say, chain.err
    m = cdj_model.load()
    for name, value in m.rig_env.items():
        export_default(env, name, value)
    if m.dsp_idle:
        export_default(env, m.dsp_idle_knob, m.dsp_idle)
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
    chosen = bool(env.get("C66X_JIT") or env.get("MODULE"))
    env["C66X_JIT"] = jit_module(env, m)
    export_default(env, "C66X_JIT_AUTO", host.native(Layout().jit_cache))
    autojit = env.get("AUTOJIT", "")
    if autojit == "0" or (autojit != "1" and env["C66X_JIT"]):
        chain.unset(env, "C66X_JIT_AUTO")
    # A module of this player that is not loaded matters unless another one
    # took its place; one loaded without a stamp always does.
    own = dsp_module.check(Layout(), m)
    if own.notice and not chosen and (own.path or not env["C66X_JIT"]):
        warn_banner(*own.notice)
    export_default(env, "CDJ_NATIVE_LIBC", "1")

    # Pro DJ Link, on by default. DJLINK=0 turns it off; DJLINK=tap:<adapter>
    # attaches the deck to a host TAP adapter (one deck) instead of the segment.
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
    dhcp = netdev = None
    if env["DJLINK"].startswith("tap:"):
        adapter = tap_adapter(env["DJLINK"][4:], ndecks)
        netdev, dhcp, where = "tap,id=djlink,ifname=" + adapter, "tap:" + TAP_HOST_IP, "adapter %s (host %s)" % (
            adapter, TAP_HOST_IP)
    elif env["DJLINK"] == "1":
        group = nonempty(env, "GROUP", DEFAULT_GROUP)
        # Windows reserves UDP ranges for Hyper-V/WSL, and a bind inside one
        # fails with only "Unknown error" from QEMU.
        gport = int(group.rsplit(":", 1)[-1])
        if host.in_ranges(gport, host.windows_reserved_ranges("udp")):
            warn("[%s] \u26a0 UDP port %d is in a Windows RESERVED range -- the" % (tag, gport))
            warn("[%s]   netdev cannot bind it and MAIN will not start. Pick another:" % tag)
            warn("[%s]   GROUP=239.77.77.1:45000 bash scripts/run/live_linked.sh %s" % (tag, tag))
            warn("[%s]   (netsh int ipv4 show excludedportrange protocol=udp)" % tag)
        netdev, dhcp, where = "socket,id=djlink,mcast=" + group, group, group
    if netdev:
        export_default(env, "CDJ_NETDEV", netdev)
        export_default(env, "CDJ_ETHER_PHYADS", "0,1,5")
        env["CDJ_ETHER_MAC"] = ifset(env, "CDJ_ETHER_MAC", "02:00:00:00:00:0%N%")
        export_default(env, "CDJ_PCALL2", "0x08345574:0x08516448:0x2a:0x50000001:0x0")
        if nonempty(env, "DJLINK_DHCP", "1") != "1":
            dhcp = None
        say("[%s] Pro DJ Link ON: %s, MAC %s, leases -> /tmp/cdj-%s-dhcpd.log" % (tag, where, env["CDJ_ETHER_MAC"], tag))
        if env["DJLINK"] == "1":
            say("[%s]   watch it: python3 scripts/net/capture_link.py %s /tmp/cdj-%s.pcap  (score with score_link.py)"
                % (tag, group, tag))
    else:
        say("[%s] Pro DJ Link off (DJLINK=0)" % tag)
    # MAIN/DSP lockstep quantum. It sets how coarsely MAIN sees the DSP's play
    # position; at 16 ms the Pro DJ Link beat sender misses a few beats. One
    # deck can afford 4 ms, two decks cannot.
    export_default(env, "CDJ_C6X_QUANTUM_US", "4000" if str(ndecks) == "1" else "16000")
    # The start-up burst needs more DSP cycles per second than a slow host has, which
    # stalls the machine's clock and starves the audio sink. The DSP clock may fall to
    # what the host delivers, down to this many MHz.
    export_default(env, "CDJ_C6X_GOVERN", "100")
    # A new heartbeat supersedes the queued ones on the GUI side, so keys reach
    # the screen quickly. 16, not 2: a queue of 2 starves the GUI link.
    export_default(env, "CDJ_SPILINK_FRESH", "16")
    # A receive that finds the queue empty gets the newest heartbeat again
    # after 20 ms; a starved GUI link driver gives up and the display freezes.
    export_default(env, "CDJ_GUI_LINK_IDLE_MS", "20")
    # Every mod in mods.conf the caller left unset gets its registry default.
    mod_list = mods.load(Layout())
    for m in mod_list:
        value = mods.default(mod_list, m.env)
        for name in mods.env_names(m.env):
            export_default(env, name, value)
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
    if cdj_model.load().tablet_peer:
        if group and not group.startswith("tap:"):
            with open(os.path.join(lay.tmp, "cdj-%s-tablet.log" % tag), "wb") as log:
                helpers.append(subprocess.Popen(host.python_argv() + [
                    os.path.join(lay.scripts, "net", "tablet_peer.py"), group], stdout=log, stderr=subprocess.STDOUT))
        else:
            chain.err("[%s] this player's browse keys go to its tablet, which needs DJLINK=1 (not tap:)" % tag)
    try:
        return chain.run_script("play_real_dsp", [tag, ndecks], env)
    finally:
        for p in helpers:
            p.terminate()
