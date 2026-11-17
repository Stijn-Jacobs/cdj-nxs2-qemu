# SPDX-License-Identifier: GPL-2.0-or-later
"""The full-replay gate must reject incomplete or unequal oracle outputs."""
import json
import os
from pathlib import Path
import subprocess
import sys

import pytest

GATE = Path(__file__).resolve().parents[1] / "scripts/test/dsp_native_replay.py"
LOG = "replayed 10 of 10 records\nfinal: state 0x1 writes 0x2 ram 0x3\nverdict: EXACT\n"


@pytest.mark.parametrize("native,code,passed", [
    (LOG, 0, True),
    (LOG.replace("10 of 10", "11 of 11"), 0, False),
    (LOG.replace("10 of 10", "9 of 10"), 0, False),
    (LOG.replace("ram 0x3", "ram 0x4"), 0, False),
    (LOG.replace("EXACT", "DIVERGED"), 0, False),
    (LOG.replace("final:", "missing:"), 0, False),
    (LOG, 1, False),
    ("c66x jit: no module for ABI 12\n" + LOG, 0, False),
])
def test_replay_gate(tmp_path, native, code, passed):
    body = ("import os\n"
            "assert 'C66X_JIT_AUTO' not in os.environ\n"
            "assert 'REPLAY_KEEP_GOING' not in os.environ\n"
            "native = bool(os.environ.get('C66X_JIT'))\n"
            f"print({native!r} if native else {LOG!r}, end='')\n"
            f"raise SystemExit({code} if native else 0)\n")
    if os.name == "nt":
        # Windows cannot start a shebang script, so a .cmd runs it instead.
        script = tmp_path / "runner.py"
        script.write_text(body)
        runner = tmp_path / "runner.cmd"
        runner.write_text(f'@"{sys.executable}" "{script}" %*\n')
    else:
        runner = tmp_path / "runner"
        runner.write_text("#!" + sys.executable + "\n" + body)
        runner.chmod(0o755)
    record, module = tmp_path / "record", tmp_path / "module"
    record.write_bytes(b"synthetic fixture")
    module.write_bytes(b"synthetic fixture")
    out = tmp_path / "out"
    env = dict(os.environ, C66X_JIT_AUTO="unwanted", REPLAY_KEEP_GOING="1")
    run = subprocess.run([sys.executable, str(GATE), "--runner", str(runner),
                          "--record", str(record), "--module", str(module), "--out", str(out)],
                         env=env, capture_output=True, text=True)
    assert run.returncode == (0 if passed else 1), run.stdout + run.stderr
    assert json.loads((out / "receipt.json").read_text())["verdict"] == ("PASS" if passed else "FAIL")


def test_missing_private_inputs_skip_without_creating_outputs(tmp_path):
    out = tmp_path / "out"
    run = subprocess.run([sys.executable, str(GATE), "--record", str(tmp_path / "missing"),
                          "--out", str(out)], capture_output=True, text=True)
    assert run.returncode == 0 and "SKIP DSP native replay" in run.stdout
    assert not out.exists()
