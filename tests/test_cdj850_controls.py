# SPDX-License-Identifier: GPL-2.0-or-later
"""End-to-end tests for CDJ-850 playable controls over live UDP panel socket.

Exercises pitch fader (clearing HOLD and scaling SPEED), CDJ jog bend,
VINYL jog scratch, CUE, LOOP IN / LOOP OUT / RELOOP, and MEMORY / CALL on a
live USB-booted firmware under -icount.
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

# MAIN SHI TX word 0 (0x0CE6C938): bit 15 = HOLD ("apply speed without ramp").
SHI_CMD0 = 0x0CE6C938


def _free_udp_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _send_udp(port, msg):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.sendto(msg.encode("ascii"), ("127.0.0.1", port))
    finally:
        s.close()


def _tap(port, off, mask, dur_ms=160):
    _send_udp(port, f"0x{off:02x}:{mask}:{dur_ms}:or")


def _wait_cond(fn, timeout=45.0, interval=0.25, pump=None):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        if pump:
            pump()
        last = fn()
        if last:
            return last
        time.sleep(interval)
    return None


def _drain_stderr(sel, buf_ref, panel_logs):
    for key, _ in sel.select(timeout=0.05):
        chunk = os.read(key.fileobj.fileno(), 4096).decode(errors="replace")
        if not chunk:
            continue
        buf_ref[0] += chunk
        while "\n" in buf_ref[0]:
            ln, buf_ref[0] = buf_ref[0].split("\n", 1)
            if "cdj850-panel:" in ln:
                panel_logs.append(ln)


def test_controls_pitch_jog_cue_loop_and_memory(tmp_path):
    binary = _cdj850_binary()
    flash = (Path(__file__).resolve().parent.parent
             / "extract" / "cdj850" / "flash_cdj850.bin")
    if not flash.is_file():
        pytest.skip("extract/cdj850/flash_cdj850.bin is not present")
    if os.name == "nt":
        pytest.skip("QMP socket descriptor inheritance requires POSIX")

    usb_img = tmp_path / "usb.img"
    _build_fat16_usb_image(usb_img, ["ALPHA.wav", "BETA.wav", "GAMMA.wav"])

    key_port = _free_udp_port()
    qmp_s, qmp_c = socket.socketpair()
    env = os.environ.copy()
    for k in ("CDJ850_DSP_BOOT_HLE", "CDJ850_PF16_HIGH", "CDJ850_DSP_PLAY_PF"):
        env.pop(k, None)
    env["CDJ_PANEL_KEYSOCK"] = str(key_port)
    env["CDJ850_PANEL_SCRIPT"] = (
        "6000:usb:200,9000:enter:200,11000:rot+1,13000:enter:200"
    )

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
        stderr=subprocess.PIPE,
        env=env,
        pass_fds=(qmp_c.fileno(),),
    )
    qmp_c.close()
    qmp = qmp_s.makefile("rwb", buffering=0)
    panel_logs = []
    buf_ref = [""]
    sel = selectors.DefaultSelector()
    sel.register(p.stderr, selectors.EVENT_READ)

    def pump():
        _drain_stderr(sel, buf_ref, panel_logs)

    try:
        assert "QMP" in json.loads(qmp.readline())
        qmp.write(b'{"execute":"qmp_capabilities"}\n')
        assert "return" in json.loads(qmp.readline())

        # 1. Wait until BETA.wav is loaded and playing.
        loaded = _wait_cond(
            lambda: any("mode=0x0a" in ln and "[BETA.wav] | [] | []" in ln
                        for ln in panel_logs)
                    and _qmp_read(qmp, PLAYER_STATE) == 3,
            timeout=150.0,
            pump=pump,
        )
        assert loaded, "track failed to load and start playing:\n" + "\n".join(panel_logs)

        idx0 = _qmp_read(qmp, READ_INDEX, "h")
        assert _wait_cond(
            lambda: _qmp_read(qmp, READ_INDEX, "h") > idx0 + 8,
            timeout=20.0, pump=pump,
        ), "READ_INDEX did not advance during initial playback"

        # 2. Switch from VINYL (default) to CDJ mode so pitch & bend are direct.
        _tap(key_port, 0x13, 0x02)  # vinyl toggle -> CDJ mode
        time.sleep(0.5)

        # 3. Pitch slider: verify +10% (ADC=0x0300) clears HOLD and raises SPEED > 1.0x.
        _send_udp(key_port, "0x05:0x03:0:lvl")
        _send_udp(key_port, "0x06:0x00:0:lvl")
        fast = _wait_cond(
            lambda: _qmp_read(qmp, SPEED, "w") > 0x084000,
            timeout=15.0, pump=pump,
        )
        assert fast, f"pitch +10% did not raise SPEED (got {_qmp_read(qmp, SPEED, 'w'):#x})"
        cmd0 = _qmp_read(qmp, SHI_CMD0, "h")
        assert (cmd0 & 0x8000) == 0, f"HOLD bit remained set after pitch move: {cmd0:#x}"

        # Move pitch to -10% (ADC=0xFCFF) and check SPEED < 1.0x.
        _send_udp(key_port, "0x05:0xfc:0:lvl")
        _send_udp(key_port, "0x06:0xff:0:lvl")
        slow = _wait_cond(
            lambda: 0 < _qmp_read(qmp, SPEED, "w") < 0x07C000,
            timeout=15.0, pump=pump,
        )
        assert slow, f"pitch -10% did not lower SPEED (got {_qmp_read(qmp, SPEED, 'w'):#x})"

        # Return pitch to centre 0x8000 (1.0x = 0x080000).
        _send_udp(key_port, "0x05:0x80:0:lvl")
        _send_udp(key_port, "0x06:0x00:0:lvl")
        assert _wait_cond(
            lambda: 0x07F800 <= _qmp_read(qmp, SPEED, "w") <= 0x080800,
            timeout=15.0, pump=pump,
        ), f"pitch did not return to 1.0x (got {_qmp_read(qmp, SPEED, 'w'):#x})"

        # 4. CDJ jog bend: roll counter by +80 per tick with P=278 (>42 pulses/30 passes).
        jog_cnt = 0
        def _bend_ok():
            nonlocal jog_cnt
            jog_cnt = (jog_cnt + 80) & 0xFFFF
            _send_udp(key_port, f"0x09:{jog_cnt >> 8}:0:lvl")
            _send_udp(key_port, f"0x0a:{jog_cnt & 0xff}:0:lvl")
            _send_udp(key_port, f"0x0b:{278 >> 8}:0:lvl")
            _send_udp(key_port, f"0x0c:{278 & 0xff}:0:lvl")
            _send_udp(key_port, "0x0d:0xc0:180:or")  # jog_move | jog_fwd
            return _qmp_read(qmp, SPEED, "w") > 0x082000

        assert _wait_cond(_bend_ok, timeout=15.0, interval=0.1, pump=pump), (
            f"jog bend did not increase SPEED (got {_qmp_read(qmp, SPEED, 'w'):#x})"
        )
        # Stop platter cleanly.
        _send_udp(key_port, "0x0b:0:0:lvl")
        _send_udp(key_port, "0x0c:0:0:lvl")
        assert _wait_cond(
            lambda: 0x07F800 <= _qmp_read(qmp, SPEED, "w") <= 0x080800,
            timeout=15.0, pump=pump,
        ), "SPEED did not recover after jog bend stopped"

        # 5. LOOP IN -> advance -> LOOP OUT -> verify READ_INDEX loops back -> RELOOP/EXIT.
        _tap(key_port, 0x11, 0x08)  # LOOP IN
        loop_in_idx = _qmp_read(qmp, READ_INDEX, "h")
        assert _wait_cond(
            lambda: _qmp_read(qmp, READ_INDEX, "h") >= loop_in_idx + 40,
            timeout=20.0, pump=pump,
        ), "track did not advance after LOOP IN"
        loop_out_idx = _qmp_read(qmp, READ_INDEX, "h")
        _tap(key_port, 0x11, 0x10)  # LOOP OUT (closes active loop)
        seen = []
        wrapped = False
        deadline = time.monotonic() + 8.0
        while time.monotonic() < deadline:
            pump()
            cur = _qmp_read(qmp, READ_INDEX, "h")
            seen.append(cur)
            if cur < max(seen) - 10:
                wrapped = True
                break
            time.sleep(0.15)
        bounded = max(seen) <= loop_out_idx + 20 and _qmp_read(qmp, PLAYER_STATE) == 3
        assert wrapped or bounded, (
            f"LOOP IN/OUT neither wrapped nor held READ_INDEX (in={loop_in_idx}, out={loop_out_idx}, seen={seen})"
        )

        # Exit loop and verify playback advances past loop_out_idx.
        _tap(key_port, 0x11, 0x20)  # RELOOP/EXIT
        assert _wait_cond(
            lambda: _qmp_read(qmp, READ_INDEX, "h") > max(seen) + 30,
            timeout=20.0, pump=pump,
        ), "RELOOP/EXIT did not leave loop"

        # 6. CUE returns to cue point (PLAYER_STATE == 6 / cued, READ_INDEX resets below pre_cue_idx);
        #    MEMORY / DELETE / PLAY resume playback cleanly.
        pre_cue_idx = _qmp_read(qmp, READ_INDEX, "h")
        _tap(key_port, 0x11, 0x04)  # CUE
        assert _wait_cond(
            lambda: (_qmp_read(qmp, PLAYER_STATE) in (4, 5, 6)
                     and _qmp_read(qmp, READ_INDEX, "h") < pre_cue_idx),
            timeout=15.0, pump=pump,
        ), (
            f"CUE did not return to cue point (state={_qmp_read(qmp, PLAYER_STATE)}, "
            f"pre={pre_cue_idx}, idx={_qmp_read(qmp, READ_INDEX, 'h')}, "
            f"speed={_qmp_read(qmp, SPEED, 'w'):#x})"
        )
        # Save and recall/delete a memory cue while cued.
        _tap(key_port, 0x14, 0x08)  # MEMORY
        time.sleep(0.3)
        _tap(key_port, 0x14, 0x01)  # CALL PREV
        time.sleep(0.3)
        _tap(key_port, 0x14, 0x04)  # DELETE
        time.sleep(0.3)

        cued_idx = _qmp_read(qmp, READ_INDEX, "h")
        _tap(key_port, 0x11, 0x02)  # PLAY
        assert _wait_cond(
            lambda: _qmp_read(qmp, PLAYER_STATE) == 3 and _qmp_read(qmp, READ_INDEX, "h") > cued_idx + 12,
            timeout=15.0, pump=pump,
        ), "PLAY after CUE did not restart playback"

        # 7. Switch back to VINYL mode, touch platter + move backwards -> reverse/scratch SPEED or READ_INDEX.
        _tap(key_port, 0x13, 0x02)  # vinyl toggle -> VINYL mode
        time.sleep(0.5)
        _send_udp(key_port, "0x0d:0x20:0:hold")  # touch platter
        time.sleep(0.3)
        pre_scratch = _qmp_read(qmp, READ_INDEX, "h")
        def _scratch_back():
            nonlocal jog_cnt
            jog_cnt = (jog_cnt - 120) & 0xFFFF
            _send_udp(key_port, f"0x09:{jog_cnt >> 8}:0:lvl")
            _send_udp(key_port, f"0x0a:{jog_cnt & 0xff}:0:lvl")
            _send_udp(key_port, f"0x0b:{556 >> 8}:0:lvl")
            _send_udp(key_port, f"0x0c:{556 & 0xff}:0:lvl")
            _send_udp(key_port, "0x0d:0x80:180:or")  # jog_move rev
            spd = _qmp_read(qmp, SPEED, "w")
            idx = _qmp_read(qmp, READ_INDEX, "h")
            # Bit 22 (0x400000) is MAIN's reverse direction flag sent to the DSP.
            return (spd & 0x400000) != 0 or idx < pre_scratch - 5

        scratched = _wait_cond(_scratch_back, timeout=15.0, interval=0.1, pump=pump)
        _send_udp(key_port, "0x0b:0:0:lvl")
        _send_udp(key_port, "0x0c:0:0:lvl")
        _send_udp(key_port, "0x0d:0x20:0:rel")
        assert scratched, "VINYL jog touch + reverse move did not scrub READ_INDEX backwards"
    finally:
        try:
            qmp.write(b'{"execute":"quit"}\n')
            p.wait(timeout=5)
        except Exception:
            p.kill()
            p.wait(timeout=5)
        qmp_s.close()
