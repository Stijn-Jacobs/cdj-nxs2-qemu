#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""c14 -- compile hot C66x code into native regions (a block compiler).

Input: a profile directory written by the core under C66X_JIT_PROFILE (the
packet counts, the idle head and the mapped RAM). Output: one C file of
regions and the shared object gcc builds from it, loaded by the core with
C66X_JIT=<so>.

A region reproduces the interpreter's fast loop (c66x_core.c, fast_cycles,
fop_run and exec_insn) cycle for cycle, with the pipeline resolved at compile
time: every write's landing cycle, the branches in flight and the NOP cycles
are part of a static state, so a write lands as an assignment instead of going
through the ring. What is dynamic -- a conditional instruction, a conditional
branch -- is a flag the code tests; a conditional branch landing forks the
state. States are explored from the root packet and merged when they repeat, so
loops close. Instructions without a native form run the interpreter's own
handler with their writes captured (the shape of those writes is asked of the
core at generation time and checked at run time).

A region exits where the static state cannot follow: a branch to a value not
known at compile time, a packet it cannot compile, the end of the budget, an
interrupt, code being dropped, a possible stall. Exits before a cycle's commit
put every write and branch in flight back where the interpreter keeps them;
exits after it (an interrupt a control-register write made deliverable, a
stall) also hand over the commit's stall mask and set jit_committed.

Exactness is checked by the rBw2 replay (qemu/c13_bench.sh with C66X_JIT).

usage: c14_jitgen.py <profile dir> <out prefix> [--roots N] [--min N] [--nodes N]
"""
import argparse
import functools
import ctypes
import mmap
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CORE_DIR = os.path.dirname(HERE)

OPK_CONST, OPK_REG, OPK_PAIR, OPK_MEM, OPK_CTRL, OPK_ADDR = 1, 2, 3, 4, 5, 6
WK_REG, WK_CTRL = 0, 1
MAX_QUEUE = 7          # NBR - 1: never let a region reach the interpreter's overflow trap
PRE_CYCLES = 17        # WQ_SLOTS + 1: writes scheduled before entry land within this many cycles
MAX_KNOWN = 4          # registers with a compile-time value carried in the state
KNOWN_AGE = 3          # ... for this many commits after the write that made it known
CHAIN_DEPTH = 24       # a region hands over to another root only this many cycles in
CAP_MAX = 16           # C66X_JIT_CAP

# c->jit_exit[] slots, printed by the core
(EXIT_STUB, EXIT_DYNPC, EXIT_UNCOMPILABLE, EXIT_QUEUE, EXIT_NODES, EXIT_BUDGET, EXIT_GEN, EXIT_IRQ,
 EXIT_POSTIRQ, EXIT_STALL, EXIT_CHAIN) = range(11)

INT_OPS = {"mvk", "mvkh", "addk", "mv", "addkpc", "add", "sub", "and", "or", "xor",
           "cmpeq", "cmpgt", "cmplt", "cmpgtu", "cmpltu", "shl", "shr", "shru", "ext", "extu"}
FLOAT_OPS = {"addsp": "+", "subsp": "-", "mpysp": "*"}
# handlers that change control flow or the loop buffer: never captured
NO_CAPTURE = {"b", "bnop", "callp", "bdec", "bpos", "idle", "swe", "swenr", "sploop", "sploopd",
              "sploopw", "spkernel", "spkernelr", "spmask", "spmaskr"}


def u32(v):
    return "0x%08xu" % (v & 0xffffffff)


# --------------------------------------------------------------------------
# packets, from the core's own decoder

class Rec:
    pass


def parse_fields(tokens):
    d = {}
    for i in range(0, len(tokens) - 1, 2):
        v = tokens[i + 1]
        try:
            d[tokens[i]] = int(v, 0)
        except ValueError:
            d[tokens[i]] = v
    return d


class Decoder:
    def __init__(self, prof_dir, lib):
        self.lib = ctypes.CDLL(lib)
        self.lib.c66x_new.restype = ctypes.c_void_p
        self.lib.c66x_new.argtypes = [ctypes.c_void_p]
        self.lib.c66x_map_ram.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_void_p]
        for fn in (self.lib.c66x_jit_describe, self.lib.c66x_jit_shape, self.lib.c66x_jit_describe_insn):
            fn.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_size_t]
            fn.restype = ctypes.c_int
        os.environ.pop("C66X_JIT", None)
        os.environ.pop("C66X_JIT_PROFILE", None)
        self.core = self.lib.c66x_new(None)
        self.maps, self.prof, self.idle_head, self.kerns = [], [], 0, []
        self.have = set()          # roots an already loaded module compiles
        # several profile directories (comma-separated) add up their counts; the
        # RAM and maps are the first one's
        dirs = prof_dir.split(",")
        prof, kerns = {}, {}
        for n, d in enumerate(dirs):
            for line in open(os.path.join(d, "profile.txt")):
                t = line.split()
                if t[0] == "idle_head" and n == 0:
                    self.idle_head = int(t[1], 0)
                elif t[0] == "map" and n == 0:
                    self.maps.append((int(t[1], 0), int(t[2], 0), int(t[3])))
                elif t[0] == "kern":
                    # kern addr kind ii dynlen creg cz cycles | body0 addrs | body1 addrs ...
                    k = Rec()
                    k.addr, k.kind, k.ii, k.dynlen = int(t[1], 0), int(t[2]), int(t[3]), int(t[4])
                    k.creg, k.cz, k.cycles = int(t[5]), int(t[6]), int(t[7])
                    k.body = [[int(x, 0) for x in part.split()] for part in " ".join(t[8:]).split("|")[1:]]
                    sig = (k.addr, k.kind, k.ii, k.dynlen, k.creg, k.cz, tuple(map(tuple, k.body)))
                    if sig in kerns:
                        kerns[sig].cycles += k.cycles
                    else:
                        kerns[sig] = k
                elif t[0] == "have":
                    self.have.add(int(t[1], 0))
                elif t[0] == "prof":
                    pc, a, b = int(t[1], 0), int(t[2]), int(t[3])
                    old = prof.get(pc, (0, 0))
                    prof[pc] = (old[0] + a, old[1] + b)
        self.prof = [(pc, a, b) for pc, (a, b) in prof.items()]
        self.kerns = list(kerns.values())
        prof_dir = dirs[0]
        self.bufs, self.mms = {}, []
        for base, size, hid in self.maps:
            if hid not in self.bufs:
                # mapped copy-on-write: a runtime batch writes only its code pages
                # into a sparse file of the full size
                f = open(os.path.join(prof_dir, "ram_%u.bin" % hid), "rb")
                mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_COPY)
                self.mms.append(mm)
                self.bufs[hid] = (ctypes.c_uint8 * size).from_buffer(mm)
            self.lib.c66x_map_ram(self.core, base, size, ctypes.addressof(self.bufs[hid]))
        self.pcache, self.scache = {}, {}
        self.out = ctypes.create_string_buffer(1 << 16)

    def bytes32(self, addr):
        for base, size, hid in self.maps:
            if base <= addr and addr + 32 <= base + size:
                return bytes(self.bufs[hid][addr - base:addr - base + 32])
        return None

    def packet(self, pc):
        if pc in self.pcache:
            return self.pcache[pc]
        pk = None
        if self.lib.c66x_jit_describe(self.core, pc, self.out, len(self.out)) > 0:
            pk = Rec()
            pk.insns = []
            for line in self.out.value.decode().splitlines():
                t = line.split()
                if t[0] == "pk":
                    pk.pc = int(t[1], 0)
                    pk.__dict__.update(parse_fields(t[2:]))
                elif t[0] == "in":
                    ins = Rec()
                    ins.addr = int(t[1], 0)
                    ins.__dict__.update(parse_fields(t[2:]))
                    ins.ops = []
                    pk.insns.append(ins)
                elif t[0] == "op":
                    op = Rec()
                    op.__dict__.update(parse_fields(t[1:]))
                    pk.insns[-1].ops.append(op)
        self.pcache[pc] = pk
        return pk

    def insn(self, addr):
        if ("i", addr) not in self.pcache:
            ins = None
            if self.lib.c66x_jit_describe_insn(self.core, addr, self.out, len(self.out)) > 0:
                for line in self.out.value.decode().splitlines():
                    t = line.split()
                    if t[0] == "in":
                        ins = Rec()
                        ins.addr = int(t[1], 0)
                        ins.__dict__.update(parse_fields(t[2:]))
                        ins.ops = []
                    elif t[0] == "op":
                        op = Rec()
                        op.__dict__.update(parse_fields(t[1:]))
                        ins.ops.append(op)
            self.pcache[("i", addr)] = ins
        return self.pcache[("i", addr)]

    def shape(self, addr):
        """(stop, [(delay, kind, idx)]) of the instruction's scheduled writes."""
        if addr not in self.scache:
            r = None
            if self.lib.c66x_jit_shape(self.core, addr, self.out, len(self.out)) > 0:
                lines = self.out.value.decode().splitlines()
                h = lines[0].split()
                ws = [tuple(int(x) for x in l.split()[1:]) for l in lines[1:]]
                r = (int(h[3]), int(h[1]), ws)
            self.scache[addr] = r
        return self.scache[addr]


# --------------------------------------------------------------------------
# what a region compiles

@functools.lru_cache(maxsize=None)
def handler_number(name):
    """A handler's number in c66x_core.c's enum hid (the profile records kinds by number)."""
    src = open(os.path.join(CORE_DIR, "c66x_core.c")).read()
    body = src[src.index("enum hid {") + len("enum hid {"):]
    body = body[:body.index("};")]
    names = []
    for line in body.splitlines():
        line = line.split("/*")[0]
        names += [x.strip().split("=")[0].strip() for x in line.split(",") if x.strip()]
    return names.index(name)


def native(ins):
    f = ins.fop
    if f == "nop" or f in INT_OPS or f in FLOAT_OPS:
        return True
    if f in ("load", "store"):
        return ins.mfast != 0
    if f == "branch":
        return ins.ops[0].kind in (OPK_REG, OPK_ADDR)
    if f == "callp":
        return ins.ops[1].kind == OPK_REG
    return False


def compilable(dec, pk):
    """Why a packet cannot be compiled, or None."""
    if pk is None:
        return "undecodable"
    if not pk.cacheable:
        return "not cacheable"
    if pk.special:
        return "sploop family"
    if dec.idle_head and pk.pc == dec.idle_head:
        return "idle head"
    for ins in pk.insns:
        if native(ins):
            continue
        if ins.name in NO_CAPTURE:
            return "handler " + ins.name
        sh = dec.shape(ins.addr)
        if sh is None or sh[0] != 0:
            return "handler %s traps" % ins.name
        if sh[1] > CAP_MAX:
            return "handler %s writes too much" % ins.name
    return None


# --------------------------------------------------------------------------
# static pipeline state
#
# Taken at the top of a cycle, before its commit. Values live in positional
# variables so equal shapes share code (a compile-time constant value is a
# literal and needs none):
#   ring  : writes landing `land` cycles from now (0 = this commit), in commit
#           order;  r<i> value, rf<i> flag
#   imm   : last cycle's delay-0 writes, committed after the ring;  m<j>, mf<j>
#   queue : branches in flight, oldest first;  bt<k> dynamic target, bf<k> flag
#   known : registers whose committed value is a compile-time constant
# A flag exists only when the write or branch was conditional.

class State:
    __slots__ = ("pc", "mcnop", "queue", "ring", "imm", "pre", "known", "pc_expr")

    def __init__(self, pc, mcnop, queue, ring, imm, pre, known):
        self.pc, self.mcnop, self.queue, self.ring, self.imm, self.pre = pc, mcnop, queue, ring, imm, pre
        self.known = known
        self.pc_expr = None

    def key(self):
        return (self.pc, self.mcnop,
                tuple((q["rem"], q["tconst"], q["flag"] is not None) for q in self.queue),
                tuple((w["land"], w["kind"], w["idx"], w["flag"] is not None, w["const"]) for w in self.ring),
                tuple((w["kind"], w["idx"], w["flag"] is not None, w["const"]) for w in self.imm),
                self.pre, tuple(sorted(self.known.items())))


def guarded(flag, stmt):
    return ("if (%s) " % flag if flag else "") + stmt


class RegionGen:
    def __init__(self, dec, root, max_nodes, uid, sites, roots=frozenset()):
        self.dec = dec
        self.root = root
        self.max_nodes = max_nodes
        self.name = "region_%08x_%d" % (root, uid)
        self.sites = sites        # shared: (pc, reason) per counted exit site
        self.cur_pc = root
        self.depth = {0: 0}       # label -> cycles from the root when first reached
        self.cur_depth = 0
        self.roots = roots        # every root in the module: a clean state there chains to it
        self.labels = {}
        self.work = []
        self.body = []
        self.deps = set()
        self.tmp = 0
        self.maxv = {"r": 0, "m": 0, "b": 0}
        self.captured = []        # instruction addresses a region runs through exec_capture
        self.cold = []            # exit stubs, emitted after every state's code
        self.kernel = False
        self.ins_name = "ins"     # the array of captured instruction pointers

    def t(self):
        self.tmp += 1
        return "t%d" % self.tmp

    def emit(self, s):
        self.body.append(s)

    def count(self, ind, why, site=None):
        self.emit(ind + "c->jit_exit[%d]++;" % why)
        if site is not None:
            self.sites.append(site)
            self.emit(ind + "jit_site_n[%d]++;" % (len(self.sites) - 1))

    # --- exits
    def put_back(self, st, pc_expr, ind, ring_after_commit=False):
        e = self.emit
        if not self.kernel:
            e(ind + "c->pc = %s; c->mcnop = %d; c->nbr = 0;" % (pc_expr, st.mcnop))
        for q in st.queue:
            tgt = u32(q["tconst"]) if q["tconst"] is not None else q["tvar"]
            e(ind + guarded(q["flag"], "c->br[c->nbr++] = (pend_branch){ %d, %s };" % (q["rem"], tgt)))
        for w in st.ring:
            if ring_after_commit and w["land"] == 0:
                continue
            e(ind + guarded(w["flag"], "{ unsigned s = wq_slot(c, %d); c->wq[s][c->wqn[s]++] = (wq_ent){ %d, %d, 0, %s }; }"
                            % (w["land"] - 1, w["kind"], w["idx"], w["val"])))
        e(ind + "c->imm_n = 0;")
        if not ring_after_commit:
            for w in st.imm:
                e(ind + guarded(w["flag"], "c->imm[c->imm_n++] = (wq_ent){ %d, %d, 0, %s };"
                                % (w["kind"], w["idx"], w["val"])))

    def materialize(self, st, pc_expr, ind, why, site=None):
        self.count(ind, why, site)
        self.put_back(st, pc_expr, ind)
        self.emit(ind + "return;")

    # --- a state reached: jump to its code, or exit where it cannot run compiled
    def transition(self, st, ind="    "):
        if st.pc is None:
            self.materialize(st, st.pc_expr, ind, EXIT_DYNPC, (self.cur_pc, "dynamic pc (from here)"))
            return
        if st.mcnop == 0:
            why = compilable(self.dec, self.dec.packet(st.pc))
            if why:
                self.materialize(st, u32(st.pc), ind, EXIT_UNCOMPILABLE, (st.pc, why))
                return
        if (st.pc != self.root and st.pc in self.roots and not st.queue and st.mcnop == 0
                and not st.known and self.cur_depth + 1 >= CHAIN_DEPTH):
            # another region starts here and nothing in flight stops it from being
            # entered: hand over instead of compiling that code twice
            self.materialize(st, u32(st.pc), ind, EXIT_CHAIN)
            return
        if len(st.queue) > MAX_QUEUE:
            self.materialize(st, u32(st.pc), ind, EXIT_QUEUE)
            return
        key = st.key()
        if key not in self.labels:
            if len(self.labels) >= self.max_nodes:
                self.materialize(st, u32(st.pc), ind, EXIT_NODES)
                return
            self.labels[key] = len(self.labels)
            pos = self.positional(st)
            self.maxv["r"] = max(self.maxv["r"], len(pos.ring))
            self.maxv["m"] = max(self.maxv["m"], len(pos.imm))
            self.maxv["b"] = max(self.maxv["b"], len(pos.queue))
            self.work.append((self.labels[key], pos))
            self.depth[self.labels[key]] = self.cur_depth + 1
        assigns = []
        for i, w in enumerate(st.ring):
            if w["const"] is None:
                assigns.append(("r%d" % i, w["val"], "uint32_t"))
            if w["flag"]:
                assigns.append(("rf%d" % i, w["flag"], "uint8_t"))
        for j, w in enumerate(st.imm):
            if w["const"] is None:
                assigns.append(("m%d" % j, w["val"], "uint32_t"))
            if w["flag"]:
                assigns.append(("mf%d" % j, w["flag"], "uint8_t"))
        for k, q in enumerate(st.queue):
            if q["tconst"] is None:
                assigns.append(("bt%d" % k, q["tvar"], "uint32_t"))
            if q["flag"]:
                assigns.append(("bf%d" % k, q["flag"], "uint8_t"))
        assigns = [a for a in assigns if a[0] != a[1]]
        if assigns:
            self.emit(ind + "{ " + " ".join("%s _%s = %s;" % (ty, d, s) for d, s, ty in assigns)
                      + " " + " ".join("%s = _%s;" % (d, d) for d, s, ty in assigns) + " }")
        self.emit(ind + self.goto(self.labels[key]))

    def goto(self, label):
        return "goto L%d;" % label

    @staticmethod
    def positional(st):
        ring = [dict(w, val=(u32(w["const"]) if w["const"] is not None else "r%d" % i),
                     flag=("rf%d" % i if w["flag"] else None)) for i, w in enumerate(st.ring)]
        imm = [dict(w, val=(u32(w["const"]) if w["const"] is not None else "m%d" % j),
                    flag=("mf%d" % j if w["flag"] else None)) for j, w in enumerate(st.imm)]
        queue = [dict(q, tvar=(None if q["tconst"] is not None else "bt%d" % k),
                      flag=("bf%d" % k if q["flag"] else None)) for k, q in enumerate(st.queue)]
        return State(st.pc, st.mcnop, queue, ring, imm, st.pre, dict(st.known))

    # --- one cycle
    def cycle(self, label, st, root=False):
        e = self.emit
        self.cur_pc = st.pc
        self.cur_depth = self.depth.get(label, 0)
        pk = self.dec.packet(st.pc) if st.mcnop == 0 else None
        e("L%d: ; /* pc 0x%08x mcnop %d pre %d q%d r%d m%d known %d */" % (
            label, st.pc, st.mcnop, st.pre, len(st.queue), len(st.ring), len(st.imm), len(st.known)))
        known = dict(st.known)
        ring = [dict(w) for w in st.ring]
        post_used = False
        if not root:
            e("    if (__builtin_expect(c->cycle >= end, 0)) { c->jit_exit[%d]++; goto X%d; }" % (EXIT_BUDGET, label))
            e("    if (__builtin_expect(c->code_gen != gen, 0)) { c->jit_exit[%d]++; goto X%d; }" % (EXIT_GEN, label))
            e("    if (__builtin_expect(c->ifr && pending_interrupt(c), 0)) { c->jit_exit[%d]++; goto X%d; }"
              % (EXIT_IRQ, label))
            # the commit: writes from before entry, this cycle's ring slot, last cycle's delay-0 writes
            ctrl = []
            if st.pre:
                e("    uint8_t pc%d = c66x_jit_pre_commit(c);" % label)
                ctrl.append("pc%d" % label)
                known = {}
            for w in [w for w in st.ring if w["land"] == 0] + st.imm:
                if w["kind"] == WK_REG:
                    e("    " + guarded(w["flag"], "c->reg[%d] = %s;" % (w["idx"], w["val"])))
                    known.pop(w["idx"], None)
                    if w["const"] is not None and not w["flag"]:
                        known[w["idx"]] = (w["const"], 0)
                else:
                    e("    " + guarded(w["flag"], "c->api->ctrl_write(c, %d, %s);" % (w["idx"], w["val"])))
                    ctrl.append(w["flag"] or "1")
            if ctrl:
                e("    if (__builtin_expect((%s) && c->ifr && pending_interrupt(c), 0)) { c->jit_exit[%d]++; goto P%d; }"
                  % (" || ".join(ctrl), EXIT_POSTIRQ, label))
                post_used = True
            ring = [dict(w) for w in st.ring if w["land"] > 0]
            if pk is not None:
                hits = [w for w in st.imm if w["kind"] == WK_REG and (pk.xmask >> w["idx"]) & 1]
                if any(not w["flag"] for w in hits):
                    e("    c->st.stalls++; c->wq_base--; c->cycle++;")
                    self.transition(State(st.pc, st.mcnop, [dict(q) for q in st.queue], ring, [], st.pre,
                                          self.trim(known)))
                    self.stubs(label, st, post_used)
                    return
                if hits:
                    e("    if (__builtin_expect(%s, 0)) { c->jit_exit[%d]++; goto P%d; }"
                      % (" || ".join(w["flag"] for w in hits), EXIT_STALL, label))
                    post_used = True
        pre = max(st.pre - 1, 0)
        new_ring, new_imm = [], []
        queue = [dict(q) for q in st.queue]
        if st.mcnop > 0:
            mcnop, next_pc = st.mcnop - 1, st.pc
        else:
            h_store = handler_number("H_STORE")
            stores = any(ins.fop == "store" or ins.handler == h_store for ins in pk.insns)
            e("    c->exec_pc = %s;" % u32(pk.pc))
            if stores and not pk.load:
                e("    c->store_now = 1;")
            for ins in pk.insns:
                self.deps.add(ins.addr & ~31)
                self.insn(ins, pk, new_ring, new_imm, queue, known)
            if stores and not pk.load:
                e("    c->store_now = 0;")
            elif stores:
                e("    if (c->npst) c->api->flush_stores(c);")
            e("    c->branch_block = %s;" % ("5" if pk.branched else "c->branch_block ? c->branch_block - 1 : 0"))
            mcnop, next_pc = pk.xnops, pk.next
        e("    c->cycle++;")
        for w in ring:
            w["land"] -= 1
        ring = ring + new_ring
        for q in queue:
            q["rem"] -= 1
        self.land(queue, ring, new_imm, next_pc, mcnop, pre, self.trim(known), "    ")
        self.stubs(label, st, post_used)

    @staticmethod
    def trim(known):
        """Age the known constants by one commit; known maps reg -> (value, age)."""
        aged = {r: (v, a + 1) for r, (v, a) in known.items() if a + 1 < KNOWN_AGE}
        while len(aged) > MAX_KNOWN:
            aged.pop(next(iter(aged)))
        return aged

    def stubs(self, label, st, post_used):
        hot, self.body = self.body, self.cold
        self._stubs(label, st, post_used)
        self.body = hot

    def _stubs(self, label, st, post_used):
        if not self.root_label(label):
            self.emit("X%d: ;" % label)
            self.put_back(st, u32(st.pc), "    ")
            self.emit("    return;")
        if post_used:
            # after the commit: the ring slot and the delay-0 writes have landed
            self.emit("P%d: ;" % label)
            bits = [("(%s ? %s : 0)" % (w["flag"], "0x%xull" % (1 << w["idx"])) if w["flag"] else "0x%xull" % (1 << w["idx"]))
                    for w in st.imm if w["kind"] == WK_REG]
            self.emit("    c->wmask = %s; c->jit_committed = 1;" % (" | ".join(bits) if bits else "0"))
            self.put_back(st, u32(st.pc), "    ", ring_after_commit=True)
            self.emit("    return;")

    def root_label(self, label):
        return label == 0

    def land(self, queue, ring, imm, next_pc, mcnop, pre, known, ind):
        """land_branches: the oldest branch present lands when its slots ran out."""
        if not queue or (queue[0]["rem"] > 0 and queue[0]["flag"] is None):
            self.transition(State(next_pc, mcnop, queue, ring, imm, pre, dict(known)), ind)
            return
        q0, rest = queue[0], queue[1:]

        def present(ind2):
            if q0["rem"] <= 0:
                st = State(q0["tconst"], 0, rest, ring, imm, pre, dict(known))
                if q0["tconst"] is None:
                    st.pc_expr = q0["tvar"]
                self.transition(st, ind2)
            else:
                self.transition(State(next_pc, mcnop, [dict(q0, flag=None)] + rest, ring, imm, pre, dict(known)), ind2)

        if q0["flag"] is None:
            present(ind)
            return
        self.emit(ind + "if (%s) {" % q0["flag"])
        present(ind + "    ")
        self.emit(ind + "} else {")
        self.land(rest, ring, imm, next_pc, mcnop, pre, known, ind + "    ")
        self.emit(ind + "}")

    # --- instructions
    def insn(self, ins, pk, ring, imm, queue, known):
        e = self.emit
        f = ins.fop
        if f == "nop":
            return
        ops = ins.ops
        flag = None
        if ins.cond_reg >= 0:
            flag = self.t()
            e("    uint8_t %s = (c->reg[%d] == 0) == %d;" % (flag, ins.cond_reg, ins.cond_z))

        def reg(i):
            return "c->reg[%d]" % ops[i].reg

        def val(i):
            return reg(i) if ops[i].kind == OPK_REG else u32(ops[i].val)

        def sval(i):
            return "(int32_t)" + reg(i) if ops[i].kind == OPK_REG else "(int32_t)%d" % ops[i].val

        def push(delay, kind, idx, v, const=None):
            w = {"kind": kind, "idx": idx, "val": u32(const) if const is not None else v, "flag": flag,
                 "const": const}
            if delay == 0:
                imm.append(w)
            else:
                w["land"] = delay
                ring.append(w)

        def kreg(i):
            return known[ops[i].reg][0] if ops[i].kind == OPK_REG and ops[i].reg in known else None

        if not native(ins):
            self.capture(ins, pk, flag, push)
            return

        if f in INT_OPS:
            a, b = val(0), (val(1) if len(ops) > 1 else "0")
            sa, sb = sval(0), (sval(1) if len(ops) > 1 else "0")
            dst, const = 2, None
            if f == "mvk":
                x, dst, const = u32(ops[0].val), 1, ops[0].val & 0xffffffff
            elif f == "mvkh":
                x, dst = "((%s & 0xffffu) | (%s & 0xffff0000u))" % (reg(1), u32(ops[0].val)), 1
                if kreg(1) is not None:
                    const = (kreg(1) & 0xffff) | (ops[0].val & 0xffff0000)
            elif f == "addk":
                x, dst = "(%s + %s)" % (reg(1), u32(ops[0].val)), 1
                if kreg(1) is not None:
                    const = (kreg(1) + ops[0].val) & 0xffffffff
            elif f == "mv":
                x, dst, const = a, 1, kreg(0)
            elif f == "addkpc":
                x, dst, const = u32(ops[0].val), 1, ops[0].val & 0xffffffff
            elif f == "add":
                x = "(%s + %s)" % (a, b)
            elif f == "sub":
                x = "(%s - %s)" % (a, b)
            elif f == "and":
                x = "(%s & %s)" % (a, b)
            elif f == "or":
                x = "(%s | %s)" % (a, b)
            elif f == "xor":
                x = "(%s ^ %s)" % (a, b)
            elif f == "cmpeq":
                x = "(uint32_t)(%s == %s)" % (sa, sb)
            elif f == "cmpgt":
                x = "(uint32_t)(%s > %s)" % (sa, sb)
            elif f == "cmplt":
                x = "(uint32_t)(%s < %s)" % (sa, sb)
            elif f == "cmpgtu":
                x = "(uint32_t)(%s > %s)" % (a, b)
            elif f == "cmpltu":
                x = "(uint32_t)(%s < %s)" % (a, b)
            elif f == "shl":
                x = "(((%s) & 0x3f) > 31 ? 0u : (%s) << ((%s) & 0x3f))" % (b, a, b)
            elif f == "shr":
                x = "(uint32_t)(%s >> (((%s) & 0x3f) > 31 ? 31 : ((%s) & 0x3f)))" % (sa, b, b)
            elif f == "shru":
                x = "(((%s) & 0x3f) > 31 ? 0u : (%s) >> ((%s) & 0x3f))" % (b, a, b)
            elif f == "ext":
                x, dst = "(uint32_t)((int32_t)(%s << %d) >> %d)" % (a, ops[1].val & 31, ops[2].val & 31), 3
            elif f == "extu":
                x, dst = "((%s << %d) >> %d)" % (a, ops[1].val & 31, ops[2].val & 31), 3
            v = None
            if const is None:
                v = self.t()
                e("    uint32_t %s = %s;" % (v, x))
            push(0, WK_REG, ops[dst].reg, v, const)
            return

        if f in FLOAT_OPS:
            v = self.t()
            e("    uint32_t %s = jit_fop(c, %d, %d, %s, %s, '%s');" % (
                v, ins.unit, ins.side, reg(0), reg(1), FLOAT_OPS[f]))
            push(ops[2].low_first - 1, WK_REG, ops[2].reg, v)
            return

        if f == "branch":
            k = kreg(0) if ops[0].kind == OPK_REG else ops[0].val & 0xffffffff
            if k is None:
                tv = self.t()
                e("    uint32_t %s = %s;" % (tv, reg(0)))
                queue.append({"rem": 6, "tconst": None, "tvar": tv, "flag": flag})
            else:
                queue.append({"rem": 6, "tconst": k, "tvar": None, "flag": flag})
            return

        if f == "callp":
            queue.append({"rem": 6, "tconst": ops[0].val & 0xffffffff, "tvar": None, "flag": flag})
            push(ops[1].low_first - 1, WK_REG, ops[1].reg, None, pk.next)
            return

        mf = ins.mfast
        n, sx, wb, dw = mf & 7, mf & 8, (mf >> 4) & 3, mf & 0x40
        if f == "load":
            b, d = ops[0].reg, ops[1]
            v = self.t()
            e("    uint32_t %s = 0;" % v)
            body = ["uint32_t base = c->reg[%d], ea = base + %s;" % (b, u32(ins.ea_delta))]
            if wb:
                wv = self.t()
                e("    uint32_t %s = c->reg[%d] + %s;" % (wv, b, u32(ins.ea_delta)))
                push(0, WK_REG, b, wv)
                if wb == 2:
                    body.append("ea = base;")
            body.append("%s = jit_load(c, ea, %d);" % (v, n))
            if sx:
                body.append("%s = (uint32_t)(%s)%s;" % (v, "int8_t" if n == 1 else "int16_t", v))
            e("    " + guarded(flag, "{ " + " ".join(body) + " }"))
            push(d.low_first - 1, WK_REG, d.reg, v)
            return

        if f == "store":
            b, s = ops[1].reg, ops[0]
            body = ["uint32_t base = c->reg[%d], ea = base + %s, v = c->reg[%d];" % (b, u32(ins.ea_delta), s.reg)]
            if wb:
                wv = self.t()
                e("    uint32_t %s = c->reg[%d] + %s;" % (wv, b, u32(ins.ea_delta)))
                push(0, WK_REG, b, wv)
                if wb == 2:
                    body.append("ea = base;")
            if dw:
                body.append("uint32_t hi = c->reg[%d]; jit_store(c, ea, v, 4); jit_store(c, ea + 4, hi, 4);" % s.reg_hi)
            else:
                body.append("jit_store(c, ea, v, %d);" % n)
            e("    " + guarded(flag, "{ " + " ".join(body) + " }"))
            return
        raise AssertionError(f)

    # --- exec_insn handlers compiled natively; the writes follow the shape the
    # core reported, so their timing is the interpreter's by construction
    def handler_native(self, ins, pk, flag, push):
        e = self.emit
        ops = ins.ops
        H = handler_number
        hd = ins.handler
        last = ins.nops - 1
        rmask = regmask = constmask = 0
        for i, o in enumerate(ops):
            if o.kind in (OPK_CONST, OPK_ADDR) or (o.rw in (1, 3) and o.kind != OPK_MEM):
                rmask |= 1 << i
                if o.kind == OPK_REG:
                    regmask |= 1 << i
                elif o.kind == OPK_CONST:
                    constmask |= 1 << i

        def v(i):
            o = ops[i]
            if not (rmask >> i) & 1:
                return None
            if (regmask >> i) & 1:
                return "(uint64_t)c->reg[%d]" % o.reg
            if (constmask >> i) & 1:
                return "(uint64_t)(int64_t)%d" % o.val
            if o.kind == OPK_PAIR:
                hi = "(c->reg[%d] & 0xff)" % o.reg_hi if o.size == 5 else "c->reg[%d]" % o.reg_hi
                return "(((uint64_t)%s << 32) | c->reg[%d])" % (hi, o.reg)
            if o.kind == OPK_ADDR:
                return "(uint64_t)%s" % u32(o.val)
            return None

        def wlist(o):
            if o.kind == OPK_REG:
                return [(o.low_first - 1, WK_REG, o.reg)]
            if o.kind == OPK_PAIR:
                return [(o.low_first - 1, WK_REG, o.reg), (o.high_first - 1, WK_REG, o.reg_hi)]
            if o.kind == OPK_CTRL:
                d = 3 if o.crlo in (0xd, 0xe) else 1 if o.crlo in (0x2, 0x3) else 0
                return [(d, WK_CTRL, o.crlo)]
            return None

        def values(o, x):
            """write_op's split of a value into its writes"""
            if o.kind == OPK_PAIR:
                hi = "((uint32_t)((%s) >> 32) & 0xff)" % x if o.size == 5 else "(uint32_t)((%s) >> 32)" % x
                return ["(uint32_t)(%s)" % x, hi]
            return ["(uint32_t)(%s)" % x]

        stop, n, ws = self.dec.shape(ins.addr)
        guard = ("if (%s) " % flag) if flag else ""
        mode = None
        pre_ws = []
        dst = None
        if hd in (H("H_ADDA"), H("H_SUBA")) and v(0) and v(1):
            x = "(uint64_t)(uint32_t)((uint32_t)%s %s ((uint32_t)%s << %d))" % (
                v(0), "+" if hd == H("H_ADDA") else "-", v(1), ins.sub)
            dst = ops[last]
        elif hd in (H("H_DADDSP"), H("H_DSUBSP"), H("H_DMPYSP")) and v(0) and v(1):
            opch = "+" if hd == H("H_DADDSP") else "-" if hd == H("H_DSUBSP") else "*"
            x = "jit_dsp(c, %d, %d, %s, %s, '%s')" % (ins.unit, ins.side, v(0), v(1), opch)
            dst = ops[2]
        elif hd in (H("H_CMPEQSP"), H("H_CMPGTSP"), H("H_CMPLTSP")) and v(0) and v(1):
            opch = "==" if hd == H("H_CMPEQSP") else ">" if hd == H("H_CMPGTSP") else "<"
            x = "(uint64_t)(jit_u2f((uint32_t)%s) %s jit_u2f((uint32_t)%s))" % (v(0), opch, v(1))
            dst = ops[2]
        elif hd == H("H_ABSSP") and v(0):
            x = "(uint64_t)((uint32_t)%s & 0x7fffffffu)" % v(0)
            dst = ops[1]
        elif hd == H("H_MVD") and v(0):
            x = v(0)
            dst = ops[1]
        elif hd in (H("H_SET"), H("H_CLR")) and ins.nops == 4 and v(0):
            csta, cstb = ops[1].val & 31, ops[2].val & 31
            if cstb < csta:
                x = "(uint64_t)(uint32_t)%s" % v(0)
            else:
                w = cstb - csta + 1
                m = ((0xffffffff if w >= 32 else (1 << w) - 1) << csta) & 0xffffffff
                x = ("(uint64_t)((uint32_t)%s & %s)" % (v(0), u32(~m & 0xffffffff)) if hd == H("H_CLR")
                     else "(uint64_t)((uint32_t)%s | %s)" % (v(0), u32(m)))
            dst = ops[last]
        elif hd == H("H_MPY16") and v(1) and (v(0) or ops[0].kind == OPK_CONST):
            sub = ins.sub
            x1 = u32(ops[0].val) if ops[0].kind == OPK_CONST else "(uint32_t)%s" % v(0)
            x2 = "(uint32_t)%s" % v(1)

            def field(xv, high, unsigned):
                if high:
                    return "(int64_t)(uint16_t)((%s) >> 16)" % xv if unsigned else "(int64_t)(int16_t)((%s) >> 16)" % xv
                return "(int64_t)(uint16_t)(%s)" % xv if unsigned else "(int64_t)(int16_t)(%s)" % xv
            x = "(uint64_t)(%s * %s)" % (field(x1, sub & 1, sub & 4), field(x2, (sub >> 1) & 1, sub & 8))
            dst = ops[2]
        elif hd in (H("H_MPYLI"), H("H_MPYHI")) and v(0) and v(1):
            x16 = v(1) if ins.sub else v(0)
            x32 = v(0) if ins.sub else v(1)
            f = ("(int64_t)(int16_t)((uint32_t)%s >> 16)" % x16 if hd == H("H_MPYHI")
                 else "(int64_t)(int16_t)(uint32_t)%s" % x16)
            x = "(uint64_t)(%s * (int64_t)(int32_t)%s)" % (f, x32)
            dst = ops[2]
        elif hd in (H("H_LOAD"), H("H_STORE")):
            mo = ops[0] if hd == H("H_LOAD") else ops[1]
            if mo.kind != OPK_MEM or (mo.mem_mode & ~4) not in (0, 1, 8, 9, 10, 11):
                return False
            mode = mo.mem_mode
            if (mode & ~4) >= 8:
                pre_ws = [(0, WK_REG, mo.reg)]
            if hd == H("H_LOAD"):
                dst = ops[1]
                if dst.kind not in (OPK_REG, OPK_PAIR):
                    return False
            else:
                if ops[0].kind not in (OPK_REG, OPK_PAIR):
                    return False
        elif hd == H("H_MVC"):
            src = ops[0]
            if src.kind == OPK_CTRL:
                x = "(uint64_t)c->api->ctrl_read(c, %d, %s)" % (src.crlo, u32(ins.addr & ~31))
            elif src.kind == OPK_REG:
                x = "(uint64_t)c->reg[%d]" % src.reg
            else:
                return False
            dst = ops[1]
        else:
            return False

        expect = list(pre_ws) + (wlist(dst) or [None] if dst is not None else [])
        if None in expect or [tuple(w) for w in ws] != expect or stop != 0:
            return False

        if hd in (H("H_LOAD"), H("H_STORE")):
            mo = ops[0] if hd == H("H_LOAD") else ops[1]
            off = ("c->reg[%d] * %du" % (mo.mem_offreg, mo.mem_scale) if mode & 4
                   else "%s" % u32((mo.val * mo.mem_scale) & 0xffffffff))
            m = mode & ~4
            wv = self.t()
            sign = "-" if m in (0, 8, 10) else "+"
            e("    uint32_t %s = c->reg[%d] %s %s;" % (wv, mo.reg, sign, off))
            ea = wv if m in (0, 1, 8, 9) else "c->reg[%d]" % mo.reg
            if pre_ws:
                push(0, WK_REG, mo.reg, wv)
            if hd == H("H_LOAD"):
                sub = ins.sub
                val = self.t()
                e("    uint64_t %s = 0;" % val)
                if sub == 5:
                    rd = "(uint64_t)jit_load(c, %s, 4) | ((uint64_t)jit_load(c, %s + 4, 4) << 32)" % (ea, ea)
                else:
                    rd = {0: "(uint64_t)jit_load(c, %s, 4)", 1: "(uint64_t)(uint32_t)(int8_t)jit_load(c, %s, 1)",
                          2: "(uint64_t)jit_load(c, %s, 1)", 3: "(uint64_t)(uint32_t)(int16_t)jit_load(c, %s, 2)",
                          4: "(uint64_t)jit_load(c, %s, 2)"}[sub] % ea
                if sub == 5:
                    e("    " + guarded(flag, "{ uint32_t ea_ = %s; %s = (uint64_t)jit_load(c, ea_, 4); %s |= (uint64_t)jit_load(c, ea_ + 4, 4) << 32; }"
                                      % (ea, val, val)))
                else:
                    e("    " + guarded(flag, "%s = %s;" % (val, rd)))
                for (d, k, idx), xv in zip(wlist(dst), values(dst, val)):
                    t = self.t()
                    e("    uint32_t %s = %s;" % (t, xv))
                    push(d, k, idx, t)
            else:
                src = ops[0]
                sv = ("(((uint64_t)%s << 32) | c->reg[%d])" % (
                    "(c->reg[%d] & 0xff)" % src.reg_hi if src.size == 5 else "c->reg[%d]" % src.reg_hi, src.reg)
                      if src.kind == OPK_PAIR else "(uint64_t)c->reg[%d]" % src.reg)
                sub = ins.sub
                if sub == 5:
                    body = "{ uint64_t v_ = %s; uint32_t ea_ = %s; jit_store(c, ea_, (uint32_t)v_, 4); jit_store(c, ea_ + 4, (uint32_t)(v_ >> 32), 4); }" % (sv, ea)
                else:
                    sz = 1 if sub == 1 else 2 if sub == 3 else 4
                    body = "jit_store(c, %s, (uint32_t)%s, %d);" % (ea, sv, sz)
                e("    " + guarded(flag, body))
            return True

        val = self.t()
        if hd == H("H_MVC"):
            e("    uint64_t %s = 0;" % val)
            e("    " + guarded(flag, "%s = %s;" % (val, x)))
        else:
            e("    uint64_t %s = %s;" % (val, x))
        for (d, k, idx), xv in zip(wlist(dst), values(dst, val)):
            t = self.t()
            e("    uint32_t %s = %s;" % (t, xv))
            push(d, k, idx, t)
        return True

    def capture(self, ins, pk, flag, push):
        """The interpreter's handler, its writes captured and landed statically."""
        if self.handler_native(ins, pk, flag, push):
            return
        stop, n, ws = self.dec.shape(ins.addr)
        k = len(self.captured)
        self.captured.append(ins.addr)
        cap = self.t()
        e = self.emit
        e("    c66x_jit_cap %s;" % cap)
        call = ("if (__builtin_expect(c->api->exec_capture(c, %s[%d], %s, &%s) != 0 || %s.n != %d, 0)) "
                "jit_fatal(c, 0x%08x, %s.n, %d);" % (self.ins_name, k, u32(pk.next), cap, cap, n, ins.addr, cap, n))
        e("    " + guarded(flag, "{ " + call + " }"))
        for i, (delay, kind, idx) in enumerate(ws):
            v = self.t()
            e("    uint32_t %s = %s.e[%d].val;" % (v, cap, i))
            push(delay, kind, idx, v)

    # --- the whole region
    def generate(self):
        st = State(self.root, 0, [], [], [], PRE_CYCLES, {})
        self.labels[st.key()] = 0
        self.cycle(0, st, root=True)
        done = {0}
        while self.work:
            label, st = self.work.pop()
            if label in done:
                continue
            done.add(label)
            self.cycle(label, st)
        decl = ["void %s(c66x_core *c, uint64_t end)" % self.name, "{"]
        decl.append("    uint64_t gen = c->code_gen;")
        self.declare(decl)
        return "\n".join(decl + self.body + self.cold + ["}"]), len(self.labels)

    def declare(self, decl):
        if self.captured:
            decl.append("    static const uint32_t ins_addr[] = { %s };" % ", ".join(u32(a) for a in self.captured))
            decl.append("    static c66x_insn *ins[%d];" % len(self.captured))
            decl.append("    static c66x_core *ins_core;")
            decl.append("    static uint64_t ins_gen;")
            decl.append("    if (ins_core != c || ins_gen != gen) {")
            decl.append("        for (unsigned i = 0; i < %d; i++) ins[i] = c->api->insn_at(c, ins_addr[i]);" % len(self.captured))
            decl.append("        ins_core = c; ins_gen = gen;")
            decl.append("    }")
        for i in range(self.maxv["r"]):
            decl.append("    uint32_t r%d = 0; uint8_t rf%d = 0;" % (i, i))
        for i in range(self.maxv["m"]):
            decl.append("    uint32_t m%d = 0; uint8_t mf%d = 0;" % (i, i))
        for i in range(self.maxv["b"]):
            decl.append("    uint32_t bt%d = 0; uint8_t bf%d = 0;" % (i, i))


# --------------------------------------------------------------------------
# a whole module: one state graph for every root
#
# States are keyed the same way whichever root reaches them, so code shared by
# many roots is compiled once. After exploration the states are cut, in the
# order they were found, into functions of FN_NODES states (gcc's time grows
# faster than a function's size); a jump into another function hands the target
# back to jit_run, which enters it, with the target state's pending values in
# c->jit_xs. Calling it directly instead would look like a tail call but is not
# compiled as one -- see jit_run for what that cost.

FN_NODES = 1500
COLD = 20000                          # profile count below which a packet's states are left to the interpreter
XS_R, XS_M, XS_B = 0, 32, 48          # c->jit_xs slots of r<i>, m<j>, bt<k> (and their flags)


class ModuleGen(RegionGen):
    def __init__(self, dec, sites, per_root, max_total):
        RegionGen.__init__(self, dec, 0, max_total, 0, sites)
        self.per_root = per_root
        self.max_total = max_total
        self.ins_name = "jit_ins"
        self.code = {}            # label -> lines of its cycle
        self.cold_of = {}         # label -> lines of its exit stubs
        self.pos_of = {}          # label -> the positional state it starts from
        self.entries = {}         # root pc -> label
        self.entry_labels = set()
        self.node_deps = {}       # label -> fetch packets its cycle decoded
        self.prof_n = {pc: n for pc, n, nf in dec.prof}
        self.cold_min = COLD
        self.fn_nodes = FN_NODES
        self.fn_deps = []
        self.cold = []
        self.clean = True         # export clean states as entries (c15)
        self.clean_entries = {}   # pc -> label of its clean state
        self.qentries = {}        # pc -> (label, rem, target): roots entered with one branch in flight (c16)

    def goto(self, label):
        return "\x01%d\x01" % label

    def root_label(self, label):
        return label in self.entry_labels

    def transition(self, st, ind="    "):
        if (st.pc is not None and st.pc in self.dec.have and not st.queue and st.mcnop == 0
                and st.key() not in self.labels):
            self.materialize(st, u32(st.pc), ind, EXIT_CHAIN)
            return
        if (st.pc is not None and st.mcnop == 0 and self.prof_n.get(st.pc, 0) < self.cold_min
                and st.key() not in self.labels):
            self.materialize(st, u32(st.pc), ind, EXIT_UNCOMPILABLE, (st.pc, "cold"))
            return
        if len(st.ring) > XS_M - XS_R or len(st.imm) > XS_B - XS_M:
            self.materialize(st, u32(st.pc) if st.pc is not None else st.pc_expr, ind, EXIT_NODES)
            return
        RegionGen.transition(self, st, ind)

    def add_root(self, pc):
        st = State(pc, 0, [], [], [], PRE_CYCLES, {})
        key = st.key()
        if key in self.labels:
            self.entries[pc] = self.labels[key]
            return
        if len(self.labels) >= self.max_total:
            return
        label = len(self.labels)
        self.labels[key] = label
        self.entries[pc] = label
        self.entry_labels.add(label)
        self.pos_of[label] = st
        self.max_nodes = min(self.max_total, len(self.labels) + self.per_root)
        self.work.append((label, st))
        # Its clean twin: entered instead when nothing is pending in the
        # interpreter, so the root's warm-up states are skipped.
        if self.clean:
            self.push_clean(pc)
        self.drain()

    def add_qroot(self, pc, rem, target):
        """A root entered with exactly one branch in flight: rem slots left, a known target."""
        q = {"rem": rem, "tconst": target, "tvar": None, "flag": None}
        st = State(pc, 0, [q], [], [], PRE_CYCLES, {})
        key = st.key()
        if key in self.labels:
            self.qentries[pc] = (self.labels[key], rem, target)
            return
        if len(self.labels) >= self.max_total:
            return
        label = len(self.labels)
        self.labels[key] = label
        self.qentries[pc] = (label, rem, target)
        self.entry_labels.add(label)
        self.pos_of[label] = st
        self.max_nodes = min(self.max_total, len(self.labels) + self.per_root)
        self.work.append((label, st))
        self.drain()

    def push_clean(self, pc):
        twin = State(pc, 0, [], [], [], 0, {})
        if twin.key() in self.labels or len(self.labels) >= self.max_total:
            return False
        tl = len(self.labels)
        self.labels[twin.key()] = tl
        self.pos_of[tl] = twin
        self.depth[tl] = 0
        self.work.append((tl, twin))
        return True

    def add_clean_root(self, pc):
        """A root entered only through its clean state: no warm-up chain."""
        self.max_nodes = min(self.max_total, len(self.labels) + self.per_root)
        if self.push_clean(pc):
            self.drain()

    def drain(self):
        while self.work:
            lab, pst = self.work.pop()
            if lab in self.code:
                continue
            self.pos_of[lab] = pst
            self.body, self.cold, self.deps = [], [], set()
            self.cycle(lab, pst, root=lab in self.entry_labels)
            self.code[lab], self.cold_of[lab], self.node_deps[lab] = self.body, self.cold, self.deps

    @staticmethod
    def xs_vars(pst):
        """(variable, value slot, flag?) for every positional variable a state names."""
        out = []
        for i, w in enumerate(pst.ring):
            if w["const"] is None:
                out.append(("r%d" % i, XS_R + i, False))
            if w["flag"]:
                out.append(("rf%d" % i, XS_R + i, True))
        for j, w in enumerate(pst.imm):
            if w["const"] is None:
                out.append(("m%d" % j, XS_M + j, False))
            if w["flag"]:
                out.append(("mf%d" % j, XS_M + j, True))
        for k, q in enumerate(pst.queue):
            if q["tconst"] is None:
                out.append(("bt%d" % k, XS_B + k, False))
            if q["flag"]:
                out.append(("bf%d" % k, XS_B + k, True))
        return out

    @staticmethod
    def is_clean(key):
        pc, mcnop, queue, ring, imm, pre, known = key
        return pc is not None and not mcnop and not queue and not ring and not imm and not pre and not known

    def functions(self):
        """C source of every function, and the name of the function each entry lives in."""
        nlab = len(self.labels)
        if self.clean:
            self.clean_entries = {key[0]: lab for key, lab in self.labels.items()
                                  if lab in self.code and self.is_clean(key) and key[0] != self.dec.idle_head}
        nfn = (nlab + self.fn_nodes - 1) // self.fn_nodes
        fn_of = lambda lab: lab // self.fn_nodes
        cross = [set() for _ in range(nfn)]
        out = []
        for f in range(nfn):
            labs = [l for l in range(f * self.fn_nodes, min(nlab, (f + 1) * self.fn_nodes)) if l in self.code]
            body = []
            for l in labs:
                for line in self.code[l]:
                    if "\x01" in line:
                        pre, tgt, post = line.split("\x01")
                        tgt = int(tgt)
                        if fn_of(tgt) == f:
                            line = pre + "goto L%d;" % tgt + post
                        else:
                            cross[fn_of(tgt)].add(tgt)
                            saves = " ".join("c->jit_xs.%s[%d] = %s;" % ("f" if fl else "v", slot, var)
                                             for var, slot, fl in self.xs_vars(self.pos_of[tgt]))
                            line = pre + "{ %s jit_hop_fn = %d; jit_hop_entry = %d; return; }" % (
                                saves, fn_of(tgt), tgt) + post
                    body.append(line)
            cold = [line for l in labs for line in self.cold_of[l]]
            out.append((labs, body, cold))
        sources = []
        clean_labels = set(self.clean_entries.values())
        for f, (labs, body, cold) in enumerate(out):
            head = ["void jit_fn%d(c66x_core *c, uint64_t end, unsigned entry, uint64_t gen)" % f, "{"]
            for i in range(32):
                head.append("    uint32_t r%d = 0; uint8_t rf%d = 0;" % (i, i))
            for i in range(16):
                head.append("    uint32_t m%d = 0; uint8_t mf%d = 0;" % (i, i))
            for i in range(8):
                head.append("    uint32_t bt%d = 0; uint8_t bf%d = 0;" % (i, i))
            head.append("    switch (entry) {")
            for l in labs:
                if l in self.entry_labels or l in clean_labels:
                    head.append("    case %d: goto L%d;" % (l, l))
                elif l in cross[f]:
                    loads = " ".join("%s = c->jit_xs.%s[%d];" % (var, "f" if fl else "v", slot)
                                     for var, slot, fl in self.xs_vars(self.pos_of[l]))
                    head.append("    case %d: %s if (__builtin_expect(!(jit_vf%d.core == c && jit_vf%d.gen == "
                                "c->code_gen + 1), 0) && !jit_verify_fn%d(c)) goto X%d; goto L%d;"
                                % (l, loads, f, f, f, l, l))
            head.append("    default: return;")
            head.append("    }")
            deps = sorted(set().union(*(self.node_deps[l] for l in labs)))
            self.fn_deps.append(deps)
            blob = b"".join(self.dec.bytes32(d) or b"\0" * 32 for d in deps)
            pre = ["const uint32_t jit_da%d[] = { %s };" % (f, ", ".join(u32(d) for d in deps) or "0"),
                   "const uint8_t jit_db%d[] = { %s };" % (f, ", ".join(str(x) for x in blob) or "0"),
                   "c66x_jit_verified jit_vf%d;" % f,
                   "static int jit_verify_fn%d(c66x_core *c)" % f,
                   "{",
                   "    if (!c->api->verify(c, jit_da%d, jit_db%d, %d)) return 0;" % (f, f, len(deps)),
                   "    jit_vf%d.core = c; jit_vf%d.gen = c->code_gen + 1;" % (f, f),
                   "    return 1;",
                   "}"]
            sources.append("\n".join(pre + head + body + cold + ["}"]))
        return sources, nfn


# --------------------------------------------------------------------------
# SPLOOP kernels
#
# spl_fast_cycles in steady state: each cycle commits, samples the SPLOOPW
# condition register, runs the loop buffer's instructions for its body offset
# (oldest iteration first), flushes its stores and, every ii cycles, passes a
# stage boundary. No stall and no interrupt test happen between boundaries. The
# state is keyed by the offset instead of a pc.

H_SPLOOPW = None   # filled from the profile's kind numbers at run time (see KernelGen.boundary)


def kernel_compilable(dec, k):
    if k.dynlen > 48 or k.ii == 0 or len(k.body) != k.dynlen:
        return "shape"
    for part in k.body:
        for addr in part:
            ins = dec.insn(addr)
            if ins is None:
                return "undecodable"
            if ins.fop in ("branch", "callp") or ins.name in NO_CAPTURE:
                return "control flow in the body"
            if native(ins):
                continue
            sh = dec.shape(addr)
            if sh is None or sh[0] != 0 or sh[1] > CAP_MAX:
                return "handler %s" % ins.name
    return None


class KernelGen(RegionGen):
    """The steady kernel, or (drain=True) the cycles after the last iteration
    started, while program-memory fetch is still off: iteration k's stage j runs
    only while k <= last_iter, so the instruction list of a drain cycle depends on
    the body offset and on how many stages have drained. State d counts those:
    d = (cycle - t0) / ii - last_iter, from 1 to nstage - 1."""

    def __init__(self, dec, k, max_nodes, uid, sites, kind_w, drain=False):
        RegionGen.__init__(self, dec, k.addr, max_nodes, uid, sites)
        self.k = k
        self.kernel = True
        self.drain = drain
        self.kind_w = kind_w      # the handler number of SPLOOPW
        self.name = ("drain_%08x_%d" if drain else "kernel_%08x_%d") % (k.addr, uid)
        self.lists = []
        nstage = (k.dynlen + k.ii - 1) // k.ii
        self.nstage = nstage
        for d in range(1, nstage) if drain else [0]:
            for o in range(k.ii):
                lst = []
                if k.dynlen <= k.ii:
                    if not drain and o < k.dynlen:
                        lst = list(k.body[o])
                else:
                    for j in range(nstage - 1, d - 1, -1):
                        bo = o + j * k.ii
                        if bo < k.dynlen:
                            lst.extend(k.body[bo])
                self.lists.append([dec.insn(a) for a in lst])

    def transition(self, st, ind="    "):
        key = st.key()
        if key not in self.labels:
            if len(self.labels) >= self.max_nodes:
                self.materialize(st, "0", ind, EXIT_NODES)
                return
            self.labels[key] = len(self.labels)
            pos = self.positional(st)
            self.maxv["r"] = max(self.maxv["r"], len(pos.ring))
            self.maxv["m"] = max(self.maxv["m"], len(pos.imm))
            self.work.append((self.labels[key], pos))
        assigns = []
        for i, w in enumerate(st.ring):
            if w["const"] is None:
                assigns.append(("r%d" % i, w["val"], "uint32_t"))
            if w["flag"]:
                assigns.append(("rf%d" % i, w["flag"], "uint8_t"))
        for j, w in enumerate(st.imm):
            if w["const"] is None:
                assigns.append(("m%d" % j, w["val"], "uint32_t"))
            if w["flag"]:
                assigns.append(("mf%d" % j, w["flag"], "uint8_t"))
        assigns = [a for a in assigns if a[0] != a[1]]
        if assigns:
            self.emit(ind + "{ " + " ".join("%s _%s = %s;" % (ty, d, s) for d, s, ty in assigns)
                      + " " + " ".join("%s = _%s;" % (d, d) for d, s, ty in assigns) + " }")
        self.emit(ind + "goto L%d;" % self.labels[key])

    def cycle(self, label, st, root=False):
        e = self.emit
        k = self.k
        off = st.pc
        e("L%d: ; /* offset %d pre %d r%d m%d */" % (label, off, st.pre, len(st.ring), len(st.imm)))
        known = dict(st.known)
        if not root:
            e("    if (__builtin_expect(c->cycle >= end, 0)) { c->jit_exit[%d]++; goto X%d; }" % (EXIT_BUDGET, label))
            e("    if (__builtin_expect(c->code_gen != gen, 0)) { c->jit_exit[%d]++; goto X%d; }" % (EXIT_GEN, label))
            if st.pre:
                e("    c66x_jit_pre_commit(c);")
                known = {}
            for w in [w for w in st.ring if w["land"] == 0] + st.imm:
                if w["kind"] == WK_REG:
                    e("    " + guarded(w["flag"], "c->reg[%d] = %s;" % (w["idx"], w["val"])))
                    known.pop(w["idx"], None)
                    if w["const"] is not None and not w["flag"]:
                        known[w["idx"]] = (w["const"], 0)
                else:
                    e("    " + guarded(w["flag"], "c->api->ctrl_write(c, %d, %s);" % (w["idx"], w["val"])))
        ring = [dict(w) for w in st.ring if (root or w["land"] > 0)]
        if k.creg >= 0:
            e("    c->cond_hist[c->cycle & 7] = c->reg[%d];" % k.creg)
        new_ring, new_imm = [], []
        lst = self.lists[off]
        nnat = sum(1 for ins in lst if native(ins))
        if nnat:
            e("    c->st.insns += %d;" % nnat)
        dummy = Rec()
        dummy.next = 0
        # The interpreter holds a kernel cycle's stores to its end only so that a
        # load in the same cycle reads the old value (fast_cycles' store_now rule):
        # with no load among the instructions they can land at once.
        h_load, h_store = handler_number("H_LOAD"), handler_number("H_STORE")
        immediate = (all(ins.handler != h_load for ins in lst)
                     and any(ins.handler == h_store for ins in lst))
        if immediate:
            e("    c->store_now = 1;")
        for ins in lst:
            self.deps.add(ins.addr & ~31)
            self.insn(ins, dummy, new_ring, new_imm, [], known)
        if immediate:
            e("    c->store_now = 0;")
        e("    if (c->npst) c->api->flush_stores(c);")
        for w in ring:
            w["land"] -= 1
        ring = ring + new_ring
        if self.drain:
            # the lists run offset-major, so the next cycle -- next offset, and
            # the next d when the offset wraps -- is always the next list
            nxt_id = off + 1
            nxt = State(nxt_id, 0, [], ring, new_imm, max(st.pre - 1, 0), self.trim(known))
            # every cycle of a terminated loop ends in the interpreter's
            # spl_end_cycle, which is where the loop goes inactive
            e("    c->api->spl_end_cycle(c);")
            e("    c->cycle++;")
            e("    if (__builtin_expect(!c->spl.active, 0)) {")
            self.count("        ", EXIT_STUB, (k.addr, "drain ends"))
            self.put_back(nxt, "0", "        ")
            e("        return;")
            e("    }")
            if nxt_id >= len(self.lists):
                self.count("    ", EXIT_STUB, (k.addr, "drain runs out"))
                self.put_back(nxt, "0", "    ")
                e("    return;")
            else:
                self.transition(nxt)
            if not root:
                hot, self.body = self.body, self.cold
                self.emit("X%d: ;" % label)
                self.put_back(st, "0", "    ")
                self.emit("    return;")
                self.body = hot
            return
        nxt = State((off + 1) % k.ii, 0, [], ring, new_imm, max(st.pre - 1, 0), self.trim(known))
        if off == k.ii - 1:
            if k.kind == self.kind_w:
                fast = "((c->cond_hist[(c->cycle - 3) & 7] == 0) == %d)" % k.cz
                step = ""
            else:
                fast = "c->cr[CR_ILC] != 0"
                step = "c->cr[CR_ILC]--; "
            e("    if (__builtin_expect(%s && !(c->ifr && pending_interrupt(c)), 1)) {" % fast)
            e("        %sc->spl.last_iter = (int)((c->cycle - c->spl.t0 + 1) / %d);" % (step, k.ii))
            e("    } else {")
            e("        c->api->spl_end_cycle(c);")
            e("        if (!c->spl.active || c->spl.terminated || c->spl.abrupt) {")
            e("            c->cycle++;")
            self.count("            ", EXIT_STUB, (k.addr, "kernel ends"))
            self.put_back(nxt, "0", "            ")
            e("            return;")
            e("        }")
            e("    }")
        e("    c->cycle++;")
        self.transition(nxt)
        if not root:
            hot, self.body = self.body, self.cold
            self.emit("X%d: ;" % label)
            self.put_back(st, "0", "    ")
            self.emit("    return;")
            self.body = hot

    def generate(self):
        k = self.k
        entries = []
        for off in range(len(self.lists)):
            st = State(off, 0, [], [], [], PRE_CYCLES, {})
            self.labels[st.key()] = len(self.labels)
            entries.append(self.labels[st.key()])
            self.work.append((self.labels[st.key()], st))
        entry_set = set(entries)
        done = set()
        while self.work:
            label, st = self.work.pop()
            if label in done:
                continue
            done.add(label)
            if label in entry_set:
                # entered after this cycle's commit
                self.body.append("E%d: ;" % label)
                self.cycle(label, st, root=True)
            else:
                self.cycle(label, st)
        decl = ["void %s(c66x_core *c, uint64_t end)" % self.name, "{"]
        decl.append("    uint64_t gen = c->code_gen;")
        self.declare(decl)
        if self.drain:
            decl.append("    unsigned d_ = (unsigned)((c->cycle - c->spl.t0) / %d - (uint64_t)c->spl.last_iter);" % k.ii)
            decl.append("    if (d_ < 1 || d_ > %d) return;" % (self.nstage - 1))
            decl.append("    switch ((unsigned)((c->cycle - c->spl.t0) %% %d) + %d * (d_ - 1)) {" % (k.ii, k.ii))
        else:
            decl.append("    switch ((unsigned)((c->cycle - c->spl.t0) %% %d)) {" % k.ii)
        for off, label in enumerate(entries):
            decl.append("    case %d: goto E%d;" % (off, label))
        decl.append("    }")
        return "\n".join(decl + self.body + self.cold + ["}"]), len(self.labels)


# --------------------------------------------------------------------------
# whole SPLOOP invocations (c16, ABI 11)
#
# A short hot loop spends ~40% of every call outside the steady state a kernel
# serves: the loading cycles run in the general loop, the ramp and the drain in
# spl_fast_cycles' generic path. A loop function runs the whole invocation from
# the SPLOOP execute packet: cycle -1 is that packet (spl_start), cycles 0.. load
# the body (the program-memory packet plus the buffered iterations, as the
# general loop runs them), the ramp follows until every stage is running, then
# the steady kernel with the ILC test at each stage boundary, then the drain
# until program fetch comes back. c->spl is kept as the interpreter keeps it, so
# the function can hand over at the top of any cycle: an ILC reaching zero before
# the steady state, an interrupt at a stage boundary, a possible stall.
#
# State pc is a tuple: ("L", r) loading/ramp cycle r, ("S", o) steady offset o,
# ("D", j) drain/refill cycle j after the last iteration started.

PM_REFILL = 6          # c66x_core_int.h
LOOP_SKIP = {"sploop", "sploopd", "sploopw", "spkernel", "spkernelr", "spmask", "spmaskr"}


def loop_insn_ok(dec, ins):
    """Why an instruction of the loop's program-memory packets cannot run compiled, or None."""
    if ins.name in LOOP_SKIP or ins.fop == "nop":
        return None
    if ins.fop in ("branch", "callp") or ins.isbranch or ins.name in NO_CAPTURE:
        return "control flow %s" % ins.name
    if native(ins):
        return None
    sh = dec.shape(ins.addr)
    if sh is None or sh[0] != 0 or sh[1] > CAP_MAX:
        return "handler %s" % ins.name
    return None


class LoopGen(KernelGen):
    def __init__(self, dec, k, max_nodes, uid, sites, kind_w, kind_sploop):
        KernelGen.__init__(self, dec, k, max_nodes, uid, sites, kind_w)
        self.name = "loop_%08x_%d" % (k.addr, uid)
        self.kind_sploop = kind_sploop
        self.steady = self.lists              # KernelGen's d = 0 lists, by offset
        self.why = None
        self.plan()

    def reject(self, why):
        if self.why is None:
            self.why = why

    def plan(self):
        """Simulate the invocation's static part as the interpreter runs it."""
        dec, k = self.dec, self.k
        ii, nstage = k.ii, self.nstage
        if k.creg >= 0:
            return self.reject("conditional loop")
        if k.kind == self.kind_w:
            return self.reject("sploopw")
        if k.dynlen <= ii:
            return self.reject("one stage")
        pk0 = dec.packet(k.addr)
        if pk0 is None or pk0.pc != k.addr or not pk0.cacheable:
            return self.reject("sploop packet")
        for ins in pk0.insns:
            why = loop_insn_ok(dec, ins)
            if why:
                return self.reject("sploop packet: " + why)
        self.pk0 = pk0
        mc, pc = pk0.xnops, pk0.next
        body, cycles, r, dynlen, fd = [], [], 0, None, 0
        while dynlen is None:
            if r >= 48:
                return self.reject("no spkernel")
            top_mc, top_pc = mc, pc
            pm = None
            if mc > 0:
                mc -= 1
            else:
                pm = dec.packet(pc)
                if pm is None:
                    return self.reject("loading packet 0x%08x" % pc)
                if not pm.cacheable:
                    # a packet across two fetch packets is never cached, so the
                    # description leaves its shape out: the general loop derives
                    # the stall mask and NOP cycles from the instructions
                    pm.xnops = max([ins.xnops for ins in pm.insns] or [0])
                    pm.branched = any(ins.isbranch for ins in pm.insns)
                    pm.xmask = 0
                    for ins in pm.insns:
                        for op in ins.ops:
                            if op.xpath and op.rw != 2:
                                pm.xmask |= 1 << op.reg
                                if op.kind == OPK_PAIR:
                                    pm.xmask |= 1 << op.reg_hi
                for ins in pm.insns:
                    why = loop_insn_ok(dec, ins)
                    if why:
                        return self.reject("loading packet 0x%08x: %s" % (pc, why))
                pc = pm.next
            mask = 0
            rec, kernel = [], False
            for ins in (pm.insns if pm else []):
                if ins.name == "spmask" and ins.ops:
                    mask |= ins.ops[0].val
            for ins in (pm.insns if pm else []):
                if ins.name == "spkernel":
                    kernel = True
                    if len(ins.ops) >= 2:
                        fd = ins.ops[0].val * ii + ins.ops[1].val
                    continue
                if ins.name in ("spmask", "bnop", "nop"):
                    continue
                if ins.unit >= 0 and (mask >> ins.unit) & 1:
                    continue
                if len(rec) < 8:
                    rec.append(ins.addr)
            buf = []
            for kk in range(1, r // ii + 1):         # oldest (largest offset) first
                for a in body[r - kk * ii]:
                    ins = dec.insn(a)
                    if not (ins.unit >= 0 and (mask >> ins.unit) & 1):
                        buf.append(ins)
            body.append(rec)
            if pm:
                mc = max(mc, pm.xnops)
            cycles.append({"pm": pm, "buf": buf, "mc": top_mc, "pc": top_pc})
            if kernel:
                dynlen = r + 1
            r += 1
        if dynlen != k.dynlen or [list(b) for b in body] != [list(b) for b in k.body]:
            return self.reject("body differs from the recorded one")
        # the ramp: loading done, not every stage running yet. A SPLOOPD's first
        # three cycles' stage boundaries skip the ILC test (early3), so a loop
        # whose steady state would start sooner keeps static cycles until then.
        lend = nstage * ii
        while k.kind != self.kind_sploop and lend < 3:
            lend += ii
        for r in range(dynlen, lend):
            top_mc = mc
            if mc > 0:
                mc -= 1
            buf = []
            for kk in range(1, r // ii + 1):
                off = r - kk * ii
                if off < dynlen:
                    buf.extend(dec.insn(a) for a in body[off])
            cycles.append({"pm": None, "buf": buf, "mc": top_mc, "pc": pc})
        if mc:
            return self.reject("nop cycles into the steady state")
        self.cycles, self.body_addrs, self.fd, self.post_pc = cycles, body, fd, pc
        self.epilog = k.dynlen - ii
        delay = min(fd, self.epilog)
        self.je = max(delay + PM_REFILL, fd)      # drain cycle at which program fetch is back
        self.delay = delay
        self.drain = []
        for j in range(self.je):
            o, d = j % ii, j // ii + 1
            lst = []
            if j < self.epilog:
                for jj in range(nstage - 1, d - 1, -1):
                    bo = o + jj * ii
                    if bo < k.dynlen:
                        lst.extend(dec.insn(a) for a in body[bo])
            self.drain.append(lst)

    def put_back(self, st, pc_expr, ind, ring_after_commit=False):
        self.emit(ind + "c->mcnop = %d;" % st.mcnop)
        RegionGen.put_back(self, st, pc_expr, ind, ring_after_commit)

    def exit_now(self, st, why, site, ind="    "):
        self.count(ind, why, site)
        self.put_back(st, "0", ind)
        self.emit(ind + "return;")

    def cycle(self, label, st, root=False):
        e = self.emit
        k = self.k
        ii = k.ii
        phase, idx = st.pc
        e("L%d: ; /* %s %d mcnop %d pre %d r%d m%d */" % (label, phase, idx, st.mcnop, st.pre, len(st.ring), len(st.imm)))
        known = dict(st.known)
        post_used = False
        if phase == "L":
            c = self.cycles[idx]
            pm, buf, r = c["pm"], c["buf"], idx
        elif phase == "S":
            pm, buf, r = None, self.steady[idx], None
        else:
            pm, buf, r = None, self.drain[idx], None
        boundary = (phase == "L" and (idx + 1) % ii == 0) or (phase == "S" and idx == ii - 1)
        early3 = phase == "L" and k.kind != self.kind_sploop and idx + 1 <= 3
        if not root:
            e("    if (__builtin_expect(c->cycle >= end, 0)) { c->jit_exit[%d]++; goto X%d; }" % (EXIT_BUDGET, label))
            e("    if (__builtin_expect(c->code_gen != gen, 0)) { c->jit_exit[%d]++; goto X%d; }" % (EXIT_GEN, label))
            if st.pre:
                e("    c66x_jit_pre_commit(c);")
                known = {}
            for w in [w for w in st.ring if w["land"] == 0] + st.imm:
                if w["kind"] == WK_REG:
                    e("    " + guarded(w["flag"], "c->reg[%d] = %s;" % (w["idx"], w["val"])))
                    known.pop(w["idx"], None)
                    if w["const"] is not None and not w["flag"]:
                        known[w["idx"]] = (w["const"], 0)
                else:
                    e("    " + guarded(w["flag"], "c->api->ctrl_write(c, %d, %s);" % (w["idx"], w["val"])))
            tests = []
            if pm is not None:
                hits = [w for w in st.imm if w["kind"] == WK_REG and (pm.xmask >> w["idx"]) & 1]
                if any(not w["flag"] for w in hits):
                    tests.append("1")
                tests += [w["flag"] for w in hits if w["flag"]]
            if boundary and phase == "L" and not early3:
                tests.append("c->cr[CR_ILC] == 0")        # ends before the steady state
            if boundary and (phase == "S" or idx >= k.dynlen - 1):
                tests.append("(c->ifr && pending_interrupt(c))")   # may drain for an interrupt
            if tests:
                e("    if (__builtin_expect(%s, 0)) { c->jit_exit[%d]++; goto P%d; }"
                  % (" || ".join(tests), EXIT_STUB, label))
                post_used = True
        ring = [dict(w) for w in st.ring if (root or w["land"] > 0)]
        new_ring, new_imm = [], []
        dummy = Rec()
        dummy.next = 0
        h_load, h_store = handler_number("H_LOAD"), handler_number("H_STORE")
        pm_ins = [ins for ins in (pm.insns if pm else []) if ins.name not in LOOP_SKIP]
        allins = list(buf) + pm_ins
        immediate = (all(ins.handler != h_load for ins in allins) and any(ins.handler == h_store for ins in allins))
        if st.mcnop:
            e("    c->mcnop = %d;" % (st.mcnop - 1))
        if pm is not None:
            e("    c->exec_pc = %s; c->pc = %s;" % (u32(pm.pc), u32(pm.next)))
        if immediate:
            e("    c->store_now = 1;")
        for ins in buf:
            self.deps.add(ins.addr & ~31)
            self.insn(ins, dummy, new_ring, new_imm, [], known)
        for ins in pm_ins:
            self.deps.add(ins.addr & ~31)
            self.insn(ins, pm, new_ring, new_imm, [], known)
        if pm is not None:
            for ins in pm.insns:
                self.deps.add(ins.addr & ~31)
        if immediate:
            e("    c->store_now = 0;")
        e("    if (c->npst) c->api->flush_stores(c);")
        if pm is not None:
            e("    c->branch_block = %s;" % ("5" if pm.branched else "c->branch_block ? c->branch_block - 1 : 0"))
            if pm.xnops:
                e("    if (c->mcnop < %d) c->mcnop = %d;" % (pm.xnops, pm.xnops))
        for w in ring:
            w["land"] -= 1
        ring = ring + new_ring
        pre = max(st.pre - 1, 0)
        kn = self.trim(known)
        if phase == "L":
            if idx == k.dynlen - 1:
                e("    c->spl.loading = 0; c->spl.dynlen = %d; c->spl.fetch_delay = %d; c->spl.kernel_cycle = c->cycle;"
                  % (k.dynlen, self.fd))
            if boundary:
                e("    %sc->spl.last_iter = %d;" % ("" if early3 else "c->cr[CR_ILC]--; ", (idx + 1) // ii))
            e("    c->cycle++;")
            if idx + 1 < len(self.cycles):
                nc = self.cycles[idx + 1]
                self.transition(State(("L", idx + 1), nc["mc"], [], ring, new_imm, pre, kn))
            else:
                self.transition(State(("S", 0), 0, [], ring, new_imm, pre, kn))
        elif phase == "S":
            if idx == ii - 1:
                e("    if (__builtin_expect(c->cr[CR_ILC] != 0, 1)) {")
                e("        c->cr[CR_ILC]--; c->spl.last_iter = (int)((c->cycle - c->spl.t0 + 1) / %d);" % ii)
                e("        c->cycle++;")
                self.transition(State(("S", 0), 0, [], ring, new_imm, pre, kn), "        ")
                e("    } else {")
                e("        c->spl.terminated = 1; c->spl.drain_start = c->cycle + 1;")
                e("        { uint64_t en_ = c->spl.drain_start + %d; if (en_ > c->spl.kernel_cycle + 1) c->pm_resume_at = en_ + %d; }"
                  % (self.delay, PM_REFILL))
                e("        c->cycle++;")
                self.transition(State(("D", 0), 0, [], ring, new_imm, pre, kn), "        ")
                e("    }")
            else:
                e("    c->cycle++;")
                self.transition(State(("S", idx + 1), 0, [], ring, new_imm, pre, kn))
        else:
            if idx == self.epilog - 1:
                e("    c->spl.active = 0; c->cr[CR_TSR] &= ~TSR_SPLX;")
            e("    c->cycle++;")
            nxt = State(("D", idx + 1), 0, [], ring, new_imm, pre, kn)
            if idx + 1 < self.je:
                self.transition(nxt)
            else:
                self.exit_now(nxt, EXIT_STUB, (k.addr, "loop hands back"))
        if not root:
            hot, self.body = self.body, self.cold
            self.emit("X%d: ;" % label)
            self.put_back(st, "0", "    ")
            self.emit("    return;")
            if post_used:
                self.emit("P%d: ;" % label)
                bits = [("(%s ? %s : 0)" % (w["flag"], "0x%xull" % (1 << w["idx"])) if w["flag"] else "0x%xull" % (1 << w["idx"]))
                        for w in st.imm if w["kind"] == WK_REG]
                self.emit("    c->wmask = %s; c->jit_committed = 1;" % (" | ".join(bits) if bits else "0"))
                self.put_back(st, "0", "    ", ring_after_commit=True)
                self.emit("    return;")
            self.body = hot

    def root_cycle(self):
        """Cycle -1: the SPLOOP execute packet, entered after its commit."""
        e = self.emit
        k, pk0 = self.k, self.pk0
        e("    if (c->cr[CR_TSR] & TSR_SPLX) return;")
        if k.kind == self.kind_sploop:
            e("    if (c->cr[CR_ILC] == 0) return;           /* the interpreter's initial_term path */")
            e("    c->cr[CR_ILC]--;")
        e("    c->exec_pc = %s; c->pc = %s;" % (u32(pk0.pc), u32(pk0.next)))
        e("    {")
        e("        spl_state *s = &c->spl;")
        e("        s->active = 1; s->kind = %d; s->ii = %d; s->creg = -1; s->cz = %d; s->addr = %s;"
          % (k.kind, k.ii, k.cz, u32(k.addr)))
        e("        s->t0 = c->cycle + 1; s->loading = 1; s->dynlen = 0; s->fetch_delay = 0; s->last_iter = 0;")
        e("        s->terminated = 0; s->int_drain = 0; s->abrupt = 0; s->initial_term = 0; s->resumed = 0;")
        e("        s->drain_start = 0; s->kernel_cycle = 0; s->pos_cycle = 0; s->pos_t0 = 0; s->pos_k = 0;")
        e("        s->pos_off = 0; s->pos_ii = 0; s->steady_built = 0; s->kr_checked = 0; s->kregion = NULL;")
        for r, rec in enumerate(self.body_addrs):
            parts = ["s->body[%d].n = %d;" % (r, len(rec))]
            for q, a in enumerate(rec):
                parts.append("s->body[%d].insn[%d] = %s[%d];" % (r, q, self.ins_name, len(self.captured)))
                self.captured.append(a)
            e("        " + " ".join(parts))
        e("    }")
        e("    c->cr[CR_TSR] |= TSR_SPLX; c->st.sploops++;")
        new_ring, new_imm = [], []
        known = {}
        pm_ins = [ins for ins in pk0.insns if ins.name not in LOOP_SKIP]
        h_load, h_store = handler_number("H_LOAD"), handler_number("H_STORE")
        immediate = (all(ins.handler != h_load for ins in pm_ins) and any(ins.handler == h_store for ins in pm_ins))
        if immediate:
            e("    c->store_now = 1;")
        for ins in pm_ins:
            self.insn(ins, pk0, new_ring, new_imm, [], known)
        for ins in pk0.insns:
            self.deps.add(ins.addr & ~31)
        if immediate:
            e("    c->store_now = 0;")
        e("    if (c->npst) c->api->flush_stores(c);")
        e("    c->branch_block = %s;" % ("5" if pk0.branched else "c->branch_block ? c->branch_block - 1 : 0"))
        if pk0.xnops:
            e("    c->mcnop = %d;" % pk0.xnops)
        e("    c->cycle++;")
        self.transition(State(("L", 0), self.cycles[0]["mc"], [], new_ring, new_imm, PRE_CYCLES - 1, self.trim(known)))

    def generate(self):
        self.root_cycle()
        done = set()
        while self.work:
            label, st = self.work.pop()
            if label in done:
                continue
            done.add(label)
            self.cycle(label, st)
        decl = ["void %s(c66x_core *c, uint64_t end)" % self.name, "{"]
        decl.append("    uint64_t gen = c->code_gen;")
        self.declare(decl)
        return "\n".join(decl + self.body + self.cold + ["}"]), len(self.labels)


# One generated function per FN_NODES states, and the compiled control flow
# crosses between them freely: the DSP's hot code is a loop whose body spans two
# of them, so a run hops back and forth for as long as the cycle budget lasts.
# Each jit_fn reserves kilobytes of frame for its spilled state, so letting them
# call one another -- even in tail position, which no compiler here turns into a
# jump at these sizes -- grows the stack by about 16 KB per hop. QEMU runs the
# DSP on a thread of its own, and macOS gives that 512 KB: some 30 hops in, the
# stack hits its guard page, and the fault leaves the DSP's run_lock held, which
# takes the BQL and the whole deck down with it. c6xreplay never saw it because
# it steps the core from main(), on an 8 MB stack.
#
# So a hop is not a call. The function records where to go and returns, and
# jit_run enters the next one from the entry wrapper's frame, leaving exactly
# one jit_fn frame on the stack however far the compiled code travels.
HOP_RUN = r'''_Thread_local int jit_hop_fn = -1;
_Thread_local unsigned jit_hop_entry;

static void jit_run(c66x_core *c, uint64_t end, int fn, unsigned entry, uint64_t gen)
{
    for (;;) {
        jit_hop_fn = -1;
        jit_fns[fn](c, end, entry, gen);
        if (jit_hop_fn < 0)
            return;
        fn = jit_hop_fn;
        entry = jit_hop_entry;
    }
}'''

PRELUDE = r'''/* Generated by qemu/c6x/tools/c14_jitgen.py -- do not edit. */
#include <fenv.h>
#include <stdlib.h>
#include <string.h>
#include "c66x_core_int.h"

/* A jump from one generated function into another leaves the target here and
 * returns, for jit_run to enter; jit_hop_fn is -1 when the function is done.
 * The generated functions are far too large to call one another directly. */
extern _Thread_local int jit_hop_fn;
extern _Thread_local unsigned jit_hop_entry;

static const int jit_fe_mode[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };

/* fop_run's F_ADDSP/F_SUBSP/F_MPYSP */
static inline uint32_t jit_fop(c66x_core *c, int unit, int side, uint32_t a, uint32_t b, char op)
{
    uint32_t r = (unit >= 6) ? c->cr[CR_FMCR] : c->cr[CR_FADCR];
    unsigned rm = side == 2 ? (r >> 25) & 3 : (r >> 9) & 3;
    if (rm) fesetround(jit_fe_mode[rm]);
    float p, q, res;
    memcpy(&p, &a, 4);
    memcpy(&q, &b, 4);
    res = op == '+' ? p + q : op == '-' ? p - q : p * q;
    if (rm) fesetround(FE_TONEAREST);
    uint32_t v;
    memcpy(&v, &res, 4);
    return v;
}

static inline float jit_u2f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }

/* exec_insn's H_DADDSP/H_DSUBSP/H_DMPYSP: the operation on each half of a pair */
static inline uint64_t jit_dsp(c66x_core *c, int unit, int side, uint64_t a, uint64_t b, char op)
{
    uint32_t r = (unit >= 6) ? c->cr[CR_FMCR] : c->cr[CR_FADCR];
    unsigned rm = side == 2 ? (r >> 25) & 3 : (r >> 9) & 3;
    if (rm) fesetround(jit_fe_mode[rm]);
    uint32_t o[2];
    for (int k = 0; k < 2; k++) {
        float p = jit_u2f((uint32_t)(a >> (32 * k))), q = jit_u2f((uint32_t)(b >> (32 * k)));
        float res = op == '+' ? p + q : op == '-' ? p - q : p * q;
        memcpy(&o[k], &res, 4);
    }
    if (rm) fesetround(FE_TONEAREST);
    return ((uint64_t)o[1] << 32) | o[0];
}

/* fop_run's F_LOAD inline RAM read */
static inline uint32_t jit_load(c66x_core *c, uint32_t ea, unsigned n)
{
    ramreg *rr = &c->ram[c->last_ram];
    if ((!c->idle_armed || c->isr_depth || c->idle_fx)
        && ea - rr->base <= rr->size - n && rr->size >= n) {
        const uint8_t *m = rr->host + (ea - rr->base);
        return n == 4 ? m[0] | (m[1] << 8) | (m[2] << 16) | ((uint32_t)m[3] << 24)
             : n == 2 ? (uint32_t)(m[0] | (m[1] << 8)) : m[0];
    }
    return c->api->mem_read(c, ea, n);
}

/* fop_run's F_STORE inline RAM store */
static inline void jit_store(c66x_core *c, uint32_t ea, uint32_t v, unsigned n)
{
    ramreg *rr = &c->ram[c->last_ram];
    if (c->store_now && !c->watch_fn && (!c->idle_armed || c->idle_fx)
        && ea - rr->base <= rr->size - n && rr->size >= n) {
        uint8_t *m = rr->host + (ea - rr->base);
        m[0] = v;
        if (n > 1) m[1] = v >> 8;
        if (n > 2) { m[2] = v >> 16; m[3] = v >> 24; }
        if (rr->codepage[(ea - rr->base) >> FP_PAGE_SHIFT])
            c->api->invalidate_code(c, ea, n);
        return;
    }
    c->api->store_defer(c, ea, v, n);
}

/* A captured handler wrote a different shape than the generator saw: the
 * static state is wrong from here, so nothing after it can be trusted. */
static void jit_fatal(c66x_core *c, uint32_t addr, unsigned got, unsigned want)
{
    fprintf(stderr, "c66x jit: capture at 0x%08x wrote %u times, compiled for %u (cycle %llu)\n",
            addr, got, want, (unsigned long long)c->cycle);
    abort();
}
'''


REFRESH = r'''
/* The captured instructions' decoded forms, for this core and code generation. */
static void jit_refresh(c66x_core *c)
{
    static c66x_core *core;
    static uint64_t gen = ~0ull;
    if (core == c && gen == c->code_gen)
        return;
    for (unsigned i = 0; i < %d; i++)
        jit_ins[i] = c->api->insn_at(c, jit_ins_addr[i]);
    core = c;
    gen = c->code_gen;
}
'''


SITES_DUMP = r'''
__attribute__((destructor)) static void jit_sites_dump(void)
{
    const char *path = getenv("C66X_JIT_SITES");
    FILE *f = path ? fopen(path, "a") : NULL;
    if (!f)
        return;
    for (unsigned i = 0; i < %d; i++)
        if (jit_site_n[i])
            fprintf(f, "0x%%08x %%llu %%s\n", jit_site_pc[i], (unsigned long long)jit_site_n[i], jit_site_why[i]);
    fclose(f);
}
'''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("prof")
    ap.add_argument("out")
    ap.add_argument("--roots", type=int, default=300)
    ap.add_argument("--min", type=int, default=100000, help="minimum root entries (no branch in flight)")
    ap.add_argument("--nodes", type=int, default=400, help="new states one root may add")
    ap.add_argument("--total", type=int, default=400000, help="states in the whole module")
    ap.add_argument("--lib", default=os.path.expanduser("~/build/c6x/libc66x.so"))
    ap.add_argument("--only", type=lambda s: int(s, 0), action="append", help="compile just these roots")
    ap.add_argument("--only-file", dest="only_file", action="append",
                    help="--only for every address in this file (one per line); a curated "
                         "module's roots plus a new workload's, without re-ranking either")
    ap.add_argument("--no-cc", action="store_true")
    # 16-17 gcc at once (~0.8 GB each) beside a running deck hung the WSL VM (c16)
    ap.add_argument("--jobs", type=int, default=min(6, os.cpu_count() or 4), help="parallel gcc processes")
    ap.add_argument("--opt", default="2", help="gcc optimisation level")
    ap.add_argument("--cflags", default="", help="extra gcc flags for the module (c16: code size under L3 contention)")
    ap.add_argument("--kernels", type=int, default=64, help="most SPLOOP kernels to compile")
    ap.add_argument("--fn-nodes", dest="fn_nodes", type=int, default=FN_NODES, help="states per generated C function")
    ap.add_argument("--clean-prof", dest="clean_prof", default=None,
                    help="profile dir (taken with a module loaded) whose free interpreted packets become clean-only roots")
    ap.add_argument("--clean-roots", dest="clean_roots", type=int, default=0, help="most clean-only roots")
    # Compiled drain cycles are exact but cost more than they save: 25.9 M entries
    # for 134 M cycles (5 cycles an entry) made the replay 104.8 vs 123.7 M/s.
    ap.add_argument("--drain", dest="drain", action="store_true",
                    help="compile each kernel's drain cycles too (ABI 10; measured slower)")
    ap.add_argument("--clean-min", dest="clean_min", type=int, default=0,
                    help="minimum free entries for a clean-only root (default: --min)")
    ap.add_argument("--no-clean", dest="clean", action="store_false",
                    help="do not export clean states as entries (ABI 9 regions0 left empty)")
    ap.add_argument("--cold", type=int, default=COLD, help="profile count below which a state exits to the interpreter")
    ap.add_argument("--kmin", type=int, default=1000000, help="minimum kernel cycles")
    ap.add_argument("--rank-total", dest="rank_total", action="store_true",
                    help="rank roots by run count (with at least --free-min free entries) instead of free entries")
    ap.add_argument("--free-min", dest="free_min", type=int, default=2000,
                    help="free entries a --rank-total or --loop-heads root needs")
    ap.add_argument("--qroots", type=int, default=0,
                    help="roots entered with one branch in flight, one per branch target (c16, ABI 12)")
    ap.add_argument("--qmin", type=int, default=100000, help="minimum interpreted runs for a queued-branch root")
    ap.add_argument("--qprof", default=None, help="profile dir whose qprof lines pick --qroots (default --clean-prof)")
    ap.add_argument("--loop-heads", dest="loop_heads", type=int, default=0,
                    help="extra roots at the heads of loops run with their back-branch in flight (qprof targets)")
    ap.add_argument("--loops", type=int, default=0,
                    help="most whole SPLOOP invocations to compile (c16, ABI 11; 0 = none)")
    ap.add_argument("--lmin", type=int, default=100000, help="minimum profiled kernel cycles for a whole-loop compile")
    ap.add_argument("--lnodes", type=int, default=2000, help="states one loop function may have")
    ap.add_argument("--loop-only", dest="loop_only", type=lambda s: int(s, 0), action="append",
                    help="compile just these loops (SPLOOP addresses)")
    ap.add_argument("--kind-w", dest="kind_w", type=int, default=-1,
                    help="handler number of SPLOOPW (default: read from c66x_core.c)")
    ap.add_argument("--census", type=int, default=0, help="print the top N uncompilable reasons and stop")
    ap.add_argument("--idle-head", dest="idle_head", type=lambda x: int(x, 0), default=None,
                    help="the busy-wait head regions stop at (the machine's CDJ_C6X_IDLE, 0x80076F00)")
    a = ap.parse_args()
    for path in a.only_file or []:
        a.only = (a.only or []) + [int(x, 0) for x in open(path).read().split()]
    if a.only:
        a.only = set(a.only)

    dec = Decoder(a.prof, a.lib)
    if a.idle_head is not None:
        dec.idle_head = a.idle_head
    if a.kind_w < 0:
        a.kind_w = handler_number("H_SPLOOPW")
    cand = sorted(dec.prof, key=lambda p: -p[2])
    if a.rank_total:
        # Ranking every packet by run count roots the insides of hot loops too and
        # doubles the code: 104 vs 163 M cycles/s (c16). Kept for the record.
        cand = sorted([p for p in dec.prof if p[2] >= a.free_min], key=lambda p: -p[1])
    if a.census:
        by = {}
        total = sum(p[1] for p in cand)
        for pc, n, nfree in cand:
            r = compilable(dec, dec.packet(pc)) or "compilable"
            by[r] = by.get(r, 0) + n
        for r, n in sorted(by.items(), key=lambda x: -x[1])[:a.census]:
            print("%-50s %12d %5.1f%%" % (r, n, 100.0 * n / total))
        return
    roots, why, examined = [], {}, []
    for pc, n, nfree in cand:
        if a.only and pc not in a.only:
            continue
        if not a.only and ((n if a.rank_total else nfree) < a.min or len(roots) >= a.roots):
            break
        if pc in dec.have:
            continue
        r = compilable(dec, dec.packet(pc))
        if r:
            why[r] = why.get(r, 0) + nfree
            examined.append(pc)
            continue
        roots.append(pc)
        examined.append(pc)
    print("roots %d; uncompilable root cycles by reason: %s" % (len(roots), why), file=sys.stderr)
    # C66X_JIT_AUTO offers a packet again unless it is listed here: --roots cuts the
    # candidates, and the ones past the cut must come back in a later batch.
    with open(a.out + ".tried", "w") as f:
        f.write("".join("0x%08x\n" % pc for pc in examined))

    if a.loop_heads:
        # A loop head entered free once per invocation and then run with its
        # back-branch in flight is worth its run count, not its free count: the
        # interpreter enters 0x8006CDE0 free 11 k times and runs it 6.5 M times
        # (c16). Heads are the targets of the profile's queued branches.
        qsrc = a.clean_prof or a.prof.split(",")[0]
        heads = {}
        for line in open(os.path.join(qsrc, "profile.txt")):
            t = line.split()
            if t[0] == "qprof":
                heads[int(t[3], 16)] = heads.get(int(t[3], 16), 0) + int(t[4])
        prof_by = {pc: (n, nfree) for pc, n, nfree in dec.prof}
        extra = 0
        for pc, q in sorted(heads.items(), key=lambda x: -x[1]):
            if extra >= a.loop_heads or q < a.min:
                break
            n, nfree = prof_by.get(pc, (0, 0))
            if pc in roots or nfree < a.free_min or pc in dec.have or compilable(dec, dec.packet(pc)):
                continue
            roots.append(pc)
            extra += 1
        print("loop-head roots %d" % extra, file=sys.stderr)

    sites, table, nodes, codes, extern = [], [], 0, [], []
    mg = ModuleGen(dec, sites, a.nodes, a.total)
    mg.cold_min = a.cold
    mg.fn_nodes = a.fn_nodes
    mg.clean = a.clean
    for pc in roots:
        mg.add_root(pc)
    if a.clean and a.clean_prof and a.clean_roots:
        extra = []
        for line in open(os.path.join(a.clean_prof, "profile.txt")):
            t = line.split()
            if t[0] == "prof":
                extra.append((int(t[3]), int(t[1], 16)))
        extra.sort(reverse=True)
        taken = 0
        for nfree, pc in extra:
            if taken >= a.clean_roots or nfree < (a.clean_min or a.min):
                break
            if pc in roots or pc == dec.idle_head or dec.packet(pc) is None or compilable(dec, dec.packet(pc)):
                continue
            mg.add_clean_root(pc)
            taken += 1
        print("clean-only roots %d" % taken, file=sys.stderr)
    if a.qroots:
        # Packets the interpreter ran with one branch in flight, one root per
        # branch target (a loop's packets all share it), the most-run first.
        qdir = a.qprof or a.clean_prof or a.prof.split(",")[0]
        qs = []
        for line in open(os.path.join(qdir, "profile.txt")):
            t = line.split()
            if t[0] == "qprof":
                qs.append((int(t[4]), int(t[1], 16), int(t[2]), int(t[3], 16)))
        qs.sort(reverse=True)
        by_target, qtaken = {}, 0
        for cnt, pc, rem, target in qs:
            if qtaken >= a.qroots or cnt < a.qmin:
                break
            if target in by_target or pc == dec.idle_head or compilable(dec, dec.packet(pc)):
                continue
            by_target[target] = pc
            mg.add_qroot(pc, rem, target)
            qtaken += 1
        print("queued-branch roots %d" % qtaken, file=sys.stderr)
    fsrcs, nfn = mg.functions()
    nodes += len(mg.labels)
    codes += fsrcs
    extern.append("\n".join("void jit_fn%d(c66x_core *c, uint64_t end, unsigned entry, uint64_t gen);" % f
                            for f in range(nfn)))
    extern.append("static void (*const jit_fns[])(c66x_core *, uint64_t, unsigned, uint64_t) = { %s };"
                  % (", ".join("jit_fn%d" % f for f in range(nfn)) or "0"))
    extern.append(HOP_RUN)
    extern.append("\n".join("extern const uint32_t jit_da%d[]; extern const uint8_t jit_db%d[]; "
                            "extern c66x_jit_verified jit_vf%d;" % (f, f, f) for f in range(nfn)))
    extern.append("c66x_insn *jit_ins[%d];" % max(len(mg.captured), 1))
    extern.append("static const uint32_t jit_ins_addr[] = { %s };" % (", ".join(u32(x) for x in mg.captured) or "0"))
    extern.append(REFRESH % len(mg.captured))
    for pc in roots:
        if pc not in mg.entries:
            continue
        lab = mg.entries[pc]
        f = lab // mg.fn_nodes
        name = "root_%08x" % pc
        extern.append("static void %s(c66x_core *c, uint64_t end) { jit_refresh(c); jit_run(c, end, %d, %d, c->code_gen); }"
                      % (name, f, lab))
        table.append("    { %s, %s, %s, %d, jit_da%d, jit_db%d, &jit_vf%d }," % (
            u32(pc), u32(dec.idle_head), name, len(mg.fn_deps[f]), f, f, f))
    table0 = []
    for pc, lab in sorted(mg.clean_entries.items()):
        f = lab // mg.fn_nodes
        name = "clean_%08x" % pc
        extern.append("static void %s(c66x_core *c, uint64_t end) { jit_refresh(c); jit_run(c, end, %d, %d, c->code_gen); }"
                      % (name, f, lab))
        table0.append("    { %s, %s, %s, %d, jit_da%d, jit_db%d, &jit_vf%d }," % (
            u32(pc), u32(dec.idle_head), name, len(mg.fn_deps[f]), f, f, f))
    qtable = []
    for pc, (lab, rem, target) in sorted(mg.qentries.items()):
        f = lab // mg.fn_nodes
        name = "qroot_%08x" % pc
        extern.append("static void %s(c66x_core *c, uint64_t end) { jit_refresh(c); jit_run(c, end, %d, %d, c->code_gen); }"
                      % (name, f, lab))
        qtable.append("    { { %s, %s, %s, %d, jit_da%d, jit_db%d, &jit_vf%d }, %d, %s }," % (
            u32(pc), u32(dec.idle_head), name, len(mg.fn_deps[f]), f, f, f, rem, u32(target)))
    ktable, kextern = [], []
    kind_w = a.kind_w
    for uid, k in enumerate(sorted(dec.kerns, key=lambda k: -k.cycles)):
        if k.cycles < a.kmin or len(ktable) >= a.kernels:
            break
        why_k = kernel_compilable(dec, k)
        if why_k:
            print("kernel 0x%08x (%d cycles) not compiled: %s" % (k.addr, k.cycles, why_k), file=sys.stderr)
            continue
        kg = KernelGen(dec, k, a.nodes, uid, sites, kind_w)
        code, nn = kg.generate()
        nodes += nn
        codes.append(code)
        dname = "0"
        if a.drain and k.dynlen > k.ii:
            dg = KernelGen(dec, k, a.nodes, uid, sites, kind_w, drain=True)
            dcode, dnn = dg.generate()
            nodes += dnn
            codes.append(dcode)
            dname = dg.name
            kextern.append("void %s(c66x_core *c, uint64_t end);" % dname)
        deps = sorted(kg.deps)
        n = kg.name
        kextern.append("void %s(c66x_core *c, uint64_t end);" % n)
        kextern.append("static const uint32_t %s_da[] = { %s };" % (n, ", ".join(u32(d) for d in deps)))
        blob = b"".join(dec.bytes32(d) or b"\0" * 32 for d in deps)
        kextern.append("static const uint8_t %s_db[] = { %s };" % (n, ", ".join(str(x) for x in blob)))
        kextern.append("static const uint8_t %s_bn[] = { %s };" % (n, ", ".join(str(len(p)) for p in k.body)))
        kextern.append("static const uint32_t %s_ba[] = { %s };" % (n, ", ".join(u32(x) for p in k.body for x in p) or "0"))
        ktable.append("    { %s, %d, %d, %d, %d, %d, %s_bn, %s_ba, %s, %d, %s_da, %s_db, %s }," % (
            u32(k.addr), k.kind, k.ii, k.dynlen, k.creg, k.cz, n, n, n, len(deps), n, n, dname))
    extern += kextern
    ltable = []
    if a.loops:
        kind_sp = handler_number("H_SPLOOP")
        seen = set()
        for uid, k in enumerate(sorted(dec.kerns, key=lambda k: -k.cycles)):
            if k.cycles < a.lmin or len(ltable) >= a.loops:
                break
            ins0 = dec.insn(k.addr)
            if a.loop_only and k.addr not in a.loop_only:
                continue
            if k.addr in seen or ins0 is None or ins0.handler != k.kind or kernel_compilable(dec, k):
                continue
            lg = LoopGen(dec, k, a.lnodes, 5000 + uid, sites, kind_w, kind_sp)
            if lg.why:
                print("loop 0x%08x (%d cycles) not compiled: %s" % (k.addr, k.cycles, lg.why), file=sys.stderr)
                continue
            seen.add(k.addr)
            code, nn = lg.generate()
            nodes += nn
            codes.append(code)
            n = lg.name
            deps = sorted(lg.deps)
            extern.append("void %s(c66x_core *c, uint64_t end);" % n)
            extern.append("static const uint32_t %s_da[] = { %s };" % (n, ", ".join(u32(d) for d in deps)))
            blob = b"".join(dec.bytes32(d) or b"\0" * 32 for d in deps)
            extern.append("static const uint8_t %s_db[] = { %s };" % (n, ", ".join(str(x) for x in blob)))
            ltable.append("    { %s, %s, %d, %s_da, %s_db }," % (u32(k.addr), n, len(deps), n, n))
            print("loop 0x%08x: %d states, %d loading/ramp cycles, drain %d" % (k.addr, nn, len(lg.cycles), lg.je),
                  file=sys.stderr)
    nsites = max(len(sites), 1)
    # the regions, in chunks gcc builds in parallel
    nchunks = max(1, min(a.jobs, len(codes)))
    chunks = [[] for _ in range(nchunks)]
    sizes = [0] * nchunks
    for code in sorted(codes, key=len, reverse=True):
        i = sizes.index(min(sizes))
        chunks[i].append(code)
        sizes[i] += len(code)
    srcs = []
    for i, ch in enumerate(chunks):
        path = "%s.part%d.c" % (a.out, i)
        open(path, "w").write(PRELUDE + "\nextern uint64_t jit_site_n[%d];\nextern c66x_insn *jit_ins[];\n" % nsites
                              + extern[0] + "\n\n" + "\n\n".join(ch) + "\n")
        srcs.append(path)
    main_parts = [PRELUDE, "uint64_t jit_site_n[%d];" % nsites] + extern
    main_parts.append("static const c66x_jit_region regions[] = {\n%s\n};" % "\n".join(table or ["    { 0 }"]))
    main_parts.append("static const c66x_jit_kernel kernels[] = {\n%s\n};" % "\n".join(ktable or ["    { 0 }"]))
    main_parts.append("static const c66x_jit_region regions0[] = {\n%s\n};" % "\n".join(table0 or ["    { 0 }"]))
    main_parts.append("static const c66x_jit_loop loops[] = {\n%s\n};" % "\n".join(ltable or ["    { 0 }"]))
    main_parts.append("static const c66x_jit_qregion qregions[] = {\n%s\n};" % "\n".join(qtable or ["    { { 0 } }"]))
    # C66X_JIT_SITES=<file>: the counted exit sites, written when the module unloads
    main_parts.append("static const uint32_t jit_site_pc[] = { %s };" % ", ".join(u32(p) for p, w in sites or [(0, "")]))
    main_parts.append("static const char *const jit_site_why[] = { %s };"
                      % ", ".join('"%s"' % w for p, w in sites or [(0, "")]))
    main_parts.append(SITES_DUMP % nsites)
    main_parts.append('__attribute__((visibility("default"))) '
                      'const c66x_jit_module c66x_jit_exports = { C66X_JIT_ABI, %d, regions, %d, kernels, %d, regions0, '
                      '%d, loops, %d, qregions };' % (len(table), len(ktable), len(table0), len(ltable), len(qtable)))
    path = a.out + ".main.c"
    open(path, "w").write("\n\n".join(main_parts) + "\n")
    srcs.append(path)
    print("%d regions, %d clean entries, %d kernels, %d states, %d MB of C in %d files" % (len(table), len(table0), len(ktable), nodes,
                                                                        sum(sizes) >> 20, len(srcs)),
          file=sys.stderr)
    if a.no_cc:
        return
    flags = ["-O%s" % a.opt, "-std=gnu11", "-fPIC", "-fvisibility=hidden", "-ffp-contract=off",
             "-frounding-math", "-march=native", "-w", "-I", CORE_DIR] + a.cflags.split()
    procs = [subprocess.Popen(["gcc"] + flags + ["-c", src, "-o", src[:-2] + ".o"]) for src in srcs]
    if any(p.wait() for p in procs):
        sys.exit("gcc failed")
    subprocess.check_call(["gcc", "-shared", "-o", a.out + ".so"] + a.cflags.split()
                          + [src[:-2] + ".o" for src in srcs])
    print("built %s.so" % a.out, file=sys.stderr)


if __name__ == "__main__":
    main()
