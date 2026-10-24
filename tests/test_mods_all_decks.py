# SPDX-License-Identifier: GPL-2.0-or-later
"""The mod framework across players: the registry offers a player only its own
mods, the patch engine takes an image as it comes (stock, already carrying
our mods, or edited by someone else), finds its free space and the firmware
addresses a routine needs by searching the image, and the NXS2's patched
images and .UPD stay byte for byte what they were before models existed."""
import glob
import hashlib
import os
import struct
import sys

import pytest

import gui_encode
import main_decode
import main_encode
import modelfw
import patch_gui
import patch_main
import patch_update
import sigpatch
from players import cdj2000nxs2 as nxs2
from helpers import path, run_script
from test_mod_hooks import HOOKS, MODS as HOOK_MODS, build_image as build_hook_image
from test_patch_update import (build_two_mod_image, build_upd, fake_section, FakeTarget,
                               run_patch_update, write_blobs)

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import mods as launcher_mods  # noqa: E402
from launcher.layout import Layout  # noqa: E402


# -- the registry offers a player its own mods -----------------------------------

def keys(model_id):
    return [m.key for m in launcher_mods.load(Layout(), model_id)]


def test_the_nxs2_is_offered_every_mod_that_lists_it():
    assert keys("cdj2000nxs2") == [m.key for m in launcher_mods.load(Layout())
                                   if "cdj2000nxs2" in m.models.split(",")]


@pytest.mark.parametrize("model_id", ["cdj2000", "cdj2000nxs", "cdj900", "cdj900nxs",
                                      "xdj1000", "xdj1000mk2"])
def test_an_older_player_is_offered_no_mod_until_it_has_a_patch(model_id):
    assert keys(model_id) == []


def test_the_xdj700_is_offered_the_track_filter_and_the_nxs2_is_not():
    assert keys("xdj700") == ["track_filter"]
    assert "track_filter" not in keys("cdj2000nxs2")


def test_apply_leaves_a_mod_not_offered_for_the_model_alone():
    rows = launcher_mods.load(Layout(), "xdj1000")
    env = {}
    launcher_mods.apply(rows, {"CDJ_MAIN_OSC": "1"}, env)
    assert env == {}


def test_a_star_offers_a_mod_to_every_model():
    row = launcher_mods.Mod("k", "CDJ_X", "1", "0", "off", "*", "d")
    assert launcher_mods.supports(row, "xdj1000")


# -- per-model firmware facts and patch sets -------------------------------------

def test_the_firmware_layout_comes_from_the_models_profile():
    assert modelfw.load("cdj2000nxs2") == modelfw.Firmware({"main": 3, "gui": 1}, 0x50000)
    assert modelfw.load("xdj1000") == modelfw.Firmware({"main": 1}, 0x40000)
    assert modelfw.load("cdj900nxs") == modelfw.Firmware({"main": 2}, 0x40000)


def test_a_player_whose_update_is_one_file_per_section_has_a_bare_main_file():
    assert modelfw.load("cdj2000") == modelfw.Firmware({"main": 1}, 0x40000, True)
    assert modelfw.load("cdj900") == modelfw.Firmware({"main": 1}, 0x40000, True)


def test_a_bare_main_file_is_patched_without_a_manifest(tmp_path, monkeypatch, synthetic_main):
    from test_patch_main import build_main_section, build_xip_image
    profile, image, mods = build_xip_image()
    bp = write_blobs(tmp_path, mods, size=16)
    base = build_main_section(image)
    section = main_encode.encode(base, image, 0x40000)
    assert main_decode.decode(section, 0x40000)[3] == image
    src, out = tmp_path / "C2KMAIN.UPD", tmp_path / "out" / "C2KMAIN.UPD"
    out.parent.mkdir()
    src.write_bytes(section)
    (tmp_path / "C2KPANL.UPD").write_bytes(b"panel")
    monkeypatch.setattr(modelfw, "other_files", lambda model: [("C2KPANL.UPD", hashlib.sha256(b"panel").hexdigest())])
    fake = FakeTarget(profile, mods, bp)
    monkeypatch.setitem(patch_update.TARGETS, "main", (patch_update.TARGETS["main"][0],
                                                       patch_update.TARGETS["main"][1], fake))
    run_patch_update(monkeypatch, "--model", "cdj2000", src, out, "xip")
    _, _, _, patched = main_decode.decode(out.read_bytes(), 0x40000)
    assert patched == sigpatch.patch(profile, bp, image, mods, ["xip"])
    assert out.read_bytes()[:32] == section[:32]
    assert gui_encode.crc16_xmodem(out.read_bytes()[:-2]) == struct.unpack("<H", out.read_bytes()[-2:])[0]
    assert (out.parent / "C2KPANL.UPD").read_bytes() == b"panel"
    again = tmp_path / "again.UPD"
    run_patch_update(monkeypatch, "--model", "cdj2000", out, again, "xip")
    assert again.read_bytes() == out.read_bytes()


def test_a_container_given_for_a_one_file_per_section_player_is_refused(tmp_path):
    src = tmp_path / "in.UPD"
    src.write_bytes(build_upd(b"CDJ-2000 MAIN   Ver4.33        0"))
    r = run_script("mods/patch_update.py", "--model", "cdj2000", src, tmp_path / "out.UPD", "osc")
    assert r.returncode != 0


def test_every_routine_lives_in_its_players_folder():
    nxs2 = modelfw.routine_source("main", "osc", "cdj2000nxs2")
    assert nxs2 == os.path.join(EMU, "mods", "osc", "cdj2000nxs2", "main_osc.s")
    xdj = modelfw.routine_source("main", "trackfilter", "xdj700")
    assert xdj == os.path.join(EMU, "mods", "track_filter", "xdj700", "main_trackfilter.s")


def declarations():
    """(folder, its patch.py) for every mods/<mod>/<model>/ that patches something."""
    for patch in sorted(glob.glob(os.path.join(EMU, "mods", "*", "*", "patch.py"))):
        folder = os.path.dirname(patch)
        yield folder, modelfw.mod_patch(os.path.basename(os.path.dirname(folder)), os.path.basename(folder))


def test_a_mod_folder_holds_every_routine_it_declares():
    for folder, decl in declarations():
        for name, mod in decl.MODS.items():
            if isinstance(mod, sigpatch.DataPatch):
                continue
            for ext in (".s", ".bin"):
                assert os.path.isfile(os.path.join(folder, "%s_%s%s" % (mod.target, name, ext))), (folder, name)


def test_no_two_mods_hand_words_over_in_the_same_bytes():
    owner = {}
    for folder, decl in declarations():
        model = os.path.basename(folder)
        for mod in decl.MODS.values():
            for _, source in getattr(mod, "imports", ()):
                if isinstance(source, sigpatch.FreeEnd):
                    for b in range(source.delta, source.delta + 4):
                        assert owner.setdefault((model, b), folder) == folder, (folder, source)


def test_a_model_with_no_patch_set_gets_its_image_back_and_a_reason():
    report = {}
    out = patch_main.patch(b"image", ["osc"], "xdj1000", report)
    assert out == b"image"
    assert report == {"osc": ("skipped", "no patch for xdj1000")}


def test_list_shows_nothing_for_a_model_without_patches():
    for tool in ("patch_main.py", "patch_gui.py", "patch_update.py"):
        r = run_script("mods/" + tool, "--model", "xdj1000", "--list")
        assert r.returncode == 0 and r.stdout == "", tool
    assert "wave3data" in run_script("mods/patch_main.py", "--model", "cdj2000nxs2", "--list").stdout


def test_a_mod_with_nothing_for_the_model_is_refused_by_patch_update(tmp_path):
    r = run_script("mods/patch_update.py", "--model", "xdj1000", tmp_path / "in.UPD",
                   tmp_path / "out.UPD", "osc")
    assert r.returncode != 0
    assert "has nothing to write into a xdj1000 .UPD" in r.stderr


# -- the NXS2's output has not moved ---------------------------------------------

REAL = {name: path("..", "extract", name) for name in ("main_unpacked.bin", "gui_unpacked.bin")}
needs_real = pytest.mark.skipif(not all(os.path.exists(p) for p in REAL.values()),
                                reason="the NXS2 images are not extracted here")

MAIN_ALL = ["wave3data", "wave3detail", "wave3ovdata", "wave3ovfetch", "phrasefetch", "phrasedata",
            "abletonlink", "abletonlinkpong", "osc", "oscbeat", "tcnet", "tcnettime", "tcnetdata",
            "usbmidi", "usbmiditick", "gatecue", "gatecuepress",
            "beatjumplong"]

# SHA-256 of what patch_main.py / patch_gui.py write for these selections. The
# routines are laid out for the whole registry, so a new NXS2 MAIN routine
# moves the ones sorted after it and every trampoline: the MAIN pins change
# with it; a new display routine makes the display's reserved block, and so
# its image, bigger.
PINNED = [
    ("main_unpacked.bin", patch_main, MAIN_ALL,
     "a30de5cf07fa4b7ceb1af019e124ff4ce4e0b8f4d4f28480fa553cf1c5cd0365"),
    ("main_unpacked.bin", patch_main, ["osc", "oscbeat"],
     "32194636cfba2c73dd48722d2dc0cd0254f66d35accedeef4157c180761749fa"),
    ("main_unpacked.bin", patch_main, ["gatecue", "gatecuepress"],
     "d37a938d3791f5c6158b3b7814ade1ddf3a4ba0586cabc89354dcf8c0fd42c37"),
    ("main_unpacked.bin", patch_main, ["beatjumplong"],
     "c44fe3b05df50cdbacbaebc7778adbe624609e0d29cfbddd1fc2ee12359fed63"),
    ("gui_unpacked.bin", patch_gui, ["wave3", "wave3ov", "phrase"],
     "777d3290ecca3a616cf9b470ec0d0ac724c62bce3e16bc9201339b1e9d056e5e"),
    ("gui_unpacked.bin", patch_gui, ["wave3"],
     "07f62cdfa4e6c3064ccc5dbe0aec4ccafc326ec2e8af47b8cecfb2f9c00a4112"),
]


@needs_real
@pytest.mark.parametrize("image, tool, names, digest", PINNED)
def test_the_nxs2_images_patch_to_the_same_bytes_as_before(image, tool, names, digest):
    with open(REAL[image], "rb") as f:
        out = tool.patch(f.read(), names)
    assert hashlib.sha256(out).hexdigest() == digest
    assert tool.patch(out, names) == out


@needs_real
def test_the_dynamic_free_space_is_where_the_nxs2_profile_puts_it():
    with open(REAL["main_unpacked.bin"], "rb") as f:
        img = sigpatch.Image(nxs2.MAIN_PROFILE, f.read())
    dynamic = nxs2.MAIN_PROFILE._replace(ram_base=None, ram_end=None)
    size = 11648
    assert (sigpatch.place_inplace(img, size)
            == sigpatch.place_inplace(sigpatch.Image(dynamic, img.d), size))


# -- an image taken as it comes ---------------------------------------------------

def patched_pair(tmp_path):
    profile, data, mods = build_two_mod_image()
    return profile, data, mods, write_blobs(tmp_path, mods, size=16)


def test_a_stock_image_gets_every_mod(tmp_path):
    profile, data, mods, bp = patched_pair(tmp_path)
    report = {}
    sigpatch.patch(profile, bp, data, mods, ["mod_a", "mod_b"], report=report)
    assert report == {"mod_a": ("applied", ""), "mod_b": ("applied", "")}


def test_an_image_already_carrying_our_mods_is_left_as_it_is(tmp_path):
    profile, data, mods, bp = patched_pair(tmp_path)
    once = sigpatch.patch(profile, bp, data, mods, ["mod_a", "mod_b"])
    report = {}
    twice = sigpatch.patch(profile, bp, once, mods, ["mod_a", "mod_b"], report=report)
    assert twice == once
    assert report == {"mod_a": ("present", ""), "mod_b": ("present", "")}


def test_a_foreign_edit_inside_a_signature_leaves_that_mod_out_and_the_rest_in(tmp_path):
    profile, data, mods, bp = patched_pair(tmp_path)
    site = sigpatch.find_sig(sigpatch.Image(profile, data), mods["mod_a"])
    edited = bytearray(data)
    struct.pack_into(">H", edited, site + 2 * 4, 0x1234)        # someone else's change in A's span
    report = {}
    out = sigpatch.patch(profile, bp, bytes(edited), mods, ["mod_a", "mod_b"], report=report)
    assert report["mod_a"] == ("skipped", "call site found 0 times")
    assert report["mod_b"] == ("applied", "")
    assert out[site:site + 2 * len(mods["mod_a"].sig)] == edited[site:site + 2 * len(mods["mod_a"].sig)]
    assert out == sigpatch.patch(profile, bp, bytes(edited), mods, ["mod_b"])


def test_a_foreign_edit_beside_a_signature_changes_nothing_for_the_mod(tmp_path):
    profile, data, mods, bp = patched_pair(tmp_path)
    site = sigpatch.find_sig(sigpatch.Image(profile, data), mods["mod_a"])
    edited = bytearray(data)
    struct.pack_into(">H", edited, site - 2, 0x1234)            # the word just before the signature
    report = {}
    out = sigpatch.patch(profile, bp, bytes(edited), mods, ["mod_a", "mod_b"], report=report)
    assert {state for state, _ in report.values()} == {"applied"}
    assert out[site - 2:site] == edited[site - 2:site]


def test_a_foreign_edit_elsewhere_in_a_patched_image_keeps_our_mods_recognised(tmp_path):
    profile, data, mods, bp = patched_pair(tmp_path)
    once = bytearray(sigpatch.patch(profile, bp, data, mods, ["mod_a", "mod_b"]))
    once[0x04] ^= 0xFF                                           # an unrelated byte
    report = {}
    assert sigpatch.patch(profile, bp, bytes(once), mods, ["mod_a", "mod_b"], report=report) == bytes(once)
    assert {state for state, _ in report.values()} == {"present"}


def test_an_edit_inside_our_call_site_is_not_taken_for_our_patch(tmp_path):
    profile, data, mods, bp = patched_pair(tmp_path)
    once = bytearray(sigpatch.patch(profile, bp, data, mods, ["mod_a"]))
    site = sigpatch.find_sig(sigpatch.Image(profile, data), mods["mod_a"]) + 2 * mods["mod_a"].start
    once[site + 1] ^= 0x10
    report = {}
    sigpatch.patch(profile, bp, bytes(once), mods, ["mod_a"], report=report)
    assert report["mod_a"][0] == "skipped"


def test_space_taken_by_someone_else_leaves_the_mods_out(tmp_path):
    profile, data, mods, bp = patched_pair(tmp_path)
    seg = sigpatch.place_segment(sigpatch.Image(profile, data), 64)[2]
    taken = bytearray(data)
    taken[seg + 4] = 0x00
    report = {}
    out = sigpatch.patch(profile, bp, bytes(taken), mods, ["mod_a", "mod_b"], report=report)
    assert out == bytes(taken)
    assert {state for state, _ in report.values()} == {"skipped"}


def test_hooks_on_a_patched_site_are_recognised_and_a_new_one_is_refused(tmp_path):
    for name, ops in HOOKS.items():
        (tmp_path / name).write_bytes(struct.pack("<%dH" % len(ops), *ops))
    bp = lambda name: str(tmp_path / name)               # noqa: E731
    profile, data = build_hook_image()
    once = sigpatch.patch(profile, bp, data, HOOK_MODS, ["a"])
    report = {}
    assert sigpatch.patch(profile, bp, once, HOOK_MODS, ["a"], report=report) == once
    assert report == {"a": ("present", "")}
    report = {}
    assert sigpatch.patch(profile, bp, once, HOOK_MODS, ["a", "b"], report=report) == once
    assert report["a"] == ("present", "")
    assert report["b"][0] == "skipped" and "earlier patch" in report["b"][1]


def test_a_second_update_pass_changes_no_byte_of_the_update(tmp_path, monkeypatch):
    profile, image, mods = build_two_mod_image()
    bp = write_blobs(tmp_path, mods, size=16)
    fake = FakeTarget(profile, mods, bp)
    monkeypatch.setitem(patch_update.TARGETS, "gui", (
        lambda section, fw: __import__("gui_decode").decode(section),
        lambda section, patched, fw: gui_encode.encode(section[:32], patched), fake))
    src = tmp_path / "in.UPD"
    src.write_bytes(build_upd(fake_section("FAKE-GUI Ver9.99", image)))
    once, twice = tmp_path / "once.UPD", tmp_path / "twice.UPD"
    run_patch_update(monkeypatch, src, once, "mod_a", "mod_b")
    run_patch_update(monkeypatch, once, twice, "mod_a", "mod_b")
    assert once.read_bytes() == twice.read_bytes()


def test_an_update_whose_s_record_checksum_is_wrong_still_decodes():
    image = bytes(range(256)) * 8
    rec = main_decode.srecords  # noqa: F841
    from test_patch_main import build_main_section
    section = bytearray(build_main_section(image))
    line = section.index(b"S2", 32)
    section[line + 10] = ord("0") if section[line + 10] != ord("0") else ord("1")
    with pytest.raises(ValueError, match="bad S-record checksum"):
        main_decode.decode(bytes(section))
    assert main_decode.decode(bytes(section), verify=False)[3] != b""


# -- space and addresses found by searching the image ----------------------------

def test_the_largest_erased_run_is_used_when_the_profile_names_none():
    d = bytearray(b"\x00" * 0x400)
    d[0x40:0x80] = b"\xff" * 0x40
    d[0x103:0x1A0] = b"\xff" * 0x9D
    profile = sigpatch.ImageProfile(flash_base=0x8000, loadtab_off=0, loadtab_n=0,
                                    ram_base=None, ram_end=None, byteorder="<")
    assert sigpatch.place_inplace(sigpatch.Image(profile, d), 0x20) == (None, 0x8110, 0x110)
    with pytest.raises(SystemExit, match="no erased span of 256 bytes in the image"):
        sigpatch.place_inplace(sigpatch.Image(profile, d), 0x100)


def anchored_image():
    """Two copies of the same four halfwords, the second behind a marker; the
    code word under test loads a literal that differs between them."""
    flash = 0x1000
    d = bytearray(0x400)
    sig = [0xD001, 0x0009, 0x2222, 0x3333]
    for at, lit in ((0x100, 0xAAAA0001), (0x200, 0xBBBB0002)):
        for k, op in enumerate(sig):
            struct.pack_into("<H", d, at + 2 * k, op)
        struct.pack_into("<I", d, at + 8, lit)             # mov.l @(1,pc) reads (at & ~3) + 4 + 4
    struct.pack_into("<H", d, 0x1F0, 0x5A5A)
    d[0x300:0x3F0] = b"\xff" * 0xF0
    profile = sigpatch.ImageProfile(flash_base=flash, loadtab_off=0, loadtab_n=0,
                                    ram_base=flash + 0x300, ram_end=flash + 0x3F0, byteorder="<")
    return profile, bytes(d), sig


def test_an_imported_address_is_found_by_its_signature_and_its_anchor(tmp_path):
    profile, data, sig = anchored_image()
    img = sigpatch.Image(profile, data)
    marker = sigpatch.Anchor(sig=[0x5A5A], index=0, literal=False)
    assert sigpatch.anchor_value(img, sigpatch.Anchor(sig, 0, True, after=marker)) == 0xBBBB0002
    with pytest.raises(sigpatch.NotApplied, match="found 2 times"):
        sigpatch.anchor_offset(img, sigpatch.Anchor(sig, 0, True))
    assert sigpatch.anchor_value(img, sigpatch.Anchor(sig[2:], 1, False, after=marker)) == 0x1000 + 0x200 + 6


def test_a_routine_gets_the_imported_address_written_into_it_and_is_recognised_after(tmp_path):
    profile, data, sig = anchored_image()
    site_sig = [0x5555] + [0x7000 + k for k in range(1, 11)] + [0x4444]
    d = bytearray(data)
    for k, op in enumerate(site_sig):
        struct.pack_into("<H", d, 0x2C0 + 2 * k, op)
    anchor = sigpatch.Anchor(sig, 0, True, after=sigpatch.Anchor([0x5A5A], 0, False))
    mod = sigpatch.Mod(what="importing mod", target="test", fw_versions=("9.99",), sig=site_sig,
                       start=1, end=10, args=[], literal=False, imports=((0xCAFE0001, anchor),))
    routine = tmp_path / "m.bin"
    routine.write_bytes(bytes(range(1, 17)) + struct.pack("<I", 0xCAFE0001) + b"\x09\x00" * 2)
    bp = lambda name: str(routine)                       # noqa: E731
    report = {}
    out = sigpatch.patch(profile, bp, bytes(d), {"m": mod}, ["m"], report=report)
    assert report["m"] == ("applied", "")
    at = out.index(bytes(range(1, 17)))
    assert struct.unpack_from("<I", out, at + 16)[0] == 0xBBBB0002
    again = {}
    assert sigpatch.patch(profile, bp, out, {"m": mod}, ["m"], report=again) == out
    assert again["m"] == ("present", "")


def test_an_import_that_is_not_found_leaves_the_mod_out_and_says_where(tmp_path):
    profile, data, sig = anchored_image()
    site_sig = [0x5555] + [0x7000 + k for k in range(1, 11)] + [0x4444]
    d = bytearray(data)
    for k, op in enumerate(site_sig):
        struct.pack_into("<H", d, 0x2C0 + 2 * k, op)
    missing = sigpatch.Anchor([0xDEAD, 0xBEEF], 0, False)
    mod = sigpatch.Mod(what="importing mod", target="test", fw_versions=("9.99",), sig=site_sig,
                       start=1, end=10, args=[], literal=False, imports=((0x01010101, missing),))
    routine = tmp_path / "m.bin"
    routine.write_bytes(b"\x01" * 24)
    report = {}
    out = sigpatch.patch(profile, lambda name: str(routine), bytes(d), {"m": mod}, ["m"], report=report)
    assert out == bytes(d)
    assert report["m"] == ("skipped", "import site found 0 times")


# -- the NXS2 routines import every firmware address ------------------------------

def all_sets():
    return [(tool, name, mod) for tool in (patch_main, patch_gui)
            for name, mod in tool.PATCH_SETS["cdj2000nxs2"].mods.items()]


@pytest.mark.parametrize("tool, name, mod", all_sets(), ids=lambda v: v if isinstance(v, str) else None)
def test_every_import_names_a_word_the_routine_holds(tool, name, mod):
    with open(tool.blob_path(name), "rb") as f:
        blob = f.read()
    order = tool.PATCH_SETS["cdj2000nxs2"].profile.byteorder
    words = [o for o, _ in sigpatch.import_offsets(mod, blob, order)]
    assert len(words) >= len(mod.imports)


def literal_pool_words(img, mods):
    """File offsets of the literals the anchors of `mods` read."""
    offs = set()
    for mod in mods:
        for _, source in mod.imports:
            if isinstance(source, sigpatch.Anchor) and source.literal:
                offs.add(sigpatch.literal_offset(img, sigpatch.anchor_offset(img, source)))
    return offs


def real_image(name):
    with open(REAL[name], "rb") as f:
        return f.read()


@needs_real
@pytest.mark.parametrize("tool, image", [(patch_main, "main_unpacked.bin"), (patch_gui, "gui_unpacked.bin")])
def test_every_anchor_finds_the_stock_address_on_the_stock_image(tool, image):
    ps = tool.PATCH_SETS["cdj2000nxs2"]
    img = sigpatch.Image(ps.profile, real_image(image))
    for name, mod in ps.mods.items():
        for stock, source in mod.imports:
            assert sigpatch.import_value(img, source) == stock, (name, hex(stock))


XDJ700_MAIN = path("..", "extract", "xdj700", "main_unpacked.bin")
needs_xdj700 = pytest.mark.skipif(not os.path.exists(XDJ700_MAIN), reason="the XDJ-700 image is not extracted here")


@needs_xdj700
def test_every_xdj700_anchor_finds_the_stock_address_on_the_stock_image():
    ps = patch_main.PATCH_SETS["xdj700"]
    with open(XDJ700_MAIN, "rb") as f:
        img = sigpatch.Image(ps.profile, f.read())
    for name, mod in ps.mods.items():
        for stock, source in mod.imports:
            assert sigpatch.import_value(img, source) == stock, (name, hex(stock))


@needs_xdj700
def test_the_xdj700_track_filter_takes_the_shell_room_and_is_left_as_it_is_when_patched_again():
    with open(XDJ700_MAIN, "rb") as f:
        stock = f.read()
    report = {}
    once = patch_main.patch(stock, ["trackfilter"], "xdj700", report)
    assert report["trackfilter"][0] == "applied"
    assert hashlib.sha256(once).hexdigest() == "856d2f508b9055fd2e471237a445bc4278cfc9418a8aed22646c6f3ae9d0b14d"
    shell = slice(0x08B4B67E - 0x08000000, 0x08B4BB0E - 0x08000000)
    assert once[shell] != stock[shell]
    assert patch_main.patch(once, ["trackfilter"], "xdj700") == once


@needs_real
def test_the_routines_follow_the_firmware_when_its_addresses_move():
    """Every literal an anchor reads is shifted, as if the functions and data
    they name sat 0x1230 bytes further on (an anchor that finds code by its
    instructions is not moved); the last 0x100 bytes of the erased
    run are taken, as if the image ended earlier. The imported words of every
    routine follow, and the whole set still patches."""
    ps = patch_main.PATCH_SETS["cdj2000nxs2"]
    data = bytearray(real_image("main_unpacked.bin"))
    img = sigpatch.Image(ps.profile, bytes(data))
    shift = 0x1230
    for off in literal_pool_words(img, ps.mods.values()):
        struct.pack_into("<I", data, off, struct.unpack_from("<I", data, off)[0] + shift)
    end = sigpatch.free_end(img)
    data[end - ps.profile.flash_base - 0x100] = 0x00
    moved = sigpatch.Image(ps.profile, bytes(data))
    assert sigpatch.free_end(moved) == end - 0x100

    for name, mod in ps.mods.items():
        with open(patch_main.blob_path(name), "rb") as f:
            blob = f.read()
        site = sigpatch.find_sig(moved, mod)
        routine = sigpatch.routine_bytes(moved, mod, blob, site)
        for off, source in sigpatch.import_offsets(mod, blob, "<"):
            stock = next(v for v, s in mod.imports if s is source)
            if isinstance(source, sigpatch.FreeEnd):
                expected = stock - 0x100
            elif source.literal:
                expected = stock + shift
            else:
                expected = stock    # a function found by its instructions, which stay where they are
            assert struct.unpack_from("<I", routine, off)[0] == expected, (name, hex(stock))

    report = {}
    patch_main.patch(bytes(data), MAIN_ALL, report=report)
    assert {state for state, _ in report.values()} == {"applied"}


@needs_real
def test_the_display_routine_follows_the_firmware_when_its_addresses_move():
    ps = patch_gui.PATCH_SETS["cdj2000nxs2"]
    data = bytearray(real_image("gui_unpacked.bin"))
    img = sigpatch.Image(ps.profile, bytes(data))
    for off in literal_pool_words(img, ps.mods.values()):
        struct.pack_into(">I", data, off, struct.unpack_from(">I", data, off)[0] + 0x40)
    moved = sigpatch.Image(ps.profile, bytes(data))
    mod = ps.mods["phrase"]
    with open(patch_gui.blob_path("phrase"), "rb") as f:
        blob = f.read()
    routine = sigpatch.routine_bytes(moved, mod, blob, sigpatch.find_sig(moved, mod))
    got = {stock: struct.unpack_from(">I", routine, off)[0]
           for stock, source in mod.imports for off, s in sigpatch.import_offsets(mod, blob, ">") if s is source}
    assert got[0x0E5BB45C] == 0x0E5BB45C + 0x40
    assert got[0x1C007DE6] == 0x1C007DE6
