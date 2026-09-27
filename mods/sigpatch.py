#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The signature-patch engine shared by every image-specific mod tool
(patch_gui.py; a patch_main.py for the MAIN image would use it the same way).

A mod replaces a stretch of firmware code -- found by its own instructions,
not its address -- with a call to a routine placed in a new scatter-load
record. The image is a flat memory-mapped blob with a scatter-load table of
12-byte records in groups of four: three copies (dest, flash source, size;
size 0 = unused) and a fourth that closes the group (start and length of its
zero-filled area, flash address of the next group). Where that table sits,
how many records it has, the image's own flash offset and the on-chip RAM
range to place new code in are all specific to one target image, so a caller
passes them in as an ImageProfile; nothing in here is GUI-specific.
"""
import collections
import struct

ERASED = 0xFF

ImageProfile = collections.namedtuple(
    'ImageProfile', 'flash_base loadtab_off loadtab_n ram_base ram_end')

# what          human name, printed by --list
# target        which image this mod patches ('gui' for a display firmware mod)
# fw_versions   the target image's own version strings (e.g. GUI "1.81") this
#               mod's signature was verified against; patch_update.py refuses
#               to apply it to any other version unless overridden
# sig           the halfwords around the replaced code. mov.l @(disp,pc)
#               matches on opcode and register only: its displacement depends
#               on where the code sits.
# start/end     sig[start:end] is what gets replaced by a call to the routine
#               plus `args`; the routine may clobber r0-r7 and nothing else.
# literal       the routine's last word is set to the one value the replaced
#               code loads with mov.l @(disp,pc) (an address that moves
#               between builds).
Mod = collections.namedtuple(
    'Mod', 'what target fw_versions sig start end args literal')


def is_pcrel_load(op):
    return op & 0xF000 == 0xD000


class Image:
    def __init__(self, profile, data):
        self.p = profile
        self.d = bytearray(data)
        self.recs = [struct.unpack_from('>III', self.d, profile.loadtab_off + 12 * i)
                     for i in range(profile.loadtab_n)]

    def copies(self):
        """(record index, dest, file offset, size) of every copy record."""
        p = self.p
        return [(i, dest, src - p.flash_base, size)
                for i, (dest, src, size) in enumerate(self.recs)
                if i % 4 != 3 and size and src >= p.flash_base
                and src - p.flash_base + size <= len(self.d)]

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
    p = img.p
    ram = [c for c in img.copies() if p.ram_base <= c[1] < p.ram_end]
    if not ram:
        raise SystemExit('no on-chip RAM segment in the load table')
    last = max(ram, key=lambda c: c[1] + c[3])
    bss_start, bss_len, _ = img.recs[last[0] | 3]
    if bss_start != last[1] + last[3]:
        raise SystemExit('on-chip RAM group does not end in its zero-fill '
                         'record -- unfamiliar load table')
    dest = (bss_start + bss_len + 0xFFF) & ~0xFFF

    src = (max(c[2] + c[3] for c in ram) + 15) & ~15
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


def layout(blob_path, mods):
    """(offset within the shared segment, total segment size) for every mod
    in the registry, in alphabetical order. Fixed by the registry alone --
    not by which of them a given run actually selects -- so a mod's own
    routine always lands at the same address whether it is patched on its
    own or alongside others, and combining mods in a different order patches
    the same bytes."""
    offsets, cur = {}, 0
    for name in sorted(mods):
        with open(blob_path(name), 'rb') as f:
            size = len(f.read())
        offsets[name] = cur
        cur += size + (-size % 16)
    return offsets, cur


def patch(profile, blob_path, data, mods, names, tag='sigpatch'):
    """Apply mods[name] for each name in names to data (one image blob), the
    routine bytes for each coming from blob_path(name). Call this once per
    target image with every mod you want in it -- that is what makes the
    shared placement in layout() correct. Patching mods one at a time and
    layering the results (or feeding one call's output into another) is not
    supported: the second call sees its own already-patched segment as part
    of the image's own on-chip RAM and refuses rather than placing a routine
    on top of it."""
    unknown = [n for n in names if n not in mods]
    if unknown:
        raise SystemExit('%s: unknown mod(s) %s' % (tag, ', '.join(unknown)))
    img = Image(profile, data)
    selected = [n for n in sorted(mods) if n in names]

    sig_off = {n: find_sig(img, mods[n]) for n in selected}
    for i, a in enumerate(selected):
        wa = (sig_off[a], sig_off[a] + 2 * len(mods[a].sig))
        for b in selected[i + 1:]:
            wb = (sig_off[b], sig_off[b] + 2 * len(mods[b].sig))
            if wa[0] < wb[1] and wb[0] < wa[1]:
                raise SystemExit('%s: %s and %s overlap -- cannot combine'
                                 % (tag, mods[a].what, mods[b].what))

    offsets, total = layout(blob_path, mods)
    rec, dest, src = place_segment(img, total)
    segment = bytearray(img.d[src:src + total])        # erased flash; place_segment checked it
    for name in selected:
        mod = mods[name]
        with open(blob_path(name), 'rb') as f:
            routine = f.read()
        if mod.literal:
            routine = routine[:-4] + struct.pack(
                '>I', replaced_literal(img, mod, sig_off[name]))
        segment[offsets[name]:offsets[name] + len(routine)] = routine
    img.d[src:src + total] = segment
    struct.pack_into('>III', img.d, profile.loadtab_off + 12 * rec,
                     dest, src + profile.flash_base, total)

    for name in selected:
        mod = mods[name]
        site_off = sig_off[name] + 2 * mod.start
        target = dest + offsets[name]
        code = call_site(mod, img.addr_of(site_off),
                         img.addr_of(sig_off[name] + 2 * mod.end), target)
        img.d[site_off:site_off + len(code)] = code
        print('%s: %s at %#x, called from %#x'
              % (tag, mod.what, target, img.addr_of(site_off)))
    print('%s: %d bytes reserved at %#x from flash +%#x (load record %d)'
          % (tag, total, dest, src, rec))
    return bytes(img.d)
