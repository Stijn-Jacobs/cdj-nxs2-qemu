# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/run/warm_jit.py: the sweep schedule and the virtual-clock reader."""
import warm_jit as wj
from helpers import run_script


def test_schedule_repeats_the_plan_every_period():
    plan = wj.schedule(60, 2 * wj.PERIOD)
    assert plan[0] == (60, "mt")
    assert plan[len(wj.PLAN)] == (60 + wj.PERIOD, "mt")
    assert len(plan) == 2 * len(wj.PLAN)


def test_schedule_stops_before_the_end():
    plan = wj.schedule(10, 20)
    assert plan == [(10, "mt"), (15, 100)]
    assert all(t < 30 for t, _ in wj.schedule(10, 20))
    assert wj.schedule(0, 0) == []


def test_every_pass_leaves_the_deck_as_it_found_it():
    mt_on, fader = False, wj.CENTRE
    for _, act in wj.PLAN:
        if act == "mt":
            mt_on = not mt_on
        else:
            assert 0 <= act <= 254
            fader = act
    assert not mt_on and fader == wj.CENTRE


def test_vclock_reads_milliseconds(tmp_path):
    f = tmp_path / "vclock"
    c = wj.VClock(str(f), stale=5, boot_timeout=5)
    assert c.read() is None
    f.write_text("12345")
    assert c.read() == 12.345
    f.write_text("garbage")
    assert c.read() is None


def test_vclock_wait_until(tmp_path, capsys):
    f = tmp_path / "vclock"
    f.write_text("5000")
    c = wj.VClock(str(f), stale=0.05, boot_timeout=0.05)
    assert c.wait_until(4.0, poll=0.01)
    assert not c.wait_until(6.0, poll=0.01)
    assert "stopped at 5.0 s" in capsys.readouterr().out
    missing = wj.VClock(str(tmp_path / "none"), stale=1, boot_timeout=0.05)
    assert not missing.wait_until(1.0, poll=0.01)
    assert "never appeared" in capsys.readouterr().out


def test_print_plan_cli():
    r = run_script("scripts/run/warm_jit.py", "t", "--start", "0", "--duration", "6", "--print-plan")
    assert r.returncode == 0, r.stderr
    assert r.stdout.split("\n")[:2] == ["    0.0  MASTER TEMPO", "    5.0  fader 100"]
