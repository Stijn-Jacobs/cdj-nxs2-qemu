# SPDX-License-Identifier: GPL-2.0-or-later
"""End-to-end save/restore state test for CDJ-850 with USB storage attached.

Verifies every cdj850 peripheral (USB, INTC timers, DSP56371, DSP SHI observer,
SCIF1 panel peer, VFD) migrates cleanly over QEMU migration stream + -incoming
while a track from USB is actively playing.
"""
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import time

import pytest

from test_cdj850_usb import (
    PLAYER_STATE,
    READ_INDEX,
    SPEED,
    _build_fat16_usb_image,
    _cdj850_binary,
    _qmp_read,
)


def _qmp_cmd(qmp, execute, arguments=None):
    msg = {"execute": execute}
    if arguments is not None:
        msg["arguments"] = arguments
    qmp.write((json.dumps(msg) + "\n").encode())
    while True:
        r = json.loads(qmp.readline())
        if "return" in r:
            return r["return"]
        if "error" in r:
            raise AssertionError(r["error"])


def test_save_and_restore_with_usb_playing(tmp_path):
    binary = _cdj850_binary()
    flash = (Path(__file__).resolve().parent.parent
             / "extract" / "cdj850" / "flash_cdj850.bin")
    if not flash.is_file():
        pytest.skip("extract/cdj850/flash_cdj850.bin is not present")
    if os.name == "nt":
        pytest.skip("QMP socket descriptor inheritance requires POSIX")

    usb_img = tmp_path / "usb.img"
    _build_fat16_usb_image(usb_img, ["ALPHA.wav", "BETA.wav", "GAMMA.wav"])
    snap_vm = tmp_path / "main.vm"

    env = os.environ.copy()
    for k in ("CDJ850_DSP_BOOT_HLE", "CDJ850_PF16_HIGH", "CDJ850_DSP_PLAY_PF"):
        env.pop(k, None)
    env["CDJ850_PANEL_SCRIPT"] = (
        "6000:usb:200,9000:enter:200,11000:rot+1,13000:enter:200"
    )

    qmp_s, qmp_c = socket.socketpair()
    p = subprocess.Popen(
        [
            binary, "-M", "cdj850",
            "-display", "none", "-monitor", "none", "-serial", "none",
            "-icount", "shift=2,sleep=off",
            "-global", "migration.store-global-state=off",
            "-drive", f"if=pflash,format=raw,file={flash},snapshot=on",
            "-drive", f"if=none,id=usbstk,format=raw,file={usb_img},snapshot=on",
            "-device", "usb-storage,drive=usbstk,port=1",
            "-chardev", f"socket,id=qmp,fd={qmp_c.fileno()}",
            "-mon", "chardev=qmp,mode=control",
        ],
        stderr=subprocess.PIPE,
        env=env,
        pass_fds=(qmp_c.fileno(),),
    )
    qmp_c.close()
    qmp = qmp_s.makefile("rwb", buffering=0)
    sel = selectors.DefaultSelector()
    sel.register(p.stderr, selectors.EVENT_READ)
    buf = ""
    loaded = False
    first_idx = None

    try:
        assert "QMP" in json.loads(qmp.readline())
        _qmp_cmd(qmp, "qmp_capabilities")

        deadline = time.monotonic() + 150.0
        while time.monotonic() < deadline:
            for key, _ in sel.select(timeout=0.3):
                chunk = os.read(key.fileobj.fileno(), 4096).decode(errors="replace")
                if chunk:
                    buf += chunk
                    if "mode=0x0a" in buf and "[BETA.wav] | [] | []" in buf:
                        loaded = True
            if loaded and _qmp_read(qmp, PLAYER_STATE) == 3:
                idx = _qmp_read(qmp, READ_INDEX, "h")
                if first_idx is None:
                    first_idx = idx
                elif idx > first_idx + 12:
                    break
        assert loaded and first_idx is not None, f"did not start playing before snapshot:\n{buf}"

        _qmp_cmd(qmp, "stop")
        snap_idx = _qmp_read(qmp, READ_INDEX, "h")
        assert _qmp_read(qmp, PLAYER_STATE) == 3
        assert _qmp_read(qmp, SPEED, "w") == 0x080000

        # Serialize full machine VMState via migration stream (same path as launcher snapshot).
        _qmp_cmd(qmp, "migrate", {"uri": f"exec:cat > {snap_vm}"})
        mig_deadline = time.monotonic() + 30.0
        while time.monotonic() < mig_deadline:
            st = _qmp_cmd(qmp, "query-migrate").get("status")
            if st == "completed":
                break
            assert st != "failed", _qmp_cmd(qmp, "query-migrate")
            time.sleep(0.1)
        assert snap_vm.is_file() and snap_vm.stat().st_size > 1024 * 1024
    finally:
        try:
            _qmp_cmd(qmp, "quit")
            p.wait(timeout=5)
        except Exception:
            p.kill()
            p.wait(timeout=5)
        qmp_s.close()

    # Restore into a fresh QEMU process with -incoming + USB attached.
    qmp_s2, qmp_c2 = socket.socketpair()
    p2 = subprocess.Popen(
        [
            binary, "-M", "cdj850",
            "-display", "none", "-monitor", "none", "-serial", "none",
            "-icount", "shift=2,sleep=off",
            "-global", "migration.store-global-state=off",
            "-drive", f"if=pflash,format=raw,file={flash},snapshot=on",
            "-drive", f"if=none,id=usbstk,format=raw,file={usb_img},snapshot=on",
            "-device", "usb-storage,drive=usbstk,port=1",
            "-incoming", f"file:{snap_vm}",
            "-chardev", f"socket,id=qmp,fd={qmp_c2.fileno()}",
            "-mon", "chardev=qmp,mode=control",
        ],
        stderr=subprocess.PIPE,
        env=env,
        pass_fds=(qmp_c2.fileno(),),
    )
    qmp_c2.close()
    qmp2 = qmp_s2.makefile("rwb", buffering=0)
    try:
        assert "QMP" in json.loads(qmp2.readline())
        _qmp_cmd(qmp2, "qmp_capabilities")

        # Wait for incoming load to complete before issuing cont.
        load_deadline = time.monotonic() + 30.0
        while time.monotonic() < load_deadline:
            st = _qmp_cmd(qmp2, "query-status").get("status")
            if st in ("paused", "prelaunch", "running"):
                if st != "incoming":
                    break
            time.sleep(0.1)

        assert _qmp_read(qmp2, PLAYER_STATE) == 3
        restored_idx = _qmp_read(qmp2, READ_INDEX, "h")
        assert abs(restored_idx - snap_idx) <= 4, (snap_idx, restored_idx)

        _qmp_cmd(qmp2, "cont")
        advance_deadline = time.monotonic() + 20.0
        advanced = False
        while time.monotonic() < advance_deadline:
            cur = _qmp_read(qmp2, READ_INDEX, "h")
            if cur > restored_idx + 12:
                advanced = True
                break
            time.sleep(0.25)
        assert advanced, f"playback stalled after snapshot restore (stuck near {restored_idx})"
        assert _qmp_read(qmp2, SPEED, "w") == 0x080000
    finally:
        try:
            _qmp_cmd(qmp2, "quit")
            p2.wait(timeout=5)
        except Exception:
            p2.kill()
            p2.wait(timeout=5)
        qmp_s2.close()
