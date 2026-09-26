# SPDX-License-Identifier: GPL-2.0-or-later
"""c14_jitgen cuts the compiled DSP code into jit_fn0..N, and control flow
crosses freely between them. Each of those functions reserves kilobytes of
frame for its spilled state, so one may never *call* another: written as a tail
call it is still compiled as a real one at these sizes, and a run that hops
back and forth for a whole cycle budget then walks off the end of the stack.
QEMU steps the DSP on a thread with 512 KB on macOS -- about 30 hops -- and the
fault leaves the DSP's run_lock held, which freezes the whole deck.

A crossing must instead leave its target in jit_hop_fn/jit_hop_entry and
return, for jit_run to enter from the entry wrapper's frame."""
import sys
import types

import pytest

from helpers import path

sys.path.insert(0, path("hw", "cdj", "c6x", "tools"))
c14_jitgen = pytest.importorskip("c14_jitgen")


class Positional:
    """A state naming no live values, so a crossing saves nothing to c->jit_xs."""
    ring = ()
    imm = ()
    queue = ()


def module_gen(code):
    """A ModuleGen holding `code` (label -> lines) and nothing else, built
    without __init__ so no decoder, profile or firmware is needed."""
    mg = object.__new__(c14_jitgen.ModuleGen)
    mg.fn_nodes = 1                      # one state per function, so 0 and 1 are apart
    mg.clean = False
    mg.clean_entries = {}
    mg.entry_labels = {0}
    mg.labels = {("k%d" % l,): l for l in code}
    mg.code = code
    mg.cold_of = {l: [] for l in code}
    mg.node_deps = {l: set() for l in code}
    mg.pos_of = {l: Positional() for l in code}
    mg.fn_deps = []
    mg.dec = types.SimpleNamespace(idle_head=0, bytes32=lambda addr: b"\0" * 32)
    return mg


def test_a_jump_into_another_function_is_a_hop_not_a_call():
    mg = module_gen({0: ["    \x011\x01"], 1: ["    /* landed */"]})
    srcs, nfn = mg.functions()
    assert nfn == 2
    assert "jit_hop_fn = 1;" in srcs[0]
    assert "jit_hop_entry = 1;" in srcs[0]
    assert "jit_fn1(" not in srcs[0], "jit_fn0 calls jit_fn1 instead of hopping"


def test_a_jump_inside_one_function_stays_a_goto():
    mg = module_gen({0: ["    \x010\x01"]})
    srcs, nfn = mg.functions()
    assert nfn == 1
    assert "goto L0;" in srcs[0]
    assert "jit_hop_fn" not in srcs[0]


def test_no_generated_function_calls_another():
    """Whatever the shape of the graph, jit_run is the only caller of a jit_fn."""
    mg = module_gen({l: ["    \x01%d\x01" % ((l + 1) % 4)] for l in range(4)})
    srcs, nfn = mg.functions()
    assert nfn == 4
    for f, src in enumerate(srcs):
        for other in range(nfn):
            assert "jit_fn%d(c, end" % other not in src, "jit_fn%d calls jit_fn%d" % (f, other)
    assert "jit_fns[fn](c, end, entry, gen);" in c14_jitgen.HOP_RUN
