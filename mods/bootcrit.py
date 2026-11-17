# SPDX-License-Identifier: GPL-2.0-or-later
"""The code of a MAIN image that a deck still needs when it boots, shows its
panel, talks to its display and runs its update mode.

    usage: bootcrit.py [--model <id>] <main_unpacked.bin> [base]

The image is the unpacked MAIN of the model --model names (the default model,
the CDJ-2000NXS2, without it), stock or modded; base is the address it starts
at, the MODEL_MAIN_BASE of that model's profile unless given.

The IPL checks nothing but the 16-bit sum of the packed MAIN, so a mod that
checksums fine and crashes leaves the deck's own update mode as the only way
out, and that mode is MAIN code. A patch must therefore never land in a
routine that boot, the panel link, the display link, the update task or the
USB stick task can reach.

The set is found by walking the image: from the tasks that carry those jobs
(looked up by name in the task table, each descriptor being entry, priority,
stack size, stack and a pointer to the task's name) and from the vector area,
through every direct call and branch, and through every code address a walked
routine loads from its literal pool.

Calls through a register that is not a pool load (function pointers kept in
RAM, jump tables) cannot be followed without running the firmware. The set is
therefore a lower bound; the count is reported next to it and the validator
lists it as not checked. Following every pool-loaded code address, not only
the ones a jsr uses, is the allowance made for it.
"""
import bisect
import re
import struct
import sys

import modelfw

# Tasks whose code the update path, the panel, the display link and the USB
# stick need. A model that has no task of a name simply lacks that root.
TASK_ROOTS = ('root', 'Idle', 'UpDtae_TASK', 'PnlCom_SndTASK', 'PnlCom_RcvTASK',
              'gui_comm', 'gui_task', 'GuiCom_SndTASK', 'GuiCom_RcvTASK',
              'RdMngRcv_TASK', 'RdMngExeUSB_TASK')

VECTORS = (0x100, 0x400, 0x600, 0x800)      # exception entries and _start, from the image base
VECTOR_AREA = 0x800
DESCRIPTOR_BYTES = 0x14
HULL_MAX = 0x4000                           # a routine this small is taken whole, gaps and jump tables included
MAX_INSNS = 50000                           # per routine; stops a walk that wandered into data

NAME = re.compile(rb'[A-Za-z_][A-Za-z0-9_]{2,31}\x00')


def sext(value, bits):
    return value - (1 << bits) if value & (1 << (bits - 1)) else value


class Closure:
    """.ranges: sorted [(lo, hi)] image offsets; .entries: {routine offset:
    the task or vector it was reached from}; .unresolved: indirect calls and
    jumps whose target is not a pool load; .roots: {task name: descriptor
    offset}."""

    def __init__(self, ranges, entries, unresolved, roots, fixed):
        self.ranges = ranges
        self.entries = entries
        self.unresolved = unresolved
        self.roots = roots
        self.fixed = fixed
        self._starts = [lo for lo, _ in ranges]

    def hit(self, lo, hi):
        """The (lo, hi) walked or fixed ranges that overlap [lo, hi)."""
        found = []
        i = max(bisect.bisect_right(self._starts, lo) - 1, 0)
        while i < len(self.ranges) and self.ranges[i][0] < hi:
            if self.ranges[i][1] > lo:
                found.append(self.ranges[i])
            i += 1
        return found

    def routine_at(self, off):
        """(entry offset, the task it was reached from) of the walked routine
        holding `off`, or None if only a fixed range does."""
        best = None
        for entry in self.entries:
            if entry <= off and (best is None or entry > best):
                best = entry
        return (best, self.entries[best]) if best is not None else None


def find_roots(img, base):
    """{task name: descriptor offset} for the TASK_ROOTS this image has."""
    roots = {}
    for name in TASK_ROOTS:
        string = name.encode() + b'\x00'
        pos = img.find(string)
        while pos >= 0:
            phys = base + pos
            for alias in (0xA0000000 | (phys & 0x1FFFFFFF), phys):
                ref = img.find(struct.pack('<I', alias))
                while ref >= 0:
                    desc = ref - 0x10
                    if desc >= 0 and ref % 4 == 0 and _plausible_descriptor(img, base, desc):
                        roots.setdefault(name, desc)
                    ref = img.find(struct.pack('<I', alias), ref + 1)
            pos = img.find(string, pos + 1)
    return roots


def _plausible_descriptor(img, base, desc):
    entry, prio, stack_size = struct.unpack_from('<III', img, desc)
    return (base <= entry < base + len(img) and entry % 2 == 0
            and 0 < prio <= 0x100 and stack_size % 4 == 0 and 0x80 <= stack_size <= 0x100000)


def find_all_roots(img, base):
    """{task name@descriptor: descriptor offset} for every entry of the task
    table: a descriptor whose last word points at a task name."""
    roots = {}
    for desc in range(0, len(img) - DESCRIPTOR_BYTES, 4):
        if not _plausible_descriptor(img, base, desc):
            continue
        pointer = struct.unpack_from('<I', img, desc + 0x10)[0]
        string = (pointer & 0x1FFFFFFF) - (base & 0x1FFFFFFF)
        named = NAME.match(img, string) if 0 <= string < len(img) else None
        if named:
            roots['%s@%#x' % (named.group(0)[:-1].decode(), base + desc)] = desc
    return roots


def closure(img, base, every_task=False):
    """The Closure of `img`, a MAIN image that starts at address `base` (its
    model's MODEL_MAIN_BASE).
    With every_task the walk starts from every task of the table, not only
    the ones boot, the update and the links need: the set is then all the
    code the firmware can reach, which is what a free span must stay out of."""
    size = len(img)
    covered = bytearray(size)
    entries = {}
    state = {'unresolved': 0}

    def code_target(value):
        """Image offset a pool word names if it is a code address, in either
        the cached or the uncached alias."""
        addr = (value & 0x1FFFFFFF) | (base & ~0x1FFFFFFF)
        off = addr - base
        return off if 0 <= off < size and off % 2 == 0 else None

    def entry_like(off):
        # A pool word is a code pointer only if it points at a routine
        # prologue; the rest are tables and strings, and walking them as code
        # would swallow the image.
        op = struct.unpack_from('<H', img, off)[0]
        if op == 0x4F22 or op & 0xFF0F == 0x2F06 or op >> 8 == 0x7F:
            return True
        # a leaf routine: the instruction pair before it is a return or jump
        # and its delay slot
        end = struct.unpack_from('<H', img, off - 4)[0] if off >= 4 else 0
        return end == 0x000B or end & 0xF0FF == 0x402B or end >> 12 == 0xA

    def mark(off, n):
        covered[off:off + n] = b'\x01' * n
        span[0] = min(span[0], off)
        span[1] = max(span[1], off + n)

    def walk(entry, label):
        current[0] = label
        span[:] = [size, 0]
        todo = [entry]
        seen = set()
        loaded = {}            # register -> instructions since it was loaded from the pool
        while todo and len(seen) < MAX_INSNS:
            a = todo.pop()
            loaded.clear()
            while 0 <= a < size - 1 and a not in seen and len(seen) < MAX_INSNS:
                seen.add(a)
                mark(a, 2)
                op = struct.unpack_from('<H', img, a)[0]
                for reg in loaded:
                    loaded[reg] += 1
                pool(a, op, loaded)
                hi, lo8 = op >> 12, op & 0xFF
                if hi == 0xA or hi == 0xB:                      # bra, bsr
                    slot(a + 2, loaded)
                    target = a + 4 + sext(op & 0xFFF, 12) * 2
                    (todo if hi == 0xA else calls).append(target if hi == 0xA else (target, label))
                    if hi == 0xA:
                        break
                elif hi == 0x8 and (op >> 8) & 0xF in (0x9, 0xB, 0xD, 0xF):   # bt, bf, bt/s, bf/s
                    if (op >> 8) & 0xF in (0xD, 0xF):
                        slot(a + 2, loaded)
                    todo.append(a + 4 + sext(lo8, 8) * 2)
                elif op in (0x000B, 0x002B):                    # rts, rte
                    slot(a + 2, loaded)
                    break
                elif hi == 0x4 and lo8 in (0x0B, 0x2B):         # jsr @rn, jmp @rn
                    reg = (op >> 8) & 0xF
                    slot(a + 2, loaded)
                    if loaded.get(reg, 99) > 8:
                        state['unresolved'] += 1
                    if lo8 == 0x2B:
                        break
                elif hi == 0x0 and lo8 in (0x03, 0x23):         # bsrf, braf
                    slot(a + 2, loaded)
                    state['unresolved'] += 1
                    if lo8 == 0x23:
                        break
                a += 2
        if span[1] - span[0] <= HULL_MAX:
            mark(span[0], span[1] - span[0])

    def pool(a, op, loaded):
        hi = op >> 12
        if hi == 0xD:                                           # mov.l @(disp,pc),rn
            lit = ((a + 4) & ~3) + (op & 0xFF) * 4
            if lit + 4 <= size:
                mark(lit, 4)
                loaded[(op >> 8) & 0xF] = 0
                target = code_target(struct.unpack_from('<I', img, lit)[0])
                if target is not None and entry_like(target):
                    calls.append((target, current[0]))
        elif hi == 0x9:                                         # mov.w @(disp,pc),rn
            lit = a + 4 + (op & 0xFF) * 2
            if lit + 2 <= size:
                mark(lit, 2)

    def slot(a, loaded):
        if a + 2 <= size:
            mark(a, 2)
            pool(a, struct.unpack_from('<H', img, a)[0], loaded)

    calls, current, span = [], [''], [0, 0]
    roots = find_all_roots(img, base) if every_task else find_roots(img, base)
    seeds = [(base_off, 'vector %#x' % (base + base_off)) for base_off in VECTORS]
    for name, desc in roots.items():
        seeds.append((struct.unpack_from('<I', img, desc)[0] - base, name))

    fixed = [(0, min(VECTOR_AREA, size))]
    for name, desc in roots.items():
        fixed.append((desc, desc + DESCRIPTOR_BYTES))
        pointer = struct.unpack_from('<I', img, desc + 0x10)[0]
        string = (pointer & 0x1FFFFFFF) - (base & 0x1FFFFFFF)
        if 0 <= string < size:
            fixed.append((string, string + len(name.split('@')[0]) + 1))

    calls += seeds
    done = set()
    while calls:
        entry, label = calls.pop()
        if entry in done or not 0 <= entry < size:
            continue
        done.add(entry)
        entries[entry] = label
        walk(entry, label)

    ranges = [(m.start(), m.end()) for m in re.finditer(rb'\x01+', bytes(covered))]
    ranges = merge(ranges + fixed)
    return Closure(ranges, entries, state['unresolved'], roots, merge(fixed))


def merge(ranges):
    out = []
    for lo, hi in sorted(ranges):
        if out and lo <= out[-1][1]:
            out[-1] = (out[-1][0], max(out[-1][1], hi))
        else:
            out.append((lo, hi))
    return out


def main():
    model, args = modelfw.model_id(sys.argv[1:])
    if len(args) not in (1, 2):
        raise SystemExit(__doc__)
    with open(args[0], 'rb') as f:
        img = f.read()
    base = int(args[1], 0) if len(args) == 2 else modelfw.main_base(model)
    c = closure(img, base)
    print('%s MAIN %s at %#x' % (model, args[0], base))
    print('roots: %s' % ', '.join('%s %#x' % (n, base + d) for n, d in sorted(c.roots.items())))
    print('%d routines, %d bytes, %d ranges, %d indirect calls not followed'
          % (len(c.entries), sum(h - l for l, h in c.ranges), len(c.ranges), c.unresolved))


if __name__ == '__main__':
    main()
