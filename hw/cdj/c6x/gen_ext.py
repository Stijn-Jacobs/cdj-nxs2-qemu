#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generate c66x_ext_table.h: decode entries for the C66x instructions that
GNU binutils 2.42 does not know (the SPRUGH7 "D"-prefixed SIMD pairs, fixed-
header forms, LAND/LOR, CROT90, ...), taken from the "Opcode for .X Unit"
figures and their operand rows in TI SPRUGH7.

The manual is a 2010 preview with some duplicated opfields; entries whose
(form, opfield) is already taken are reported and dropped, first page wins.
usage: gen_ext.py sprugh7.txt > c66x_ext_table.h"""
import re
import sys

FORMS = [
    # (match text, form id, opfield width)
    (".L Unit, 1/2 src - same as L2S but fixed hdr", "XF_L12U", 7),
    (".L Unit, 1/2 src — same as L2S but fixed hdr", "XF_L12U", 7),
    (".L Unit, 1/2 src", "XF_L12", 7),
    (".S Unit, 1/2 src, unconditional", "XF_S12U", 6),
    (".S Unit, 1/2 src", "XF_S12", 6),
    (".S Unit, 2 src fixed hdr", "XF_S2U", 4),
    (".S Unit, 2 src", "XF_S2", 4),
    (".D Unit, 2 src", "XF_D2", 4),
    (".M Unit, 32-bit, unconditional", "XF_M32U", 5),
    (".M Unit, Compound Results, new opcode space", "XF_MCRU", 5),
    (".M Unit, Compound Results", "XF_MCR", 5),
    (".M Unit, 32-bit", "XF_MCR", 5),
    (".M Unit, 2 src fixed hdr", "XF_MCRU", 5),
    (".L Unit, 1 src", "XF_L1", 5),
    (".S Unit, 1 src", "XF_S1", 5),
    (".M Unit, 1 src", "XF_M1", 5),
]

TYPES = {
    "dst": "XT_REG", "op1": "XT_REG", "op2": "XT_REG", "xop1": "XT_XREG", "xop2": "XT_XREG",
    "xop": "XT_XREG", "dwdst": "XT_PAIR", "dwop1": "XT_PAIR", "xdwop1": "XT_XPAIR",
    "xdwop2": "XT_XPAIR", "xdwop": "XT_XPAIR", "qwdst": "XT_QUAD", "qwop1": "XT_QUAD",
    "qwop2": "XT_QUAD", "scst5": "XT_SCST5", "ucst5": "XT_UCST5", "xlong": "XT_XLONG",
}
FIELDS = {"src1": "XFLD_SRC1", "src2": "XFLD_SRC2", "dst": "XFLD_DST"}

name = None
form = None
rows = []
slots = {}
# LANDN's page says "4, Load", copied from a neighbouring page; LAND is single-cycle.
OVERRIDE_SLOTS = {"LANDN": 0}
# Section 4.93 repeats the heading "DINTHSP", but its title ("Convert 32-bit
# Signed Integer ... Packed Signed 32-bit") and its Execution block
# (sp(src2_e) -> dst_e, sp(src2_o) -> dst_o) are DINTSP, the signed twin of
# 4.95 DINTSPU. Keeping the heading made its opfields decode as the 16-bit
# DINTHSP, which broke the DSPINT/DINTSP round() pairs the firmware uses.
OVERRIDE_NAME = {"4.93": "DINTSP"}
# DINTSP/DINTSPU read a register pair (src2_e, src2_o); the operand rows say "xop".
OVERRIDE_TYPES = {("DINTSP", "xop"): "xdwop", ("DINTSPU", "xop"): "xdwop"}
for line in open(sys.argv[1], errors="replace"):
    s = " ".join(line.split())
    m = re.match(r"^(4\.\d+) ([A-Z][A-Z0-9]+)$", s)
    if m:
        name = OVERRIDE_NAME.get(m.group(1), m.group(2))
        continue
    m = re.match(r"^Delay Slots (\d+)", s)
    if m and name and name not in slots:
        slots[name] = int(m.group(1))
        continue
    k = s.find("Opcode for ")
    if k >= 0:
        txt = s[k + len("Opcode for "):]
        form = None
        for pat, fid, width in FORMS:
            if txt.startswith(pat):
                form = (fid, width)
                break
        continue
    # DINTSPU's operand rows are printed "src2 ,dst  xop, dwdst". Only its rows
    # are tidied: doing it everywhere admits rows the table never had.
    if name == "DINTSPU":
        s = re.sub(r" ?, ?", ",", s)
    m = re.match(r"^(\S+) (\S+) (\.\w+(?: or \.\w+)?) ([01]+)$", s)
    if m and name and form:
        fields, types, unit, opf = m.groups()
        if len(opf) != form[1]:
            print("/* skipped %s %s: opfield %s is not %d bits */" % (name, form[0], opf, form[1]))
            continue
        fl = fields.split(",")
        tl = [OVERRIDE_TYPES.get((name, t), t) for t in types.split(",")]
        if len(fl) != len(tl) or any(t not in TYPES for t in tl):
            print("/* skipped %s %s: operands %s %s */" % (name, form[0], fields, types))
            continue
        rows.append((name, form[0], unit[1], int(opf, 2), fl, tl))

# SPRUGH7 4.152 FADDSP / 4.155 FSUBSP, "Opcode for .S Unit, 2 src": bits 11..8
# are fixed 1110 and the 3-bit opfield (100 / 110) sits in bits 7..5, above the
# .L-style 110 in bits 4..2, i.e. opfields 0x74 / 0x76 of the 7-bit field. The
# figure's 3-bit opfield is why the generic row parser skips them.
rows.append(("FADDSP", "XF_L12", "S", 0x74, ["src1", "src2", "dst"], ["op1", "xop2", "dst"]))
rows.append(("FSUBSP", "XF_L12", "S", 0x76, ["src1", "src2", "dst"], ["op1", "xop2", "dst"]))

# The constant-vs-40-bit-long compares (CMPEQ 4.41 opfield 1010000, and the
# CMPGT/CMPGTU/CMPLT/CMPLTU twins) list src2 as "slong"/"ulong" with no cross
# path, and binutils fixes x=0 for them. The firmware's 40-bit helpers use the
# cross form (cmpeq .L2X 0,a5:a4,b8 at DSP 0x0080AA24, reached by SLIP with a
# 1/16 beat loop), so the pair comes from the other side's register file.
# binutils still decodes x=0; these rows only ever see x=1.
rows.append(("CMPEQ", "XF_L12", "L", 0x50, ["src1", "src2", "dst"], ["scst5", "xlong", "dst"]))
rows.append(("CMPGT", "XF_L12", "L", 0x44, ["src1", "src2", "dst"], ["scst5", "xlong", "dst"]))
rows.append(("CMPLT", "XF_L12", "L", 0x54, ["src1", "src2", "dst"], ["scst5", "xlong", "dst"]))
rows.append(("CMPGTU", "XF_L12", "L", 0x4c, ["src1", "src2", "dst"], ["ucst5", "xlong", "dst"]))
rows.append(("CMPLTU", "XF_L12", "L", 0x5c, ["src1", "src2", "dst"], ["ucst5", "xlong", "dst"]))

print("/* Generated by gen_ext.py from TI SPRUGH7 (Nov 2010); do not edit. */")
taken = {}
out = []
for r in rows:
    key = (r[1], r[3], tuple(t.startswith("x") for t in r[5]))
    if (r[1], r[3]) in taken and taken[(r[1], r[3])] != r[0]:
        print("/* duplicate opfield: %s %s %#x also claimed by %s; kept %s */"
              % (r[0], r[1], r[3], taken[(r[1], r[3])], taken[(r[1], r[3])]))
        continue
    if (r[1], r[3]) in taken:
        continue
    taken[(r[1], r[3])] = r[0]
    ops = ", ".join("{ %s, %s }" % (FIELDS[f], TYPES[t]) for f, t in zip(r[4], r[5]))
    ds = OVERRIDE_SLOTS.get(r[0], slots.get(r[0], 0))
    out.append('    { "%s", \'%s\', %s, 0x%x, %d, %d, { %s } },'
               % (r[0].lower(), r[2], r[1], r[3], ds, len(r[4]), ops))
print("static const c66x_ext_op c66x_ext_table[] = {")
print("\n".join(out))
print("};")
