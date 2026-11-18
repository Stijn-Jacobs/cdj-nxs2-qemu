# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit, validation, and QEMU boot regression tests for CDJ-850 mods."""
import json
import os
from pathlib import Path
import selectors
import socket
import subprocess
import sys
import time

import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "mods"))
sys.path.insert(0, str(ROOT / "scripts" / "firmware"))

import bootcrit
import dsppatch
import modelfw
import patch_main
import sigpatch
import validate
from launcher import model as launcher_model, mods as launcher_mods

from test_cdj850_controls import (
    SHI_CMD0,
    _drain_stderr,
    _free_udp_port,
    _send_udp,
    _tap,
    _wait_cond,
)
from test_cdj850_usb import (
    PLAYER_STATE,
    READ_INDEX,
    SPEED,
    _build_fat16_usb_image,
    _cdj850_binary,
    _qmp_read,
)

C850 = Path("/Users/julius/Documents/cdj850-emu/C850MAIN.UPD")
FLASH_BIN = ROOT / "extract" / "cdj850" / "flash_cdj850.bin"
UNPACKED_BIN = ROOT / "extract" / "cdj850" / "main_unpacked.bin"


def _make_synthetic_shi_blob(extra_tail=()):
    """Build a synthetic 4-segment DSP56371 SHI sequence matching DSP_PROFILE shape."""
    s1 = [0x300013, 0x04D03F, 0, 0, 0, 0, 0, 0x0AF080, 0xFF0000]
    s2 = [0] * 10 + [0x0AF080, 0xFF0000]
    s3 = [0] * 10 + [0x0AF080, 0xFF0000]
    s4 = [0] * 0x200
    s4[0] = 0x0AF080
    s4[1] = 0x000102
    s4[0x01EA] = 0x000000
    if extra_tail:
        s4.extend(extra_tail)

    chunks = []
    for words in (s1, s2, s3, s4):
        chunks.append(len(words).to_bytes(3, "big"))
        chunks.append((0).to_bytes(3, "big"))
        chunks.extend(w.to_bytes(3, "big") for w in words)
    blob = b"".join(chunks)
    profile = dsppatch.DspShiProfile(
        head=(0x000009, 0x000000, 0x300013, 0x04D03F),
        length=len(blob),
        seg_count=4,
        free=(0x0080, 0x00FE),
    )
    return profile, blob


def test_cdj850_mod_registry_and_profile():
    m = launcher_model.load("cdj850")
    assert m.main_arch == "sh4eb"
    fw = modelfw.load("cdj850")
    assert fw.container == "c850"
    assert modelfw.update_window_end("cdj850") == 0x3F0000

    assert "tempowide" in patch_main.PATCH_SETS["cdj850"].mods
    assert "dspprobe" in patch_main.DSP_SETS["cdj850"].mods

    rows = {r.key: r for r in launcher_mods.load(type("L", (), {"emu": str(ROOT)})(), "cdj850")}
    assert "tempo_wide" in rows
    assert "dsp_probe" in rows


def test_dsp_shi_profile_patch_idempotent_and_boot_checks():
    profile, blob = _make_synthetic_shi_blob()
    prefix = b"\xAA" * 0x40
    image = prefix + blob + b"\xBB" * 0x40
    mod = patch_main.DSP_SETS["cdj850"].mods["dspprobe"]

    report = {}
    spans = {}
    patched = dsppatch.patch(
        profile, image, {"dspprobe": mod}, ["dspprobe"],
        report=report, spans=spans,
        blob_path=lambda n: patch_main.dsp_blob_path(n, "cdj850"),
    )
    assert report == {"dspprobe": ("applied", "")}
    assert any(k == "dsp" for _, _, k in spans["dspprobe"])
    assert any(k == "dspfree" for _, _, k in spans["dspprobe"])

    # Validation checks pass cleanly.
    rep = validate.Report()
    flat = validate.check_writes("main", image, patched,
                                patch_main.PATCH_SETS["cdj850"].profile,
                                spans, rep, dsp=(0x40, 0x40 + len(blob)))
    validate.check_dsp_writes(image, flat, profile, 0x40, rep, out_img=patched)
    assert not rep.failed, rep.text()

    # Re-applying reports present and leaves bytes identical.
    report2 = {}
    patched2 = dsppatch.patch(
        profile, patched, {"dspprobe": mod}, ["dspprobe"],
        report=report2, blob_path=lambda n: patch_main.dsp_blob_path(n, "cdj850"),
    )
    assert report2 == {"dspprobe": ("present", "")}
    assert patched2 == patched

    # Breaking stage-1 JMP $FF0000 or zeroing P:0000 fails verify_shi_boot.
    bad_stage = bytearray(blob)
    segs = dsppatch.shi_segments(blob, 4)
    jmp_off = segs[0][3] + 3 * (segs[0][1] - 2)
    bad_stage[jmp_off:jmp_off + 3] = b"\x00\x00\x00"
    assert dsppatch.verify_shi_boot(bytes(bad_stage), profile)

    bad_reset = bytearray(blob)
    app_off = segs[3][3]
    bad_reset[app_off:app_off + 3] = b"\x00\x00\x00"
    assert dsppatch.verify_shi_boot(bytes(bad_reset), profile)


def test_cdj850_real_bootcrit_patch_update_validate_and_persist_flash(tmp_path):
    if not C850.is_file() or not UNPACKED_BIN.is_file() or not FLASH_BIN.is_file():
        pytest.skip("CDJ-850 firmware files are not present")

    stock = UNPACKED_BIN.read_bytes()
    profile = patch_main.PATCH_SETS["cdj850"].profile
    cave_lo, cave_hi = sigpatch.find_cave(sigpatch.Image(profile, stock))
    assert cave_hi - cave_lo == 1118

    cl = bootcrit.closure(stock, profile.flash_base, byteorder=">")
    assert "FlashTask" in cl.roots
    assert not cl.hit(cave_lo, cave_hi)
    # tempo_wide hook span 0x0C0D8DC2..0x0C0D8DCE must stay outside bootcrit.
    assert not cl.hit(0x0D8DC2, 0x0D8DCE)

    out_upd = tmp_path / "C850MAIN_MODDED.UPD"
    r = subprocess.run(
        [sys.executable, str(ROOT / "mods" / "patch_update.py"),
         "--model", "cdj850", str(C850), str(out_upd), "tempo_wide", "dsp_probe"],
        capture_output=True, text=True, check=False,
    )
    assert r.returncode == 0, r.stdout + "\n" + r.stderr
    assert "all checks passed" in r.stdout

    rv = subprocess.run(
        [sys.executable, str(ROOT / "mods" / "validate.py"),
         "--model", "cdj850", "--stock", str(C850), str(out_upd)],
        capture_output=True, text=True, check=False,
    )
    assert rv.returncode == 0, rv.stdout + "\n" + rv.stderr
    assert "all checks passed" in rv.stdout

    # Persistent settings sector 0x3F8000..0x400000 is untouched by patch_flash.
    flash = bytearray(FLASH_BIN.read_bytes())
    sentinel = b"CDJ850_PERSIST_SETTINGS_SECTOR_TEST"
    flash[0x3F8000:0x3F8000 + len(sentinel)] = sentinel
    rep = {}
    mod_flash = patch_main.patch_flash(
        bytes(flash), ["tempowide", "tempowidemath1", "tempowidemath2", "dspprobe"], "cdj850", rep
    )
    assert rep["tempowide"][0] == "applied"
    assert rep["tempowidemath1"][0] == "applied"
    assert rep["tempowidemath2"][0] == "applied"
    assert rep["dspprobe"][0] == "applied"
    assert mod_flash[0x3F8000:] == bytes(flash[0x3F8000:])


def test_cdj850_modded_flash_boots_and_runs_both_mods(tmp_path):
    binary = _cdj850_binary()
    if not FLASH_BIN.is_file():
        pytest.skip("extract/cdj850/flash_cdj850.bin is not present")
    if os.name == "nt":
        pytest.skip("QMP socket descriptor inheritance requires POSIX")

    flash_bytes = patch_main.patch_flash(
        FLASH_BIN.read_bytes(), ["tempowide", "tempowidemath1", "tempowidemath2", "dspprobe"], "cdj850"
    )
    mod_flash = tmp_path / "flash_modded.bin"
    mod_flash.write_bytes(flash_bytes)

    usb_img = tmp_path / "usb.img"
    _build_fat16_usb_image(usb_img, ["ALPHA.wav", "BETA.wav", "GAMMA.wav"])

    key_port = _free_udp_port()
    qmp_s, qmp_c = socket.socketpair()
    env = os.environ.copy()
    for k in ("CDJ850_DSP_BOOT_HLE", "CDJ850_PF16_HIGH", "CDJ850_DSP_PLAY_PF"):
        env.pop(k, None)
    env["CDJ_PANEL_KEYSOCK"] = str(key_port)
    env["CDJ850_PANEL_SCRIPT"] = "6000:usb:200,9000:enter:200,11000:rot+1,13000:enter:200"

    p = subprocess.Popen(
        [
            binary, "-M", "cdj850",
            "-display", "none", "-monitor", "none", "-serial", "none",
            "-icount", "shift=2,sleep=off",
            "-drive", f"if=pflash,format=raw,file={mod_flash},snapshot=on",
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

        loaded = _wait_cond(
            lambda: any("mode=0x0a" in ln and "[BETA.wav] | [] | []" in ln for ln in panel_logs)
                    and _qmp_read(qmp, PLAYER_STATE) == 3,
            timeout=150.0,
            pump=pump,
        )
        assert loaded, "modded flash failed to load track:\n" + "\n".join(panel_logs)

        # Verify DSP56371 (with edited seg4 JSR $0080) boots all stages and advances READ_INDEX.
        idx0 = _qmp_read(qmp, READ_INDEX, "h")
        assert _wait_cond(lambda: _qmp_read(qmp, READ_INDEX, "h") > idx0 + 8,
                          timeout=20.0, pump=pump)

        # Switch to CDJ mode, pull pitch to full '+', check stock ±10% (+10% = ~0x08CCCD).
        _tap(key_port, 0x13, 0x02)
        time.sleep(0.5)
        _send_udp(key_port, "0x05:0x00:0:lvl")
        _send_udp(key_port, "0x06:0x00:0:lvl")
        assert _wait_cond(
            lambda: (0x08B000 <= _qmp_read(qmp, SPEED, "w") <= 0x08F000)
                    and (_qmp_read(qmp, SHI_CMD0, "h") & 0x8000) == 0,
            timeout=15.0, pump=pump,
        ), f"initial ±10% pitch did not reach ~+10% ({_qmp_read(qmp, SPEED, 'w'):#x})"

        # Tap TEMPO once: tempowide hook skips ±16% (~0x0947AE) straight to WIDE +100% (~0x100000).
        _tap(key_port, 0x13, 0x08)
        wide = _wait_cond(
            lambda: _qmp_read(qmp, SPEED, "w") >= 0x0F8000,
            timeout=60.0, pump=pump,
        )
        assert wide, f"tempowide did not step straight to WIDE (SPEED={_qmp_read(qmp, SPEED, 'w'):#x})"
    finally:
        sel.close()
        qmp.close()
        qmp_s.close()
        p.terminate()
        p.wait(timeout=10)
