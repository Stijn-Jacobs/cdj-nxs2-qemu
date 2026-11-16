# SPDX-License-Identifier: GPL-2.0-or-later
"""Exercise the real CDJ-850 USB/DMAC with QTest and a tiny synthetic ROM.

No Pioneer firmware is required. MMIO checks run with the CPU stopped;
interrupt checks run a ROM that enables interrupts and parks on a branch.
"""
import json
import os
import select
import socket
import struct
import subprocess
import time

import pytest


USB = 0xFFFC1C00
DMA = 0xFFFE1000


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
    binary = os.environ.get("GUI_QEMU", os.path.expanduser(
        "~/qemu-build-eb/qemu-system-sh4eb"))
    if not os.path.isfile(binary):
        pytest.skip("qemu-system-sh4eb is not built")
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
