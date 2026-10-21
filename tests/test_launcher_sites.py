# SPDX-License-Identifier: GPL-2.0-or-later
"""launcher/sites.py: the firmware addresses the launcher pokes are looked up
in the installed MAIN image by the anchors of the model's player module, and
the poke that needs a site that is not found is left out."""

import os
import sys
from types import SimpleNamespace

import pytest

EMU = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import boot_decks, host, instrumented_batch, model, play_real_dsp, rig, sites  # noqa: E402

REAL_MAIN = os.path.join(EMU, "..", "extract", "main_unpacked.bin")
needs_real_main = pytest.mark.skipif(not os.path.exists(REAL_MAIN), reason="extract/main_unpacked.bin not present")

# Where the anchors land in the supported update (CDJ-2000NXS2 MAIN 1.87).
NXS2_187 = {
    "load_poke_pc": 0x082F04D8,
    "load_poke_addr": 0x0994CC18,
    "pcall_pc": 0x08345574,
    "set_flag": 0x08516448,
    "own_mac": 0x0A35F754,
    "deck_ready": 0x0B5125B8,
    "colour_capability": 0x0A35FAAA,
    "sr_save_slot": 0x0AB87D8C,
    "native_memcmp": 0x085336B0,
    "native_strlen": 0x085341C0,
    "native_memcpy": 0x085336DC,
    "native_memset": 0x08533D50,
    "native_movmem": 0x08531F88,
}


def layout_with(tmp_path, main=None, m=None):
    extract = tmp_path / "extract"
    lay = SimpleNamespace(root=str(tmp_path), extract=str(extract), emu=EMU)
    folder = model.extract_dir(lay, m or model.load())
    os.makedirs(folder, exist_ok=True)
    if main is not None:
        with open(os.path.join(folder, "main_unpacked.bin"), "wb") as f:
            f.write(main)
    return lay


def test_no_installed_image_has_no_sites(tmp_path):
    found = sites.find(layout_with(tmp_path))
    assert found.pinned and found.board_image and found.values == {}
    assert found.switched_off(model.load()) == []


def test_an_image_without_the_sites_leaves_every_poke_out(tmp_path):
    found = sites.find(layout_with(tmp_path, bytes(0x40000)))
    assert not found.pinned and not found.board_image
    assert found.values == dict.fromkeys(NXS2_187)
    off = found.switched_off(model.load())
    assert len(off) == len(sites.OFF) + len(sites.UNPINNED)
    assert any("load poke" in line for line in off)
    assert found.sr_seed_slot() == "0"


def test_a_player_with_no_launcher_sites_is_warned_about_none_of_them(tmp_path):
    xdj700 = model.load("xdj700")
    found = sites.find(layout_with(tmp_path, bytes(0x40000), xdj700), xdj700)
    assert not found.pinned and found.values == {}
    assert found.switched_off(xdj700) == [sites.UNPINNED_DSP_IDLE]


def test_an_image_run_in_place_of_the_installed_one_is_pinned_by_its_own_hash(tmp_path, monkeypatch):
    m = model.load()
    lay = layout_with(tmp_path, bytes(0x40000))
    other = tmp_path / "under_test.bin"
    other.write_bytes(bytes(0x40000) + b"\x01")
    assert not sites.find(lay, m, str(other)).pinned
    stock = sites.firmware.stock_main_sha256(m)
    real = sites.firmware.installed_sha256
    monkeypatch.setattr(sites.firmware, "installed_sha256", lambda path: stock if path == str(other) else real(path))
    found = sites.find(lay, m, str(other))
    assert found.pinned and found.board_image
    assert not sites.find(lay, m).pinned


def test_an_image_run_in_place_of_the_installed_one_gets_its_own_sites(tmp_path, monkeypatch):
    m = model.load()
    lay = layout_with(tmp_path, bytes(0x40000))
    other = tmp_path / "under_test.bin"
    other.write_bytes(bytes(0x40000) + b"\x01")
    seen = []
    monkeypatch.setattr(sites, "_found", lambda path, *rest: seen.append(path) or dict.fromkeys(NXS2_187))
    sites.find(lay, m)
    sites.find(lay, m, str(other))
    assert seen[0].endswith("main_unpacked.bin") and seen[1] == str(other)


def test_the_load_driver_waits_for_the_ready_word_only_where_it_is_found(tmp_path, monkeypatch):
    lay = layout_with(tmp_path, bytes(0x40000))
    lay.run = str(tmp_path)
    env = {"AUTOLOAD": "1"}
    boot_decks.batch_env(env, lay)
    assert env["READY_WAIT"] == "0" and "READY_ADDR" not in env
    monkeypatch.setattr(sites, "find", lambda lay: sites.Sites(dict(NXS2_187), True, True))
    env = {"AUTOLOAD": "1"}
    boot_decks.batch_env(env, lay)
    assert env["READY_ADDR"] == "0x0B5125B8" and "READY_WAIT" not in env


@pytest.mark.parametrize("board_image", [True, False])
def test_the_native_libc_hooks_follow_the_found_routines_and_the_ram_reads_the_nxs2_image(monkeypatch, board_image):
    monkeypatch.setattr(host, "is_windows", lambda: True)
    monkeypatch.setattr(host, "windows_reserved_ranges", lambda proto: [])
    monkeypatch.setattr(sites, "find", lambda lay, m: sites.Sites(dict(NXS2_187), True, board_image))
    env = {"NOSOUND": "1", "C66X_JIT": "", "MODULE": "none", "DJLINK": "0"}
    rig.rig_env(env, "t", "1", "120")
    assert env["CDJ_NATIVE_LIBC"] == "0x085336B0:0x085341C0"
    assert env.get("CDJ_RAM_DIAG") == (None if board_image else "0")


def test_the_native_copy_tier_is_opt_in_and_needs_all_its_routines(monkeypatch):
    monkeypatch.setattr(host, "is_windows", lambda: True)
    monkeypatch.setattr(host, "windows_reserved_ranges", lambda proto: [])
    base = {"NOSOUND": "1", "C66X_JIT": "", "MODULE": "none", "DJLINK": "0"}
    monkeypatch.setattr(sites, "find", lambda lay, m: sites.Sites(dict(NXS2_187), True, True))
    env = dict(base)
    rig.rig_env(env, "t", "1", "120")
    assert "CDJ_NATIVE_COPY" not in env
    env = dict(base, CDJ_NATIVE_COPY="1")
    rig.rig_env(env, "t", "1", "120")
    assert env["CDJ_NATIVE_COPY"] == "0x085336DC:0x08533D50:0x08531F88"
    monkeypatch.setattr(sites, "find", lambda lay, m: sites.Sites(dict(NXS2_187, native_movmem=None), True, True))
    env = dict(base, CDJ_NATIVE_COPY="1")
    rig.rig_env(env, "t", "1", "120")
    assert env["CDJ_NATIVE_COPY"] == "0"
    monkeypatch.setattr(sites, "find", lambda lay, m: sites.Sites(dict(NXS2_187, native_strlen=None), True, True))
    env = dict(base)
    rig.rig_env(env, "t", "1", "120")
    assert env["CDJ_NATIVE_LIBC"] == "0"


def test_the_seed_slot_is_the_found_one_else_the_boards_on_the_boards_own_image():
    assert sites.Sites({"sr_save_slot": 0x0AB87D8C}, True, True).sr_seed_slot() == "0x0AB87D8C"
    assert sites.Sites({"sr_save_slot": None}, True, True).sr_seed_slot() is None
    assert sites.Sites({}, False, False).sr_seed_slot() == "0"


def test_the_load_poke_needs_both_its_sites(tmp_path):
    env = {}
    instrumented_batch.batch_env(env, sites.Sites({}, False, False), model.load())
    assert env["CDJ_PPOKE"] == ""
    assert "CDJ_FWTRACE" not in env and "CDJ_FWLOG" not in env
    env = {}
    instrumented_batch.batch_env(env, sites.Sites(dict(NXS2_187), True, True), model.load())
    assert env["CDJ_PPOKE"] == "0x082f04d8:0x0994CC18:1:0"
    assert env["CDJ_FWLOG"] == "0x0853410C,0x084933F4"
    assert env["RAMSNAP"] == "0x09947000:0x01713000"
    env = {"TAPS": "0x08000100"}
    instrumented_batch.batch_env(env, sites.Sites({}, False, False), model.load())
    assert env["CDJ_FWTRACE"] == "0x08000100"


def test_a_found_site_replaces_the_supported_address():
    found = sites.Sites(dict(NXS2_187, load_poke_pc=0x082F0238, colour_capability=0x0A35F9AA), False, False)
    env = {}
    instrumented_batch.batch_env(env, found, model.load())
    assert env["CDJ_PPOKE"].startswith("0x082f0238:0x0994CC18:")
    env = {}
    play_real_dsp.batch_env(env, found)
    assert env["MPOKE"] == "0x0A35F9AA:3:1"
    assert "TAPS" not in env and "CDJ_PPOKE_ARM" not in env


def test_the_supported_update_keeps_its_pokes():
    env = {"LADDER": "0"}
    play_real_dsp.batch_env(env, sites.Sites(dict(NXS2_187), True, True))
    assert env["MPOKE"] == "0x0A35FAAA:3:1"
    assert env["TAPS"] == "0x08443252,0x08443276,0x0842A160,0x084E294E"


@needs_real_main
def test_the_signatures_find_the_supported_addresses_in_the_supported_image(tmp_path):
    values = sites._found(os.path.abspath(REAL_MAIN), 0, 0, EMU, "cdj2000nxs2")
    assert values == NXS2_187


# The SR seed slots the SH7763 boards (hw/cdj/boards/cdj2000, cdj900) default to.
SH7763_SR_SLOTS = {"cdj2000": 0x04FC45F0, "cdj2000nxs": 0x04D13694, "cdj900": 0x04FC287C}


@pytest.mark.parametrize("model_id", sorted(SH7763_SR_SLOTS))
def test_the_sh7763_decks_find_their_boards_seed_slot_in_their_stock_image(model_id):
    path = os.path.join(EMU, "..", model.load(model_id).extract, "main_unpacked.bin")
    if not os.path.exists(path):
        pytest.skip("%s not present" % path)
    values = sites._found(os.path.abspath(path), 0, 0, EMU, model_id)
    assert values == {"sr_save_slot": SH7763_SR_SLOTS[model_id]}
