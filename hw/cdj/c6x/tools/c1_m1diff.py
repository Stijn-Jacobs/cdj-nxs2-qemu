#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check c6xdis against objdump: disassemble every address in the reference
table and diff the text. /tmp/dsp-disasm.json maps each instruction address
(a decimal string) to the text binutils objdump prints for it.
usage: c1_m1diff.py <c6xdis binary> <main_unpacked.bin>"""
import collections
import json
import re
import subprocess
import sys

table = json.load(open("/tmp/dsp-disasm.json"))
addrs = sorted(int(k) for k in table)
p = subprocess.run([sys.argv[1], sys.argv[2]], input="\n".join(map(str, addrs)) + "\n",
                   capture_output=True, text=True, check=True)
mine = {}
for line in p.stdout.splitlines():
    a, t = line.split("\t", 1)
    mine[int(a)] = t

diff = collections.Counter()
examples = {}
same = 0
undef_same = 0
extended = 0
rs5 = 0
REG = re.compile(r"\b[ab]\d+\b")
for a in addrs:
    ref = table[str(a)]
    t = mine.get(a)
    if t == ref["text"]:
        same += 1
        if ref["mnem"] == "__undef__":
            undef_same += 1
        continue
    # objdump (binutils 2.42) knows no C66x additions; c66x_ext_table.h does.
    if ref["text"].startswith("<undefined") and t and not t.lstrip("| ").startswith("<undefined"):
        extended += 1
        continue
    # binutils applies the RS header bit to the 5-bit register of the compact
    # moves too; the decoder follows SPRU732J instead (c66x_decode.c).
    if ref["mnem"] == "mv" and ref.get("size") == 2 and t and REG.sub("r", t) == REG.sub("r", ref["text"]):
        rs5 += 1
        continue
    key = ref["mnem"]
    diff[key] += 1
    examples.setdefault(key, []).append((a, ref["text"], t))

print("entries %d, identical %d (of which undefined %d), decoded by the C66x table %d, "
      "compact mv corrected for RS %d, different %d"
      % (len(addrs), same, undef_same, extended, rs5, sum(diff.values())))
for k, n in diff.most_common():
    print("  %-12s %d" % (k, n))
    for a, r, t in examples[k][:3]:
        print("      %#010x  objdump: %s" % (a, r))
        print("      %10s  c66x:    %s" % ("", t))
