# SPDX-License-Identifier: GPL-2.0-or-later
"""emulator/mods/: the shared signature-patch engine (sigpatch.py), the LZSS
encoder and GUI section re-encoder (scripts/firmware/), and the .UPD
container patcher (patch_update.py) that ties them together.

Everything below builds its own tiny synthetic images -- the real firmware is
never needed for these. A handful of tests at the bottom exercise the real
fw/unzipped/C2KNXS2.UPD and skip when it is not present, the same way the
rest of the suite treats firmware-gated tests."""
import os
import re
import struct
import sys

import pytest

import gui_decode
import gui_encode
import lzss_decode
import lzss_encode
import patch_gui
import patch_main
import patch_update
import sigpatch
from helpers import path, run_script


# -- lzss_encode.py: the production encoder --------------------------------------

@pytest.mark.parametrize("data", [
    b"",
    b"A",
    b"AAAAAAAAAAAAAAAAAAAA",
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZ" * 40,
    bytes((i * 37) % 256 for i in range(3000)),          # no short repeats
    (b"PUSH MEMORY rekordbox Database not found! " * 30),
])
def test_lzss_encode_round_trips(data):
    enc = lzss_encode.pack(data)
    assert lzss_decode.unpack(enc, 0) == data


def test_lzss_encode_uses_matches_on_repeated_data():
    data = b"REPEATME" * 500
    enc = lzss_encode.pack(data)
    assert len(enc) < len(data) // 4
    assert lzss_decode.unpack(enc, 0) == data


def test_lzss_encode_matches_reach_into_the_initial_space_fill():
    # The first bytes of a real stream are often spaces/padding; a match
    # referencing the ring's initial fill (before any real output exists)
    # must decode correctly, not just a match against prior real output.
    data = b"   " + b"padded header" + b"    trailer"
    enc = lzss_encode.pack(data)
    assert lzss_decode.unpack(enc, 0) == data


# -- gui_encode.py: the section re-encoder ---------------------------------------

def test_crc16_xmodem_matches_the_standard_check_value():
    # The well-known CRC-16/XMODEM check value for the ASCII string
    # "123456789" -- confirms the polynomial/init/no-reflect choice
    # independently of anything in this project.
    assert gui_encode.crc16_xmodem(b"123456789") == 0x31C3


def test_gui_encode_round_trips_through_gui_decode():
    header = b"CDJ-2000NXS2GUI Ver9.99        0"
    payload = b"synthetic GUI image payload " * 200
    section = gui_encode.encode(header, payload)
    hdr, declared, avail, out = gui_decode.decode(section)
    assert patch_update.section_version(hdr) == "9.99"
    assert out == payload
    # the trailer is a real checksum, not a placeholder: flipping a stream
    # byte must be caught by recomputing it, not silently accepted
    tampered = bytearray(section)
    tampered[0x24] ^= 0xFF
    assert gui_encode.crc16_xmodem(tampered[:-2]) != struct.unpack(">H", tampered[-2:])[0]


def test_gui_encode_output_bounds_gui_decode_input():
    # gui_decode.decode() must read exactly the declared stream, not the
    # whole section -- otherwise the trailer bytes get fed to the LZSS
    # decoder as if they were more compressed data.
    header = b"CDJ-2000NXS2GUI Ver1.81        0"
    section = gui_encode.encode(header, b"x" * 5000)
    declared = struct.unpack_from(">I", section, 0x20)[0]
    assert len(section) == 0x24 + declared + 2


# -- sigpatch.py: the shared engine, on a made-up (non-GUI) profile --------------

def build_synthetic_image():
    """A tiny scatter-loaded image: one copy record carrying 16 halfwords of
    'code' with a signature in the middle, flanked by two untouched anchor
    halfwords; erased flash right after it for a new segment to land in."""
    flash_base, ram_base = 0x1000, 0x2000000
    profile = sigpatch.ImageProfile(flash_base=flash_base, loadtab_off=0x10,
                                    loadtab_n=8, ram_base=ram_base, ram_end=ram_base + 0x1000)
    sig = [0xAAA1, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888, 0x9999, 0xBBB2]
    code = [0x1010, 0x2020, 0x3030] + sig + [0xE0E0, 0xF0F0, 0xC0C0]
    assert len(code) == 16

    d = bytearray(0x100)
    for i, v in enumerate(code):
        struct.pack_into(">H", d, 0x70 + 2 * i, v)
    d[0x90:0xD0] = b"\xff" * 64                        # erased flash to place the routine in

    struct.pack_into(">III", d, 0x10 + 12 * 0, ram_base, flash_base + 0x70, 32)   # the code
    struct.pack_into(">III", d, 0x10 + 12 * 1, 0, 0, 0)                          # free record
    struct.pack_into(">III", d, 0x10 + 12 * 2, 0, 0, 0)                          # free record
    struct.pack_into(">III", d, 0x10 + 12 * 3, ram_base + 32, 16, 0)             # zero-fill closer

    mod = sigpatch.Mod(what="synthetic mod", target="test", fw_versions=("9.99",),
                       sig=sig, start=1, end=9, args=[], literal=False)
    return profile, bytes(d), {"synth": mod}


def test_sigpatch_places_the_routine_and_splices_the_call_site(tmp_path):
    profile, data, mods = build_synthetic_image()
    routine = b"\x12\x34\x56\x78"
    blob = tmp_path / "synth.bin"
    blob.write_bytes(routine)

    out = sigpatch.patch(profile, lambda name: str(blob), data, mods, ["synth"])

    img = sigpatch.Image(profile, out)
    rec = img.recs[1]                          # the first free record, now used
    assert rec[2] == 16                        # routine padded up to 16 bytes
    seg_off = rec[1] - profile.flash_base
    assert out[seg_off:seg_off + len(routine)] == routine

    # sig[0] and sig[9] are outside start/end and must survive untouched
    assert struct.unpack_from(">H", out, 0x70 + 2 * 3)[0] == 0xAAA1
    assert struct.unpack_from(">H", out, 0x70 + 2 * 12)[0] == 0xBBB2
    # the replaced span in between is no longer the original signature
    assert out[0x70 + 2 * 4:0x70 + 2 * 12] != data[0x70 + 2 * 4:0x70 + 2 * 12]


def test_sigpatch_refuses_a_signature_found_zero_or_twice(tmp_path):
    profile, data, mods = build_synthetic_image()
    mods["synth"] = mods["synth"]._replace(sig=[0xDEAD, 0xBEEF, 0xF00D])
    blob = tmp_path / "synth.bin"
    blob.write_bytes(b"\0" * 4)
    with pytest.raises(SystemExit, match="found 0 times"):
        sigpatch.patch(profile, lambda name: str(blob), data, mods, ["synth"])


# -- sigpatch.py: two mods must stack, in either order, without colliding -------

def build_two_mod_image():
    """Two independent signatures in one image -- for the mod-combination
    guarantees: each mod gets its own non-overlapping routine space, that
    space and each mod's own call site do not move depending on which other
    mods are selected or in what order."""
    flash_base, ram_base = 0x1000, 0x2000000
    profile = sigpatch.ImageProfile(flash_base=flash_base, loadtab_off=0x10,
                                    loadtab_n=8, ram_base=ram_base, ram_end=ram_base + 0x1000)
    sig_a = [0xAAA1, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666, 0x7777, 0x8888, 0x9999, 0xBBB2]
    sig_b = [0xCCC1, 0x1112, 0x1113, 0x1114, 0x1115, 0x1116, 0x1117, 0x1118, 0x1119, 0xDDD2]
    code = ([0x1010, 0x2020, 0x3030] + sig_a + [0xE0E0, 0xF0F0, 0xC0C0]
            + [0x4040, 0x5050, 0x6060] + sig_b + [0x7070, 0x8080, 0x9090])

    d = bytearray(0x100)
    for i, v in enumerate(code):
        struct.pack_into(">H", d, 0x70 + 2 * i, v)
    seg_start = 0x70 + 2 * len(code)
    d[seg_start:seg_start + 64] = b"\xff" * 64

    struct.pack_into(">III", d, 0x10 + 12 * 0, ram_base, flash_base + 0x70, 2 * len(code))
    struct.pack_into(">III", d, 0x10 + 12 * 1, 0, 0, 0)
    struct.pack_into(">III", d, 0x10 + 12 * 2, 0, 0, 0)
    struct.pack_into(">III", d, 0x10 + 12 * 3, ram_base + 2 * len(code), 16, 0)

    mod_a = sigpatch.Mod(what="mod A", target="test", fw_versions=("9.99",),
                        sig=sig_a, start=1, end=9, args=[], literal=False)
    mod_b = sigpatch.Mod(what="mod B", target="test", fw_versions=("9.99",),
                        sig=sig_b, start=1, end=9, args=[], literal=False)
    return profile, bytes(d), {"mod_a": mod_a, "mod_b": mod_b}


def write_blobs(tmp_path, mods):
    """A routine file per mod, deliberately different sizes so padding to the
    next 16 bytes isn't accidentally the same for both."""
    paths = {}
    for i, name in enumerate(sorted(mods)):
        p = tmp_path / (name + ".bin")
        p.write_bytes(bytes([0x10 + i]) * (4 + 3 * i))
        paths[name] = str(p)
    return lambda name: paths[name]


def replaced_span(profile, data, mod):
    off = sigpatch.find_sig(sigpatch.Image(profile, data), mod)
    return off + 2 * mod.start, off + 2 * mod.end


def test_mods_stack_regardless_of_order(tmp_path):
    profile, data, mods = build_two_mod_image()
    bp = write_blobs(tmp_path, mods)

    alone_a = sigpatch.patch(profile, bp, data, mods, ["mod_a"])
    both_ab = sigpatch.patch(profile, bp, data, mods, ["mod_a", "mod_b"])
    both_ba = sigpatch.patch(profile, bp, data, mods, ["mod_b", "mod_a"])

    assert both_ab == both_ba                     # combining does not care about order

    span_a = replaced_span(profile, data, mods["mod_a"])
    assert alone_a[slice(*span_a)] == both_ab[slice(*span_a)]   # same call site alone or combined

    offsets, _ = sigpatch.layout(bp, mods)
    rec = sigpatch.Image(profile, both_ab).recs[1]
    seg_off = rec[1] - profile.flash_base
    routine_a = open(bp("mod_a"), "rb").read()
    lo, hi = seg_off + offsets["mod_a"], seg_off + offsets["mod_a"] + len(routine_a)
    assert both_ab[lo:hi] == alone_a[lo:hi] == routine_a


def test_mods_refuse_to_combine_when_signatures_overlap(tmp_path):
    profile, data, mods = build_two_mod_image()
    mods["mod_a_dup"] = mods["mod_a"]._replace(what="mod A duplicate")
    bp = write_blobs(tmp_path, mods)
    with pytest.raises(SystemExit, match="overlap"):
        sigpatch.patch(profile, bp, data, mods, ["mod_a", "mod_a_dup"])


def test_patching_one_at_a_time_and_chaining_is_refused_not_silently_wrong(tmp_path):
    # The only supported way to combine mods is one patch() call with all of
    # them named. Feeding one call's output into another must not produce a
    # second, colliding routine placement -- it must fail loudly instead.
    profile, data, mods = build_two_mod_image()
    bp = write_blobs(tmp_path, mods)
    once = sigpatch.patch(profile, bp, data, mods, ["mod_a"])
    with pytest.raises(SystemExit):
        sigpatch.patch(profile, bp, once, mods, ["mod_b"])


# -- patch_gui.py keeps sigpatch under the hood, CLI unchanged -------------------

def test_patch_gui_list_still_prints_name_then_description():
    r = run_script("mods/patch_gui.py", "--list")
    assert r.returncode == 0, r.stderr
    assert r.stdout.split()[0] == "wave3"


# -- patch_update.py: the container-level tool -----------------------------------

def gui_section(version="1.81", payload=b"gui image bytes " * 50):
    header = ("CDJ-2000NXS2GUI Ver%s" % version).encode().ljust(32, b" ")
    return gui_encode.encode(header, payload)


def build_upd(*sections):
    manifest = b"".join(b"%d\r\n" % len(s) for s in sections)
    return manifest + b"".join(sections)


def test_registry_only_lists_firmware_patchable_mods():
    reg = patch_update.registry()
    assert set(reg) == set(patch_gui.MODS) | set(patch_main.MODS)
    # the emulator-only knobs from mods.conf must never appear here
    conf = open(path("mods", "mods.conf"), encoding="utf-8").read()
    knob_names = re.findall(r"^(\w+)\|", conf, re.M)
    assert "high_fps" not in reg and "live_clock" not in reg
    assert "three_band" not in reg          # that row's key, not the mod names
    assert knob_names == ["high_fps", "live_clock", "three_band", "osc_beat"]


def test_list_cli_shows_target_and_versions():
    r = run_script("mods/patch_update.py", "--list")
    assert r.returncode == 0, r.stderr
    assert re.search(r"wave3\s+\[gui Ver1\.81\]", r.stdout)


def test_repack_with_no_mods_is_byte_identical(tmp_path):
    upd = build_upd(gui_section(), b"driv-section", b"main-section", b"panl-section")
    src = tmp_path / "in.UPD"
    src.write_bytes(upd)
    out = tmp_path / "out.UPD"
    r = run_script("mods/patch_update.py", src, out)
    assert r.returncode == 0, r.stderr
    assert out.read_bytes() == upd


def test_unknown_mod_errors_clearly(tmp_path):
    src = tmp_path / "in.UPD"
    src.write_bytes(build_upd(gui_section()))
    r = run_script("mods/patch_update.py", src, tmp_path / "out.UPD", "not_a_mod")
    assert r.returncode != 0
    assert "unknown mod" in r.stderr


def test_version_mismatch_is_refused_unless_forced(tmp_path):
    src = tmp_path / "in.UPD"
    src.write_bytes(build_upd(gui_section(version="1.99")))
    out = tmp_path / "out.UPD"

    r = run_script("mods/patch_update.py", src, out, "wave3")
    assert r.returncode != 0
    assert "verified only for gui Ver1.81" in r.stderr
    assert "Ver1.99" in r.stderr
    assert not out.exists()

    # --force-version bypasses the gate and reaches the real patch attempt,
    # which then fails for its own reason (no real wave3 signature here) --
    # proving the flag changed which check ran, not that patching succeeded.
    r = run_script("mods/patch_update.py", src, out, "wave3", "--force-version")
    assert r.returncode != 0
    assert "verified only for" not in r.stderr
    assert "call site found" in r.stderr


def test_patch_update_calls_decode_then_patch_gui_before_encoding(tmp_path):
    # --force-version walks the container all the way to patch_gui.patch(),
    # which fails on this synthetic payload for its own reason (no real
    # wave3 signature) -- proving decode() ran and handed patch_gui real
    # image bytes, not just that the version gate can be bypassed.
    src = tmp_path / "in.UPD"
    src.write_bytes(build_upd(gui_section(version="1.81"), b"other-section"))
    r = run_script("mods/patch_update.py", src, tmp_path / "out.UPD", "wave3")
    assert r.returncode != 0
    assert "call site found 0 times" in r.stderr


# -- patch_update.py combining mods, including across two target images ---------

class FakeTarget:
    """A stand-in for patch_gui.py (or a future patch_main.py): a MODS
    registry plus a patch() that runs it through sigpatch, for a target that
    is not the real GUI image."""

    def __init__(self, profile, mods, blob_path):
        self.MODS = mods
        self._profile = profile
        self._blob_path = blob_path

    def patch(self, image, names):
        return sigpatch.patch(self._profile, self._blob_path, image, self.MODS, names)


def run_patch_update(monkeypatch, *args):
    monkeypatch.setattr(sys, "argv", ["patch_update.py"] + [str(a) for a in args])
    patch_update.main()


def fake_section(label, image):
    header = label.encode().ljust(32, b" ")
    return gui_encode.encode(header, image)


def test_patch_update_combines_two_mods_on_one_target(tmp_path, monkeypatch):
    profile, image, mods = build_two_mod_image()
    bp = write_blobs(tmp_path, mods)
    fake = FakeTarget(profile, mods, bp)
    monkeypatch.setitem(patch_update.TARGETS, "gui", (gui_decode.decode, gui_encode.encode, fake))

    src = tmp_path / "in.UPD"
    src.write_bytes(build_upd(fake_section("FAKE-GUI Ver9.99", image)))

    out_ab, out_ba = tmp_path / "out_ab.UPD", tmp_path / "out_ba.UPD"
    run_patch_update(monkeypatch, src, out_ab, "mod_a", "mod_b")
    run_patch_update(monkeypatch, src, out_ba, "mod_b", "mod_a")
    assert out_ab.read_bytes() == out_ba.read_bytes()          # order does not reach the .UPD either

    sizes, hdrlen = patch_update.parse_manifest(out_ab.read_bytes())
    new_section = out_ab.read_bytes()[hdrlen:hdrlen + sizes[0]]
    _, _, _, patched_image = gui_decode.decode(new_section)
    assert patched_image == sigpatch.patch(profile, bp, image, mods, ["mod_a", "mod_b"])


def test_patch_update_patches_two_targets_in_one_run(tmp_path, monkeypatch):
    # The shape "phrase colours" needs later: one mod for the display image,
    # one for MAIN, applied together -- each must only touch its own section.
    gui_profile, gui_image, gui_mods = build_two_mod_image()
    del gui_mods["mod_b"]                          # keep this target to one mod
    main_profile, main_image, main_mods = build_synthetic_image()

    gui_bp = write_blobs(tmp_path, gui_mods)
    main_bp = write_blobs(tmp_path, main_mods)

    fake_gui = FakeTarget(gui_profile, gui_mods, gui_bp)
    fake_main = FakeTarget(main_profile, main_mods, main_bp)
    monkeypatch.setitem(patch_update.TARGETS, "gui", (gui_decode.decode, gui_encode.encode, fake_gui))
    # main() calls a 'main' target's encode with the whole original section
    # (the real main_encode.encode() needs it, to carry its bootloader/
    # emergency-updater S-records forward unchanged); this fake only needs
    # gui_decode/gui_encode's flat-blob shape, so it slices the header itself.
    monkeypatch.setitem(patch_update.TARGETS, "main",
                        (gui_decode.decode, lambda section, image: gui_encode.encode(section[:32], image), fake_main))
    monkeypatch.setitem(patch_update.SECTION_OF, "main", 2)

    src = tmp_path / "in.UPD"
    src.write_bytes(build_upd(fake_section("FAKE-GUI Ver9.99", gui_image),
                              fake_section("FAKE-MAIN Ver9.99", main_image)))
    out = tmp_path / "out.UPD"
    run_patch_update(monkeypatch, src, out, "mod_a", "synth")

    sizes, hdrlen = patch_update.parse_manifest(out.read_bytes())
    blob = out.read_bytes()
    _, _, _, patched_gui = gui_decode.decode(blob[hdrlen:hdrlen + sizes[0]])
    _, _, _, patched_main = gui_decode.decode(blob[hdrlen + sizes[0]:hdrlen + sizes[0] + sizes[1]])

    assert patched_gui == sigpatch.patch(gui_profile, gui_bp, gui_image, gui_mods, ["mod_a"])
    assert patched_main == sigpatch.patch(main_profile, main_bp, main_image, main_mods, ["synth"])
    assert patched_gui != gui_image and patched_main != main_image     # both sections actually changed


# -- gated on the real, user-supplied firmware -----------------------------------

REAL_UPD = path("..", "fw", "unzipped", "C2KNXS2.UPD")
needs_real_upd = pytest.mark.skipif(not os.path.exists(REAL_UPD),
                                    reason="fw/unzipped/C2KNXS2.UPD not present")


@needs_real_upd
def test_real_upd_repacks_byte_identical_with_no_mods(tmp_path):
    data = open(REAL_UPD, "rb").read()
    out = tmp_path / "out.UPD"
    r = run_script("mods/patch_update.py", REAL_UPD, out)
    assert r.returncode == 0, r.stderr
    assert out.read_bytes() == data


@needs_real_upd
def test_real_gui_section_lzss_round_trips_through_our_own_encoder():
    sizes, hdrlen = patch_update.parse_manifest(open(REAL_UPD, "rb").read())
    data = open(REAL_UPD, "rb").read()
    section = data[hdrlen:hdrlen + sizes[0]]
    header, declared, avail, decoded = gui_decode.decode(section)

    reencoded = gui_encode.encode(section[:32], decoded)
    header2, declared2, avail2, decoded2 = gui_decode.decode(reencoded)
    assert decoded2 == decoded                  # decodes identically ...
    trailer = struct.unpack(">H", reencoded[-2:])[0]
    assert gui_encode.crc16_xmodem(reencoded[:-2]) == trailer   # ... and the checksum validates


@needs_real_upd
def test_real_patched_upd_unpacks_to_patch_guis_own_output(tmp_path):
    out = tmp_path / "patched.UPD"
    r = run_script("mods/patch_update.py", REAL_UPD, out, "wave3")
    assert r.returncode == 0, r.stderr

    split_dir = tmp_path / "split"
    r = run_script("scripts/firmware/split_update.py", out, split_dir)
    assert r.returncode == 0, r.stderr
    header, declared, avail, from_upd = gui_decode.decode((split_dir / "section1.bin").read_bytes())

    orig_gui = open(path("..", "extract", "gui_unpacked.bin"), "rb").read()
    from_patch_gui = patch_gui.patch(orig_gui, ["wave3"])
    assert from_upd == from_patch_gui
