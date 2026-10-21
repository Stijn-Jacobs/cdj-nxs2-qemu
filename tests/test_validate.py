# SPDX-License-Identifier: GPL-2.0-or-later
"""emulator/mods/validate.py and bootcrit.py: the checks every patched update
passes before patch_update.py writes it, the free-space proof and the cave
profile in sigpatch.py.

The synthetic tests build tiny images. The ones on the real updates skip when
the firmware is not present, like the rest of the suite."""
import hashlib
import os
import struct

import pytest

import bootcrit
import modelfw
import patch_gui
import patch_main
import sigpatch
import validate
from players import cdj2000nxs2 as nxs2
from helpers import path, run_script

REAL_NXS2 = path("..", "fw", "unzipped", "C2KNXS2.UPD")
needs_nxs2 = pytest.mark.skipif(not os.path.exists(REAL_NXS2), reason="fw/unzipped/C2KNXS2.UPD not present")
REAL_NXS_MAIN = path("..", "extract", "cdj2000nxs", "main_unpacked.bin")
needs_nxs_main = pytest.mark.skipif(not os.path.exists(REAL_NXS_MAIN), reason="extract/cdj2000nxs not present")
REAL_XDJ700 = path("..", "fw", "crossmodel", "XDJ700_v115", "XDJ700.UPD")
needs_xdj700 = pytest.mark.skipif(not os.path.exists(REAL_XDJ700), reason="XDJ700.UPD 1.15 not present")

ROWS = ("osc", "usb_midi", "ableton_link", "tcnet", "three_band", "phrase")
PROFILES = {"main": nxs2.MAIN_PROFILE, "gui": nxs2.GUI_PROFILE}


# -- sigpatch.py: free space is proven, not assumed ------------------------------

def xip_image(table_to):
    """A MAIN-shaped image with a 0x40-byte run of 0xFF at 0x40 and a table of
    three addresses at 0xC0 that point at `table_to`, `table_to + 0x10`, ..."""
    flash_base = 0x08000000
    profile = sigpatch.ImageProfile(flash_base=flash_base, loadtab_off=0, loadtab_n=0,
                                    ram_base=None, ram_end=None, byteorder="<")
    sig = [0xAAA1, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888]
    d = bytearray(0x100)
    for i, v in enumerate([0x1010, 0x2020] + sig + [0xE0E0, 0xF0F0]):
        struct.pack_into("<H", d, 0x10 + 2 * i, v)
    d[0x40:0x80] = b"\xff" * 0x40
    for k in range(3):
        struct.pack_into("<I", d, 0xC0 + 4 * k, flash_base + table_to + 0x10 * k)
    mod = sigpatch.Mod(what="xip test mod", target="test", fw_versions=("9.99",),
                       sig=sig, start=0, end=len(sig), args=[], literal=False)
    return profile, bytes(d), {"xip": mod}


def routine(tmp_path):
    blob = tmp_path / "xip.bin"
    blob.write_bytes(b"\x01\x02\x03\x04")
    return lambda name: str(blob)


def test_a_run_of_ff_that_an_address_table_points_into_is_not_free(tmp_path):
    profile, data, mods = xip_image(0x40)           # the table points into the run: it is data
    report = {}
    out = sigpatch.patch(profile, routine(tmp_path), data, mods, ["xip"], report=report)
    assert out == data
    assert report["xip"][0] == "skipped" and "not proven free" in report["xip"][1]


def test_an_unreferenced_run_of_ff_is_free(tmp_path):
    profile, data, mods = xip_image(0xA0)           # the table points elsewhere
    report = {}
    out = sigpatch.patch(profile, routine(tmp_path), data, mods, ["xip"], report=report)
    assert report["xip"][0] == "applied"
    assert out != data


# -- sigpatch.py: the cave -------------------------------------------------------

CAVE_AT, CAVE_END = 0x300, 0x380


def cave_image(fill=0x09):
    """A MAIN-shaped image whose shell table (string address, handler address)
    names two handlers that start the cave and one that follows it. There is
    no erased run in it."""
    flash_base = 0x08000000
    d = bytearray(b"\x00" * 0x400)
    for i, v in enumerate([0x1010, 0x2020, 0xAAA1, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888,
                           0xE0E0, 0xF0F0]):
        struct.pack_into("<H", d, 0x10 + 2 * i, v)
    strings = {b"show a": 0x200, b"show b": 0x208, b"next": 0x210}
    for name, at in strings.items():
        d[at:at + len(name) + 1] = name + b"\0"
    for slot, (name, handler) in enumerate([(b"show a", 0x300), (b"show b", 0x340), (b"next", CAVE_END)]):
        struct.pack_into("<II", d, 0x220 + 8 * slot, flash_base + strings[name], flash_base + handler)
    d[CAVE_AT:CAVE_END] = bytes([fill]) * (CAVE_END - CAVE_AT)
    cave = sigpatch.Cave((b"show a", b"show b"), b"next", hashlib.sha256(d[CAVE_AT:CAVE_END]).hexdigest())
    profile = sigpatch.ImageProfile(flash_base=flash_base, loadtab_off=0, loadtab_n=0,
                                    ram_base=None, ram_end=None, byteorder="<", cave=cave)
    sig = [0xAAA1, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888]
    mod = sigpatch.Mod(what="cave test mod", target="test", fw_versions=("9.99",),
                       sig=sig, start=0, end=len(sig), args=[], literal=False)
    return profile, bytes(d), {"xip": mod}


def test_the_cave_is_found_by_its_handler_strings():
    profile, data, _ = cave_image()
    assert sigpatch.find_cave(sigpatch.Image(profile, data)) == (CAVE_AT, CAVE_END)


def test_a_mod_is_placed_in_the_cave_though_it_is_not_erased(tmp_path):
    profile, data, mods = cave_image()
    report = {}
    out = sigpatch.patch(profile, routine(tmp_path), data, mods, ["xip"], report=report)
    assert report["xip"][0] == "applied"
    assert out[CAVE_AT:CAVE_AT + 4] == b"\x01\x02\x03\x04"
    assert out[CAVE_AT + 4:CAVE_END] == data[CAVE_AT + 4:CAVE_END]


def test_a_cave_that_holds_other_bytes_is_refused(tmp_path):
    profile, data, mods = cave_image()
    other = bytearray(data)
    other[CAVE_AT + 8] ^= 0xFF
    report = {}
    out = sigpatch.patch(profile, routine(tmp_path), bytes(other), mods, ["xip"], report=report)
    assert out == bytes(other)
    assert report["xip"][0] == "skipped" and "other bytes" in report["xip"][1]


def test_the_validator_accepts_cave_space_and_refuses_the_same_bytes_elsewhere():
    profile, data, _ = cave_image()
    image = sigpatch.Image(profile, data)
    assert validate.cave_or_unproven(image, CAVE_AT, CAVE_AT + 16) is None
    assert "not erased" in validate.cave_or_unproven(image, 0x100, 0x110)


# -- validate.py: the checks, on synthetic images --------------------------------

def check_main(monkeypatch, spans, ranges, stock=None):
    stock = stock or b"\xff" * 0x1000
    out = bytearray(stock)
    for _, items in spans.items():
        for off, size, _ in items:
            out[off:off + size] = b"\x00" * size
    monkeypatch.setattr(bootcrit, "closure", lambda img, base=0, every_task=False: bootcrit.Closure(
        ranges, {0x100: "UpDtae_TASK"}, 3, {"UpDtae_TASK": 0}, []))
    profile = nxs2.MAIN_PROFILE
    rep = validate.Report()
    validate.check_main_image(stock, bytes(out), profile, spans, rep)
    return rep


def test_a_write_inside_boot_critical_code_is_refused_by_mod_site_and_routine(monkeypatch):
    stock = bytearray(b"\xff" * 0x1000)
    stock[0x150:0x158] = b"\x09\x00" * 4                 # a call site: code, not erased
    rep = check_main(monkeypatch, {"modx": [(0x150, 8, "site")]}, [(0x100, 0x200)], bytes(stock))
    failures = [text for state, text in rep.lines if state == "FAIL"]
    assert any("modx" in t and "site" in t and "0x8000150" in t and "routine 0x8000100" in t
               and "UpDtae_TASK" in t for t in failures)


def test_free_space_claimed_inside_boot_critical_code_is_refused(monkeypatch):
    rep = check_main(monkeypatch, {"modx": [(0x180, 16, "free")]}, [(0x100, 0x200)])
    assert any(state == "FAIL" and "modx" in text and "free" in text for state, text in rep.lines)


def test_writes_outside_boot_critical_code_pass(monkeypatch):
    stock = bytearray(b"\xff" * 0x1000)
    stock[0x550:0x558] = b"\x09\x00" * 4
    rep = check_main(monkeypatch, {"modx": [(0x550, 8, "site"), (0x800, 32, "free")]}, [(0x100, 0x200)], bytes(stock))
    assert not rep.failed, rep.text()


def test_a_change_no_mod_declared_is_refused(monkeypatch):
    stock = b"\xff" * 0x1000
    out = bytearray(stock)
    out[0x900:0x904] = b"\x00" * 4
    monkeypatch.setattr(bootcrit, "closure", lambda img, base=0, every_task=False: bootcrit.Closure(
        [(0x100, 0x200)], {}, 0, {"UpDtae_TASK": 0}, []))
    rep = validate.Report()
    validate.check_main_image(stock, bytes(out), nxs2.MAIN_PROFILE, {"modx": [(0x550, 8, "site")]}, rep)
    assert any(state == "FAIL" and "no mod declared" in text for state, text in rep.lines)


def test_free_space_an_address_table_points_into_is_refused_by_the_validator(monkeypatch):
    profile, data, _ = xip_image(0x40)
    out = bytearray(data)
    out[0x40:0x44] = b"\x01\x02\x03\x04"
    monkeypatch.setattr(bootcrit, "closure", lambda img, base=0, every_task=False: bootcrit.Closure(
        [], {}, 0, {"UpDtae_TASK": 0}, []))
    rep = validate.Report()
    validate.check_main_image(data, bytes(out), profile, {"modx": [(0x40, 4, "free")]}, rep)
    assert any(state == "FAIL" and "not proven free" in text for state, text in rep.lines)


def test_free_space_the_tasks_can_reach_is_refused_even_outside_the_boot_critical_set(monkeypatch):
    def closure(img, base=0, every_task=False):
        return bootcrit.Closure([(0x7F0, 0x810)] if every_task else [], {0x7F0: "other_TASK"}, 0, {"UpDtae_TASK": 0}, [])

    monkeypatch.setattr(bootcrit, "closure", closure)
    rep = validate.Report()
    stock = b"\xff" * 0x1000
    out = b"\xff" * 0x800 + b"\x00" * 32 + b"\xff" * 0x7E0
    validate.check_main_image(stock, out,
                              nxs2.MAIN_PROFILE, {"modx": [(0x800, 32, "free")]}, rep)
    assert any(state == "FAIL" and "entered by code the tasks reach" in text for state, text in rep.lines)


# -- validate.py: the real updates -----------------------------------------------

@pytest.fixture(scope="module")
def patched_nxs2(tmp_path_factory):
    out = tmp_path_factory.mktemp("nxs2") / "out.UPD"
    r = run_script("mods/patch_update.py", REAL_NXS2, out, *ROWS)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr
    return out.read_bytes()


def verify(stock, out, model="cdj2000nxs2"):
    return validate.verify(stock, out, model, profiles=PROFILES)


@needs_nxs2
def test_a_stock_update_validates_against_itself():
    data = open(REAL_NXS2, "rb").read()
    rep = verify(data, data)
    assert not rep.failed, rep.text()
    assert "emergency image sum 0x4c2a" in rep.text() and "packed MAIN sum 0x9499" in rep.text()


@needs_nxs2
def test_every_nxs2_mod_passes_the_validator(patched_nxs2):
    stock = open(REAL_NXS2, "rb").read()
    rep = verify(stock, patched_nxs2)
    assert not rep.failed, rep.text()
    text = rep.text()
    assert "emergency image sum 0x4c2a" in text and "packed MAIN sum" in text
    assert "none in boot-critical code" in text and "unreferenced and outside every declared segment" in text
    assert "NOT CHECKED:" in text and "has not been run" in text


@needs_nxs2
def test_the_nxs2_free_range_is_proven_free_where_the_routines_sit(patched_nxs2):
    stock = open(REAL_NXS2, "rb").read()
    fw = modelfw.load("cdj2000nxs2")
    image = sigpatch.Image(nxs2.MAIN_PROFILE, validate_image(stock, fw))
    run = sigpatch.largest_erased_run(image)
    assert sigpatch.unproven(image, run.start(), run.start() + 0x2D80) is None


def validate_image(data, fw):
    u = validate.Upd(data, fw)
    return validate.main_decode.decode(u.sections[fw.sections["main"] - 1], fw.main_off, verify=False)[3]


@needs_nxs2
def test_a_second_pass_over_a_patched_update_changes_nothing(patched_nxs2, tmp_path):
    src, out = tmp_path / "once.UPD", tmp_path / "twice.UPD"
    src.write_bytes(patched_nxs2)
    r = run_script("mods/patch_update.py", src, out, *ROWS)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr
    assert out.read_bytes() == patched_nxs2


@needs_nxs2
def test_a_stored_sum_that_does_not_match_is_caught(patched_nxs2):
    fw = modelfw.load("cdj2000nxs2")
    u = validate.Upd(patched_nxs2, fw)
    section = u.sections[fw.sections["main"] - 1]
    parsed = validate.Srec(section)
    run, _ = parsed.run(fw.main_off)
    sum_at = fw.main_off + 4 + struct.unpack_from("<I", run, 0)[0]
    lines = list(parsed.lines)
    for i, (line, addr) in enumerate(zip(parsed.lines, parsed.addrs)):
        if addr is not None and addr <= sum_at < addr + 32:
            rec = bytearray(bytes.fromhex(line[2:].decode()))
            rec[4 + sum_at - addr] ^= 1
            rec[-1] = (~sum(rec[:-1])) & 0xFF
            lines[i] = b"S2" + rec.hex().upper().encode()
    body = section[:validate.LABEL_SIZE] + b"\r\n".join(lines) + b"\r\n"
    tampered = body + struct.pack("<H", validate.crc16(body))
    start = u.header + sum(u.sizes[:fw.sections["main"] - 1])
    broken = patched_nxs2[:start] + tampered + patched_nxs2[start + len(section):]
    rep = verify(open(REAL_NXS2, "rb").read(), broken)
    assert any(state == "FAIL" and "stored sum" in text for state, text in rep.lines), rep.text()


@needs_nxs2
def test_a_changed_boot_loader_byte_is_caught(patched_nxs2):
    fw = modelfw.load("cdj2000nxs2")
    u = validate.Upd(patched_nxs2, fw)
    section = bytearray(u.sections[fw.sections["main"] - 1])
    parsed = validate.Srec(bytes(section))
    line = parsed.lines[1]                                  # the first record after the S0 header
    at = bytes(section).index(line)
    section[at + 12] = ord("0") if section[at + 12] != ord("0") else ord("1")
    start = u.header + sum(u.sizes[:fw.sections["main"] - 1])
    broken = patched_nxs2[:start] + bytes(section) + patched_nxs2[start + len(section):]
    rep = verify(open(REAL_NXS2, "rb").read(), broken)
    assert rep.failed


def main_sections(shrink_stock):
    """(stock section, output section) of the NXS2 MAIN; the stock one with its
    last data record dropped when `shrink_stock`, so the output is longer."""
    fw = modelfw.load("cdj2000nxs2")
    u = validate.Upd(open(REAL_NXS2, "rb").read(), fw)
    section = u.sections[fw.sections["main"] - 1]
    if not shrink_stock:
        return fw, section, section
    parsed = validate.Srec(section)
    last = max(i for i, addr in enumerate(parsed.addrs) if addr is not None)
    kept = [l for i, l in enumerate(parsed.lines) if i != last]
    body = section[:validate.LABEL_SIZE] + b"\r\n".join(kept) + b"\r\n"
    return fw, body + struct.pack("<H", validate.crc16(body)), section


@needs_nxs2
def test_a_player_without_a_known_window_may_not_grow_its_packed_main(monkeypatch):
    monkeypatch.setattr(modelfw, "update_window_end", lambda model: None)
    fw, stock, out = main_sections(shrink_stock=True)
    rep = validate.Report()
    validate.check_srec_section(2, stock, out, fw, "cdj2000nxs2", rep)
    assert any(state == "FAIL" and "no update window is known" in text for state, text in rep.lines), rep.text()
    fw, stock, out = main_sections(shrink_stock=False)
    rep = validate.Report()
    validate.check_srec_section(2, stock, out, fw, "cdj2000nxs2", rep)
    assert not any("update window" in text and state == "FAIL" for state, text in rep.lines), rep.text()


@needs_nxs2
def test_a_packed_main_past_the_known_window_is_caught(monkeypatch):
    fw, stock, out = main_sections(shrink_stock=False)
    monkeypatch.setattr(modelfw, "update_window_end", lambda model: 0x3B9000)
    rep = validate.Report()
    validate.check_srec_section(2, stock, out, fw, "cdj2000nxs2", rep)
    assert any(state == "FAIL" and "past the update window" in text for state, text in rep.lines), rep.text()


def test_a_display_site_outside_the_known_renderer_sites_is_refused():
    stock = bytearray(0x400)
    out = bytearray(stock)
    out[0x300:0x304] = bytes([0, 9, 0, 9])
    rep = validate.Report()
    validate.check_gui_image(bytes(stock), bytes(out), nxs2.GUI_PROFILE, {"newmod": [(0x300, 4, "site")]}, rep)
    assert any(state == "FAIL" and "not one of the known display sites" in text for state, text in rep.lines), rep.text()


@needs_nxs2
def test_a_stock_main_with_a_wrong_crc_trailer_is_refused():
    fw, stock, out = main_sections(shrink_stock=False)
    damaged = stock[:-2] + bytes([stock[-2] ^ 1, stock[-1]])
    rep = validate.Report()
    validate.check_srec_section(2, damaged, out, fw, "cdj2000nxs2", rep)
    assert any(state == "FAIL" and "stock CRC-16" in text for state, text in rep.lines), rep.text()


def test_every_player_with_mods_has_a_known_update_window():
    for model in patch_main.PATCH_SETS:
        assert modelfw.update_window_end(model)


@needs_nxs2
def test_the_closure_finds_the_tasks_and_none_of_the_nxs2_sites_in_it():
    stock = open(REAL_NXS2, "rb").read()
    fw = modelfw.load("cdj2000nxs2")
    img = validate_image(stock, fw)
    closure = bootcrit.closure(img, modelfw.main_base("cdj2000nxs2"))
    assert {"UpDtae_TASK", "PnlCom_SndTASK", "PnlCom_RcvTASK", "RdMngRcv_TASK", "RdMngExeUSB_TASK",
            "gui_comm", "root", "Idle"} <= set(closure.roots)
    assert closure.roots["UpDtae_TASK"] + 0x08000000 == 0x08118714
    for site in (0x08289C54, 0x0828A592, 0x082D872C, 0x082DB248, 0x083E1B8A,
                 0x08448F9E, 0x0849F83A, 0x084B4540, 0x084E7CAA, 0x084F6250):
        assert not closure.hit(site - 0x08000000, site - 0x08000000 + 32)
    assert closure.hit(0x084F4C7C - 0x08000000, 0x084F4C7C - 0x08000000 + 2)       # the updater itself


def test_the_sh7763_players_start_their_main_at_physical_dram():
    assert modelfw.main_base("cdj2000nxs") == modelfw.main_base("cdj900") == 0x04000000
    assert modelfw.main_base("cdj2000nxs2") == modelfw.main_base("xdj700") == 0x08000000


@needs_nxs_main
def test_the_cdj2000nxs_closure_is_found_from_its_own_base():
    img = open(REAL_NXS_MAIN, "rb").read()
    closure = bootcrit.closure(img, modelfw.main_base("cdj2000nxs"))
    assert {"UpDtae_TASK", "PnlCom_SndTASK", "RdMngExeUSB_TASK", "GuiCom_SndTASK", "root", "Idle"} <= set(closure.roots)
    assert closure.hit(closure.roots["UpDtae_TASK"], closure.roots["UpDtae_TASK"] + 2)


def test_the_files_next_to_a_bare_main_file_must_be_there_and_hold_together(tmp_path, monkeypatch):
    shipped = b"CDJ-2000 DRIV   Ver1.01         S9030000FC\r\n"
    crc = validate.crc16(shipped)
    shipped += struct.pack("<H", crc)
    (tmp_path / "C2KDRIV.UPD").write_bytes(shipped)
    (tmp_path / "C2KPANL.UPD").write_bytes(shipped[:-1] + b"\x00")
    monkeypatch.setattr(modelfw, "other_files", lambda model: [
        ("C2KDRIV.UPD", hashlib.sha256(shipped).hexdigest()), ("C2KPANL.UPD", "0" * 64), ("C2KGUI.UPD", "0" * 64)])
    rep = validate.Report()
    found = validate.check_other_files(str(tmp_path), "cdj2000", rep)
    assert [name for name, _ in found] == ["C2KDRIV.UPD", "C2KPANL.UPD"]
    states = {text.split(":")[0].split(" ")[0]: state for state, text in rep.lines}
    assert states == {"C2KDRIV.UPD": "PASS", "C2KPANL.UPD": "FAIL", "C2KGUI.UPD": "FAIL"}


def test_only_a_player_with_one_file_per_section_has_other_files():
    assert [n for n, _ in modelfw.other_files("cdj2000")] == ["C2KGUI.UPD", "C2KDRIV.UPD", "C2KPANL.UPD"]
    assert [n for n, _ in modelfw.other_files("cdj900")] == ["C900GUI.UPD", "C900DRIV.UPD", "C900PANL.UPD"]
    assert modelfw.other_files("cdj2000nxs2") == []


@needs_xdj700
def test_a_cave_mod_passes_the_validator_run_on_its_own(tmp_path):
    out = tmp_path / "out.UPD"
    r = run_script("mods/patch_update.py", "--model", "xdj700", REAL_XDJ700, out, "track_filter")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr
    r = run_script("mods/validate.py", "--model", "xdj700", REAL_XDJ700, out)
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr
