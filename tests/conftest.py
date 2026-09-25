# SPDX-License-Identifier: GPL-2.0-or-later
"""Make the tools importable the way they import each other.

The scripts are not a package: each directory puts itself on sys.path and
imports its siblings by bare name, so the tests do the same.
"""
import os
import sys

TESTS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TESTS)

for sub in ("midi", "scripts/run", "scripts/net", "scripts/firmware", "scripts/media"):
    path = os.path.join(ROOT, *sub.split("/"))
    if path not in sys.path:
        sys.path.insert(0, path)
if TESTS not in sys.path:
    sys.path.insert(0, TESTS)
