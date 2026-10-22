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
GUI- or MAIN-specific. An executed-in-place profile may leave ram_base None:
the range is then the erased tail of the image itself.

An image is taken as it comes. A mod whose site is not found exactly once, is
overlapped or finds its space taken is left out and reported; the others
still go in. A mod this engine placed earlier is recognised by its routine
bytes and its call site, and left alone, so patching a patched image
changes nothing and a missing mod can be added to it.
"""
import collections
import hashlib
import os
import re
import struct

ERASED = 0xFF

# byteorder: '>' (default) for a big-endian CPU (the GUI's SH7269), '<' for a
# little-endian one (MAIN's SH7724) -- every halfword and word this module
# reads or writes in the image goes through it, so a mod's sig/args/literal
# are always given as the instruction's logical value, never raw bytes.
ImageProfile = collections.namedtuple(
    'ImageProfile', 'flash_base loadtab_off loadtab_n ram_base ram_end byteorder cave',
    defaults=('>', None))

# Code the firmware never runs, found by the strings of its entry points
# instead of by being erased: an executed-in-place image with no erased run
# worth the name can still give a mod room there. The shell table holds, after
# the word equal to the address of each handler's name string, the handler's
# own address. The cave runs from the lowest handler named in `handlers` to the
# handler named in `end`, and must still hold the bytes of the stock image
# (sha256), so another firmware version, or a second claimant, is refused.
Cave = collections.namedtuple('Cave', 'handlers end sha256')

# A firmware address a routine needs, found in the image instead of written
# into the routine.
# sig       halfwords around it, as in Mod.sig
# index     the halfword the address belongs to
# literal   True: the address is the value that halfword loads with
#           mov.l @(disp,pc); False: the address of the halfword itself
# after     another Anchor; only a match behind that one counts, which settles
#           a signature that occurs more than once
# delta     added to the address found (a field inside the record it names)
Anchor = collections.namedtuple('Anchor', 'sig index literal after delta', defaults=(None, 0))

# An address in the free range the routines are placed in: `delta` bytes from
# the end of the erased run (negative to be inside it). Words the placed
# routines hand over to each other sit there, out of reach of the routines.
FreeEnd = collections.namedtuple('FreeEnd', 'delta')

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
# imports       ((stock value, Anchor or FreeEnd), ...): every word of the
#               routine that holds the stock value (the address in the
#               firmware the routine was written against) is set to the
#               address the Anchor finds in this image
# hook          False: the routine replaces the span and repeats it itself.
#               True: the routine is one of possibly several hooks on the
#               span; the span is moved into a trampoline shared by every
#               selected hook mod with the same sig/start/end, which runs it
#               once and then calls each hook -- see trampoline().
# after         names of hook mods on the same span whose routines must run
#               before this one when both are selected; hooks without such a
#               constraint run in alphabetical order
Mod = collections.namedtuple(
    'Mod', 'what target fw_versions sig start end args literal hook imports after',
    defaults=(False, (), ()))


# A patch that rewrites bytes the firmware reads as data (the USB descriptors,
# say) instead of calling a routine placed beside it.
# old, new  the stock bytes and what replaces them, of the same length; `count`
#           places in the window hold `old`
Edit = collections.namedtuple('Edit', 'old new count', defaults=(1,))

# what, target, fw_versions as in Mod
# table     bytes that occur exactly once in the image and are not themselves
#           edited, so a patched image is still found
# before    where the window the edits are searched in starts, in bytes
#           before the table
# reach     the window's length
# edits     a tuple of Edit, or a function of the environment that returns one
#           (what a patch takes from the person applying it, such as a PID)
# kind      'site' (the default) for a rewrite of firmware code, which the
#           validator holds to a call site's size; 'data' for a block of bytes
#           the firmware reads as data (a USB descriptor), which may be large
#           but is still checked against the boot-critical code closure
DataPatch = collections.namedtuple('DataPatch', 'what target fw_versions table before reach edits kind',
                                   defaults=('site',))


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

    def addr_of(self, off, required=True):
        for _, dest, src, size in self.copies():
            if src <= off < src + size:
                return dest + off - src
        if required:
            raise SystemExit('file offset %#x is not in any copied segment' % off)
        return None

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


class NotApplied(Exception):
    """A mod that is left out of a patch, with the reason it is reported."""


def word_matches(img, off, want):
    op = img.u16(off)
    return op == want or (is_pcrel_load(want) and op & 0xFF00 == want & 0xFF00)


def sig_hits(img, sig, after=0):
    """File offsets of every place `sig` occurs behind file offset `after`.
    A mov.l @(disp,pc) matches on opcode and register only. The image is
    searched for the longest run of exact halfwords and each find is checked
    against the whole signature."""
    order = img.p.byteorder
    runs, start = [], None
    for k, want in enumerate(sig + [None]):
        if want is not None and not is_pcrel_load(want):
            start = k if start is None else start
        elif start is not None:
            runs.append((k - start, start))
            start = None
    if not runs:
        raise SystemExit('a signature of nothing but literal loads cannot be searched for')
    length, first = max(runs)
    probe = struct.pack('%s%dH' % (order, length), *sig[first:first + length])
    hits = []
    for _, dest, src, size in img.copies():
        if dest >= 0xFFF80000:      # the on-chip data copy, not code
            continue
        end = src + size - 2 * len(sig)
        at = img.d.find(probe, max(src, after) + 2 * first)
        while 0 <= at - 2 * first <= end:
            off = at - 2 * first
            if (off - src) % 2 == 0 and off >= max(src, after) and all(
                    word_matches(img, off + 2 * k, want) for k, want in enumerate(sig)):
                hits.append(off)
            at = img.d.find(probe, at + 1)
    return hits


def find_sig(img, mod):
    """The one place a mod's signature occurs."""
    hits = sig_hits(img, mod.sig)
    if len(hits) != 1:
        raise NotApplied('call site found %d times -- not patched' % len(hits))
    return hits[0]


def find_applied(img, mod, target):
    """File offsets of the start of the replaced span wherever the image holds
    the call this engine writes for `mod` to `target`, between the halfwords
    the signature has around the span. The call is found by its literal, the
    address it jumps to, because a span that is the whole signature has no
    context to search for."""
    order = img.p.byteorder
    pool = struct.pack(order + 'I', target)
    span = 2 * (mod.end - mod.start)
    found = set()
    lit = img.d.find(pool)
    while lit >= 0:
        for site in range(max(lit - 2 * (len(mod.args) + 7), 2 * mod.start), lit + 1, 2):
            addr = img.addr_of(site, required=False)
            if addr is None or site + span > len(img.d):
                continue
            try:
                want = call_site(mod, addr, addr + span, target, byteorder=order, jump=mod.hook)
            except NotApplied:
                continue
            around = ([(site - 2 * (mod.start - k), w) for k, w in enumerate(mod.sig[:mod.start])]
                      + [(site + span + 2 * (k - mod.end), w) for k, w in enumerate(mod.sig[mod.end:], mod.end)])
            if (bytes(img.d[site:site + len(want)]) == want
                    and all(word_matches(img, off, w) for off, w in around)):
                found.add(site)
        lit = img.d.find(pool, lit + 1)
    return sorted(found)


def anchor_offset(img, anchor):
    """File offset of the halfword an Anchor names."""
    after = anchor_offset(img, anchor.after) if anchor.after else 0
    hits = sig_hits(img, anchor.sig, after=after)
    if anchor.after:
        hits = hits[:1]
    if len(hits) != 1:
        raise NotApplied('import site found %d times' % len(hits))
    return hits[0] + 2 * anchor.index


def anchor_value(img, anchor):
    off = anchor_offset(img, anchor)
    if not anchor.literal:
        return img.addr_of(off) + anchor.delta
    return img.u32(literal_offset(img, off)) + anchor.delta


def literal_offset(img, off):
    """File offset of the literal the mov.l @(disp,pc) at `off` loads."""
    op = img.u16(off)
    if not is_pcrel_load(op):
        raise NotApplied('import site does not load a literal')
    return img.off_of((img.addr_of(off) & ~3) + 4 + (op & 0xFF) * 4)


def largest_erased_run(img):
    return max(re.finditer(rb'\xff+', bytes(img.d)), key=lambda m: m.end() - m.start())


def all_finds(data, probe):
    at = data.find(probe)
    while at >= 0:
        yield at
        at = data.find(probe, at + 1)


def handler_offset(img, name):
    """File offset of the shell handler whose name string is `name`. The table
    names the string, on a word boundary, by its cached or its uncached address."""
    text = img.d.find(name + b'\0')
    if text < 0 or img.d.find(name + b'\0', text + 1) >= 0:
        raise NotApplied('the shell string %r is not in the image exactly once' % name)
    here = img.p.flash_base + text
    words = [struct.pack(img.p.byteorder + 'I', here), struct.pack(img.p.byteorder + 'I', here | 0xA0000000)]
    hits = [at for word in words for at in all_finds(img.d, word) if at % 4 == 0]
    if len(hits) != 1 or hits[0] + 8 > len(img.d):
        raise NotApplied('the shell table does not name %r exactly once' % name)
    return img.off_of(img.u32(hits[0] + 4))


def find_cave(img):
    """(start, end) file offsets of the profile's cave."""
    cave = img.p.cave
    start = min(handler_offset(img, name) for name in cave.handlers)
    end = handler_offset(img, cave.end)
    if end <= start:
        raise NotApplied('the shell handlers are not in the order the cave expects')
    if hashlib.sha256(img.d[start:end]).hexdigest() != cave.sha256:
        raise NotApplied('the cave holds other bytes than the stock firmware')
    return start, end


def referenced(img, lo, hi):
    """File offsets of the words that point into [lo, hi) in either cache alias
    and sit where a pointer would: in a literal pool some load reads, or in a
    table of three or more image addresses in a row. Isolated words that look
    like an address are mostly instruction pairs and data, so they are not
    counted. A literal-pool read that lands in the range counts too."""
    mask = 0x1FFFFFFF
    base = img.p.flash_base & mask
    cached = img.p.flash_base >> 24
    n = len(img.d) // 4
    words = struct.unpack_from(img.p.byteorder + '%dI' % n, img.d, 0)
    # a word whose two halves start with the same byte is a pair of 16-bit
    # table entries, not an address
    points = [(w >> 24 in (cached, cached | 0xA0) and w >> 24 != (w >> 8) & 0xFF
               and 0 <= (w & mask) - base < len(img.d)) for w in words]
    pooled = {lit // 4 for _, lit, width in literal_pool_reads(img) if width == 4}
    hits, i = [], 0
    while i < n:
        if not points[i]:
            i += 1
            continue
        j = i
        while j < n and points[j]:
            j += 1
        table = j - i >= 3
        hits += [4 * k for k in range(i, j) if (table or k in pooled) and lo <= (words[k] & mask) - base < hi]
        i = j
    return hits + [lit for _, lit, _ in literal_pool_reads(img) if lo <= lit < hi]


def unproven(img, lo, hi):
    """Why [lo, hi) cannot be taken as free space, or None when it can. A run
    of 0xFF proves nothing (bitmap art holds such runs): nothing may point
    into it, no literal pool may be read from it, and it must lie outside
    every segment the load table copies."""
    if set(img.d[lo:hi]) != {ERASED}:
        return 'not erased'
    if img.p.loadtab_n:
        for _, _, src, size in img.copies():
            if src < hi and lo < src + size:
                return 'inside the segment the load table copies from %#x' % src
    hits = referenced(img, lo, hi)
    if hits:
        return 'the word at %#x points into it' % min(hits)
    return None


def free_end(img):
    """Address where the erased range the routines are placed in ends."""
    p = img.p
    if p.cave:
        return p.flash_base + find_cave(img)[1]
    if p.ram_base is not None:
        return p.ram_end
    return p.flash_base + largest_erased_run(img).end()


def import_value(img, source):
    if isinstance(source, FreeEnd):
        if img.p.loadtab_n:
            raise NotApplied('a scatter-loaded image has no free range to hand words over in')
        return free_end(img) + source.delta
    return anchor_value(img, source)


def import_offsets(mod, blob, order):
    """[(offset in the routine, source)] for every stock value in mod.imports."""
    found = []
    for value, source in mod.imports:
        packed = struct.pack(order + 'I', value)
        offs = [o for o in range(0, len(blob) - 3, 4) if blob[o:o + 4] == packed]
        if not offs:
            raise SystemExit('the routine of %r holds no word %#x to import' % (mod.what, value))
        found += [(o, source) for o in offs]
    return found


def replaced_literal(img, mod, sig_off):
    values = set()
    for k in range(mod.start, mod.end):
        if is_pcrel_load(mod.sig[k]):
            pc = img.addr_of(sig_off + 2 * k)
            lit = (pc & ~3) + 4 + (img.u16(sig_off + 2 * k) & 0xFF) * 4
            values.add(img.u32(img.off_of(lit)))
    if len(values) != 1:
        raise NotApplied('replaced code loads %d different literals' % len(values))
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
    why = unproven(img, src, src + size)
    if why:
        raise SystemExit('the flash after the on-chip RAM segments is not proven free: %s' % why)
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
    if p.cave:
        try:
            start, end = find_cave(img)
        except NotApplied as e:
            raise SystemExit(str(e))
        start = (start + 3) & ~3
        if end - start >= size:
            return None, p.flash_base + start, start
    elif p.ram_base is None:
        run = largest_erased_run(img)
        start = (run.start() + 15) & ~15
        if run.end() - start >= size:
            why = unproven(img, start, start + size)
            if why:
                raise SystemExit('the largest erased run, %#x, is not proven free: %s'
                                 % (p.flash_base + start, why))
            return None, p.flash_base + start, start
    else:
        lo, hi = p.ram_base - p.flash_base, p.ram_end - p.flash_base
        run = 0
        for off in range(lo, hi):
            run = run + 1 if img.d[off] == ERASED else 0
            if run >= size:
                start = off + 1 - run
                why = unproven(img, start, start + size)
                if why:
                    raise SystemExit('the erased span at %#x is not proven free: %s' % (p.flash_base + start, why))
                return None, p.flash_base + start, start
    raise SystemExit('no erased span of %d bytes in %s' % (size, 'the image' if p.ram_base is None
                     else '%#x-%#x' % (p.ram_base, p.ram_end)))


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
        raise NotApplied('call does not fit the replaced code')
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
    the order a trampoline calls them in: alphabetical, with each hook moved
    behind the ones it names in `after`."""
    sites = {}
    for name in sorted(mods):
        mod = mods[name]
        if mod.hook:
            sites.setdefault(site_key(mod), []).append(name)
    return {key: call_order(names, mods) for key, names in sites.items()}


def call_order(names, mods):
    ordered, waiting = [], list(names)
    while waiting:
        ready = next((n for n in waiting if not any(a in waiting for a in mods[n].after)), None)
        if ready is None:
            raise SystemExit('hooks %s each wait for another to run first' % ', '.join(waiting))
        ordered.append(ready)
        waiting.remove(ready)
    return ordered


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
        offsets[key] = cur
        size = trampoline_size(key, len(hooks))
        cur += size + (-size % 16)
    return offsets, cur


def trampoline_size(key, hooks):
    sig, start, end = key
    span = sig[start:end]
    return len(trampoline(span, [0] * sum(map(is_pcrel_load, span)),
                          [0] * hooks, 0, 0))


# The patches of one model's image: its ImageProfile, the routines of the mods
# (placed in the image, and so laid out together) and the DataPatches (rewrites
# of bytes already there).
PatchSet = collections.namedtuple('PatchSet', 'profile mods tables', defaults=({},))


def patch_set(profile, declared):
    """The PatchSet of the patches a model's mod folders declare."""
    return PatchSet(profile,
                    {n: p for n, p in declared.items() if isinstance(p, Mod)},
                    {n: p for n, p in declared.items() if isinstance(p, DataPatch)})


def patch_model(sets, blob_path, data, names, model, tag, report=None, spans=None):
    """patch() with the PatchSet `sets` holds for `model`, then its DataPatches.
    A mod the model has no patch for is reported as not applied; an image with
    none gets back as it is."""
    report = {} if report is None else report
    found = sets.get(model)
    known = {**found.mods, **found.tables} if found else {}
    for name in names:
        if name not in known:
            report[name] = ('skipped', 'no patch for %s' % model)
            print('%s: %s not applied: no patch for %s' % (tag, name, model))
    if not any(name in known for name in names):
        return bytes(data)
    out = bytes(data)
    if any(n in found.mods for n in names):
        out = patch(found.profile, lambda name: blob_path(name, model), data, found.mods,
                    [n for n in names if n in found.mods], tag=tag, report=report, spans=spans)
    if any(n in found.tables for n in names):
        out = patch_tables(out, found.tables, [n for n in names if n in found.tables], tag, report, spans)
    return out


def read_blobs(blob_path, mods):
    blobs = {}
    for name in mods:
        with open(blob_path(name), 'rb') as f:
            blobs[name] = f.read()
    return blobs


def fixed_part(mod, blob, order):
    """(offset, bytes) of the longest stretch of a routine that is the same in
    every image: between the words a value specific to the image is written
    into."""
    masked = [o for o, _ in import_offsets(mod, blob, order)] + ([len(blob) - 4] if mod.literal else [])
    edges = [0] + sorted(w for o in masked for w in (o, o + 4)) + [len(blob)]
    best = max(zip(edges[::2], edges[1::2]), key=lambda r: r[1] - r[0])
    return best[0], blob[best[0]:best[1]]


def earlier_segment(img, mods, blobs, offsets):
    """The address a previous patch put its shared segment at, from where
    the routines of the mods it applied now sit; None when there is none."""
    bases = set()
    for name, mod in mods.items():
        skip, probe = fixed_part(mod, blobs[name], img.p.byteorder)
        if len(probe) < 8:
            continue
        hit = img.d.find(probe)
        if hit < 0 or img.d.find(probe, hit + 1) >= 0:
            continue
        addr = img.addr_of(hit, required=False)
        if addr is not None:
            bases.add(addr - skip - offsets[name])
    if len(bases) > 1:
        raise NotApplied('the image carries routines from more than one earlier patch')
    return bases.pop() if bases else None


def routine_bytes(img, mod, blob, site_off):
    """The routine as this image gets it: the replaced literal and the
    imported addresses filled in."""
    order = img.p.byteorder
    routine = bytearray(blob)
    if mod.literal:
        routine[-4:] = struct.pack(order + 'I', replaced_literal(img, mod, site_off))
    for off, source in import_offsets(mod, blob, order):
        routine[off:off + 4] = struct.pack(order + 'I', import_value(img, source))
    return bytes(routine)


def matches_routine(img, mod, blob, off):
    """Whether the image holds this mod's routine at file offset `off`,
    apart from the words that differ per image."""
    skipped = [o for o, _ in import_offsets(mod, blob, img.p.byteorder)] + ([len(blob) - 4] if mod.literal else [])
    have = img.d[off:off + len(blob)]
    return len(have) == len(blob) and all(
        have[k] == blob[k] for k in range(len(blob))
        if not any(s <= k < s + 4 for s in skipped))


def hooks_at(mods, key):
    return [n for n in mods if mods[n].hook and site_key(mods[n]) == key]


def is_applied(img, mods, name, blob, dest, offsets):
    """Whether this engine already patched `name` into the image: its
    routine sits in the segment at `dest` and the call site is the exact
    call to it that the engine writes."""
    mod = mods[name]
    off = img.off_of(dest + offsets[name], required=False)
    if off is None or not matches_routine(img, mod, blob, off):
        return False
    if mod.hook:
        key = site_key(mod)
        target = dest + offsets[key]
        tramp = img.off_of(target, required=False)
        pool = struct.pack(img.p.byteorder + 'I', dest + offsets[name])
        size = trampoline_size(key, len(hooks_at(mods, key)))
        if tramp is None or pool not in img.d[tramp:tramp + size]:
            return False
    else:
        target = dest + offsets[name]
    return len(find_applied(img, mod, target)) == 1


def patch(profile, blob_path, data, mods, names, tag='sigpatch', report=None, spans=None):
    """Apply mods[name] for each name in names to data (one image blob), the
    routine bytes for each coming from blob_path(name). Call this once per
    target image with every mod you want in it -- that is what makes the
    shared placement in layout() correct.

    The image is taken as it comes. A mod that cannot go in -- its site is
    not found exactly once, it overlaps another, its space is taken -- is
    left out and the others still go in; `report`, when given, is filled
    with {name: ('applied' | 'present' | 'skipped', reason)}. A mod this
    engine patched into the image earlier is 'present' and left as it is, so
    patching a patched image returns it unchanged, and a mod missing from it
    is added into the same segment. A hook site that already carries hooks
    takes no further one: patch from the stock image.

    `spans`, when given, is filled with {name: [(offset, size, kind)]} for
    every byte range written for that mod: 'free' for routines and
    trampolines placed in erased space, 'site' for the call site that
    replaces firmware code, 'record' for a load-table entry."""
    unknown = [n for n in names if n not in mods]
    if unknown:
        raise SystemExit('%s: unknown mod(s) %s' % (tag, ', '.join(unknown)))
    img = Image(profile, data)
    selected = [n for n in sorted(mods) if n in names]
    report = {} if report is None else report
    spans = {} if spans is None else spans
    blobs = read_blobs(blob_path, mods)
    offsets, total = layout(blob_path, mods)

    def skip(name, reason):
        report[name] = ('skipped', reason)
        print('%s: %s not applied: %s' % (tag, mods[name].what, reason))

    try:
        dest = earlier_segment(img, mods, blobs, offsets)
    except NotApplied as e:
        for name in selected:
            skip(name, str(e))
        return bytes(img.d)

    sig_off, pending = {}, []
    for name in selected:
        hits = sig_hits(img, mods[name].sig)
        if len(hits) == 1:
            sig_off[name] = hits[0]
            pending.append(name)
        elif not hits and dest is not None and is_applied(img, mods, name, blobs[name], dest, offsets):
            report[name] = ('present', '')
            print('%s: %s already applied' % (tag, mods[name].what))
        elif not hits and dest is not None and mods[name].hook and any(
                is_applied(img, mods, n, blobs[n], dest, offsets)
                for n in hooks_at(mods, site_key(mods[name])) if n != name):
            skip(name, 'its site already carries hooks of an earlier patch')
        else:
            skip(name, 'call site found %d times' % len(hits))

    for a in list(pending):
        if a not in pending:
            continue
        for b in pending[pending.index(a) + 1:]:
            if b not in pending:
                continue
            if mods[a].hook and mods[b].hook and site_key(mods[a]) == site_key(mods[b]):
                continue        # one site, shared through its trampoline
            wa = (sig_off[a], sig_off[a] + 2 * len(mods[a].sig))
            wb = (sig_off[b], sig_off[b] + 2 * len(mods[b].sig))
            if wa[0] < wb[1] and wb[0] < wa[1]:
                pending.remove(b)
                skip(b, 'overlaps %s -- cannot combine' % mods[a].what)

    reads = literal_pool_reads(img)
    for name in list(pending):
        mod = mods[name]
        lo, hi = sig_off[name] + 2 * mod.start, sig_off[name] + 2 * mod.end
        for instr_off, lit_off, width in reads:
            if lo <= instr_off < hi:
                continue        # the reading instruction is itself replaced
            if lit_off < hi and lo < lit_off + width:
                pending.remove(name)
                skip(name, 'would overwrite a literal pool at %#x still read from %#x '
                     '-- narrow start/end to exclude it' % (lit_off, instr_off))
                break

    routines = {}
    for name in list(pending):
        mod = mods[name]
        try:
            routines[name] = routine_bytes(img, mod, blobs[name], sig_off[name])
        except NotApplied as e:
            pending.remove(name)
            skip(name, str(e))
    if not pending:
        return bytes(img.d)

    new_hooks = hook_sites({n: mods[n] for n in pending})
    rec = None
    try:
        if dest is None:
            rec, dest, src = place_segment(img, total)
        elif profile.cave:
            raise SystemExit('the cave already holds an earlier patch; patch from the stock image')
        else:
            src = img.off_of(dest)
            if img.off_of(dest + total - 1, required=False) != src + total - 1:
                raise SystemExit('the earlier segment is cut short')
        segment = bytearray(img.d[src:src + total])
        slots = [(offsets[n], len(routines[n])) for n in pending]
        slots += [(offsets[key], trampoline_size(key, len(hooks))) for key, hooks in new_hooks.items()]
        for start, size in slots:
            if not profile.cave and any(b != ERASED for b in segment[start:start + size]):
                raise SystemExit('the space at %#x is taken' % (dest + start))
    except SystemExit as e:
        for name in pending:
            skip(name, str(e))
        return bytes(img.d)

    for name in pending:
        segment[offsets[name]:offsets[name] + len(routines[name])] = routines[name]
        spans.setdefault(name, []).append((src + offsets[name], len(routines[name]), 'free'))

    for key, hooks in new_hooks.items():
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
        for name in hooks:
            spans.setdefault(name, []).append((src + offsets[key], len(code), 'free'))
    img.d[src:src + total] = segment
    if rec is not None:
        struct.pack_into(profile.byteorder + 'III', img.d, profile.loadtab_off + 12 * rec,
                         dest, src + profile.flash_base, total)
        for name in pending:
            spans.setdefault(name, []).append((profile.loadtab_off + 12 * rec, 12, 'record'))

    for name in pending:
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
        spans.setdefault(name, []).append((site_off, len(code), 'site'))
        report[name] = ('applied', '')
    if rec is not None:
        print('%s: %d bytes reserved at %#x from flash +%#x (load record %d)'
              % (tag, total, dest, src, rec))
    elif not profile.loadtab_n:
        print('%s: %d bytes reserved in place at %#x' % (tag, total, dest))
    return bytes(img.d)


def edit_runs(old, new):
    """(offset, size) of the stretches where two equal-length byte strings
    differ, joined across gaps shorter than three bytes the way validate.py
    reads them back."""
    runs = []
    for i, (a, b) in enumerate(zip(old, new)):
        if a != b:
            if runs and i - sum(runs[-1]) < 3:
                runs[-1] = (runs[-1][0], i + 1 - runs[-1][0])
            else:
                runs.append((i, 1))
    return runs


def occurrences(window, probe):
    found, at = [], window.find(probe)
    while at >= 0:
        found.append(at)
        at = window.find(probe, at + len(probe))
    return found


def patch_tables(data, tables, names, tag, report, spans):
    """Apply the DataPatch `tables[name]` for each name to data. A patch whose
    table is not found exactly once, or whose window holds neither all of the
    stock bytes nor all of the new ones, is left out and reported; one whose
    window already holds the new bytes is 'present'. Written ranges go to
    `spans` as 'site', since each is a few bytes of the image that existed."""
    img = bytearray(data)
    spans = {} if spans is None else spans
    for name in names:
        table = tables[name]
        hits = occurrences(bytes(img), table.table)
        if len(hits) != 1:
            report[name] = ('skipped', 'table found %d times' % len(hits))
            print('%s: %s not applied: table found %d times' % (tag, table.what, len(hits)))
            continue
        lo = hits[0] - table.before
        window = bytes(img[lo:lo + table.reach])
        edits = table.edits(os.environ) if callable(table.edits) else table.edits
        stock = [len(occurrences(window, e.old)) for e in edits]
        done = [len(occurrences(window, e.new)) for e in edits]
        if all(n == e.count for n, e in zip(done, edits)):
            report[name] = ('present', '')
            print('%s: %s already applied' % (tag, table.what))
            continue
        if not all(n == e.count for n, e in zip(stock, edits)):
            why = 'the bytes to change are not what this firmware has'
            report[name] = ('skipped', why)
            print('%s: %s not applied: %s' % (tag, table.what, why))
            continue
        patched = window
        for e in edits:
            patched = patched.replace(e.old, e.new)
        img[lo:lo + len(patched)] = patched
        spans.setdefault(name, []).extend((lo + off, size, table.kind) for off, size in edit_runs(window, patched))
        report[name] = ('applied', '')
        print('%s: %s rewritten at %#x' % (tag, table.what, lo))
    return bytes(img)
