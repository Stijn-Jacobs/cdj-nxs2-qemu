# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise CDJ-850 USB/DMAC over QTest and check live USB and display flows.

MMIO and interrupt checks use a synthetic ROM. The live tests use prepared
firmware with disposable USB media and examine firmware state or the graphic
console through QMP.
"""
import json
import math
import os
import select
import selectors
import shutil
import socket
import struct
import subprocess
import time
from contextlib import contextmanager
from pathlib import Path

import pytest

from test_cdj850_panel_peer import sum8ec
from test_cdj850_scif1_replay import scif1_replay_exe


USB = 0xFFFC1C00
DMA = 0xFFFE1000


def _cdj850_binary():
    binary = os.environ.get("GUI_QEMU", os.path.expanduser(
        "~/qemu-build-eb/qemu-system-sh4eb"))
    if not os.path.isfile(binary):
        pytest.skip("qemu-system-sh4eb is not built")
    return binary


class Deck:
    def __init__(self, process, qmp, log):
        self.process, self.qmp, self.log = process, qmp, log

    def command(self, command):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()
        assert select.select([self.process.stdout], [], [], 5)[0], command
        reply = self.process.stdout.readline().strip()
        assert reply.startswith("OK"), (command, reply)
        return reply.split()[1:]

    def write(self, address, value, size=2):
        self.command("write%s %#x %#x" % ({1: "b", 2: "w", 4: "l"}[size],
                                          address, value))

    def read(self, address, size=2):
        return int(self.command("read%s %#x" %
                                ({1: "b", 2: "w", 4: "l"}[size], address))[0], 0)

    def execute(self, command):
        self.qmp.write((json.dumps({"execute": command}) + "\n").encode())
        self.qmp.flush()
        while True:
            result = json.loads(self.qmp.readline())
            if "return" in result:
                return result["return"]
            assert "error" not in result, result


@pytest.fixture
def deck(tmp_path):
    binary = _cdj850_binary()
    # VBR = SDRAM; SR.IMASK = 0; park. Only synthetic instructions/data.
    flash = bytearray(4 * 1024 * 1024)
    struct.pack_into(">II", flash, 0, 0x100, 0xFFF90000)
    struct.pack_into(">8HI", flash, 0x100,
                     0xD003, 0x402E, 0xE000, 0x400E,
                     0xAFFE, 0x0009, 0x0009, 0x0009, 0x0C000000)
    rom = tmp_path / "flash.bin"
    rom.write_bytes(flash)
    if os.name == "nt":
        pytest.skip("QMP socket descriptor inheritance requires POSIX")
    # Connected descriptors need no listening socket, network permissions,
    # or Unix socket path (macOS limits those to 104 characters).
    connection, child = socket.socketpair()
    with (tmp_path / "qemu.log").open("w+") as log:
        process = subprocess.Popen(
            [binary, "-M", "cdj850", "-S", "-display", "none",
             "-monitor", "none", "-serial", "none",
             "-drive", "if=pflash,format=raw,file=" + str(rom),
             "-qtest", "stdio", "-qtest-log", os.devnull,
             "-chardev", "socket,id=qmp,fd=%d" % child.fileno(),
             "-mon", "chardev=qmp,mode=control"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=log,
            text=True, bufsize=1, pass_fds=(child.fileno(),))
        child.close()
        try:
            connection.settimeout(5)
            qmp = connection.makefile("rwb")
            assert "QMP" in json.loads(qmp.readline())
            d = Deck(process, qmp, log)
            d.execute("qmp_capabilities")
            yield d
        finally:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=5)
            connection.close()


@pytest.mark.parametrize("select_off,count_off", [(0x24, 0x28), (0x2A, 0x2E)])
def test_transaction_clear_preserves_preset(deck, select_off, count_off):
    deck.write(USB + count_off, 8)
    deck.write(USB + select_off, 0x0101)  # TRCLR, CURPIPE = 1
    assert deck.read(USB + select_off) & 0x0100 == 0
    assert deck.read(USB + count_off) == 8


def arm_transmit(deck, channel=7):
    """One real DMA longword from SDRAM to USB's transmitting FIFO."""
    deck.write(USB + 0x64, 1)        # PIPESEL
    deck.write(USB + 0x66, 0x8011)   # bulk OUT endpoint 1
    deck.write(USB + 0x6A, 64)
    deck.write(USB + 0x24, 0x1801)   # DREQE, 32-bit, pipe 1
    deck.write(0x0C003000, 0x12345678, 4)
    deck.write(DMA + 0x300 + (channel // 2) * 4,
               0x0300 if channel & 1 else 0x0003)
    base = DMA + channel * 16
    deck.write(base, 0x0C003000, 4)
    deck.write(base + 4, USB + 0x14, 4)
    deck.write(base + 8, 1, 4)
    deck.write(base + 12, 0x1815, 4)
    deck.write(DMA + 0x200, 1)
    deck.write(USB + 0x70, 1)        # PID = BUF serves the DMA
    assert deck.read(base + 8, 4) == 0
    assert deck.read(base + 12, 4) & 2  # TE
    assert deck.read(base, 4) == 0x0C003004
    assert deck.read(base + 4, 4) == USB + 0x14


def test_dma_completion_delivers_when_priority_enabled_later(deck):
    # ISR: store a sentinel in SDRAM, clear CHCR, return. The transfer
    # completes with IPR07.DEI7 = 0, then is unmasked before the CPU starts.
    deck.write(0x0C000000 + 136 * 4, 0x0C001000, 4)
    words = [0xD104, 0xE001, 0x2102, 0xD104, 0xE000,
             0x2102, 0x002B, 0x0009, 0x0009, 0x0009]
    for i, word in enumerate(words):
        deck.write(0x0C001000 + 2 * i, word)
    deck.write(0x0C001014, 0x0C002000, 4)
    deck.write(0x0C001018, DMA + 0x7C, 4)
    arm_transmit(deck)
    deck.write(0xFFFE0C02, 7)       # IPR07.DEI7
    deck.execute("cont")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if deck.read(0x0C002000, 4) == 1:
            break
        time.sleep(0.005)
    else:
        pytest.fail("DEI7 was lost while its INTC priority was zero")
    deck.execute("stop")
    assert deck.read(DMA + 0x7C, 4) == 0


def test_dma_uncleared_completion_keeps_requesting_interrupts(deck):
    deck.write(0x0C000000 + 136 * 4, 0x0C001000, 4)
    # ISR increments a counter and returns without clearing the source.
    words = [0xD103, 0x6012, 0x7001, 0x2102,
             0x002B, 0x0009, 0x0009, 0x0009]
    for i, word in enumerate(words):
        deck.write(0x0C001000 + 2 * i, word)
    deck.write(0x0C001010, 0x0C002000, 4)
    deck.write(0xFFFE0C02, 7)
    arm_transmit(deck)
    deck.execute("cont")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if deck.read(0x0C002000, 4) >= 2:
            break
        time.sleep(0.005)
    else:
        pytest.fail("DEI7 stopped requesting while TE and IE remained set")
    deck.execute("stop")


def test_dma_longword_addressing_and_reserved_storage(deck):
    arm_transmit(deck, channel=0)
    # A reserved offset is storage, not a nonexistent channel 8. This
    # previously indexed past dei[8] into the DMA statistics array.
    deck.write(DMA + 0x80, 0xA5A55A5A, 4)
    assert deck.read(DMA + 0x80, 4) == 0xA5A55A5A
    deck.write(DMA + 0x8C, 0x00080002, 4)
    assert deck.read(DMA + 0x8C, 4) == 0x00080002


@pytest.mark.parametrize("ssi,channel,dmars", [(0, 1, 0x23), (1, 2, 0x27)])
def test_ssi_dma_transmit_requests(deck, ssi, channel, dmars):
    """SSI needs EN, DMEN and transmit mode; clocked slots drain the DMA."""
    port = 0xFFFFC000 + 0x800 * ssi
    base = DMA + channel * 16
    for i, value in enumerate((0x12345600, 0xFEDCBA00, 0x65432100)):
        deck.write(0x0C003000 + i * 4, value, 4)
    deck.write(DMA + 0x300 + (channel // 2) * 4,
               dmars << (8 if channel & 1 else 0))
    deck.write(base, 0x0C003000, 4)
    deck.write(base + 4, port + 8, 4)
    deck.write(base + 8, 3, 4)
    deck.write(base + 12, 0x1811, 4)
    deck.write(DMA + 0x200, 1)
    deck.write(port, 0x10000001, 4)  # Receive mode must not request TX DMA.
    deck.execute("cont")
    time.sleep(0.01)
    deck.execute("stop")
    assert deck.read(base + 8, 4) == 3
    deck.write(port, 0x10000003, 4)
    deck.execute("cont")
    deadline = time.monotonic() + 3
    while time.monotonic() < deadline:
        if deck.read(base + 8, 4) == 0:
            break
        time.sleep(0.005)
    else:
        pytest.fail("SSI transmit slots did not drain the armed DMA")
    deck.execute("stop")
    assert deck.read(base + 12, 4) & 2
    assert deck.read(base, 4) == 0x0C00300C
    assert deck.read(port + 8, 4) == 0x65432100


def _make_pcm_wav(seconds=10, rate=44100):
    """16-bit stereo PCM: 440 Hz left, 660 Hz right, about -8.7 dBFS.

    Non-silent and long enough that playback does not hit end-of-track
    at once, so an audio-path check can tell real samples from silence.
    Both tones repeat exactly every second, so one second is tiled.
    """
    second = b"".join(
        struct.pack("<hh",
                    int(12000 * math.sin(2 * math.pi * 440 * i / rate)),
                    int(12000 * math.sin(2 * math.pi * 660 * i / rate)))
        for i in range(rate))
    data = second * seconds
    hdr = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF", 36 + len(data), b"WAVE",
        b"fmt ", 16, 1, 2, rate, rate * 4, 4, 16,
        b"data", len(data),
    )
    return hdr + data


def test_pcm_wav_fixture_is_audible():
    """The USB fixture track must be non-silent and longer than a blip."""
    wav = _make_pcm_wav()
    assert wav[:4] == b"RIFF" and wav[8:16] == b"WAVEfmt "
    (data_len,) = struct.unpack_from("<I", wav, 40)
    assert data_len == len(wav) - 44 == 10 * 44100 * 4
    samples = struct.unpack_from("<2000h", wav, 44)
    assert max(samples) > 10000 and min(samples) < -10000


# MAIN player state for the USB deck (0x0CE6D25D + source index 2). After a
# load it is 3 = playing (auto-play), 5 = paused/braking, 4 = paused in CDJ
# mode; the audio command and audio-task state follow (1/3 play, 3/4 pause).
PLAYER_STATE = 0x0CE6D25F
SPEED = 0x0CE6C940          # speed sent to the DSP, 0x080000 = 1.0x
READ_INDEX = 0x0C7F645C     # sector index the IRQ7 feeder reads next


def _qmp_execute(qmp, command, **arguments):
    qmp.write(json.dumps({"execute": command, "arguments": arguments}).encode() + b"\n")
    while True:
        line = qmp.readline()
        assert line, f"QMP disconnected during {command}"
        result = json.loads(line)
        assert "error" not in result, (command, result)
        if "return" in result:
            return result["return"]


def _qmp_read(qmp, addr, unit="b"):
    """Read one guest-physical byte/halfword/word through HMP `xp` over QMP."""
    value = _qmp_execute(qmp, "human-monitor-command",
                        **{"command-line": f"xp /1{unit}x {addr:#x}"})
    return int(value.split(":")[-1].strip(), 16)


def _fat16_lfn_entry(name, short_name_11, order=0x41):
    csum = 0
    for b in short_name_11:
        csum = (((csum & 1) << 7) + (csum >> 1) + b) & 0xFF
    u16 = [ord(ch) for ch in name] + [0]
    u16.extend([0xFFFF] * (13 - len(u16)))
    return struct.pack(
        "<B10sBBB12sH4s",
        order,
        b"".join(struct.pack("<H", c) for c in u16[:5]),
        0x0F, 0, csum,
        b"".join(struct.pack("<H", c) for c in u16[5:11]),
        0,
        b"".join(struct.pack("<H", c) for c in u16[11:13]),
    )


def _build_fat16_usb_image(img_path, track_names, track_seconds=10):
    bps, spc, res, fats, roots, spf = 512, 4, 1, 2, 512, 64
    part_lba, part_secs, total_secs = 63, 65473, 65536
    cluster_bytes = bps * spc
    data_lba = part_lba + res + fats * spf + (roots * 32) // bps

    img = bytearray(total_secs * bps)
    # MBR partition entry 1 (FAT16B, type 0x06)
    struct.pack_into("<B3sB3sII", img, 446, 0x80, b"\x01\x01\x00", 0x06,
                     b"\xfe\xff\xff", part_lba, part_secs)
    struct.pack_into("<H", img, 510, 0xAA55)

    # FAT16 VBR
    vbr_off = part_lba * bps
    struct.pack_into("<3s8sHBHBHHBHHHII", img, vbr_off,
                     b"\xeb\x3c\x90", b"MSDOS5.0", bps, spc, res, fats, roots,
                     part_secs, 0xF8, spf, 63, 255, part_lba, 0)
    struct.pack_into("<BBBI11s8s", img, vbr_off + 36,
                     0x80, 0, 0x29, 0x12345678, b"CDJ        ", b"FAT16   ")
    struct.pack_into("<H", img, vbr_off + 510, 0xAA55)

    fat = [0] * (spf * bps // 2)
    fat[0] = 0xFFF8
    fat[1] = 0xFFFF

    root_off = (part_lba + res + fats * spf) * bps
    # Root entry 0: volume label
    struct.pack_into("<11sB10sHHHI", img, root_off,
                     b"CDJ        ", 0x08, b"\x00" * 10, 0, 0, 0, 0)
    ent_off = root_off + 32
    next_clus = 2
    wav_blob = _make_pcm_wav(seconds=track_seconds)

    for name in track_names:
        stem, ext = name.rsplit(".", 1)
        assert len(name) <= 12 and len(stem) <= 8 and len(ext) <= 3
        short11 = f"{stem.upper():<8}{ext.upper():<3}".encode("ascii")
        nclus = (len(wav_blob) + cluster_bytes - 1) // cluster_bytes
        first_clus = next_clus
        for c in range(first_clus, first_clus + nclus):
            fat[c] = 0xFFFF if c == first_clus + nclus - 1 else c + 1
        data_off = (data_lba + (first_clus - 2) * spc) * bps
        img[data_off:data_off + len(wav_blob)] = wav_blob
        next_clus += nclus

        lfn = _fat16_lfn_entry(name, short11)
        img[ent_off:ent_off + 32] = lfn
        ent_off += 32
        struct.pack_into("<11sB10sHHHI", img, ent_off,
                         short11, 0x20, b"\x00" * 10, 0, 0, first_clus, len(wav_blob))
        ent_off += 32

    fat_bytes = struct.pack(f"<{len(fat)}H", *fat)
    for fi in range(fats):
        foff = (part_lba + res + fi * spf) * bps
        img[foff:foff + len(fat_bytes)] = fat_bytes

    img_path.write_bytes(img)


@contextmanager
def _live_usb_deck(tmp_path, panel_script, capture=None,
                   track_names=("ALPHA.wav", "BETA.wav", "GAMMA.wav"),
                   track_seconds=10, *, usb_image=None):
    binary = _cdj850_binary()
    flash = Path(__file__).resolve().parent.parent / "extract" / "cdj850" / "flash_cdj850.bin"
    if not flash.is_file():
        pytest.skip("extract/cdj850/flash_cdj850.bin is not present")
    if os.name == "nt":
        pytest.skip("QMP socket descriptor inheritance requires POSIX")

    usb_img = tmp_path / "usb.img"
    if usb_image is None:
        _build_fat16_usb_image(usb_img, track_names, track_seconds)
    else:
        shutil.copyfile(usb_image, usb_img)

    env = os.environ.copy()
    for k in ("CDJ850_DSP_BOOT_HLE", "CDJ850_PF16_HIGH", "CDJ850_DSP_PLAY_PF"):
        env.pop(k, None)
    # Run under -icount: virtual time is MAIN's instruction count, so the
    # scripted presses land at the same point in the firmware every run.
    # Without it the virtual clock follows the host and MAIN mounts the stick
    # anywhere between ~3 s and ~27 s, racing a fixed `usb` press (the old
    # flake). Under icount the stick is mounted at 2.9 s.
    env["CDJ850_PANEL_SCRIPT"] = panel_script
    env.pop("CDJ850_SCIF1_CAPTURE", None)
    if capture is not None:
        env["CDJ850_SCIF1_CAPTURE"] = str(capture)

    qmp_s, qmp_c = socket.socketpair()
    with qmp_s, qmp_c:
        p = subprocess.Popen(
            [
                binary, "-M", "cdj850",
                "-display", "none", "-monitor", "none", "-serial", "none",
                "-icount", "shift=2,sleep=off",
                "-drive", f"if=pflash,format=raw,file={flash},snapshot=on",
                "-drive", f"if=none,id=usbstk,format=raw,file={usb_img},snapshot=on",
                "-device", "usb-storage,drive=usbstk,port=1",
                "-chardev", f"socket,id=qmp,fd={qmp_c.fileno()}",
                "-mon", "chardev=qmp,mode=control",
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
            pass_fds=(qmp_c.fileno(),),
            env=env,
        )
        qmp_c.close()
        qmp_s.settimeout(15)
        with qmp_s.makefile("rwb", buffering=0) as qmp, p.stderr:
            try:
                greeting = qmp.readline()
                if not greeting and p.poll() is not None:
                    pytest.fail(p.stderr.read().decode(errors="replace"))
                assert greeting, "QMP disconnected before greeting"
                assert "QMP" in json.loads(greeting), greeting
                _qmp_execute(qmp, "qmp_capabilities")
                yield p, qmp
            finally:
                try:
                    qmp.write(b'{"execute":"quit"}\n')
                    p.wait(timeout=5)
                except (OSError, subprocess.TimeoutExpired):
                    p.kill()
                    p.wait(timeout=5)


def test_live_usb_source_select_browse_load_and_play(tmp_path, scif1_replay_exe):
    # The deck plays a track as soon as it is loaded. In VINYL mode (the
    # factory default) PLAY then brakes to a stop and PLAY again restarts.
    script = ("6000:usb:200,9000:enter:200,11000:rot+1,13000:enter:200,"
              "19000:play:200,24000:play:200")
    capture = tmp_path / "usb-playback.scif1"
    display_lines = []
    names = ["browse root", "file list", "cursor moved", "BETA.wav loaded",
             "playing after load", "PLAY braked to a stop", "PLAY restarted"]
    with _live_usb_deck(tmp_path, script, capture) as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180.0
        buf = ""
        milestones = [False] * len(names)
        presses = 0
        first_index = None
        eof = False
        while time.monotonic() < deadline and not all(milestones) and not eof:
            for key, _ in sel.select(timeout=0.3):
                chunk = os.read(key.fileobj.fileno(), 4096).decode(errors="replace")
                if not chunk:
                    eof = True
                    break
                buf += chunk
                while "\n" in buf:
                    ln, buf = buf.split("\n", 1)
                    if "cdj850-panel:" not in ln:
                        continue
                    display_lines.append(ln)
                    if "input press play" in ln:
                        presses += 1
                    if not milestones[0] and "[.FOLDER.] | [] | []" in ln:
                        milestones[0] = True
                    elif (milestones[0] and not milestones[1] and
                          "tag=25/02/03" in ln and
                          "[ALPHA.wav] | [BETA.wav] | [GAMMA.wav]" in ln):
                        milestones[1] = True
                    elif (milestones[1] and not milestones[2] and
                          "tag=01/26/03" in ln and
                          "[ALPHA.wav] | [BETA.wav] | [GAMMA.wav]" in ln):
                        milestones[2] = True
                    elif (milestones[2] and not milestones[3] and
                          "mode=0x0a" in ln and "[BETA.wav] | [] | []" in ln):
                        milestones[3] = True
            if not milestones[3]:
                continue
            state = _qmp_read(qmp, PLAYER_STATE)
            if presses == 0 and not milestones[4] and state == 3:
                index = _qmp_read(qmp, READ_INDEX, "h")
                if first_index is None:
                    first_index = index
                elif index > first_index + 4:
                    milestones[4] = True      # the read index advances
            elif presses == 1 and milestones[4] and not milestones[5]:
                milestones[5] = state == 5 and _qmp_read(qmp, SPEED, "w") == 0
            elif presses >= 2 and milestones[5] and not milestones[6]:
                milestones[6] = (state == 3 and
                                 _qmp_read(qmp, SPEED, "w") == 0x080000)
        missing = [n for n, ok in zip(names, milestones) if not ok]
        assert not missing, (
            f"Missing milestones {missing}; display log:\n" +
            "\n".join(display_lines)
        )

        # Freeze the guest and drain the diagnostics corresponding to any
        # transactions recorded while the last QMP replies were in flight.
        _qmp_execute(qmp, "stop")
        while sel.select(timeout=0):
            chunk = os.read(p.stderr.fileno(), 4096)
            if not chunk:
                break
            buf += chunk.decode(errors="replace")
        display_lines.extend(buf.splitlines())

    prefix = "cdj850-panel: display "
    live = "\n".join(line[line.index(prefix):] for line in display_lines
                     if prefix in line) + "\n"
    (tmp_path / "live-display.log").write_text(live, encoding="ascii")
    replay = subprocess.run([str(scif1_replay_exe), str(capture)],
                            capture_output=True, text=True, timeout=30, check=True)
    (tmp_path / "replayed-display.log").write_text(replay.stdout, encoding="ascii")
    assert replay.stderr == ""
    assert replay.stdout == live


def _screen_dots(qmp, path):
    """Sample the centre of each DATA FL dot in a QMP screenshot."""
    _qmp_execute(qmp, "stop")
    _qmp_execute(qmp, "screendump", filename=str(path))
    _qmp_execute(qmp, "cont")
    pixels = _ppm_pixels(path)
    blue = b"\x9f\xe8\xff"
    off = b"\x0a\x12\x1b"
    dots = []
    for y in range(27):
        row = []
        for x in range(100):
            offset = ((166 + 8 * y) * 1280 + 176 + 9 * x) * 3
            pixel = pixels[offset:offset + 3]
            assert pixel in (blue, off), (x, y, pixel)
            row.append(pixel == blue)
        dots.append(row)
    return dots


def test_live_usb_display_screendump(tmp_path):
    script = "6000:usb:200,9000:enter:200,11000:rot+1,13000:enter:200"
    files = "[ALPHA.wav] | [BETA.wav] | [GAMMA.wav]"
    stages = [
        ("no-disc", ("[NO DISC]",)),
        ("usb-root", ("[.FOLDER.] | [] | []",)),
        ("browser", ("tag=25/02/03", files)),
        ("cursor", ("tag=01/26/03", files)),
        ("loaded", ("mode=0x0a", "[BETA.wav] | [] | []")),
    ]
    screens = {}
    display_lines = []
    with _live_usb_deck(tmp_path, script) as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        buf = b""
        eof = False
        while time.monotonic() < deadline and len(screens) < len(stages) and not eof:
            for key, _ in sel.select(timeout=0.3):
                chunk = os.read(key.fileobj.fileno(), 4096)
                if not chunk:
                    eof = True
                    break
                buf += chunk
                while b"\n" in buf and len(screens) < len(stages):
                    line, buf = buf.split(b"\n", 1)
                    if b"cdj850-panel: display mode=" not in line:
                        continue
                    text = line.decode(errors="replace")
                    display_lines.append(text)
                    name, required = stages[len(screens)]
                    if all(value in text for value in required):
                        screens[name] = _screen_dots(qmp, tmp_path / f"{name}.ppm")

    assert len(screens) == len(stages), (
        f"Missing {stages[len(screens)][0]}; display log:\n" + "\n".join(display_lines)
    )
    idle, root, browser, cursor, loaded = [screens[name] for name, _ in stages]
    assert any(any(row[5:]) for row in idle[10:17])
    assert not any(any(row[5:]) for row in idle[:10] + idle[17:])
    assert root != idle
    # Each visible row has both lit dots and glyph cutouts / inter-letter gaps.
    for band in range(3):
        cells = [dot for row in browser[band * 9 + 1:band * 9 + 8] for dot in row[7:45]]
        assert any(cells) and not all(cells), f"missing browser row {band + 1}"
    assert (browser[4][99], browser[13][99]) == (True, False)
    assert (cursor[4][99], cursor[13][99]) == (False, True)
    assert cursor[18:] == browser[18:]
    assert loaded[4][99]
    # Compare the inverted BETA.wav glyphs with the selected browser row.
    assert [row[8:58] for row in loaded[1:8]] == [row[7:57] for row in cursor[10:17]]
    assert not any(any(row) for row in loaded[9:])


def _ppm_pixels(ppm):
    magic, size, maxval, pixels = ppm.read_bytes().split(b"\n", 3)
    assert (magic, size, maxval) == (b"P6", b"1280 480", b"255")
    assert len(pixels) == 1280 * 480 * 3
    return pixels


def _track_pixels(pixels):
    return tuple(pixels[(y * 1280 + 32) * 3:(y * 1280 + 150) * 3]
                 for y in range(38, 128))


def _seven_segment_masks(pixels, origins, *, y_origin=40,
                         points=((24, 3), (42, 24), (41, 63), (22, 82),
                                 (5, 63), (6, 24), (24, 43))):
    # Sample each segment well inside its silhouette, clockwise from top.
    blue = b"\x9f\xe8\xff"
    return tuple(sum(1 << i for i, (x, y) in enumerate(points)
                     if pixels[((y + y_origin) * 1280 + x + left) * 3:
                               ((y + y_origin) * 1280 + x + left) * 3 + 3] == blue)
                 for left in origins)


def _capture_replay_snapshot(qmp, tmp_path, name, capture, replay_exe):
    _qmp_execute(qmp, "stop")
    screen = tmp_path / f"{name}-live.ppm"
    _qmp_execute(qmp, "screendump", filename=str(screen))
    prefix = tmp_path / f"{name}.scif1"
    prefix.write_bytes(capture.read_bytes())
    snapshot = tmp_path / f"{name}-replay.ppm"
    result = subprocess.run([str(replay_exe), str(prefix), "--ppm", str(snapshot)],
                            capture_output=True, text=True, check=True, timeout=30)
    assert result.stderr == ""
    return _ppm_pixels(screen), _ppm_pixels(snapshot), prefix


def test_live_scif1_track_number_matches_replay(tmp_path, scif1_replay_exe):
    script = ("6000:usb:200,9000:enter:200,11000:rot+9,13000:enter:200,"
              "16000:next:200,22000:usb_stop:1500")
    capture = tmp_path / "track.scif1"
    stages = [
        ("no-disc", "[NO DISC]", (0x3F, 0x3F)),
        ("track-10", "[TRACK10.wav] | [] | []", (0x06, 0x3F)),
        ("track-11", "[TRACK11.wav] | [] | []", (0x06, 0x06)),
        ("unloaded", "track=00", (0x3F, 0x3F)),
        ("reset", "track=--", (0, 0)),
    ]
    names = [f"TRACK{i:02d}.wav" for i in range(1, 13)]
    observed = []
    stage = 0
    with _live_usb_deck(tmp_path, script, capture, names) as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        buf = b""
        while time.monotonic() < deadline and stage < len(stages):
            if not sel.select(timeout=0.3):
                continue
            chunk = os.read(p.stderr.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf and stage < len(stages):
                line, buf = buf.split(b"\n", 1)
                if b"cdj850-panel: display " not in line:
                    continue
                text = line.decode(errors="replace")
                observed.append(text)
                name, trigger, expected = stages[stage]
                if trigger not in text:
                    continue
                live_pixels, replay_pixels, _ = _capture_replay_snapshot(
                    qmp, tmp_path, name, capture, scif1_replay_exe)
                assert _seven_segment_masks(live_pixels, (35, 101)) == expected, name
                assert _track_pixels(replay_pixels) == _track_pixels(live_pixels), name
                stage += 1

                if name == "unloaded":
                    _qmp_execute(qmp, "system_reset")
                elif stage < len(stages):
                    _qmp_execute(qmp, "cont")

    assert stage == len(stages), (
        f"Missing {stages[stage][0]}; display log:\n" + "\n".join(observed)
    )


def _captured_status_pages(prefix):
    """Read valid status frames independently of the replay executable."""
    reset_ns = -1
    for record in prefix.read_text().splitlines()[1:]:
        cols = record.split()
        if cols[0] == "R":
            reset_ns = int(cols[1])
            yield None
        elif int(cols[2]) > reset_ns and int(cols[4]) >= 2:
            frame = bytes.fromhex(cols[5].split(":")[1])
            if sum8ec(frame[:15]) == frame[15]:
                yield frame


def _capture_time_fields(prefix):
    msf = remain = None
    for frame in _captured_status_pages(prefix):
        if frame is None:
            msf = remain = None
        elif frame[0] == 1:
            m, s, f = frame[5:8]
            msf = (m, s, f) if m < 100 and s < 60 and f < 75 else None
        elif frame[0] == 2:
            remain = bool(frame[13] & 1)
    return msf, remain


def test_live_scif1_time_matches_replay(tmp_path, scif1_replay_exe):
    script = ("6000:usb:200,9000:enter:200,13000:enter:200,15500:time:200,"
              "19000:time:200,22000:usb_stop:1500")
    capture = tmp_path / "time.scif1"
    stages = [
        ("idle", ("[NO DISC]",), (0, 0, 0), True),
        ("remaining", ("time=01:05:74", "remain=1"), (1,), True),
        ("elapsed", ("time=00:", "remain=0"), (0,), False),
        ("remaining-again", ("time=01:", "remain=1"), (1,), True),
        ("unloaded", ("time=00:00:00", "remain=1"), (0, 0, 0), True),
        ("reset", ("time=--:--:--", "remain=?"), None, None),
    ]
    digit_masks = (0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F)
    observed = []
    stage = 0
    deck = _live_usb_deck(tmp_path, script, capture, ("LONG.wav",), track_seconds=66)
    with deck as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        buf = b""
        while time.monotonic() < deadline and stage < len(stages):
            if not sel.select(timeout=0.3):
                continue
            chunk = os.read(p.stderr.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf and stage < len(stages):
                line, buf = buf.split(b"\n", 1)
                if b"cdj850-panel: display " not in line:
                    continue
                text = line.decode(errors="replace")
                observed.append(text)
                name, trigger, time_prefix, expect_remain = stages[stage]
                if not all(value in text for value in trigger):
                    continue
                live, replay, prefix = _capture_replay_snapshot(
                    qmp, tmp_path, name, capture, scif1_replay_exe)
                msf, remain = _capture_time_fields(prefix)
                assert remain is expect_remain, name
                if time_prefix is None:
                    assert msf is None, name
                    expected = (0,) * 6
                else:
                    assert msf is not None and msf[:len(time_prefix)] == time_prefix, name
                    digits = "".join(f"{part:02d}" for part in msf)
                    expected = tuple(digit_masks[int(digit)] for digit in digits)
                assert _seven_segment_masks(live, (316, 381, 479, 542, 632, 695)) == expected, name
                remain_lit = any(live[(y * 1280 + x) * 3 + 2] > 128
                                 for y in range(40, 64) for x in range(157, 301))
                assert remain_lit == bool(remain), name
                assert live == replay, name
                stage += 1
                # Budget each milestone separately because the live firmware
                # can advance much more slowly than wall time under icount.
                deadline = time.monotonic() + 180
                if name == "unloaded":
                    _qmp_execute(qmp, "system_reset")
                elif stage < len(stages):
                    _qmp_execute(qmp, "cont")

    assert stage == len(stages), (
        f"Missing {stages[stage][0]}; display log:\n" + "\n".join(observed)
    )


def test_live_scif1_bpm_matches_replay(tmp_path, scif1_replay_exe):
    image = os.environ.get("CDJ850_BPM_TEST_IMAGE")
    if not image:
        pytest.skip("set CDJ850_BPM_TEST_IMAGE to the GRID128/GRID96 rekordbox export")
    script = ("6000:usb:200,7600:rot+2,9000:enter:200,11500:enter:200,"
              "17000:next:200,22500:usb_stop:1500")
    capture = tmp_path / "bpm.scif1"
    stages = [
        ("idle", ("[NO DISC]",), 0xFFFF, (0, 0, 0)),
        ("bpm-128", ("mode=0x0a", "[GRID128]"), 128, (0x06, 0x5B, 0x7F)),
        ("bpm-96", ("mode=0x0a", "[GRID96]"), 96, (0, 0x6F, 0x7D)),
        ("unloaded", ("[NO USB]",), 0xFFFF, (0, 0, 0)),
    ]
    observed = []
    stage = 0
    deck = _live_usb_deck(tmp_path, script, capture, usb_image=image)
    with deck as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        buf = b""
        while time.monotonic() < deadline and stage < len(stages):
            if not sel.select(timeout=0.3):
                continue
            chunk = os.read(p.stderr.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf and stage < len(stages):
                line, buf = buf.split(b"\n", 1)
                if b"cdj850-panel: display mode=" not in line:
                    continue
                text = line.decode(errors="replace")
                observed.append(text)
                name, trigger, raw_bpm, masks = stages[stage]
                if not all(value in text for value in trigger):
                    continue
                live, replay, prefix = _capture_replay_snapshot(
                    qmp, tmp_path, name, capture, scif1_replay_exe)
                bpm_words = [int.from_bytes(frame[8:10], "big")
                             for frame in _captured_status_pages(prefix)
                             if frame is not None and frame[0] == 1]
                assert bpm_words[-1] == raw_bpm, name
                bpm_points = ((20, 3), (34, 17), (33, 48), (18, 61),
                              (4, 48), (5, 17), (19, 32))
                assert _seven_segment_masks(live, (1061, 1107, 1153), y_origin=47,
                                            points=bpm_points) == masks, name
                assert live == replay, name
                stage += 1
                if stage < len(stages):
                    deadline = time.monotonic() + 180
                    _qmp_execute(qmp, "cont")

    assert stage == len(stages), (
        f"Missing {stages[stage][0]}; display log:\n" + "\n".join(observed)
    )


def _assert_pitch_pixels(pixels, masks, sign, range_label, *, leading=False):
    points = ((22, 3), (36, 22), (34, 52), (18, 68),
              (4, 52), (6, 22), (20, 35))
    assert _seven_segment_masks(pixels, (855, 910, 965), y_origin=50,
                                points=points) == masks

    def rgb(x, y):
        offset = (y * 1280 + x) * 3
        return pixels[offset:offset + 3]

    blue, off = b"\x9f\xe8\xff", b"\x0a\x12\x1b"
    red, green = b"\xff\x58\x4c", b"\x68\xe4\xa5"
    for x, y, lit in (
        (813, 84, bool(sign)), (813, 73, sign > 0), (837, 65, leading),
        (906, 119, range_label != "WIDE"), (960, 119, range_label == "WIDE"),
        (1012, 99, True),
    ):
        assert rgb(x, y) == (blue if lit else off), (x, y)
    for label, y, color in (("16", 273, blue), ("10", 337, red), ("6", 402, green)):
        assert rgb(1115, y) == (color if range_label == label else off), label
    wide_lit = any(rgb(x, y) == red for x in range(1110, 1210) for y in range(227, 262))
    assert wide_lit == (range_label == "WIDE")


def test_live_scif1_pitch_matches_replay(tmp_path, scif1_replay_exe):
    script = ("6000:usb:200,9000:enter:200,11500:enter:200,"
              "14000:fader=256,16500:tempo:200,19500:tempo:200,"
              "22500:fader=0,25000:fader=1023,28000:fader=512")
    capture = tmp_path / "pitch.scif1"
    stages = [
        ("center", "0.00", "10", (0, 0x10, 0x42, 0), (0x3F, 0x3F, 0x3F)),
        ("positive", "+5.10", "10", (3, 0x10, 0x42, 510), (0x6D, 0x06, 0x3F)),
        ("range-16", "+8.15", "16", (3, 0x10, 0x22, 815), (0x7F, 0x06, 0x6D)),
        ("range-6", "+3.06", "6", (3, 0x10, 0x82, 306), (0x4F, 0x3F, 0x7D)),
        ("positive-limit", "+6.00", "6", (3, 0x10, 0x82, 600), (0x7D, 0x3F, 0x3F)),
        ("negative-limit", "-6.00", "6", (2, 0x10, 0x82, 600), (0x7D, 0x3F, 0x3F)),
        ("recenter", "0.00", "6", (0, 0x10, 0x82, 0), (0x3F, 0x3F, 0x3F)),
    ]
    _check_live_pitch(tmp_path, scif1_replay_exe, script, capture, stages)


def test_live_scif1_wide_pitch_matches_replay(tmp_path, scif1_replay_exe):
    # The firmware offers WIDE with DISC selected and no USB file loaded.
    script = "2500:tempo:200,4000:tempo:200,5200:fader=0,6200:fader=1023"
    capture = tmp_path / "wide-pitch.scif1"
    stages = [
        ("wide-center", "0.0", "WIDE", (0, 0x20, 6, 0), (0, 0x3F, 0x3F)),
        ("wide-positive", "+100.0", "WIDE", (3, 0x20, 6, 1000), (0x3F,) * 3),
        ("wide-negative", "-100.0", "WIDE", (2, 0x20, 6, 1000), (0x3F,) * 3),
    ]
    _check_live_pitch(tmp_path, scif1_replay_exe, script, capture, stages)


def _check_live_pitch(tmp_path, scif1_replay_exe, script, capture, stages):
    observed = []
    stage = 0
    deck = _live_usb_deck(tmp_path, script, capture, ("PITCH.wav",), track_seconds=30)
    with deck as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        buf = b""
        while time.monotonic() < deadline and stage < len(stages):
            if not sel.select(timeout=0.3):
                continue
            chunk = os.read(p.stderr.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf and stage < len(stages):
                line, buf = buf.split(b"\n", 1)
                if b"cdj850-panel: display pitch=" not in line:
                    continue
                text = line.decode(errors="replace")
                observed.append(text)
                name, value, range_label, expected, masks = stages[stage]
                if f"pitch={value} range={range_label} (" not in text:
                    continue
                live, replay, prefix = _capture_replay_snapshot(
                    qmp, tmp_path, name, capture, scif1_replay_exe)
                pages = list(_captured_status_pages(prefix))
                magnitude = next(frame for frame in reversed(pages) if frame[0] == 1)
                flags = next(frame for frame in reversed(pages) if frame[0] == 2)
                assert (flags[11] & 3, flags[12] & 0x30, flags[14] & 0xE6,
                        int.from_bytes(magnitude[10:12], "big")) == expected, name
                sign = 1 if value.startswith("+") else -1 if value.startswith("-") else 0
                _assert_pitch_pixels(live, masks, sign, range_label, leading=expected[3] >= 1000)
                assert live == replay, name
                stage += 1
                if stage < len(stages):
                    deadline = time.monotonic() + 180
                    _qmp_execute(qmp, "cont")

    assert stage == len(stages), (
        f"Missing {stages[stage][0]}; display log:\n" + "\n".join(observed)
    )


def test_live_scif1_master_tempo_matches_replay(tmp_path, scif1_replay_exe):
    script = "2500:master_tempo:200,5400:master_tempo:200"
    capture = tmp_path / "master-tempo.scif1"
    stages = [
        ("initial-off", "[NO DISC]", False),
        ("on", "display master_tempo=1", True),
        ("off", "display master_tempo=0", False),
    ]
    stage = 0
    observed = []
    with _live_usb_deck(tmp_path, script, capture) as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        buf = b""
        while time.monotonic() < deadline and stage < len(stages):
            if not sel.select(timeout=0.3):
                continue
            chunk = os.read(p.stderr.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf and stage < len(stages):
                line, buf = buf.split(b"\n", 1)
                if b"cdj850-panel: display " not in line:
                    continue
                message = line.decode(errors="replace")
                observed.append(message)
                name, trigger, enabled = stages[stage]
                if trigger not in message:
                    continue

                live, replay, prefix = _capture_replay_snapshot(
                    qmp, tmp_path, name, capture, scif1_replay_exe)
                pages = list(_captured_status_pages(prefix))
                page = next(frame for frame in reversed(pages) if frame[0] == 2)
                assert bool(page[11] & 0x80) == enabled, name

                # Check the top edge and pixels in the traced MT legend separately.
                red, off = b"\xff\x58\x4c", b"\x0a\x12\x1b"
                border = (164 * 1280 + 1159) * 3
                assert live[border:border + 3] == (red if enabled else off), name
                label_rows = (live[(y * 1280 + 1122) * 3:(y * 1280 + 1196) * 3]
                              for y in range(172, 199))
                assert any(red in row for row in label_rows) == enabled, name
                assert live == replay, name
                stage += 1
                if stage < len(stages):
                    deadline = time.monotonic() + 180
                    _qmp_execute(qmp, "cont")

    assert stage == len(stages), (
        f"Missing {stages[stage][0]}; display log:\n" + "\n".join(observed)
    )


def test_live_scif1_auto_cue_matches_replay(tmp_path, scif1_replay_exe):
    script = "2200:time:200,3300:time:1600,6100:time:1600"
    capture = tmp_path / "auto-cue.scif1"
    stages = [
        ("initial", ("[NO DISC]",), False, True),
        ("short-press", ("display time=", "remain=0"), False, False),
        ("auto-cue-on", ("display auto_cue=1",), True, False),
        ("auto-cue-off", ("display auto_cue=0",), False, False),
    ]
    stage = 0
    observed = []
    with _live_usb_deck(tmp_path, script, capture) as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        buf = b""
        while time.monotonic() < deadline and stage < len(stages):
            if not sel.select(timeout=0.3):
                continue
            chunk = os.read(p.stderr.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf and stage < len(stages):
                line, buf = buf.split(b"\n", 1)
                if b"cdj850-panel: display " not in line:
                    continue
                message = line.decode(errors="replace")
                observed.append(message)
                name, triggers, auto_cue, remain = stages[stage]
                if not all(trigger in message for trigger in triggers):
                    continue

                live, replay, prefix = _capture_replay_snapshot(
                    qmp, tmp_path, name, capture, scif1_replay_exe)
                pages = list(_captured_status_pages(prefix))
                page = next(frame for frame in reversed(pages) if frame is not None and frame[0] == 2)
                assert (bool(page[12] & 1), bool(page[13] & 1)) == (auto_cue, remain), name

                red, off = b"\xff\x58\x4c", b"\x0a\x12\x1b"
                border = (77 * 1280 + 225) * 3
                assert live[border:border + 3] == (red if auto_cue else off), name
                label_rows = (live[(y * 1280 + 165) * 3:(y * 1280 + 291) * 3]
                              for y in range(89, 113))
                assert any(red in row for row in label_rows) == auto_cue, name
                remain_lit = any(live[(y * 1280 + x) * 3 + 2] > 128
                                 for y in range(40, 64) for x in range(157, 301))
                assert remain_lit == remain, name
                assert live == replay, name
                stage += 1
                if stage < len(stages):
                    deadline = time.monotonic() + 180
                    _qmp_execute(qmp, "cont")

    assert stage == len(stages), (
        f"Missing {stages[stage][0]}; display log:\n" + "\n".join(observed)
    )


def test_live_scif1_navigation_lamps_match_replay(tmp_path, scif1_replay_exe):
    script = ("6000:usb:200,9000:enter:200,11500:enter:200,14000:info:200,"
              "16500:browse:200,18500:info:200,21000:tag:200,23000:info:200,"
              "25000:disc:200")
    capture = tmp_path / "navigation.scif1"
    stages = [
        ("no-disc", ("mode=0x08", "[NO DISC]"), 0x10),
        ("usb-root", ("mode=0x01", "[.FOLDER.]"), 0x10),
        ("loaded", ("mode=0x0a", "[INFO.wav]"), 0x40),
        ("browse", ("mode=0x01", "attr=5a:", "[INFO.wav]"), 0x10),
        ("info-from-browse", ("mode=0x0a", "[INFO.wav]"), 0x40),
        ("tag", ("mode=0x08", "[EMPTY]"), 0x20),
        ("info-from-tag", ("mode=0x0a", "[INFO.wav]"), 0x40),
        ("disc-dialog", ("mode=0x07", "[USB is connected.]"), 0x40),
    ]
    stage = 0
    observed = []
    deck = _live_usb_deck(tmp_path, script, capture, ("INFO.wav",), track_seconds=30)
    with deck as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        buf = b""
        while time.monotonic() < deadline and stage < len(stages):
            if not sel.select(timeout=0.3):
                continue
            chunk = os.read(p.stderr.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf and stage < len(stages):
                line, buf = buf.split(b"\n", 1)
                if b"cdj850-panel: display mode=" not in line:
                    continue
                message = line.decode(errors="replace")
                observed.append(message)
                name, triggers, lamps = stages[stage]
                if not all(trigger in message for trigger in triggers):
                    continue

                live, replay, prefix = _capture_replay_snapshot(
                    qmp, tmp_path, name, capture, scif1_replay_exe)
                pages = list(_captured_status_pages(prefix))
                page = next(frame for frame in reversed(pages) if frame is not None and frame[0] == 2)
                assert page[7] & 0xF0 == lamps, name

                blue, off = b"\x9f\xe8\xff", b"\x0a\x12\x1b"
                for mask, top, left, label_top, width, height in (
                    (0x10, 164, 41, 183, 96, 28),  # BROWSE
                    (0x20, 239, 61, 246, 56, 51),  # TAG LIST
                    (0x40, 314, 61, 335, 56, 24),  # INFO
                ):
                    enabled = bool(lamps & mask)
                    border = (top * 1280 + 89) * 3
                    assert live[border:border + 3] == (blue if enabled else off), (name, mask)
                    label = (live[(y * 1280 + left) * 3:(y * 1280 + left + width) * 3]
                             for y in range(label_top, label_top + height))
                    assert any(blue in row for row in label) == enabled, (name, mask)
                assert live == replay, name
                stage += 1
                if stage < len(stages):
                    deadline = time.monotonic() + 180
                    _qmp_execute(qmp, "cont")

    assert stage == len(stages), (
        f"Missing {stages[stage][0]}; display log:\n" + "\n".join(observed)
    )


def test_live_scif1_menu_matches_replay(tmp_path, scif1_replay_exe):
    # A hold opens UTILITY. Releasing preserves MENU until another press.
    script = "3800:menu:1700,6500:menu:200"
    capture = tmp_path / "menu.scif1"
    stages = [
        ("initial", "[NO DISC]", False),
        ("utility", "[.UTILITY.]", True),
        ("released", "input release menu", True),
        ("closed", "[NO DISC]", False),
    ]
    stage = 0
    observed = []
    with _live_usb_deck(tmp_path, script, capture) as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        buf = b""
        while time.monotonic() < deadline and stage < len(stages):
            if not sel.select(timeout=0.3):
                continue
            chunk = os.read(p.stderr.fileno(), 4096)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf and stage < len(stages):
                line, buf = buf.split(b"\n", 1)
                message = line.decode(errors="replace")
                name, trigger, enabled = stages[stage]
                if "cdj850-panel: display " in message or "input " in message:
                    observed.append(message)
                if trigger not in message:
                    continue

                live, replay, prefix = _capture_replay_snapshot(
                    qmp, tmp_path, name, capture, scif1_replay_exe)
                pages = list(_captured_status_pages(prefix))
                page = next(frame for frame in reversed(pages) if frame is not None and frame[0] == 2)
                assert page[7] == (0x8F if enabled else 0x1F), name

                blue, background = b"\x9f\xe8\xff", b"\x06\x0b\x10"
                border = (452 * 1280 + 1159) * 3
                assert live[border:border + 3] == (blue if enabled else background), name
                assert live == replay, name
                stage += 1
                if stage < len(stages):
                    deadline = time.monotonic() + 180
                    _qmp_execute(qmp, "cont")

    assert stage == len(stages), (
        f"Missing {stages[stage][0]}; display log:\n" + "\n".join(observed)
    )


def _captured_playing_packets(prefix):
    memory = waveform = None
    wave = []
    for line in prefix.read_text().splitlines()[1:]:
        cols = line.split()
        if cols[0] != "T" or int(cols[4]) < 2:
            continue
        for field in cols[5:]:
            frame = bytes.fromhex(field.split(":")[1])
            if sum8ec(frame[:15]) != frame[15]:
                continue
            if frame[0] == 0x60:
                memory = frame[2:15]
            elif frame[0] == 0x70:
                wave = [frame[3:15]]
            elif wave and frame[0] == 0x70 + len(wave):
                wave.append(frame[1:15])
                if frame[0] == 0x77:
                    waveform = b"".join(wave)[:100]
                    wave = []
    assert memory is not None
    return memory, waveform


def test_live_scif1_playing_fields_match_replay(tmp_path, scif1_replay_exe):
    image = os.environ.get("CDJ850_PLAYING_TEST_IMAGE")
    if not image:
        pytest.skip("set CDJ850_PLAYING_TEST_IMAGE to the SCIF ARTIST export")
    script = ("6000:usb:200,7600:rot+2,9000:enter:200,11500:enter:200,"
              "15000:play:200,16800:cue:200,20300:next:200,25000:usb_stop:1500")
    capture = tmp_path / "playing.scif1"
    stages = [
        ("loaded-128", ("[GRID128]", "[SCIF ARTIST]"), {12, 36, 66}, 0),
        ("cue-set", ("display playing ", "cue=2 ("), {12, 36, 66}, 2),
        ("loaded-96", ("[GRID96]", "[SCIF ARTIST]"), set(), 0),
        ("unloaded", ("[NO USB]",), set(), 0),
    ]
    waveforms = {}
    observed = []
    rows = ""
    wave_ready = False
    stage = 0
    deck = _live_usb_deck(tmp_path, script, capture, usb_image=image)
    with deck as (p, qmp), selectors.DefaultSelector() as sel:
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 180
        buf = b""
        while time.monotonic() < deadline and stage < len(stages):
            if not sel.select(timeout=0.3):
                continue
            chunk = os.read(p.stderr.fileno(), 16384)
            if not chunk:
                break
            buf += chunk
            while b"\n" in buf and stage < len(stages):
                line, buf = buf.split(b"\n", 1)
                if b"cdj850-panel: display" not in line:
                    continue
                message = line.decode(errors="replace")
                observed.append(message)
                if "display mode=" in message:
                    rows = message
                if "display playing " in message:
                    wave_ready = "wave=1 " in message
                name, triggers, expected_memory, expected_cue = stages[stage]
                if name.startswith("loaded"):
                    # The title and the complete overview can arrive in either order.
                    if not wave_ready or not all(trigger in rows for trigger in triggers):
                        continue
                elif not all(trigger in message for trigger in triggers):
                    continue

                live, replay, prefix = _capture_replay_snapshot(
                    qmp, tmp_path, name, capture, scif1_replay_exe)
                assert live == replay, name
                page = next(frame for frame in reversed(list(_captured_status_pages(prefix)))
                            if frame is not None and frame[0] == 2)
                memory, wave = _captured_playing_packets(prefix)
                assert {x for x in range(100) if memory[x // 8] & (0x80 >> (x % 8))} == expected_memory
                assert (page[10], bool(page[11] & 0x10)) == (expected_cue, bool(expected_memory)), name

                def rgb(x, y):
                    offset = (y * 1280 + x) * 3
                    return live[offset:offset + 3]

                red, green, blue = b"\xff\x58\x4c", b"\x68\xe4\xa5", b"\x9f\xe8\xff"
                playing = name != "unloaded"
                assert (rgb(52, 390) == red) == bool(expected_memory), name
                assert (rgb(52, 424) == blue) == bool(expected_cue), name
                if playing:
                    assert wave is not None and len(wave) == 100 and max(wave) == 15, name
                    waveforms[name] = wave
                    assert any(rgb(176 + x * 9, 166 + y * 8) == blue
                               for x in range(8, 60) for y in range(11, 18)), name
                for x in range(100):
                    px = 176 + x * 9
                    assert (rgb(px, 391) == red) == (x in expected_memory), (name, x)
                    assert (rgb(px, 407) == green) == (playing and x < page[4]), (name, x)
                    assert (rgb(px, 439) == blue) == (x + 1 == expected_cue), (name, x)
                    # Message mode can light the five-column navigation rail.
                    if playing or x >= 5:
                        height = (wave[x] + 2) // 3 if playing else 0
                        for y in range(5):
                            assert (rgb(px, 166 + (26 - y) * 8) == blue) == (y < height), (name, x, y)
                stage += 1
                if stage < len(stages):
                    deadline = time.monotonic() + 180
                    _qmp_execute(qmp, "cont")

    assert stage == len(stages), (
        f"Missing {stages[stage][0]}; latest display log:\n" + "\n".join(observed[-35:])
    )
    assert waveforms["loaded-128"] == waveforms["cue-set"]
    assert waveforms["loaded-128"] != waveforms["loaded-96"]
