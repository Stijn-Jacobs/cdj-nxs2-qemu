# SPDX-License-Identifier: GPL-2.0-or-later
"""Make the tools importable the way they import each other.

The scripts are not a package: each directory puts itself on sys.path and
imports its siblings by bare name, so the tests do the same.
"""
import os
import sys

import pytest

TESTS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TESTS)

for sub in ("midi", "scripts/run", "scripts/net", "scripts/firmware", "scripts/media", "mods"):
    path = os.path.join(ROOT, *sub.split("/"))
    if path not in sys.path:
        sys.path.insert(0, path)
if TESTS not in sys.path:
    sys.path.insert(0, TESTS)


@pytest.fixture
def synthetic_main(monkeypatch):
    """A made-up MAIN image has no tasks to walk from; give the validator an
    empty boot-critical set instead."""
    import bootcrit
    monkeypatch.setattr(bootcrit, "closure",
                        lambda img, base=0, every_task=False: bootcrit.Closure([], {}, 0, {"UpDtae_TASK": 0}, []))
