# SPDX-License-Identifier: GPL-2.0-or-later
"""web/firmware.js, the browser's firmware preparation, run in Node: it must
expect the same digests as the profile and, on the real update, produce every
image byte for byte as the Python setup does."""
import json
import os
import shutil
import subprocess
import sys

import pytest

from helpers import path

EMU = path()
if EMU not in sys.path:
    sys.path.insert(0, EMU)

from launcher import firmware  # noqa: E402

NODE = shutil.which("node")
REAL_UPD = path("..", "fw", "unzipped", "C2KNXS2.UPD")
needs_node = pytest.mark.skipif(not NODE, reason="no node on PATH")
needs_upd = pytest.mark.skipif(not os.path.exists(REAL_UPD), reason="fw/unzipped/C2KNXS2.UPD not present")

PROFILE = {os.path.basename(rel): sha for rel, sha in firmware.EXPECTED}


@needs_node
def test_module_expects_the_profile_digests():
    script = ("import('./web/firmware.js').then(m => console.log(JSON.stringify("
              "{upd: m.UPD_SHA256, expected: m.EXPECTED})))")
    run = subprocess.run([NODE, "--input-type=module", "-e", script], cwd=EMU,
                         capture_output=True, text=True, timeout=60)
    assert run.returncode == 0, run.stderr
    out = json.loads(run.stdout)
    assert out["upd"] == firmware.UPD_SHA256
    assert out["expected"] == PROFILE


@needs_node
@needs_upd
def test_real_update_unpacks_to_the_python_images():
    run = subprocess.run([NODE, path("web", "test_firmware.mjs"), REAL_UPD],
                         capture_output=True, text=True, timeout=300)
    assert run.returncode == 0, run.stderr
    out = json.loads(run.stdout)
    assert out["upd"] == firmware.UPD_SHA256
    assert out["got"] == PROFILE
