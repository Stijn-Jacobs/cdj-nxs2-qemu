#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The signature-patch engine shared by every image-specific mod tool
(patch_gui.py, patch_main.py).

A mod replaces a stretch of firmware code -- found by its own instructions,
not its address -- with a call to a routine placed in newly-claimed space.
Two placement shapes are supported, chosen by the ImageProfile:

  scatter-load (loadtab_n > 0, e.g. the GUI image): the image is a flat
  memory-mapped blob with a scatter-load table of 12-byte records in groups
  of four -- three copies (dest, flash source, size; size 0 = unused) and a
  fourth that closes the group (start and length of its zero-filled area,
  flash address of the next group). The routine's bytes go into the erased
  flash right after the on-chip RAM segments' own source, copied out to
  on-chip RAM by one new record.

  executed-in-place (loadtab_n == 0, e.g. the MAIN image): the image runs
  directly from its own mapped address (address == flash_base + file
  offset), so there is no separate flash-source/RAM-dest split and nothing
  to copy -- the routine's bytes are written straight into a run of erased
  flash inside ram_base..ram_end, which is both their storage and their
  execution address.

Where the load table sits, how many records it has, the image's own flash
offset and the range to place new code in are all specific to one target
image, so a caller passes them in as an ImageProfile; nothing in here is
GUI- or MAIN-specific.
"""
import collections
import struct

ERASED = 0xFF

# byteorder: '>' (default) for a big-endian CPU (the GUI's SH7269), '<' for a
# little-endian one (MAIN's SH7724) -- every halfword and word this module
# reads or writes in the image goes through it, so a mod's sig/args/literal
# are always given as the instruction's logical value, never raw bytes.
ImageProfile = collections.namedtuple(
    'ImageProfile', 'flash_base loadtab_off loadtab_n ram_base ram_end byteorder',
    defaults=('>',))

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
# hook          False: the routine replaces the span and repeats it itself.
#               True: the routine is one of possibly several hooks on the
#               span; the span is moved into a trampoline shared by every
#               selected hook mod with the same sig/start/end, which runs it
#               once and then calls each hook -- see trampoline().
Mod = collections.namedtuple(
    'Mod', 'what target fw_versions sig start end args literal hook',
    defaults=(False,))


def is_pcrel_load(op):
    return op & 0xF000 == 0xD000


class Image:
    def __init__(self, profile, data):
        self.p = profile
        self.d = bytearray(data)
        self.recs = [struct.unpack_from(profile.byteorder + 'III', self.d, profile.loadtab_off + 12 * i)
                     for i in range(profile.loadtab_n)]

    def copies(self):
        """(record index, dest, file offset, size) of every copy record. A
        profile with no load table (loadtab_n == 0) runs in place: the whole
        image is one identity-mapped copy, dest == flash_base + file offset."""
        p = self.p
        if not p.loadtab_n:
            return [(-1, p.flash_base, 0, len(self.d))]
        return [(i, dest, src - p.flash_base, size)
                for i, (dest, src, size) in enumerate(self.recs)
                if i % 4 != 3 and size and src >= p.flash_base
                and src - p.flash_base + size <= len(self.d)]

    def addr_of(self, off):
        for _, dest, src, size in self.copies():
            if src <= off < src + size:
                return dest + off - src
        raise SystemExit('file offset %#x is not in any copied segment' % off)

    def off_of(self, addr, required=True):
        for _, dest, src, size in self.copies():
            if dest <= addr < dest + size:
                return src + addr - dest
        if required:
            raise SystemExit('address %#x is not in any copied segment' % addr)
        return None

    def u16(self, off):
        return struct.unpack_from(self.p.byteorder + 'H', self.d, off)[0]

    def u32(self, off):
        return struct.unpack_from(self.p.byteorder + 'I', self.d, off)[0]


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
    """(record index, dest, file offset) for a new on-chip RAM segment. A
    profile with no load table has nothing to relocate -- see place_inplace()."""
    if not img.p.loadtab_n:
        return place_inplace(img, size)
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


def place_inplace(img, size):
    """(None, dest, file offset) for a run of erased flash inside an
    executed-in-place image (no load table -- see the module docstring). The
    routine's bytes are written straight there: that erased span IS its own
    RAM, so there is no separate copy record to write."""
    p = img.p
    lo, hi = p.ram_base - p.flash_base, p.ram_end - p.flash_base
    run = 0
    for off in range(lo, hi):
        run = run + 1 if img.d[off] == ERASED else 0
        if run >= size:
            start = off + 1 - run
            return None, p.flash_base + start, start
    raise SystemExit('no erased span of %d bytes in %#x-%#x' % (size, p.ram_base, p.ram_end))


def call_site(mod, site, resume, target, byteorder='>', jump=False):
    """args; mov.l target,r1; jsr @r1; nop; bra resume; nop; the literal;
    nops up to resume. With jump, mov.l target,r1; jmp @r1; nop instead: the
    target (a hook trampoline) comes back to resume itself."""
    def h(op):
        return struct.pack(byteorder + 'H', op)

    ops = list(mod.args)
    load_at = len(ops)
    ops += [None, 0x412B, 0x0009] if jump else [None, 0x410B, 0x0009, None, 0x0009]
    lit = (site + 2 * len(ops) + 3) & ~3
    load_pc = site + 2 * load_at
    ops[load_at] = 0xD100 | (lit - ((load_pc & ~3) + 4)) // 4
    if not jump:
        bra_pc = site + 2 * (load_at + 3)
        ops[load_at + 3] = 0xA000 | ((resume - (bra_pc + 4)) // 2 & 0xFFF)
    code = b''.join(h(op) for op in ops)
    code += h(0x0009) * ((lit - site - len(code)) // 2)
    code += struct.pack(byteorder + 'I', target)
    code += h(0x0009) * ((resume - site - len(code)) // 2)
    if len(code) != resume - site:
        raise SystemExit('%s: call does not fit the replaced code' % mod.what)
    return code


def trampoline(span, span_literals, hooks, resume, at, byteorder='>'):
    """The code shared by every selected hook mod on one span, placed at `at`
    (4-aligned) and entered by call_site(jump=True):

        the span, moved here unchanged except that each mov.l @(disp,pc)
          reads its value (span_literals, in order) from this pool
        mov.l r7..r0,@-r15              the span's r0-r7, r0 at @r15
        for each hook:
          mov r15,r5; add #32,r5        r5 = r15 as the span left it
          mov.l hook,r1; jsr @r1
          mov r15,r4                    r4 = the saved r0-r7
        mov.l @(4..28,r15),r1..r7       r0-r7 as the hooks left them
        mova resume,r0; lds.l @r0+,pr; mov.l @r15,r0; rts; add #32,r15

    The span runs first and with r15 untouched, so stack-relative operands
    and a call inside it behave as at the site; pr is dead there (the site's
    own jsr, or the span's, already overwrote it), which is what lets the
    way back go through pr and leave every general register as the hooks
    left it. A branch inside the span must land inside it or on its end,
    where the pushes start."""
    n = len(span)
    for i, op in enumerate(span):
        if (op & 0xF000 in (0x9000, 0xB000) or op & 0xFF00 == 0xC700
                or op & 0xF0DF == 0x0003):
            raise SystemExit('span op %#06x at +%d cannot be moved' % (op, 2 * i))
        if op & 0xF900 == 0x8900:                       # bt, bf, bt/s, bf/s
            disp = (op & 0xFF) - (0x100 if op & 0x80 else 0)
        elif op & 0xF000 == 0xA000:                     # bra
            disp = (op & 0xFFF) - (0x1000 if op & 0x800 else 0)
        else:
            continue
        if not 0 <= i + 2 + disp <= n:
            raise SystemExit('span branch at +%d leaves the span' % (2 * i))
    if span[-1] & 0xFD00 == 0x8D00 or span[-1] & 0xF000 == 0xA000 or span[-1] & 0xF0DF == 0x400B:
        raise SystemExit('span ends in a delayed branch without its slot')

    code_len = 2 * (n + 20 + 5 * len(hooks))
    pool = at + ((code_len + 3) & ~3)
    lits = [resume] + list(hooks) + list(span_literals)

    def disp(pc, k):
        return (pool + 4 * k - ((pc & ~3) + 4)) // 4

    ops = []
    k = 1 + len(hooks)
    for i, op in enumerate(span):
        if is_pcrel_load(op):
            op = op & 0xFF00 | disp(at + 2 * i, k)
            k += 1
        ops.append(op)
    ops += [0x2F06 | r << 4 for r in range(7, -1, -1)]
    for j in range(len(hooks)):
        pc = at + 2 * (len(ops) + 2)
        ops += [0x65F3, 0x7520, 0xD100 | disp(pc, 1 + j), 0x410B, 0x64F3]
    ops += [0x50F0 | r << 8 | r for r in range(1, 8)]
    ops += [0xC700 | disp(at + 2 * len(ops), 0), 0x4026, 0x60F2, 0x000B, 0x7F20]
    code = struct.pack(byteorder + '%dH' % len(ops), *ops)
    code += struct.pack(byteorder + 'H', 0x0009) * ((pool - at - len(code)) // 2)
    return code + struct.pack(byteorder + '%dI' % len(lits), *lits)


def site_key(mod):
    return tuple(mod.sig), mod.start, mod.end


def hook_sites(mods):
    """{(sig, start, end): names} for the hook mods of a registry, names in
    registry (alphabetical) order -- the order a trampoline calls them in."""
    sites = {}
    for name in sorted(mods):
        mod = mods[name]
        if mod.hook:
            sites.setdefault(site_key(mod), []).append(name)
    return sites


def literal_pool_reads(img):
    """(instruction file offset, literal file offset, width) for every
    mov.l/mov.w @(disp,PC) load anywhere in the image's copied segments --
    every instruction that depends on a literal pool, and where it reads it
    from. A patch must never overwrite a literal a *surviving* instruction
    still reads: the phrase-colours mod's first version replaced a span that
    happened to include its own function's literal pool, silently corrupting
    whatever else read it. An instruction inside the replaced span reading a
    literal that is also inside it is not a conflict -- both vanish together."""
    reads = []
    for _, dest, src, size in img.copies():
        if dest >= 0xFFF80000:      # the on-chip data copy, not code -- see find_sig
            continue
        n = size // 2
        words = struct.unpack_from(img.p.byteorder + '%dH' % n, img.d, src)
        for k, op in enumerate(words):
            pc = dest + 2 * k
            if op & 0xF000 == 0xD000:              # mov.l @(disp,PC),Rn
                lit, width = (pc & ~3) + 4 + (op & 0xFF) * 4, 4
            elif op & 0xF000 == 0x9000:             # mov.w @(disp,PC),Rn
                lit, width = pc + 4 + (op & 0xFF) * 2, 2
            else:
                continue
            off = img.off_of(lit, required=False)
            if off is not None:
                reads.append((src + 2 * k, off, width))
    return reads


def layout(blob_path, mods):
    """(offset within the shared segment, total segment size) for every mod
    in the registry, in alphabetical order, then for every hook site's
    trampoline (keyed by its hook_sites() key, sized for all of the site's
    registered hooks). Fixed by the registry alone -- not by which of them a
    given run actually selects -- so a mod's own routine always lands at the
    same address whether it is patched on its own or alongside others, and
    combining mods in a different order patches the same bytes."""
    offsets, cur = {}, 0
    for name in sorted(mods):
        with open(blob_path(name), 'rb') as f:
            size = len(f.read())
        offsets[name] = cur
        cur += size + (-size % 16)
    for key, hooks in sorted(hook_sites(mods).items(), key=lambda site: site[1]):
        sig, start, end = key
        span = sig[start:end]
        size = len(trampoline(span, [0] * sum(map(is_pcrel_load, span)),
                              [0] * len(hooks), 0, 0))
        offsets[key] = cur
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
            if mods[a].hook and mods[b].hook and site_key(mods[a]) == site_key(mods[b]):
                continue        # one site, shared through its trampoline
            wb = (sig_off[b], sig_off[b] + 2 * len(mods[b].sig))
            if wa[0] < wb[1] and wb[0] < wa[1]:
                raise SystemExit('%s: %s and %s overlap -- cannot combine'
                                 % (tag, mods[a].what, mods[b].what))

    reads = literal_pool_reads(img)
    for name in selected:
        mod = mods[name]
        lo, hi = sig_off[name] + 2 * mod.start, sig_off[name] + 2 * mod.end
        for instr_off, lit_off, width in reads:
            if lo <= instr_off < hi:
                continue        # the reading instruction is itself replaced
            if lit_off < hi and lo < lit_off + width:
                raise SystemExit('%s: %s would overwrite a literal pool at %#x still '
                                 'read from %#x -- narrow start/end to exclude it'
                                 % (tag, mod.what, lit_off, instr_off))

    offsets, total = layout(blob_path, mods)
    rec, dest, src = place_segment(img, total)
    segment = bytearray(img.d[src:src + total])        # erased flash; place_segment checked it
    for name in selected:
        mod = mods[name]
        with open(blob_path(name), 'rb') as f:
            routine = f.read()
        if mod.literal:
            routine = routine[:-4] + struct.pack(
                profile.byteorder + 'I', replaced_literal(img, mod, sig_off[name]))
        segment[offsets[name]:offsets[name] + len(routine)] = routine

    for key, hooks in hook_sites({n: mods[n] for n in selected}).items():
        _, start, end = key
        span_off = sig_off[hooks[0]] + 2 * start
        span = [img.u16(span_off + 2 * k) for k in range(end - start)]
        span_literals = [img.u32(img.off_of((img.addr_of(span_off + 2 * k) & ~3) + 4
                                            + (op & 0xFF) * 4))
                         for k, op in enumerate(span) if is_pcrel_load(op)]
        code = trampoline(span, span_literals, [dest + offsets[n] for n in hooks],
                          img.addr_of(span_off + 2 * len(span)), dest + offsets[key],
                          byteorder=profile.byteorder)
        segment[offsets[key]:offsets[key] + len(code)] = code
    img.d[src:src + total] = segment
    if rec is not None:
        struct.pack_into(profile.byteorder + 'III', img.d, profile.loadtab_off + 12 * rec,
                         dest, src + profile.flash_base, total)

    for name in selected:
        mod = mods[name]
        site_off = sig_off[name] + 2 * mod.start
        if mod.hook:
            target = dest + offsets[site_key(mod)]
            print('%s: %s at %#x, hooked on %#x'
                  % (tag, mod.what, dest + offsets[name], img.addr_of(site_off)))
        else:
            target = dest + offsets[name]
            print('%s: %s at %#x, called from %#x'
                  % (tag, mod.what, target, img.addr_of(site_off)))
        code = call_site(mod, img.addr_of(site_off),
                         img.addr_of(sig_off[name] + 2 * mod.end), target,
                         byteorder=profile.byteorder, jump=mod.hook)
        img.d[site_off:site_off + len(code)] = code
    if rec is not None:
        print('%s: %d bytes reserved at %#x from flash +%#x (load record %d)'
              % (tag, total, dest, src, rec))
    else:
        print('%s: %d bytes reserved in place at %#x' % (tag, total, dest))
    return bytes(img.d)
