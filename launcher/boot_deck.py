# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot one deck: the MAIN board and the GUI board joined by the SPI link, with
no gdbstub attached (a gdbstub client halts MAIN and starves the link). The
boards run for [seconds] and are then stopped, MAIN through its monitor so its
exit counters print.

  usage: ./scripts/run/boot_deck.sh <tag> [seconds]
  env:   SERVICE=1 boots into the service manual's SERVICE MODE screen
         instead of the player (SERVICE_HOLD_MS overrides how long the entry
         keys are held, default 20000)
"""

import os
import re
import shutil
import subprocess
import sys
import time

from . import chain, host
from . import model as cdj_model
from .chain import export_default, ifset, nonempty
from .layout import Layout


def _monsock(lay):
    sys.path.insert(0, lay.run)
    try:
        import cdj_monsock
    finally:
        sys.path.remove(lay.run)
    return cdj_monsock


def _append_mpoke(env, spec):
    env["CDJ_MPOKE"] = env["CDJ_MPOKE"] + "," + spec if env.get("CDJ_MPOKE") else spec


class Deck:
    """The command lines and environment of one deck, worked out before
    anything starts; start() and stop() then run it."""

    def __init__(self, tag, env, lay, profile):
        self.tag = tag
        self.lay = lay
        self.env = env
        self.profile = profile
        self.notes = []          # (stream, line) printed as the deck starts
        self.mint = None         # player_flash.py argv
        self.gui_patch = None    # patch_gui.py argv
        self.main_patch = None   # patch_main.py argv
        self.flash_copy = None   # (src, dst)
        self.persist_new = None  # the deck's own flash image, made on this start
        self.media_cache = None  # (src, dst)
        self.media_copy = None   # (src, dst)
        self.gui_env = None
        self.gui_respawns = 0
        self.closed = False        # the screen's window was closed: the deck stops
        self.vnc_port_file = None  # where the app learns this deck's VNC port
        self.vnc_port = None       # the port the GUI QEMU took, once known
        self._plan()

    def _plan(self):
        env, lay, tag, profile = self.env, self.lay, self.tag, self.profile
        tmp = lay.tmp
        extract_n = host.native(lay.extract)
        # Only the two kernel images live under the model's own folder; the
        # flash image, the USB medium and the GUI archives are the
        # CDJ-2000NXS2's own, which is the only model this launcher runs (a
        # model still in bring-up boots MAIN alone, with boot_main.sh).
        model_extract_n = host.native(cdj_model.extract_dir(lay, profile))
        sfx = host.exe_suffix()
        home = host.home()
        # MAIN_QEMU / GUI_QEMU override the binaries; the defaults follow the
        # build scripts' QEMU_BUILD / QEMU_EB_BUILD.
        self.main_qemu = env.get("MAIN_QEMU") or "%s/qemu-system-sh4%s" % (
            host.native(nonempty(env, "QEMU_BUILD", os.path.join(home, "qemu-build"))), sfx)
        self.gui_qemu = env.get("GUI_QEMU") or "%s/qemu-system-sh4eb%s" % (
            host.native(nonempty(env, "QEMU_EB_BUILD", os.path.join(home, "qemu-build-eb"))), sfx)
        # A checkout's QEMU_BUILD carries no DLLs of its own: MAIN and GUI are
        # mingw64 builds that resolve SDL2.dll and the rest through PATH. The
        # shell scripts always ran inside MSYS2, where that is a given; this
        # launcher can be started from one that never put it there, and QEMU
        # then dies before it writes a byte to its own log.
        dll_dir = host.qemu_dll_dir()
        if dll_dir and dll_dir.lower() not in (p.lower() for p in env.get("PATH", "").split(os.pathsep)):
            env["PATH"] = dll_dir + os.pathsep + env.get("PATH", "")
        self.main_log = "%s/bridge-main-%s.log" % (nonempty(env, "LOGDIR", tmp), tag)
        self.gui_log = "%s/bridge-gui-%s.log" % (nonempty(env, "LOGDIR", tmp), tag)
        mediadir = host.native(nonempty(env, "MEDIADIR", os.path.join(lay.extract, "usbmedia3")))
        self.sock = "%s/cdj-%s-spi.sock" % (tmp, tag)

        # A dump path is per process, and a batch runs several QEMUs at once,
        # so these may contain %TAG%. The C side treats an empty value as set.
        for n in ("CDJ_DSP_TXDUMP", "CDJ_SPILINK_DUMP", "CDJ_C6X_PCM", "CDJ_C6X_RECORD"):
            if env.get(n):
                env[n] = env[n].replace("%TAG%", tag)
        # %N% is this deck's number, the last digit of the tag, so one value
        # can give each deck its own address (CDJ_ETHER_MAC=02:00:00:00:00:0%N%).
        n = tag[-1] if tag[-1:].isdigit() else "1"
        self.n = n
        if env.get("CDJ_ETHER_MAC"):
            env["CDJ_ETHER_MAC"] = env["CDJ_ETHER_MAC"].replace("%N%", n)
        # CDJ_ETHER_MAC rewrites the Ethernet header only. The firmware keeps
        # its own MAC at 0x0A35F754 (read through the getter 0x08232104 by the
        # MAHR/MALR writer and the Pro DJ Link announce builder), blank in the
        # dump, so every instance is 00:00:00:00:00:01 and two decks never
        # settle the device-number claim. OWNMAC=<byte> holds the last byte
        # (default: the deck number), OWNMAC=0 leaves it; with CDJ_ETHER_MAC
        # set and OWNMAC unset the whole address is held to the wire MAC. Only
        # non-zero bytes and byte 5 are poked: CDJ_MPOKE has 8 slots.
        if env.get("CDJ_NETDEV") and nonempty(env, "OWNMAC", n) != "0":
            if not env.get("OWNMAC") and env.get("CDJ_ETHER_MAC"):
                for i, b in enumerate(env["CDJ_ETHER_MAC"].split(":")):
                    if i == 5 or int(b, 16) != 0:
                        _append_mpoke(env, "0x%08X:0x%s:1" % (0x0A35F754 + i, b))
                self.notes.append((1, "[%s] own MAC 0x0A35F754 held to the wire address %s (OWNMAC=0 to leave it)"
                                   % (tag, env["CDJ_ETHER_MAC"])))
            else:
                om = nonempty(env, "OWNMAC", n)
                _append_mpoke(env, "0x0A35F759:%s:1" % om)
                self.notes.append((1, "[%s] own MAC byte 0x0A35F759 held at %s (OWNMAC=0 to leave it)" % (tag, om)))
        if env.get("CDJ_AUDIODEV"):
            env["CDJ_AUDIODEV"] = env["CDJ_AUDIODEV"].replace("%TAG%", tag)

        self.gui_mon = "%s/cdj-%s-gui-mon.sock" % (tmp, tag)

        # PERSIST=1: this deck keeps its own settings in extract/flash-<tag>.bin.
        # The default is the shared image with snapshot=on, so no run can drift.
        # PLAYERNO=<1..4|%N%>: boot with PLAYER No. already set; two decks both
        # left at 1 deadlock the Pro DJ Link device-number negotiation.
        # SERIAL=<12 chars|auto>: the deck's serial (every instance has
        # PDJ0000001XX).
        playerno = env.get("PLAYERNO", "").replace("%N%", n)
        serial = env.get("SERIAL", "").replace("%N%", n)
        flash = extract_n + "/flash.bin"
        snap = "snapshot=on"
        if nonempty(env, "PERSIST", "0") == "1":
            fl = os.path.join(lay.extract, "flash-%s.bin" % tag)
            if not os.path.isfile(fl):
                if playerno:
                    self.mint = self._mint_argv(playerno, serial, fl)
                else:
                    self.flash_copy = (os.path.join(lay.extract, "flash.bin"), fl)
                self.persist_new = fl
            flash = host.native(fl)
            snap = "snapshot=off"
        elif playerno:
            fl = "%s/flash-%s.bin" % (tmp, tag)
            self.mint = self._mint_argv(playerno, serial, fl)
            flash = host.native(fl)

        # MEDIA_MODE: how the USB medium is attached.
        #   rw  (default) fat:rw:$MEDIADIR          -- vvfat, guest writes allowed
        #   ro            fat:$MEDIADIR,readonly=on -- guest writes refused
        #   img           a real FAT16 image (extract/usbmedia3.img) with a
        #                 throwaway overlay; vvfat commits the tree on every write
        self.media_img = None
        mode = nonempty(env, "MEDIA_MODE", "rw")
        if mode == "ro":
            drive = "format=raw,file=fat:%s,readonly=on" % mediadir
        elif mode == "img":
            src = nonempty(env, "MEDIA_IMG_SRC", os.path.join(lay.extract, "usbmedia3.img"))
            if "MEDIA_IMG_SRC" not in env and not _same_filesystem(src, tmp):
                # Cache the source on the local disk; a remote or slow mount
                # (e.g. WSL's /mnt/c) is not worth reading from on every boot.
                # Already-local (same filesystem as tmp) skips this copy.
                cached = "%s/usbmedia3.img" % tmp
                self.media_cache = (src, cached)
                src = cached
            # snapshot=on puts the guest's writes in a throwaway overlay, which
            # costs nothing, where copying the 256 MB image took 7-15 s a run.
            # MEDIA_COPY=1 goes back to a private copy.
            if nonempty(env, "MEDIA_COPY", "0") == "1":
                self.media_img = "%s/media-%s.img" % (tmp, tag)
                self.media_copy = (src, self.media_img)
                mfile = host.native(self.media_img)
            else:
                mfile = host.native(src) + ",snapshot=on"
            # MEDIA_CACHE: unsafe ignores the guest's flushes, writeback honours them.
            drive = "format=raw,file=%s,cache=%s" % (mfile, nonempty(env, "MEDIA_CACHE", "writeback"))
        else:
            drive = "format=raw,file=fat:rw:%s" % mediadir
        self.mediadir = mediadir
        # NOMEDIA=1 attaches no stick. UTILITY refuses to change PLAYER No.
        # while a device is mounted, so set it on a bare deck with PERSIST=1.
        if nonempty(env, "NOMEDIA", "0") == "1":
            media_args = []
            self.notes.append((1, "[%s] media: NONE (NOMEDIA=1) -- no track will load" % tag))
        else:
            media_args = ["-drive", "if=none,id=usbstick," + drive,
                          "-device", "usb-storage,drive=usbstick,port=1"]
            self.notes.append((1, "[%s] media: %s (%s)" % (tag, mode, drive)))

        env["CDJ_SPILINK_OVERFLOW"] = "1"
        env["CDJ_SPILINK_QUEUE"] = "0"
        export_default(env, "CDJ_SPILINK_DEDUP", "0")
        export_default(env, "SPILINK_KEEP_FRAMES", "64")
        export_default(env, "CDJ_SPILINK_KEEP_FRAMES", "64")
        env.update({"CDJ_AREA4": "1", "CDJ_DSP_LINK": "1", "CDJ_DSP_READY": "1", "CDJ_DMA1_IEACK": "1",
                    # Subsystem 5 (E-7206 AUTH CHIP ERROR) needs IIC0 to answer 0x10.
                    "CDJ_IIC_SLAVE": "1", "CDJ_USB_OC": "1"})
        export_default(env, "CDJ_IIC_ADDR", "0x30,0x2c")
        export_default(env, "CDJ_IIC_CH", "1")
        env["CDJ_GUI_VDC_SCANOUT"] = "1"
        env["USB_MEDIA"] = "1"
        # GUI font/art archives, installed by prepare_firmware.sh --install.
        env["CDJ_GUI_FONTBLOB"] = host.native(os.path.join(lay.extract, "resblob.bin"))
        env["CDJ_GUI_ARTBLOB"] = host.native(os.path.join(lay.extract, "artblob.bin"))
        # The front panel is a datagram socket, so keys can be pressed without
        # halting MAIN.
        env["CDJ_PANEL_KEYSOCK"] = "%s/cdj-panel-keys-%s.sock" % (tmp, tag)
        env["CDJ_PANEL_RX"] = "1"
        env["CDJ_PANEL_MAX_IRQ"] = "4000000"
        # SERVICE=1: boot into the service manual's SERVICE MODE instead of
        # the regular player screen, by holding TEMPO RANGE (report 0x15,
        # mask 0x08) and MEMORY (0x0c, mask 0x08) from reset. Both keys
        # release after SERVICE_HOLD_MS so nothing stays stuck down once the
        # logo clears. A caller's own CDJ_PANEL_PRESS always wins.
        if nonempty(env, "SERVICE", "0") == "1":
            hold = nonempty(env, "SERVICE_HOLD_MS", "20000")
            env["CDJ_PANEL_PRESS"] = ifset(env, "CDJ_PANEL_PRESS", "0x15:0x08:0:%s,0x0c:0x08:0:%s" % (hold, hold))
        else:
            env["CDJ_PANEL_PRESS"] = ifset(env, "CDJ_PANEL_PRESS", "0x13:0x04:20000:3000")
        # ICOUNT: MAIN's virtual clock from executed instructions instead of
        # host time, which removes host jitter from the firmware's timing.
        icount = ["-icount", env["ICOUNT"]] if env.get("ICOUNT") else []
        # MAIN's virtual clock, published for load_track.py so it waits in
        # virtual seconds.
        env["CDJ_VCLOCK_FILE"] = "%s/cdj-%s-vclock" % (tmp, tag)

        mon = _monsock(lay)
        # MAIN_MON=1: a monitor for MAIN too. Unlike a gdbstub it never halts
        # the CPU, so memsave reads RAM of a running deck. The JIT profile is
        # written at exit, and on Windows a kill runs no exit handlers: a
        # profile run needs the monitor so it can quit cleanly.
        self.main_mon = ""
        mon_args = ["-monitor", "none"]
        if env.get("C66X_JIT_PROFILE"):
            env["MAIN_MON"] = "1"
        if nonempty(env, "MAIN_MON", "0") == "1":
            self.main_mon = "%s/cdj-%s-main-mon.sock" % (tmp, tag)
            mon_args = ["-monitor", mon.spec(self.main_mon)]
        # -audio, not -audiodev: the model's sound card is not a qdev device and
        # binds to the default audiodev, which only -audio creates.
        audio = ["-audio", env["CDJ_AUDIODEV"]] if env.get("CDJ_AUDIODEV") else []
        # CDJ_NETDEV: one -netdev for Pro DJ Link; the EtherMAC's NIC finds its
        # backend by id, so decks can share an L2 segment.
        net = ["-netdev", env["CDJ_NETDEV"]] if env.get("CDJ_NETDEV") else []
        # MAIN firmware mods are patched into a copy of the image, per run:
        # each patch_main.py mod <name> is on when CDJ_MAIN_<NAME>=1. A
        # -kernel boot patches main_unpacked.bin the way gui_patch does above;
        # MAIN_BOOT=flash instead patches the packed image inside a copy of
        # the flash the -drive below attaches, since that is where the
        # bootloader reads it from.
        main_kernel = model_extract_n + "/main_unpacked.bin"
        main_mods = [m for m in self._main_mod_names() if env.get("CDJ_MAIN_" + m.upper()) == "1"]
        if main_mods:
            if env.get("MAIN_BOOT") == "flash":
                patched_flash = host.native("%s/cdj-%s-flash.bin" % (nonempty(env, "LOGDIR", tmp), tag))
                self.main_patch = host.python_argv() + [self._patch_main(), "--flash", flash, patched_flash] + main_mods
                flash = patched_flash
            else:
                patched_main = host.native("%s/cdj-%s-main.bin" % (nonempty(env, "LOGDIR", tmp), tag))
                self.main_patch = host.python_argv() + [self._patch_main(), main_kernel, patched_main] + main_mods
                main_kernel = patched_main

        # MAIN_BOOT=flash: MAIN resets into Pioneer's bootloader in the flash
        # image instead of starting from main_unpacked.bin (the default, kernel).
        boot = [] if env.get("MAIN_BOOT") == "flash" else ["-kernel", main_kernel]
        self.main_argv = ([self.main_qemu, "-M", profile.main_machine] + boot
                          + ["-drive", "if=pflash,format=raw,file=%s,%s" % (flash, snap)] + media_args
                          + ["-chardev", "socket,id=spilink,path=%s,server=on,wait=off" % self.sock]
                          + icount + ["-nographic"] + audio + net + mon_args)

        # Display firmware mods are patched into a copy of the image, per run:
        # each patch_gui.py mod <name> is on when CDJ_GUI_<NAME>=1.
        gui_image = model_extract_n + "/gui_unpacked.bin"
        gui_mods = [m for m in self._gui_mod_names() if env.get("CDJ_GUI_" + m.upper()) == "1"]
        if gui_mods:
            patched = host.native("%s/cdj-%s-gui.bin" % (nonempty(env, "LOGDIR", tmp), tag))
            self.gui_patch = host.python_argv() + [self._patch_gui(), gui_image, patched] + gui_mods
            gui_image = patched

        self.gui_env = dict(env)
        display = self._display()
        self.gui_argv = [self.gui_qemu, "-M", profile.gui_machine, "-kernel", gui_image,
                         "-chardev", "socket,id=spilink,path=%s" % self.sock,
                         "-display", display, "-serial", "null", "-monitor", mon.spec(self.gui_mon)]

    def _patch_gui(self):
        return os.path.join(self.lay.emu, "mods", "patch_gui.py")

    def _gui_mod_names(self):
        out = subprocess.run(host.python_argv() + [self._patch_gui(), "--list"],
                             capture_output=True, text=True, check=True).stdout
        return [line.split()[0] for line in out.splitlines() if line.strip()]

    def _patch_main(self):
        return os.path.join(self.lay.emu, "mods", "patch_main.py")

    def _main_mod_names(self):
        out = subprocess.run(host.python_argv() + [self._patch_main(), "--list"],
                             capture_output=True, text=True, check=True).stdout
        return [line.split()[0] for line in out.splitlines() if line.strip()]

    def _mint_argv(self, playerno, serial, out):
        argv = host.python_argv() + [os.path.join(self.lay.scripts, "firmware", "player_flash.py"),
                                      playerno, out, os.path.join(self.lay.extract, "flash.bin")]
        return argv + ["--serial", serial] if serial else argv

    def _display(self):
        """GUI_DISPLAY: gtk (default; cocoa on macOS) shows the panel; none for
        a headless batch; vnc is the virtual deck app's screen."""
        env, tag = self.env, self.tag
        d = nonempty(env, "GUI_DISPLAY", "gtk")
        k = host.kind()
        if d == "vnc":
            # The virtual deck app: this deck's screen on a loopback VNC server
            # at CDJ_APP_VNC_BASE + its number (5921 for show1), and on a frame
            # file the app reads at the firmware's own frame rate
            # (CDJ_GUI_FRAME_FILE, sh7269gui.c); VNC then carries the touch
            # screen and the keyboard.
            port = int(nonempty(env, "CDJ_APP_VNC_BASE", "5920")) + int(self.n)
            self.gui_qemu = env.get("CDJ_APP_GUI_QEMU") or self.gui_qemu
            if not env.get("CDJ_APP_FRAME_DIR"):
                self.notes.append((2, "[%s] screen on VNC 127.0.0.1:%d for the virtual deck app" % (tag, port)))
                return "vnc=127.0.0.1:%d" % (port - 5900)
            d = env["CDJ_APP_FRAME_DIR"]
            self.gui_env["CDJ_GUI_FRAME_FILE"] = "%s/cdj-lcd-%s.bin" % (d, tag)
            # The port is only where QEMU starts looking: with to=, a port some
            # other program (or a leftover QEMU) took since the launcher looked
            # makes it take the next one instead of exiting. Which one it took
            # is read back from its monitor and published in the port file,
            # the only port the app connects to (watch_gui).
            self.vnc_port_file = "%s/cdj-vnc-%s.port" % (d, tag)
            last = port + self.VNC_SPAN - 1
            # A bind inside a range Windows reserves fails with something other
            # than "in use", which ends QEMU's search: stop short of one.
            for lo, _hi in sorted(host.windows_reserved_ranges("tcp")):
                if port < lo <= last:
                    last = lo - 1
                    break
            self.notes.append((2, "[%s] screen on VNC 127.0.0.1:%d (or the next free port to %d) and %s "
                               "for the virtual deck app" % (tag, port, last, self.gui_env["CDJ_GUI_FRAME_FILE"])))
            return "vnc=127.0.0.1:%d,to=%d" % (port - 5900, last - 5900)
        # Fall back to headless when there is no X or Wayland socket (WSLg can
        # lose its X server mid-session, and -display gtk then kills the GUI
        # QEMU). macOS has no GTK build: its window is Cocoa, and it needs the
        # logged-in desktop session (Aqua), which ssh does not have.
        if k == host.MACOS:
            if d == "gtk":
                d = "cocoa"
            if d in ("cocoa", "sdl") and _launchctl_manager() != "Aqua":
                self.notes.append((2, "[%s] no desktop session (ssh?) -- falling back to GUI_DISPLAY=none" % tag))
                d = "none"
        elif k != host.WINDOWS and d in ("gtk", "sdl"):
            wayland = os.path.join(os.environ.get("XDG_RUNTIME_DIR", "/nonexistent"),
                                   os.environ.get("WAYLAND_DISPLAY", "wayland-0"))
            if not (os.path.isdir("/tmp/.X11-unix") and os.listdir("/tmp/.X11-unix")) and not _is_socket(wayland):
                self.notes.append((2, "[%s] no X or Wayland socket -- falling back to GUI_DISPLAY=none" % tag))
                d = "none"
        # The touch screen makes the window an absolute pointer, and QEMU then
        # hides the host cursor for a guest that never draws one.
        if d != "none" and not d.startswith("vnc") and "show-cursor=" not in d:
            d += ",show-cursor=on"
        return d

    # ------------------------------------------------------------ running --

    def start(self):
        """Start both boards. False (after saying why) if MAIN never opened
        the link socket."""
        lay, tag, env = self.lay, self.tag, self.env
        for stream, line in self.notes:
            (chain.say if stream == 1 else chain.err)(line)
        if self.flash_copy:
            shutil.copyfile(*self.flash_copy)
            chain.say("[%s] made %s -- this deck now keeps its own settings" % (tag, self.flash_copy[1]))
        if self.mint:
            out = subprocess.run(self.mint, capture_output=True, text=True)
            for line in (out.stdout).splitlines():
                chain.say("[%s] %s" % (tag, line))
            if out.returncode != 0:
                sys.stderr.write(out.stderr)
                return False
            if nonempty(env, "PERSIST", "0") == "1":
                chain.say("[%s] made %s -- this deck now keeps its own settings" % (tag, self.persist_new))
        if self.gui_patch:
            out = subprocess.run(self.gui_patch, capture_output=True, text=True)
            for line in out.stdout.splitlines():
                chain.say("[%s] %s" % (tag, line))
            if out.returncode != 0:
                sys.stderr.write(out.stderr)
                chain.err("[%s] display firmware not patched" % tag)
                return False
        if self.main_patch:
            out = subprocess.run(self.main_patch, capture_output=True, text=True)
            for line in out.stdout.splitlines():
                chain.say("[%s] %s" % (tag, line))
            if out.returncode != 0:
                sys.stderr.write(out.stderr)
                chain.err("[%s] MAIN firmware not patched" % tag)
                return False
        mon = _monsock(lay)
        # A leftover QEMU on the same tag still holds the monitor address.
        if not mon.wait_free(self.gui_mon, 30):
            chain.err("[%s] ⚠ the GUI monitor address is still held -- an older run of this tag is alive" % tag)
        for p in (self.sock, env["CDJ_PANEL_KEYSOCK"], env["CDJ_VCLOCK_FILE"]):
            chain.remove(p)
        if self.main_mon:
            chain.remove(self.main_mon)
        if os.path.isdir(self.mediadir):
            for f in os.listdir(self.mediadir):
                if f.startswith("tmp") and f.endswith(".tmp"):
                    chain.remove(os.path.join(self.mediadir, f))
        if self.media_cache:
            src, dst = self.media_cache
            if not (os.path.exists(dst) and os.path.getmtime(dst) > os.path.getmtime(src)):
                shutil.copyfile(src, dst)
        if self.media_copy:
            shutil.copyfile(*self.media_copy)
        self.main = _spawn(self.main_argv, self.main_log, env)
        # Exists, not is-a-socket: on Windows the unix socket is a reparse point.
        for _ in range(100):
            if os.path.lexists(self.sock):
                break
            time.sleep(0.1)
        if not os.path.lexists(self.sock):
            chain.err("[%s] spilink socket never appeared" % tag)
            self.main.kill()
            return False
        self._spawn_gui()
        return True

    # A GUI QEMU that exits this soon never got going: most often its connect
    # to MAIN's link socket lost the race with MAIN's listen (the socket file
    # appears at bind, before listen), and QEMU gives up on a refused connect.
    GUI_EARLY_S = 15
    GUI_RESPAWNS = 3
    VNC_SPAN = 32               # ports the GUI QEMU may try for its VNC server

    def _spawn_gui(self):
        if self.vnc_port_file:
            chain.remove(self.vnc_port_file)
        self.vnc_port = None
        self.gui = _spawn(self.gui_argv, self.gui_log, self.gui_env)
        self.gui_started = time.time()

    def _publish_vnc_port(self):
        """Ask the GUI QEMU which port its VNC server took and write it where
        the app reads it. Quietly tries again next time while the monitor is
        not up yet."""
        try:
            out = _monsock(self.lay).command(self.gui_mon, "info vnc", settle=0.2, timeout=0.5)
        except OSError:
            return
        m = re.search(r"Server: \S+:(\d+) \(", out.decode("utf-8", "replace"))
        if not m:
            return
        self.vnc_port = int(m.group(1))
        tmp = self.vnc_port_file + ".new"
        with open(tmp, "w") as f:
            f.write("%d\n" % self.vnc_port)
        os.replace(tmp, self.vnc_port_file)
        chain.say("[%s] screen's VNC server is on 127.0.0.1:%d" % (self.tag, self.vnc_port))

    def watch_gui(self):
        """Start the GUI board again if it died early while MAIN runs on, and
        say why it died when it will not stay up: without it the deck has no
        screen, and the virtual deck app waits for one forever."""
        if self.gui.poll() is None:
            if self.vnc_port_file and self.vnc_port is None:
                self._publish_vnc_port()
            return
        if self.main.poll() is not None or self.gui_respawns is None:
            return
        if self.gui.returncode == 0:
            # Status 0 is a quit, not a crash: its window was closed (or quit
            # typed at its monitor). The deck goes with it, and so does the run.
            self.closed = True
            chain.say("[%s] the screen's window was closed; stopping" % self.tag)
            chain.request_stop()
            return
        tail = _tail(self.gui_log, 8)
        early = time.time() - self.gui_started < self.GUI_EARLY_S
        if early and self.gui_respawns < self.GUI_RESPAWNS:
            self.gui_respawns += 1
            chain.err("[%s] the GUI board exited at once (code %s); starting it again (%d/%d)"
                      % (self.tag, self.gui.returncode, self.gui_respawns, self.GUI_RESPAWNS))
            time.sleep(0.5 * self.gui_respawns)
            self._spawn_gui()
            return
        self.gui_respawns = None
        chain.err("[%s] ⚠ the GUI board exited (code %s); the deck has no screen. %s:"
                  % (self.tag, self.gui.returncode, host.posix(self.gui_log)))
        for line in tail or ["(empty log)"]:
            chain.err("[%s]   %s" % (self.tag, line))

    def running(self):
        return not self.closed and (self.main.poll() is None or self.gui.poll() is None)

    def stop(self):
        """The exit notifiers print the counters and write a JIT profile.
        SIGTERM runs them on Linux; on Windows it is TerminateProcess, so MAIN
        quits through its monitor first."""
        if self.main_mon and self.main.poll() is None:
            try:
                _monsock(self.lay).command(self.main_mon, "quit")
            except OSError:
                pass
            else:
                deadline = time.time() + 60
                while self.main.poll() is None and time.time() < deadline:
                    time.sleep(0.5)
        for p in (self.main, self.gui):
            if p.poll() is None:
                p.terminate()
        for p in (self.main, self.gui):
            try:
                p.wait(30)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
        chain.remove(self.sock)
        if self.vnc_port_file:
            chain.remove(self.vnc_port_file)
        if self.media_img:
            chain.remove(self.media_img)


def _spawn(argv, log, env):
    with open(log, "wb") as f:
        # Its own process group: a Ctrl-C reaches the launcher, which stops the
        # boards in order, instead of killing them where they stand.
        kw = {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP} if host.is_windows() \
            else {"start_new_session": True}
        return subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.STDOUT, env=env, **kw)


def _tail(path, n):
    try:
        with open(path, "rb") as f:
            lines = f.read()[-8192:].decode("utf-8", "replace").splitlines()
    except OSError:
        return []
    return [line for line in lines if line.strip()][-n:]


def _same_filesystem(src, tmp_dir):
    """True if src and tmp_dir already live on the same drive/filesystem, in
    which case a copy into tmp_dir buys nothing. Any stat failure answers
    False, which keeps the older, always-safe caching behaviour."""
    try:
        return os.stat(src).st_dev == os.stat(tmp_dir).st_dev
    except OSError:
        return False


def _launchctl_manager():
    try:
        return subprocess.run(["launchctl", "managername"], capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""


def _is_socket(path):
    import stat

    try:
        return stat.S_ISSOCK(os.stat(path).st_mode)
    except OSError:
        return False


def flash_has_bootloader(path):
    """An image from before the flash carried the MAIN section is erased at 0."""
    with open(path, "rb") as f:
        return f.read(2) != bytes([0xFF, 0xFF])


def main(argv):
    if not argv:
        chain.err("usage: boot_deck.sh <tag> [seconds]")
        return 1
    tag = argv[0]
    dur = int(argv[1]) if len(argv) > 1 else 60
    lay = Layout()
    try:
        profile = cdj_model.load()
    except cdj_model.ModelError as e:
        chain.err(str(e))
        return 1
    if not profile.gui_machine:
        chain.err("boot_deck.sh: %s has no GUI board yet; use scripts/run/boot_main.sh" % profile.title)
        return 1
    if os.environ.get("MAIN_BOOT") == "flash" and not flash_has_bootloader(os.path.join(lay.extract, "flash.bin")):
        chain.err("[%s] extract/flash.bin holds no bootloader (made by an older setup); run "
                  "./setup.sh --firmware <C2KNXS2.UPD> again, or leave MAIN_BOOT unset" % tag)
        return 1
    deck = Deck(tag, dict(os.environ), lay, profile)
    if not deck.start():
        return 1
    chain.say("[%s] main pid %d  gui pid %d  running free for %ds" % (tag, deck.main.pid, deck.gui.pid, dur))
    try:
        # An optional driver, run while the boards are up. It may press panel
        # keys and screendump through the monitor; it must NOT open either
        # gdbstub.
        if deck.env.get("DRIVER"):
            drv = subprocess.Popen(host.python_argv() + [deck.env["DRIVER"], tag], cwd=lay.root,
                                   env=dict(deck.env, PYTHONPATH=lay.run))
            while drv.poll() is None:
                if chain.stop_requested():
                    drv.terminate()
                deck.watch_gui()
                time.sleep(0.3)
        end = time.time() + dur
        parent = None if host.is_windows() else os.getppid()
        while time.time() < end and deck.running() and not chain.stop_requested(parent):
            deck.watch_gui()
            chain.sleep(min(1.0, end - time.time()), until=lambda: not deck.running())
    finally:
        deck.stop()
    chain.say("[%s] done -- %s %s" % (tag, host.posix(deck.main_log), host.posix(deck.gui_log)))
    return 0
