# SPDX-License-Identifier: GPL-2.0-or-later
"""The patch engine for the C6x DSP program a player's MAIN image carries as
data (patch_dsp.py), next to sigpatch.py, which patches MAIN's own code.

MAIN does not run the program; it copies the block, in chunks, through the
DSP's host port to the SDRAM at its load address. The block is found in MAIN
by its first words and patched in place, so MAIN stays the same length and the
section is re-encoded the way patch_main does it.

A mod is a list of word edits and some code. Each edit names a 32-bit word of
the program by its address in the DSP's memory and the stock word it holds;
the code is placed at fixed addresses inside the program's own erased run.
The image is taken as it comes:

  an edit whose word is neither the stock word nor the new one means the
  program was changed by someone else, and the mod is left out; a mod whose
  edits and code are all in is present and left alone, so patching a patched
  image changes nothing; a mod found half applied is left out.

Code goes only where the stock bytes are erased, nothing in the program points
at (references() below), and 512 bytes from either end of the run, out of reach
of the short branches of the code around it. The words are written as given,
built by a mod's patch.py or assembled from its dsp_<name>.s (patch_main.py
--assemble), and the tests decode them with the emulator's own decoder.

Some players carry the program as a TI boot table instead of one block
(DspTable): sections, each copied to its own address, so a DSP address is found
in MAIN through the section that loads it. Such a program may have no erased
run, and MAIN sends the table at a length it does not read from the table, so
no section can be added. Its room is a cave: code of the program that nothing
in it reaches, whose stock bytes are pinned by their hash, so another firmware
version, or a second claimant, is refused.
"""
import collections
import hashlib
import struct

import sigpatch

# load     DSP address the block is copied to
# length   bytes of the block
# head     the block's first words, which find it in MAIN
# free     (start, end) DSP addresses of an erased run code may be placed in
DspProfile = collections.namedtuple('DspProfile', 'load length head free')

# head     the table's first words (entry point, first section header), which find it in MAIN
# length   bytes of the table, its terminator included
# cave     (start, end, sha256 of the stock bytes): DSP addresses of code nothing in the
#          program reaches, which code may be placed in
DspTable = collections.namedtuple('DspTable', 'head length cave')

# what          human name, printed by --list
# target        'dsp'
# fw_versions   the MAIN version strings the addresses were read from
# edits         ((address, stock word, new word), ...)
# code          ((address, (word, ...) or the name of an assembled dsp_<name>.s), ...)
#               placed in the profile's free run or cave
DspMod = collections.namedtuple('DspMod', 'what target fw_versions edits code')

ERASED = 0xFFFFFFFF
FETCH_PACKET = 32
EDGE = 512
# a pair of constants (mvk, mvkh) written to one register this close together
# is read as one address
PAIR_WINDOW = 16


def find_blob(data, profile):
    """File offset of the DSP program in MAIN."""
    probe = struct.pack('<%dI' % len(profile.head), *profile.head)
    hits = list(sigpatch.all_finds(data, probe))
    if len(hits) != 1:
        raise sigpatch.NotApplied('the DSP program was found %d times' % len(hits))
    if hits[0] + profile.length > len(data):
        raise sigpatch.NotApplied('the DSP program runs past the end of MAIN')
    return hits[0]


def sections(blob):
    """(load address, offset in the table, bytes) of each section of a boot table."""
    at = 4
    while True:
        size, = struct.unpack_from('<I', blob, at)
        if not size:
            return
        yield struct.unpack_from('<I', blob, at + 4)[0], at + 8, size
        at += 8 + (size + 3 & ~3)


def program_offset(profile, blob, addr, size=4):
    """Offset in the program found in MAIN of the `size` bytes at DSP address `addr`."""
    if isinstance(profile, DspTable):
        for load, at, length in sections(blob):
            if load <= addr and addr + size <= load + length:
                return at + addr - load
    elif profile.load <= addr and addr + size <= profile.load + profile.length:
        return addr - profile.load
    raise sigpatch.NotApplied('%#x is outside the DSP program' % addr)


def program_address(profile, blob, offset):
    """The DSP address of the byte at `offset` of the program found in MAIN."""
    if not isinstance(profile, DspTable):
        return profile.load + offset
    for load, at, size in sections(blob):
        if at <= offset < at + size:
            return load + offset - at
    raise sigpatch.NotApplied('offset %#x of the boot table is no section byte' % offset)


def program_references(profile, blob, lo, hi):
    """references() over the whole program, for a boot table each section with its own load address."""
    if not isinstance(profile, DspTable):
        return references(blob, profile.load, lo, hi)
    return sorted(set(ref for load, at, size in sections(blob)
                      for ref in references(blob[at:at + (size & ~3)], load, lo, hi)))


def cave_is_stock(profile, blob):
    start, end, sha256 = profile.cave
    at = program_offset(profile, blob, start, end - start)
    return hashlib.sha256(blob[at:at + end - start]).hexdigest() == sha256


def words_of(blob):
    return struct.unpack('<%dI' % (len(blob) // 4), blob)


def sign_extend(value, bits):
    return value - (1 << bits) if value >> (bits - 1) else value


def instructions(words, load):
    """(address, word) of every 32-bit instruction of a program. A fetch
    packet whose last word has 0xE in its top nibble is headed: bit 21 + i of
    the header marks word i as two 16-bit instructions, and the header itself
    is no instruction."""
    for packet in range(0, len(words) - 7, 8):
        header = words[packet + 7]
        headed = header >> 28 == 0xE
        for i in range(8):
            if headed and (i == 7 or header >> (21 + i) & 1):
                continue
            yield load + 4 * (packet + i), words[packet + i]


def references(blob, load, lo, hi):
    """Addresses of the words in the program that point into [lo, hi): a word
    holding such an address, a branch or call whose target is in it, and an
    mvk / mvkh pair that builds such an address. The pair is any two within
    PAIR_WINDOW instructions that write one register. Addresses are
    conservative: a word that only looks like one is a hit too."""
    words = words_of(blob)
    hits = [load + 4 * i for i, w in enumerate(words) if lo <= w < hi]
    recent = {}
    for n, (pc, w) in enumerate(instructions(words, load)):
        base = pc & ~(FETCH_PACKET - 1)
        if w & 0x7C == 0x10:                    # b / callp, 21-bit displacement
            target = base + 4 * sign_extend(w >> 7 & 0x1FFFFF, 21)
        elif w & 0x1FFC == 0x120:               # bnop, 12-bit displacement
            target = base + 4 * sign_extend(w >> 16 & 0xFFF, 12)
        elif w & 0x1FFC == 0x160:               # addkpc, 7-bit displacement
            target = base + 4 * (w >> 16 & 0x7F)
        elif w & 0x7C in (0x28, 0x68):          # mvk, mvkh
            kind, reg, const = w & 0x7C, (w >> 1 & 1, w >> 23 & 0x1F), w >> 7 & 0xFFFF
            recent[kind, reg] = (n, const)
            other = recent.get((0x28 if kind == 0x68 else 0x68, reg))
            if other and n - other[0] <= PAIR_WINDOW:
                low, high = (const, other[1]) if kind == 0x28 else (other[1], const)
                target = high << 16 | low
            else:
                continue
        else:
            continue
        if lo <= target < hi:
            hits.append(pc)
    return sorted(set(hits))


def patch(profile, data, mods, names, tag='dsppatch', report=None, spans=None, blob_path=None):
    """Apply mods[name] for each name in names to data (a MAIN image). A mod
    that cannot go in is left out and the others still go in; `report`, when
    given, is filled with {name: ('applied' | 'present' | 'skipped', reason)}.
    Code given by name is read from blob_path(name).

    `spans`, when given, is filled with {name: [(offset, size, kind)]} for
    every byte range written: 'dsp' for a word edit, 'dspfree' for code placed
    in the erased run, 'dspcave' for code placed in a boot table's cave."""
    unknown = [n for n in names if n not in mods]
    if unknown:
        raise SystemExit('%s: unknown mod(s) %s' % (tag, ', '.join(unknown)))
    report = {} if report is None else report
    spans = {} if spans is None else spans
    out = bytearray(data)

    def skip(name, reason):
        report[name] = ('skipped', reason)
        print('%s: %s not applied: %s' % (tag, mods[name].what, reason))

    try:
        start = find_blob(data, profile)
    except sigpatch.NotApplied as e:
        for name in names:
            skip(name, str(e))
        return bytes(out)
    blob = bytes(data[start:start + profile.length])
    taken = []

    def offset(addr):
        return start + program_offset(profile, blob, addr)

    def word(addr):
        return struct.unpack_from('<I', out, offset(addr))[0]

    def plan(mod):
        """(edits to write, pieces of code to write, whether it is all there
        already); NotApplied when the mod cannot go in."""
        table = isinstance(profile, DspTable)
        low, high = profile.cave[:2] if table else (profile.free[0] + EDGE, profile.free[1] - EDGE)
        writes, pieces, here = [], [], 0
        for addr, old, new in mod.edits:
            if word(addr) == new:
                here += 1
            elif word(addr) == old:
                writes.append((addr, new))
            else:
                raise sigpatch.NotApplied('the word at %#x is %#x, neither the stock %#x nor %#x'
                                          % (addr, word(addr), old, new))
        for addr, code in mod.code:
            if isinstance(code, str):
                with open(blob_path(code), 'rb') as f:
                    code = words_of(f.read())
            end = addr + 4 * len(code)
            if not (low <= addr and end <= high):
                raise sigpatch.NotApplied('code at %#x is not inside the %s %#x-%#x'
                                          % (addr, 'cave' if table else 'free run', low, high))
            now = [word(addr + 4 * k) for k in range(len(code))]
            if tuple(now) == tuple(code):
                here += 1
            elif cave_is_stock(profile, blob) if table else set(now) == {ERASED}:
                if program_references(profile, blob, addr, end):
                    raise sigpatch.NotApplied('the DSP program points into %#x-%#x' % (addr, end))
                pieces.append((addr, code))
            else:
                raise sigpatch.NotApplied('the space at %#x is taken' % addr)
        if here and (writes or pieces):
            raise sigpatch.NotApplied('it is half applied')
        return writes, pieces, here > 0

    for name in sorted(names):
        mod = mods[name]
        try:
            writes, pieces, present = plan(mod)
            ranges = [(a, a + 4) for a, _ in writes] + [(a, a + 4 * len(c)) for a, c in pieces]
            clash = [r for r in ranges for t in taken if r[0] < t[1] and t[0] < r[1]]
            if clash:
                raise sigpatch.NotApplied('it overlaps an earlier mod at %#x' % clash[0][0])
        except sigpatch.NotApplied as e:
            skip(name, str(e))
            continue
        if present:
            report[name] = ('present', '')
            print('%s: %s already applied' % (tag, mod.what))
            continue
        for addr, new in writes:
            struct.pack_into('<I', out, offset(addr), new)
            spans.setdefault(name, []).append((offset(addr), 4, 'dsp'))
        for addr, code in pieces:
            struct.pack_into('<%dI' % len(code), out, offset(addr), *code)
            kind = 'dspcave' if isinstance(profile, DspTable) else 'dspfree'
            spans.setdefault(name, []).append((offset(addr), 4 * len(code), kind))
        taken += ranges
        report[name] = ('applied', '')
        print('%s: %s, %d words changed and %d placed' % (tag, mod.what, len(writes),
                                                         sum(len(c) for _, c in pieces)))
    return bytes(out)
