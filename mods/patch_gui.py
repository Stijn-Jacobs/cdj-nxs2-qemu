#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Apply code mods to the unpacked display (GUI) firmware image.

    usage: patch_gui.py <gui_unpacked.bin> <patched.bin> <mod> [<mod>...]
           patch_gui.py --assemble     rebuild every mod's .bin from its .s
           patch_gui.py --list

Each mod is a routine written in SH-2A assembly (gui_<mod>.s, assembled to
gui_<mod>.bin, which is kept next to it so patching needs no cross tools) and
a stretch of firmware code it replaces with a call to that routine. The patch
is made to the image, the way a firmware update would carry it:

  1. The routines go into the erased flash right after the on-chip RAM
     segments' source, and one new load-table record copies them to on-chip
     RAM past the end of the zero-filled area, where nothing in the image
     points.
  2. Each mod's call site is found by its instructions, not its address, and
     the image is left alone unless every site occurs exactly once.

The load table at 0x10 is twenty 12-byte records in groups of four: three
copies (dest, flash source, size; size 0 = unused) and a fourth that closes
the group (start and length of its zero-filled area, flash address of the
next group). The image starts at flash offset 0x30000. That reading fits
every record of v1.80 and v1.81, but the boot loader that walks the table is
not in the image, so it is inferred, not read.
"""
import collections
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

FLASH_BASE = 0x30000
LOADTAB_OFF = 0x10
LOADTAB_N = 20
OCRAM_BASE, OCRAM_END = 0x1C000000, 0x1C280000
ERASED = 0xFF

# sig: the halfwords around the replaced code. mov.l @(disp,pc) matches on
# opcode and register only: its displacement depends on where the code sits.
# The instructions sig[start:end] are replaced by `args`, then a call to the
# routine and a branch past the rest. `args` may use only what the replaced
# code had in hand; the routine may clobber r0-r7 and nothing else.
# literal: the routine's last word is set to the one value the replaced code
# loads with mov.l @(disp,pc) (an address that moves between builds).
Mod = collections.namedtuple('Mod', 'what sig start end args literal')

MODS = {
    # The RGB branch of the centre-waveform renderer, from the height clamp to
    # the end of the column fill. r5 = peak sample index, r8 = PWV5 array,
    # r12 = height, @(44,r15) = screen column; the canvas is the literal.
    'wave3': Mod(
        what='3-band centre waveform',
        sig=[
            0x2CC8, 0x8B00, 0xEC01, 0x4500, 0xEB07, 0x6053, 0x058D, 0x0A00,
            0xE000, 0x4B18, 0x605D, 0xE9FB, 0x6103, 0xE400, 0x4121, 0x4121,
            0x21B9, 0x6603, 0x26A9, 0x409C, 0x67C3, 0x77FF, 0x316C, 0xC91C,
            0x3C77, 0x8F22, 0x310C, 0x4715, 0x8B12, 0x56FB, 0xD28A, 0x4600,
            0x0510, 0x9E00, 0x0910, 0xA400, 0x362C, 0x356C, 0x369C, 0x7402,
            0xE2F4, 0x4218, 0x3473, 0x2611, 0x362C, 0x2511, 0x8FF7, 0x352C,
            0x34C3, 0x891A, 0x60E3, 0x0710, 0xA400, 0x4480, 0x50FB, 0xD27E,
            0x4000, 0x3748, 0x307C, 0xA010, 0x0215, 0x4C15, 0x8B0D, 0x52FB,
            0x66C3, 0xD479, 0x4200, 0x0710, 0xA400, 0x324C, 0x327C, 0xE7FA,
            0x4610, 0x4718, 0x2211, 0x8FFA, 0x327C, 0x5BFA, 0xE00C,
        ],
        start=3,            # `shll r5`, after the height clamp
        end=77,             # up to `mov.l @(40,r15),r11`
        # mov.l @(44,r15),r4; mov r12,r6; mov r8,r7 (r5 is already the index)
        args=[0x54FB, 0x66C3, 0x6783],
        literal=True,
    ),
}


def source(name):
    return os.path.join(HERE, 'gui_%s.s' % name)


def blob_path(name):
    return os.path.join(HERE, 'gui_%s.bin' % name)


def is_pcrel_load(op):
    return op & 0xF000 == 0xD000


class Image:
    def __init__(self, data):
        self.d = bytearray(data)
        self.recs = [struct.unpack_from('>III', self.d, LOADTAB_OFF + 12 * i)
                     for i in range(LOADTAB_N)]

    def copies(self):
        """(record index, dest, file offset, size) of every copy record."""
        return [(i, dest, src - FLASH_BASE, size)
                for i, (dest, src, size) in enumerate(self.recs)
                if i % 4 != 3 and size and src >= FLASH_BASE
                and src - FLASH_BASE + size <= len(self.d)]

    def addr_of(self, off):
        for _, dest, src, size in self.copies():
            if src <= off < src + size:
                return dest + off - src
        raise SystemExit('file offset %#x is not in any copied segment' % off)

    def off_of(self, addr):
        for _, dest, src, size in self.copies():
            if dest <= addr < dest + size:
                return src + addr - dest
        raise SystemExit('address %#x is not in any copied segment' % addr)

    def u16(self, off):
        return struct.unpack_from('>H', self.d, off)[0]

    def u32(self, off):
        return struct.unpack_from('>I', self.d, off)[0]


def find_sig(img, mod):
    hits = []
    for _, dest, src, size in img.copies():
        if dest >= 0xFFF80000:      # the on-chip data copy, not code
            continue
        for off in range(src, src + size - 2 * len(mod.sig), 2):
            for k, want in enumerate(mod.sig):
                op = img.u16(off + 2 * k)
                if op != want and not (is_pcrel_load(want)
                                       and op & 0xFF00 == want & 0xFF00):
                    break
            else:
                hits.append(off)
    if len(hits) != 1:
        raise SystemExit('%s: call site found %d times -- not patched'
                         % (mod.what, len(hits)))
    return hits[0]


def replaced_literal(img, mod, sig_off):
    values = set()
    for k in range(mod.start, mod.end):
        if is_pcrel_load(mod.sig[k]):
            pc = img.addr_of(sig_off + 2 * k)
            lit = (pc & ~3) + 4 + (img.u16(sig_off + 2 * k) & 0xFF) * 4
            values.add(img.u32(img.off_of(lit)))
    if len(values) != 1:
        raise SystemExit('%s: replaced code loads %d different literals'
                         % (mod.what, len(values)))
    return values.pop()


def place_segment(img, size):
    """(record index, dest, file offset) for a new on-chip RAM segment."""
    ocram = [c for c in img.copies() if OCRAM_BASE <= c[1] < OCRAM_END]
    if not ocram:
        raise SystemExit('no on-chip RAM segment in the load table')
    last = max(ocram, key=lambda c: c[1] + c[3])
    bss_start, bss_len, _ = img.recs[last[0] | 3]
    if bss_start != last[1] + last[3]:
        raise SystemExit('on-chip RAM group does not end in its zero-fill '
                         'record -- unfamiliar load table')
    dest = (bss_start + bss_len + 0xFFF) & ~0xFFF

    src = (max(c[2] + c[3] for c in ocram) + 15) & ~15
    if any(b != ERASED for b in img.d[src:src + size]):
        raise SystemExit('no erased flash after the on-chip RAM segments')
    for _, cdest, _, csize in img.copies():
        if cdest < dest + size and dest < cdest + csize:
            raise SystemExit('%#x is already loaded' % dest)

    free = [i for i, r in enumerate(img.recs) if i % 4 != 3 and r[2] == 0]
    if not free:
        raise SystemExit('no free load-table record')
    return free[0], dest, src


def call_site(mod, site, resume, target):
    """args; mov.l target,r1; jsr @r1; nop; bra resume; nop; the literal;
    nops up to resume."""
    ops = list(mod.args)
    load_at = len(ops)
    ops += [None, 0x410B, 0x0009, None, 0x0009]
    lit = (site + 2 * len(ops) + 3) & ~3
    load_pc = site + 2 * load_at
    ops[load_at] = 0xD100 | (lit - ((load_pc & ~3) + 4)) // 4
    bra_pc = site + 2 * (load_at + 3)
    ops[load_at + 3] = 0xA000 | ((resume - (bra_pc + 4)) // 2 & 0xFFF)
    code = b''.join(struct.pack('>H', op) for op in ops)
    code += b'\x00\x09' * ((lit - site - len(code)) // 2)
    code += struct.pack('>I', target)
    code += b'\x00\x09' * ((resume - site - len(code)) // 2)
    if len(code) != resume - site:
        raise SystemExit('%s: call does not fit the replaced code' % mod.what)
    return code


def patch(data, names):
    img = Image(data)
    sites, routines = [], []
    for name in names:
        mod = MODS[name]
        sig_off = find_sig(img, mod)
        with open(blob_path(name), 'rb') as f:
            routine = f.read()
        if mod.literal:
            routine = routine[:-4] + struct.pack(
                '>I', replaced_literal(img, mod, sig_off))
        sites.append((mod, sig_off))
        routines.append(routine + b'\x00' * (-len(routine) % 16))

    segment = b''.join(routines)
    rec, dest, src = place_segment(img, len(segment))
    img.d[src:src + len(segment)] = segment
    struct.pack_into('>III', img.d, LOADTAB_OFF + 12 * rec,
                     dest, src + FLASH_BASE, len(segment))

    target = dest
    for (mod, sig_off), routine in zip(sites, routines):
        site_off = sig_off + 2 * mod.start
        code = call_site(mod, img.addr_of(site_off),
                         img.addr_of(sig_off + 2 * mod.end), target)
        img.d[site_off:site_off + len(code)] = code
        print('patch_gui: %s at %#x, called from %#x'
              % (mod.what, target, img.addr_of(site_off)))
        target += len(routine)
    print('patch_gui: %d bytes loaded at %#x from flash +%#x (load record %d)'
          % (len(segment), dest, src, rec))
    return bytes(img.d)


def assemble():
    with tempfile.TemporaryDirectory() as tmp:
        for name in MODS:
            obj = os.path.join(tmp, name + '.o')
            subprocess.run(['sh4-linux-gnu-as', '--isa=sh2a', '-big',
                            '-o', obj, source(name)], check=True)
            subprocess.run(['sh4-linux-gnu-objcopy', '-O', 'binary',
                            obj, blob_path(name)], check=True)
            print('patch_gui: wrote %s (%d bytes)'
                  % (blob_path(name), os.path.getsize(blob_path(name))))


def main():
    args = sys.argv[1:]
    if args == ['--assemble']:
        assemble()
        return
    if args == ['--list']:
        for name, mod in MODS.items():
            print('%-8s %s' % (name, mod.what))
        return
    if len(args) < 3 or any(n not in MODS for n in args[2:]):
        raise SystemExit(__doc__)
    with open(args[0], 'rb') as f:
        data = f.read()
    out = patch(data, args[2:])
    with open(args[1], 'wb') as f:
        f.write(out)


if __name__ == '__main__':
    main()
