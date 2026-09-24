#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""One line of little-endian words read out of a memsave snapshot.

Prints all the addresses on one line, so a per-run table stays readable. A
word outside the snapshot prints dashes rather than being skipped, so a missing
value cannot be mistaken for a shifted column.

usage: snapshot_peek.py <base> <file> <addr> [addr ...]
"""
import struct
import sys

base = int(sys.argv[1], 0)
data = open(sys.argv[2], "rb").read()

out = []
for a in sys.argv[3:]:
    off = int(a, 0) - base
    if off < 0 or off + 4 > len(data):
        out.append("%10s" % "--------")
    else:
        out.append(" %08x" % struct.unpack_from("<I", data, off)[0])
sys.stdout.write("".join(out) + "\n")
