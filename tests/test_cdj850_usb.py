# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the real CDJ-850 USB/DMAC with QTest and a tiny synthetic ROM.

No Pioneer firmware is required. MMIO checks run with the CPU stopped;
interrupt checks run a ROM that enables interrupts and parks on a branch.
"""
import json
import math
import os
import select
import selectors
import socket
import struct
import subprocess
import time
from pathlib import Path

import pytest


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


AUDIO_PLAY_STATE = 0x0C7F6627


def _qmp_read_byte(qmp, addr):
    """Read one guest-physical byte through HMP `xp` over QMP."""
    cmd = {"execute": "human-monitor-command",
           "arguments": {"command-line": f"xp /1bx {addr:#x}"}}
    qmp.write(json.dumps(cmd).encode() + b"\n")
    while True:
        r = json.loads(qmp.readline())
        if "return" in r:
            return int(r["return"].split(":")[-1].strip(), 16)
        if "error" in r:
            raise AssertionError(r["error"])


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


def _build_fat16_usb_image(img_path, track_names):
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
    wav_blob = _make_pcm_wav()

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


def test_live_usb_source_select_browse_load_and_play(tmp_path):
    binary = _cdj850_binary()
    flash = Path(__file__).resolve().parent.parent / "extract" / "cdj850" / "flash_cdj850.bin"
    if not flash.is_file():
        pytest.skip("extract/cdj850/flash_cdj850.bin is not present")
    if os.name == "nt":
        pytest.skip("QMP socket descriptor inheritance requires POSIX")

    usb_img = tmp_path / "usb.img"
    _build_fat16_usb_image(usb_img, ["ALPHA.wav", "BETA.wav", "GAMMA.wav"])

    qmp_s, qmp_c = socket.socketpair()
    env = os.environ.copy()
    env.pop("CDJ850_DSP_BOOT_HLE", None)
    # VINYL jog mode is the factory default and defers PLAY until a jog
    # condition the model does not provide yet. Switching to CDJ mode
    # first makes PLAY send audio command (3, 1) straight away.
    env["CDJ850_PANEL_SCRIPT"] = (
        "20000:usb:200,24000:enter:200,26000:rot+1,28000:enter:200,"
        "38000:vinyl:200,42000:play:200"
    )

    p = subprocess.Popen(
        [
            binary, "-M", "cdj850",
            "-display", "none", "-monitor", "none", "-serial", "none",
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
    qmp = qmp_s.makefile("rwb", buffering=0)
    display_lines = []
    try:
        assert "QMP" in json.loads(qmp.readline())
        qmp.write(b'{"execute":"qmp_capabilities"}\n')
        assert "return" in json.loads(qmp.readline())

        sel = selectors.DefaultSelector()
        sel.register(p.stderr, selectors.EVENT_READ)
        deadline = time.monotonic() + 90.0
        buf = ""
        milestones = [False] * 5
        play_pressed = False  # only counts once the track is cued
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
                    if milestones[3] and "input press play" in ln:
                        play_pressed = True
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
            # Play reached the audio task: its play-state byte (status
            # struct 0x0C7F6620 + 7, set by FUN_0C009310) becomes 4, the
            # USB play state handled by FUN_0C0F6E6C.
            if play_pressed and not milestones[4]:
                milestones[4] = _qmp_read_byte(qmp, AUDIO_PLAY_STATE) == 4
        assert all(milestones), (
            f"Missing USB browse/load/play milestones {milestones}; display log:\n" +
            "\n".join(display_lines)
        )
    finally:
        try:
            qmp.write(b'{"execute":"quit"}\n')
            p.wait(timeout=5)
        except Exception:
            p.kill()
            p.wait(timeout=5)
        qmp_s.close()
