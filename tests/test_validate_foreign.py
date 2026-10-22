# SPDX-License-Identifier: GPL-2.0-or-later
"""emulator/mods/validate.py on a file of unknown origin: player and version
from the labels, the official copy, and the three outcomes (0 safe as far as
checked, 1 failed, 2 not shown safe).

The synthetic tests need no firmware. The ones on real updates skip when the
file is not present, like the rest of the suite."""
import hashlib
import os
import struct

import pytest

import bootcrit
import main_decode
import main_encode
import modelfw
import validate
from helpers import path, run_script

REAL_NXS2 = path("..", "fw", "unzipped", "C2KNXS2.UPD")
REAL_XDJ700 = path("..", "fw", "crossmodel", "XDJ700_v115", "XDJ700.UPD")
REAL_XDJ700_OLD = path("..", "fw", "crossmodel", "XDJ700_v114", "XDJ700.UPD")
REAL_NXS = path("..", "fw", "crossmodel", "CDJ2000NXS_v144", "C2KNXS.UPD")
REAL_NXS_OLD = path("..", "fw", "crossmodel", "CDJ2000NXS_v143", "C2KNXS.UPD")
REAL_C2K = path("..", "fw", "crossmodel", "CDJ-2000v433", "CDJ-2000v433", "C2KMAIN.UPD")


def needs(*files):
    return pytest.mark.skipif(not all(os.path.exists(f) for f in files),
                              reason="%s not present" % ", ".join(os.path.basename(f) for f in files))


def label(text):
    return text.encode("latin1").ljust(validate.LABEL_SIZE, b" ")


def container(*labels):
    sections = [label(text) + b"\0" * 8 for text in labels]
    return b"".join(b"%d\r\n" % len(s) for s in sections) + b"".join(sections)


# -- the player and version are read from the labels ------------------------------

@pytest.mark.parametrize("labels, want", [
    (["XDJ-700     MAINVer1.15", "XDJ-700     PANLVer1.00"], ("xdj700", "MAIN", "1.15")),
    (["CDJ-2000NXS2GUI Ver1.81", "CDJ-2000NXS2MAINVer1.87", "CDJ-2000NXS2PANLVer1.00"], ("cdj2000nxs2", "MAIN", "1.87")),
    (["CDJ-2000NXS GUI Ver1.200", "CDJ-2000NXS MAINVer1.44"], ("cdj2000nxs", "MAIN", "1.44")),
    (["XDJ-1000MK2 MAINVer1.45"], ("xdj1000mk2", "MAIN", "1.45")),
    (["XDJ-1000    MAINVer1.13"], ("xdj1000", "MAIN", "1.13")),
    (["CDJ-900NXS  MAINVer1.31"], ("cdj900nxs", "MAIN", "1.31")),
])
def test_the_player_and_version_come_from_the_main_label(labels, want):
    assert validate.identify(container(*labels)) == want


def test_the_cdj2000_and_cdj900_files_name_their_player_by_any_of_the_four():
    assert validate.identify(label("CDJ-2000 MAIN   Ver4.33") + b"\0" * 8) == ("cdj2000", "MAIN", "4.33")
    assert validate.identify(label("CDJ-900 GUI     Ver4.200") + b"\0" * 8) == ("cdj900", "GUI", "4.200")


def test_a_file_with_no_known_label_is_not_an_update(tmp_path):
    f = tmp_path / "junk.UPD"
    f.write_bytes(os.urandom(4096))
    r = run_script("mods/validate.py", f)
    assert r.returncode == 1 and "not a firmware update" in r.stdout


# -- the exit status ------------------------------------------------------------

def test_exit_status_is_failed_over_open_over_clean():
    rep = validate.Report()
    assert rep.exit_code == 0
    rep.unproven("a write nobody can place")
    assert rep.exit_code == 2 and "NOT SHOWN SAFE" in rep.text() and "FAILED" not in rep.text()
    rep.fail("a sum")
    assert rep.exit_code == 1 and "FAILED" in rep.text()


def test_an_official_copy_is_found_by_its_sha(tmp_path, monkeypatch):
    stock = b"official bytes"
    monkeypatch.setattr(modelfw, "main_file", lambda model: ("XDJ700.UPD", hashlib.sha256(stock).hexdigest()))
    (tmp_path / "XDJ700.UPD").write_bytes(b"someone's edit")
    assert validate.find_stock("xdj700", str(tmp_path)) is None
    (tmp_path / "XDJ700.UPD").write_bytes(stock)
    assert validate.find_stock("xdj700", str(tmp_path)) == str(tmp_path / "XDJ700.UPD")


def test_a_write_into_erased_bytes_something_points_at_is_open_not_failed_without_a_declaration(monkeypatch):
    from test_validate import xip_image
    profile, data, _ = xip_image(0x40)
    out = bytearray(data)
    out[0x40:0x44] = b"\x01\x02\x03\x04"
    monkeypatch.setattr(bootcrit, "closure", lambda img, base=0, every_task=False: bootcrit.Closure(
        [], {}, 0, {"UpDtae_TASK": 0}, []))
    rep = validate.Report()
    validate.check_main_image(data, bytes(out), profile, None, rep)
    assert not rep.failed and rep.open, rep.text()


def test_a_rewrite_of_code_longer_than_a_call_site_fails_without_a_declaration(monkeypatch):
    stock = bytearray(b"\xff" * 0x1000)
    stock[0x500:0x540] = b"\x09\x00" * 32
    out = bytearray(stock)
    out[0x500:0x540] = b"\x00" * 64
    monkeypatch.setattr(bootcrit, "closure", lambda img, base=0, every_task=False: bootcrit.Closure(
        [(0x100, 0x200)], {}, 0, {"UpDtae_TASK": 0}, []))
    rep = validate.Report()
    validate.check_main_image(bytes(stock), bytes(out), validate.default_main_profile("xdj700"), None, rep)
    assert any(state == "FAIL" and "more than a call site" in text for state, text in rep.lines), rep.text()


def test_a_model_without_patches_still_gets_its_writes_judged(monkeypatch):
    """No image profile is no reason to fail: the write below lies outside every
    task, in erased bytes nothing points at, and is accepted."""
    stock = b"\xff" * 0x1000
    out = bytearray(stock)
    out[0x800:0x820] = b"\x00" * 32
    monkeypatch.setattr(bootcrit, "closure", lambda img, base=0, every_task=False: bootcrit.Closure(
        [(0x100, 0x200)], {}, 0, {"UpDtae_TASK": 0}, []))
    rep = validate.Report()
    validate.check_main_image(stock, bytes(out), validate.default_main_profile("cdj2000nxs"), None, rep)
    assert rep.exit_code == 0, rep.text()


def test_a_player_whose_main_has_no_update_task_leaves_a_code_write_open(monkeypatch):
    stock = bytearray(b"\xff" * 0x1000)
    stock[0x500:0x508] = b"\x09\x00" * 4
    out = bytearray(stock)
    out[0x500:0x508] = b"\x00" * 8
    monkeypatch.setattr(bootcrit, "closure", lambda img, base=0, every_task=False: bootcrit.Closure(
        [], {}, 0, {}, []))
    rep = validate.Report()
    validate.check_main_image(bytes(stock), bytes(out), validate.default_main_profile("cdj2000nxs"), None, rep)
    assert rep.exit_code == 2, rep.text()


# -- real updates -----------------------------------------------------------------

def run_validate(*args):
    r = run_script("mods/validate.py", *args)
    return r.returncode, r.stdout + r.stderr


@pytest.mark.parametrize("real, title", [
    (REAL_NXS2, "CDJ-2000NXS2"), (REAL_XDJ700, "XDJ-700"), (REAL_C2K, "CDJ-2000"), (REAL_NXS, "CDJ-2000NXS")])
def test_an_official_update_is_named_and_passes(real, title):
    if not os.path.exists(real):
        pytest.skip("%s not present" % real)
    code, text = run_validate(real)
    assert code == 0 and "is the official %s update" % title in text, text


def remake(real, model, edit):
    """The update `real` with its MAIN image changed by edit(bytearray), the
    section re-encoded so every sum in the file is right again."""
    fw = modelfw.load(model)
    upd = validate.Upd(open(real, "rb").read(), fw)
    at = fw.sections["main"] - 1
    section = upd.sections[at]
    image = bytearray(main_decode.decode(section, fw.main_off, verify=False)[3])
    edit(image)
    sections = list(upd.sections)
    sections[at] = main_encode.encode(section, bytes(image), fw.main_off)
    manifest = b"" if fw.bare else b"".join(b"%d\r\n" % len(s) for s in sections)
    return manifest + b"".join(sections) + upd.trailer


def main_image(real, model):
    fw = modelfw.load(model)
    section = validate.Upd(open(real, "rb").read(), fw).sections[fw.sections["main"] - 1]
    return main_decode.decode(section, fw.main_off, verify=False)[3]


@pytest.fixture(scope="module")
def nxs_image():
    return main_image(REAL_NXS, "cdj2000nxs")


@needs(REAL_NXS)
def test_a_write_into_unreached_erased_space_of_an_older_deck_passes(nxs_image, tmp_path):
    base = modelfw.main_base("cdj2000nxs")
    at = 0x23D0
    assert set(nxs_image[at:at + 928]) == {0xFF}
    mod = tmp_path / "C2KNXS.UPD"

    def edit(image):
        image[at:at + 32] = b"\x09\x00" * 16

    mod.write_bytes(remake(REAL_NXS, "cdj2000nxs", edit))
    code, text = run_validate("--stock", REAL_NXS, mod)
    assert code == 0, text
    assert "%#x-%#x" % (base + at, base + at + 32) in text and "in erased space" in text
    assert "section 3" in text and "bytes (+" in text


@needs(REAL_NXS)
def test_a_changed_byte_in_boot_critical_code_of_an_older_deck_fails(nxs_image, tmp_path):
    entry = bootcrit.closure(nxs_image, modelfw.main_base("cdj2000nxs")).roots["UpDtae_TASK"]
    mod = tmp_path / "C2KNXS.UPD"
    stock_entry = struct_entry(nxs_image, entry)

    def edit(image):
        image[stock_entry + 2] ^= 0xFF

    mod.write_bytes(remake(REAL_NXS, "cdj2000nxs", edit))
    code, text = run_validate("--stock", REAL_NXS, mod)
    assert code == 1 and "overlaps boot-critical code" in text, text


def struct_entry(image, descriptor):
    """Offset of the update task's first instruction, from its task descriptor."""
    return struct.unpack_from("<I", image, descriptor)[0] - modelfw.main_base("cdj2000nxs")


@needs(REAL_NXS)
def test_a_found_official_copy_is_used_without_naming_it(nxs_image, tmp_path):
    mod = tmp_path / "C2KNXS.UPD"

    def edit(image):
        image[0x23D0:0x23D4] = b"\x09\x00\x09\x00"

    mod.write_bytes(remake(REAL_NXS, "cdj2000nxs", edit))
    code, text = run_validate(mod)
    assert code == 0 and "compared with the official update" in text, text


@needs(REAL_NXS)
def test_a_flipped_byte_with_the_sums_left_alone_fails(tmp_path):
    data = bytearray(open(REAL_NXS, "rb").read())
    data[len(data) // 2] ^= 0x01
    mod = tmp_path / "C2KNXS.UPD"
    mod.write_bytes(bytes(data))
    code, text = run_validate("--stock", REAL_NXS, mod)
    assert code == 1 and "FAIL" in text, text


@needs(REAL_XDJ700_OLD)
def test_a_flipped_byte_fails_with_no_official_copy_to_compare_with(tmp_path):
    data = bytearray(open(REAL_XDJ700_OLD, "rb").read())
    data[len(data) // 2] ^= 0x01
    mod = tmp_path / "XDJ700.UPD"
    mod.write_bytes(bytes(data))
    code, text = run_validate(mod)
    assert code == 1 and "FAIL" in text, text


@needs(REAL_XDJ700_OLD)
def test_an_official_file_of_a_version_with_no_stock_is_not_shown_safe():
    code, text = run_validate(REAL_XDJ700_OLD)
    assert code == 2 and "NOT SHOWN SAFE" in text and "to compare with" in text, text
    assert "FAIL" not in text.split("NOT CHECKED:")[0], text
    assert "checksums verify" in text and "packed application ends at" in text


@needs(REAL_NXS_OLD)
def test_an_older_deck_version_with_no_stock_is_not_shown_safe():
    code, text = run_validate(REAL_NXS_OLD)
    assert code == 2 and "update window" in text, text


@needs(REAL_XDJ700)
def test_our_own_patcher_output_passes_and_is_recognised(tmp_path):
    out = tmp_path / "XDJ700.UPD"
    r = run_script("mods/patch_update.py", "--model", "xdj700", REAL_XDJ700, out, "track_filter")
    assert r.returncode == 0, r.stdout[-2000:] + r.stderr
    code, text = run_validate(out)
    assert code == 0, text
    assert "writes the same bytes as our patcher does for: track_filter" in text, text
