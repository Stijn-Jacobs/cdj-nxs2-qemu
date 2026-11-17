# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/test/mod_check.py without a running deck: the steps planned from a
model's profile and face, the judging of pictures, the result, and where the
stock idle point is kept."""

import os
import sys
from types import SimpleNamespace

import pytest

from helpers import path

sys.path.insert(0, path("scripts", "test"))
import mod_check  # noqa: E402
from launcher import deck, host, model  # noqa: E402

Image = pytest.importorskip("PIL.Image")
ImageDraw = pytest.importorskip("PIL.ImageDraw")

W, H = 80, 48


def frame(*boxes, base=(0, 0, 0)):
    """A W x H picture with white boxes on it."""
    im = Image.new("RGB", (W, H), base)
    draw = ImageDraw.Draw(im)
    for x0, y0, x1, y1 in boxes:
        draw.rectangle((x0, y0, x1 - 1, y1 - 1), fill=(255, 255, 255))
    return im


def fake_face(*names, knob=True):
    keys = [SimpleNamespace(name=n, off=0x14, mask=1 << i, wired=True,
                            datagram=lambda op, i=i: "0x14:%d:0:%s" % (1 << i, op))
            for i, n in enumerate(names)]
    return SimpleNamespace(keys=keys, rotary_off=0x0E if knob else None,
                           turn_datagram=lambda d: "0x0e:%d:0:rot" % d)


def fake_model(load_steps="key 0x13:0x04:300; wait 12; key 0x11:0x01:300; wait 30"):
    return SimpleNamespace(id="deck", load_steps=load_steps)


# -- steps ---------------------------------------------------------------------

def test_a_box_is_four_ordered_pixels():
    assert mod_check.parse_box("131,144,662,262") == (131, 144, 662, 262)
    for bad in ("1,2,3", "5,0,4,9", "0,9,4,9", "a,b,c,d", "-1,0,4,4"):
        with pytest.raises(Exception):
            mod_check.parse_box(bad)


def test_the_load_steps_get_a_picture_after_every_wait_but_the_last():
    steps = mod_check.load_steps("key 0x13:0x04:300; wait 12; key 0x0e:1:0:rot; wait 3; "
                                 "sendkey ret 400; wait 45")
    assert steps == [("key", "0x13:0x04:300"), ("wait", 12.0), ("shot", "load"),
                     ("key", "0x0e:1:0:rot"), ("wait", 3.0), ("shot", "load"),
                     ("sendkey", "ret", 400), ("wait", 45.0)]


def test_load_steps_without_a_wait_get_no_picture():
    assert mod_check.load_steps("key 0x13:0x04:300") == [("key", "0x13:0x04:300")]


def test_an_unreadable_load_step_stops_the_check():
    with pytest.raises(mod_check.CheckError, match="cannot read step"):
        mod_check.load_steps("press usb; wait 3")
    with pytest.raises(mod_check.CheckError, match="cannot read step"):
        mod_check.load_steps("wait soon")


ALL_KEYS = ("PLAY/PAUSE", "BROWSE", "ROTARY PUSH", "TIME MODE", "MENU", "INFO", "TAG LIST")


def test_the_plan_presses_the_faces_keys_and_names_every_picture_in_order():
    steps, skipped = mod_check.plan(fake_model(), fake_face(*ALL_KEYS))
    assert skipped == []
    assert mod_check.shots(steps) == ["01-idle", "02-load", "03-paused", "04-time-mode", "05-utility",
                                      "06-utility-down", "07-info", "08-tag-list"]
    keys = [s[1] for s in steps if s[0] == "key"]
    assert keys[:2] == ["0x13:0x04:300", "0x11:0x01:300"]          # the profile's own
    # From the face: pause, back to the list, one track down, load it.
    assert keys[2:6] == ["0x14:1:300:or", "0x14:2:300:or", "0x0e:1:0:rot", "0x14:4:300:or"]
    assert "0x14:16:0:hold" in keys and "0x14:16:0:rel" in keys    # MENU held for UTILITY
    assert keys.count("0x0e:1:0:rot") == 4                         # one detent per datagram


def test_a_point_whose_key_the_face_lacks_is_skipped_and_named():
    steps, skipped = mod_check.plan(fake_model(), fake_face("PLAY/PAUSE", "BROWSE", "ROTARY PUSH", "MENU"))
    assert mod_check.shots(steps) == ["01-idle", "02-load", "03-paused", "04-utility", "05-utility-down"]
    assert skipped == [("time-mode", ["TIME MODE"]), ("info", ["INFO"]), ("tag-list", ["TAG LIST"])]


def test_without_the_paused_track_every_later_point_is_skipped():
    steps, skipped = mod_check.plan(fake_model(), fake_face(*ALL_KEYS, knob=False))
    assert mod_check.shots(steps) == ["01-idle", "02-load"]
    assert skipped[0] == ("paused", ["select knob"])
    assert ("time-mode", ["a paused track"]) in skipped and ("info", ["a paused track"]) in skipped


def test_a_dark_key_counts_as_missing():
    face = fake_face(*ALL_KEYS)
    face.keys[1].wired = False
    steps, skipped = mod_check.plan(fake_model(), face)
    assert ("paused", ["BROWSE"]) in skipped


def test_a_deck_without_a_face_still_loads_its_track():
    steps, skipped = mod_check.plan(fake_model(), None)
    assert mod_check.shots(steps) == ["01-idle", "02-load"] and len(skipped) == 5


def test_a_profile_without_load_steps_cannot_be_checked():
    with pytest.raises(mod_check.CheckError, match="MODEL_LOAD_STEPS"):
        mod_check.plan(fake_model(""), None)


def test_the_cdj900nxs_face_wires_every_key_the_plan_presses():
    m = model.load("cdj900nxs")
    face = mod_check.load_face(m.app_face)
    steps, skipped = mod_check.plan(m, face)
    assert skipped == []
    keys = [s[1] for s in steps if s[0] == "key"]
    for wire in ("0x10:1:300:or", "0x14:1:300:or", "0x11:1:300:or", "0x13:32:300:or", "0x14:8:0:hold",
                 "0x14:4:300:or", "0x14:2:300:or"):
        assert wire in keys
    assert "0x0e:1:0:rot" in keys


def test_every_one_window_deck_with_a_face_can_be_planned():
    for mid in model.list_models():
        m = model.load(mid)
        if m.is_rig or not m.load_steps:
            continue
        steps, _ = mod_check.plan(m, mod_check.load_face(m.app_face) if m.app_face else None)
        assert mod_check.shots(steps)[:2] == ["01-idle", "02-load"]


def test_the_rig_is_refused(monkeypatch):
    args = SimpleNamespace(model="cdj2000nxs2", image="x", media="")
    with pytest.raises(mod_check.CheckError, match="two-board rig"):
        mod_check.prepare(args)


# -- judging -------------------------------------------------------------------

def judge(image, stock1, stock2, **kw):
    return mod_check.judge_point(image, [stock1, stock2], **kw)


def test_equal_pictures_are_the_same():
    r = judge([frame((10, 10, 20, 20))], [frame((10, 10, 20, 20))], [frame((10, 10, 20, 20))])
    assert r["verdict"] == "same" and r["differ"] == 0 and r["noise"] == 0


def test_a_difference_outside_the_allowed_regions_differs():
    r = judge([frame((10, 10, 20, 20))], [frame()], [frame()])
    assert r["verdict"] == "DIFFERS" and r["differ"] == 100 and r["box"] == (10, 10, 20, 20)


def test_a_difference_inside_an_allowed_region_is_expected():
    r = judge([frame((10, 10, 20, 20))], [frame()], [frame()], allow=[(5, 5, 30, 30)])
    assert r["verdict"] == "expected" and r["differ"] == 0 and r["in_allow"] == 100


def test_partly_outside_the_allowed_region_still_differs():
    r = judge([frame((10, 10, 20, 20))], [frame()], [frame()], allow=[(5, 5, 15, 30)])
    assert r["verdict"] == "DIFFERS" and r["differ"] == 50 and r["box"] == (15, 10, 20, 20)


def test_where_the_stock_runs_disagree_is_not_judged():
    # A lamp at (60,5) lights in one stock run only; the image under test has it too.
    lamp = (60, 5, 64, 9)
    r = judge([frame(lamp)], [frame()], [frame(lamp)])
    assert r["verdict"] == "same" and r["in_noise"] == 16 and r["noise"] > 16


def test_a_blinking_digit_matches_either_phase():
    digit = (30, 30, 34, 38)
    r = judge([frame(digit), frame()], [frame(), frame(digit)], [frame(digit), frame()], max_noise_pct=10.0)
    assert r["verdict"] == "same" and r["differ"] == 0


def test_a_change_next_to_the_noise_is_still_judged():
    lamp = (60, 5, 64, 9)
    r = judge([frame((10, 10, 20, 20))], [frame()], [frame(lamp)])
    assert r["verdict"] == "DIFFERS" and r["differ"] == 100


def test_stock_runs_that_disagree_over_much_of_the_screen_are_unstable():
    popup = (0, 0, 40, 24)          # a quarter of the screen: an error message in one run
    r = judge([frame()], [frame()], [frame(popup)])
    assert r["verdict"] == "unstable"
    r = judge([frame()], [frame()], [frame(popup)], max_noise_pct=50.0)
    assert r["verdict"] == "same"


def test_ignored_regions_are_never_compared():
    r = judge([frame((0, 0, 5, 5))], [frame()], [frame()], ignore=[(0, 0, 8, 8)])
    assert r["verdict"] == "same" and r["differ"] == 0


def test_a_run_without_pictures_is_missing():
    assert judge([], [frame()], [frame()])["verdict"] == "missing"
    assert judge([frame()], [], [frame()])["verdict"] == "missing"


def test_another_screen_size_differs():
    r = judge([Image.new("RGB", (W + 8, H))], [frame()], [frame()])
    assert r["verdict"] == "DIFFERS" and "sizes" in r["note"]


def test_the_diff_picture_marks_judged_and_noise_pixels(tmp_path):
    lamp = (60, 5, 64, 9)
    r = judge([frame((10, 10, 20, 20), lamp)], [frame()], [frame(lamp)])
    out = tmp_path / "d.png"
    mod_check.draw_diff(r, [(0, 0, 8, 8)], [], str(out))
    im = Image.open(out).convert("RGB")
    assert im.getpixel((15, 15)) == (255, 0, 0)
    assert im.getpixel((61, 6)) == (255, 220, 0)
    assert im.getpixel((0, 0)) == (0, 255, 0)


# -- the result ----------------------------------------------------------------

def rows(*verdicts):
    return [("%02d" % i, {"verdict": v}) for i, v in enumerate(verdicts)]


def test_the_result():
    assert mod_check.result_of(rows("same", "expected"), []) == ("PASS", 0)
    assert mod_check.result_of(rows("same", "DIFFERS"), []) == ("FAIL", 1)
    assert mod_check.result_of(rows("same", "unstable"), []) == ("INCONCLUSIVE", 3)
    # The image under test stopped: its own failure, whatever the pictures.
    assert mod_check.result_of(rows("same", "missing"), ["image"]) == ("FAIL", 1)
    # A stock run stopped: the check cannot tell.
    assert mod_check.result_of(rows("same", "missing"), ["stock2"]) == ("INCONCLUSIVE", 3)
    assert mod_check.result_of(rows("same", "same"), ["stock1"]) == ("INCONCLUSIVE", 3)
    assert mod_check.result_of(rows("DIFFERS", "missing"), ["stock1"]) == ("FAIL", 1)


# -- running -------------------------------------------------------------------

def test_the_deck_runs_headless_without_sound_link_or_start_options():
    env = mod_check.base_env({"PATH": "/bin", "DJLINK": "1", "MAIN_ARGS": "-S", "SNAPSHOT": "loaded",
                              "CDJ_MAIN_HIGH_FPS": "1", "CDJ_PANEL_KEYSOCK": "40124", "GUI_DISPLAY": "gtk"})
    assert env == {"PATH": "/bin", "GUI_DISPLAY": "none", "NOSOUND": "1"}


def test_the_knobs_are_what_the_launcher_added():
    before = {"PATH": "/bin", "NOSOUND": "1"}
    after = dict(before, CDJ_ATA="1", CDJ_C6747_IDLE="0x1:0x2:0x3", CDJ_PANEL_KEYSOCK="40123")
    assert mod_check.knobs(after, before) == {"CDJ_ATA": "1", "CDJ_C6747_IDLE": "0x1:0x2:0x3"}


def test_the_stock_point_follows_everything_that_shapes_the_machine(tmp_path):
    files = {}
    for name in ("qemu", "stock", "stick", "stick2", "m.so"):
        files[name] = tmp_path / name
        files[name].write_bytes(name.encode())
    argv = [str(files["qemu"]), "-M", "cdj900nxs", "-kernel", str(files["stock"])]
    env = {"CDJ_ATA": "1", "C66X_JIT": str(files["m.so"])}

    def point(media=str(files["stick"]), left_out=()):
        return deck.point_dir(str(tmp_path), argv, env, media, "idle", left_out)

    p = point()
    assert p == point() and os.path.basename(p) == "idle" and p.startswith(str(tmp_path))
    assert point(media=str(files["stick2"])) != p
    assert point(left_out=["CDJ_C6747_IDLE"]) != p
    files["m.so"].write_bytes(b"rebuilt")
    assert point() != p


def test_a_save_run_records_no_pause_and_a_restore_reads_the_point():
    argv = ["q", "-M", "x"]
    assert mod_check.run_argv(argv, "unix:/t/m.sock,server=on,wait=off") ==         argv + ["-monitor", "unix:/t/m.sock,server=on,wait=off"]
    saving = mod_check.run_argv(argv, "M", save=True)
    assert saving[-2:] == ["-global", "migration.store-global-state=off"]
    restoring = mod_check.run_argv(argv, "M", restore="/tmp/p/idle")
    assert restoring[-2:] == ["-incoming", "file:%s/main.vm" % host.native("/tmp/p/idle")]


def test_the_key_ports_are_distinct_and_free():
    ports = mod_check.free_udp_ports(3)
    assert len(set(ports)) == 3 and all(20000 < p < 45000 for p in ports)


def test_the_run_layout_swaps_only_the_stick_and_the_log_folder(tmp_path):
    lay = mod_check.RunLayout(str(tmp_path / "stick.img"), str(tmp_path / "run"))
    assert lay.usb_image == str(tmp_path / "stick.img") and lay.logs == str(tmp_path / "run")
    assert lay.extract == mod_check.Layout().extract
