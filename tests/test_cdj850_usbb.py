# SPDX-License-Identifier: GPL-2.0-or-later
"""End-to-end tests for CDJ-850 USB-B (PC / HID + MIDI) device mode."""
import os
import socket
import struct
import subprocess
import time
from contextlib import contextmanager
from pathlib import Path

import pytest

import usb_hid_port as up
from test_cdj850_usb import _build_fat16_usb_image, _cdj850_binary


EXPECTED_STATUS = (
    "configured vid=08e4 pid=0157 midi_in=83 midi_out=00 "
    "hid_in=87 hid_out=06"
)
EXPECTED_HID_DESC = bytes.fromhex(
    "06a0ff0901a1010902a10006a1ff090309041580257f350045ff750895148102"
    "090509061580257f350045ff750895249102c0c0"
)


def _flash_path():
    flash = (Path(__file__).resolve().parent.parent
             / "extract" / "cdj850" / "flash_cdj850.bin")
    if not flash.is_file():
        pytest.skip("flash_cdj850.bin not prepared")
    return flash


@contextmanager
def _usbb_deck(tmp_path, script, usb_tracks=None):
    binary = _cdj850_binary()
    flash = _flash_path()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    sock.settimeout(0.1)

    env = dict(os.environ)
    for k in ("CDJ850_DSP_BOOT_HLE", "CDJ850_PF16_HIGH", "CDJ850_DSP_PLAY_PF"):
        env.pop(k, None)
    env["CDJ_USBF_HOST"] = str(port)
    env["CDJ850_PANEL_SCRIPT"] = script
    log_path = tmp_path / "qemu.log"
    cmd = [
        binary,
        "-M", "cdj850",
        "-display", "none",
        "-monitor", "none",
        "-serial", "none",
        "-icount", "shift=3,sleep=off",
        "-drive", f"if=pflash,file={flash},format=raw,snapshot=on",
    ]
    if usb_tracks is not None:
        usb_img = tmp_path / "usb.img"
        _build_fat16_usb_image(usb_img, usb_tracks)
        env["CDJ_USB_ATTACH_MS"] = "1500"
        cmd += [
            "-drive", f"if=none,id=usbstick,file={usb_img},format=raw,snapshot=on",
            "-device", "usb-storage,drive=usbstick,port=1",
        ]

    with open(log_path, "wb") as log_f:
        proc = subprocess.Popen(
            cmd, env=env, stdout=subprocess.DEVNULL, stderr=log_f,
        )
        try:
            yield proc, sock, log_path
        finally:
            sock.close()
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)


def test_usbb_enumeration_midi_and_hid_in(tmp_path):
    script = "3000:pc:300,6000:play:200,7000:cue:200"
    hid_reports = []
    midi_packets = []
    deck = up.Deck(lambda report, vt: hid_reports.append(report))
    statuses = []
    peer_addr = None

    with _usbb_deck(tmp_path, script) as (proc, sock, log_path):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            assert proc.poll() is None, log_path.read_text(errors="replace")
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                continue
            peer_addr = addr
            if data[0] == 0:
                text = data[1:].decode("ascii", "replace")
                statuses.append(text)
            elif data[0] == 0x83:
                midi_packets.append(data[9:])
            deck.datagram(data, addr)
            if (any(p.startswith(bytes.fromhex("0990007f")) for p in midi_packets)
                    and any(p.startswith(bytes.fromhex("0990017f")) for p in midi_packets)
                    and any(r[0] & 0x80 for r in hid_reports)):
                break

    log_txt = log_path.read_text(errors="replace")
    assert peer_addr is not None, log_txt
    assert EXPECTED_STATUS in statuses, (statuses, log_txt)
    assert deck.vid == 0x08E4
    assert deck.pid == 0x0157
    assert (deck.hid_in, deck.hid_out) == (0x87, 0x06)
    assert deck.hid_desc == EXPECTED_HID_DESC
    assert any(p == bytes.fromhex("0990007f") for p in midi_packets)
    assert any(p == bytes.fromhex("09900000") for p in midi_packets)
    assert any(p == bytes.fromhex("0990017f") for p in midi_packets)
    assert any(len(r) == 20 and (r[0] & 0x80) for r in hid_reports)


def test_usbb_control_relay_and_mode_switch_roundtrip(tmp_path):
    script = ("5000:pc:300,8000:usb:300,9500:enter:300,"
              "11500:pc:300,14500:play:200")
    stages = []
    control_replies = []
    hid_reports = []
    peer_addr = None
    queried_ctrl = False

    with _usbb_deck(tmp_path, script, usb_tracks=["TRACK01.wav"]) as (proc, sock, log_path):
        deadline = time.monotonic() + 45
        while time.monotonic() < deadline:
            assert proc.poll() is None, log_path.read_text(errors="replace")
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                continue
            peer_addr = addr
            if data[0] == 0:
                text = data[1:].decode("ascii", "replace")
                if not stages or stages[-1] != text:
                    stages.append(text)
                if text == EXPECTED_STATUS and not queried_ctrl:
                    # Control GET_DESCRIPTOR(DEVICE, 18) and a 36B HID OUT frame.
                    setup = struct.pack("<BBHHH", 0x80, 0x06, 0x0100, 0, 18)
                    sock.sendto(bytes([0x80]) + setup, addr)
                    sock.sendto(bytes([0x06]) + bytes(36), addr)
                    queried_ctrl = True
            elif data[0] == 0x80:
                control_replies.append(data[9:])
            elif data[0] == 0x87:
                hid_reports.append(data[9:])

            if ("detached" in stages
                    and stages.count(EXPECTED_STATUS) >= 2
                    and control_replies
                    and any(len(r) == 20 and (r[0] & 0x80) for r in hid_reports)):
                break

    log_txt = log_path.read_text(errors="replace")
    assert peer_addr is not None, (stages, log_txt)
    assert "detached" in stages, (stages, log_txt)
    assert stages.count(EXPECTED_STATUS) >= 2, (stages, log_txt)
    assert control_replies and control_replies[0][0] == 0, control_replies
    dev_desc = control_replies[0][1:]
    assert len(dev_desc) == 18
    assert struct.unpack_from("<HH", dev_desc, 8) == (0x08E4, 0x0157)
    assert any(len(r) == 20 and (r[0] & 0x80) for r in hid_reports)
