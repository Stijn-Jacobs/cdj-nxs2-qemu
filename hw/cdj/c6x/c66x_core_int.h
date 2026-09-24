/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x core internals shared by c66x_core.c and the compiled regions that
 * the block compiler (tools/c14_jitgen.py) generates: the core struct and the
 * few constants a generated region reads. A region is built against this exact
 * header; C66X_JIT_ABI changes whenever the struct does, and the loader refuses
 * a region built for another ABI.
 */
#ifndef C66X_CORE_INT_H
#define C66X_CORE_INT_H

#include <stdio.h>

#include "c66x.h"
#include "c66x_decode.h"

#define C66X_JIT_ABI 12

enum {
    CR_AMR = 0x0, CR_CSR = 0x1, CR_IFR = 0x2, CR_ICR = 0x3, CR_IER = 0x4,
    CR_ISTP = 0x5, CR_IRP = 0x6, CR_NRP = 0x7, CR_TSCL = 0xa, CR_TSCH = 0xb,
    CR_ILC = 0xd, CR_RILC = 0xe, CR_REP = 0xf, CR_PCE1 = 0x10, CR_DNUM = 0x11,
    CR_FADCR = 0x12, CR_FAUCR = 0x13, CR_FMCR = 0x14, CR_SSR = 0x15,
    CR_GPLYA = 0x16, CR_GPLYB = 0x17, CR_GFPGFR = 0x18, CR_TSR = 0x1a,
    CR_ITSR = 0x1b, CR_NTSR = 0x1c, CR_EFR = 0x1d, CR_IERR = 0x1f,
};

#define CSR_GIE   (1u << 0)
#define CSR_PGIE  (1u << 1)
#define CSR_SAT   (1u << 9)
#define TSR_GIE   (1u << 0)
#define TSR_SGIE  (1u << 1)
#define TSR_XEN   (1u << 3)
#define TSR_CXM   (3u << 6)
#define TSR_INT   (1u << 9)
#define TSR_EXC   (1u << 10)
#define TSR_SPLX  (1u << 14)
#define IER_NMIE  (1u << 1)

/* Interrupt entry: SPRU732 Fig. 5-6, no new instruction enters E1 for 9 cycles. */
#define INT_ENTRY_CYCLES 9

typedef struct c66x_jit_region c66x_jit_region;

/* Writes an instruction scheduled, captured instead of queued: a region runs
 * the interpreter's own handler for instructions it has no native form for and
 * lands the writes itself (tools/c14_jitgen.py knows their shape statically). */
#define C66X_JIT_CAP 16
typedef struct c66x_jit_cap {
    unsigned n;
    struct { uint8_t delay, kind, idx; uint32_t val; } e[C66X_JIT_CAP];
} c66x_jit_cap;

#define WQ_SLOTS 16          /* > the latest write cycle (mpyid/mpydp at 10) */
#define WQ_DEPTH 96
#define IMM_DEPTH 96
#define MAX_PK   16          /* instructions in one E1 cycle (PM + loop buffer) */
#define NHOOK    256
/* ~13k packets are hot during playback; 8192 thrashed. */
#define PKC_SIZE 65536
/* L2 (0x008xxxxx) and DDR (0x800xxxxx) code share low address bits, so the
 * slot mixes in the high byte. */
#define PKC_SLOT(pc) ((((pc) >> 1) ^ ((pc) >> 17) ^ ((pc) >> 23)) & (PKC_SIZE - 1))
#define NBR      8       /* at most 6 branches can be in flight */
#define IDLE_RS_SLOTS 2048   /* the app's idle pass reads a few hundred words */
#define PST_DEPTH 64         /* stores in one cycle: a packet plus loop-buffer instructions */
#define IDLE_IDEM 8

typedef struct wq_ent { uint8_t kind; uint8_t idx; uint8_t load; uint32_t val; } wq_ent;
enum { WK_REG, WK_CTRL };

typedef struct fpblk {
    uint16_t valid;           /* bit n = insn[n] decoded */
    c66x_insn insn[16];       /* indexed by fetch-packet offset / 2 */
} fpblk;

/* Decoded fetch packets, two levels (4 KB page -> 128 fetch packets): a flat
 * table over 256 MB of DDR3 was 64 MB of pointers and a cache miss per store.
 * A NULL page also means "no code here", the store fast path. */
#define FP_PAGE_SHIFT 12
typedef struct ramreg {
    uint32_t base, size;
    uint8_t *host;
    fpblk ***fpp;
    uint8_t *codepage;        /* per 4 KB page; shared by every mapping of the same host bytes */
} ramreg;

typedef struct spl_ent { c66x_insn *insn[8]; uint8_t n; } spl_ent;

typedef struct spl_state {
    int      active;
    int      kind;           /* H_SPLOOP / H_SPLOOPD / H_SPLOOPW */
    int      ii;
    int8_t   creg;
    uint8_t  cz;
    uint32_t addr;           /* the SPLOOP execute packet: the interrupt return point */
    uint64_t t0;             /* cycle of body offset 0 of iteration 0 */
    int      loading;
    int      dynlen;
    int      fetch_delay;
    int      last_iter;      /* highest iteration started */
    int      terminated;
    int      int_drain;
    int      abrupt;
    int      initial_term;
    int      resumed;
    uint64_t drain_start;
    uint64_t kernel_cycle;   /* cycle the SPKERNEL packet was loaded */
    spl_ent  body[48];
    /* (cycle - t0) split into iteration and offset, stepped instead of divided */
    uint64_t pos_cycle, pos_t0, pos_k;
    uint32_t pos_off, pos_ii;
    int      steady_built;   /* c66x_core.steady holds this kernel's lists */
    uint8_t  kr_checked;     /* a compiled kernel was looked up for this loop */
    const struct c66x_jit_kernel *kregion;
} spl_state;

/* A running kernel's instructions per body offset once every overlapping
 * iteration is past its first (k >= ceil(dynlen/ii)): what spl_buffer_insns
 * returns then depends only on the offset. Kept outside spl_state, which
 * spl_start clears. */
typedef struct spl_steady { c66x_insn *in[MAX_PK]; uint8_t n; } spl_steady;

/* Program-memory fetch disabled for the loop kernel restarts at PG, so post-loop
 * code reaches E1 this many cycles after the fetch is re-enabled. Chosen from
 * the image: only 6 gives no register or functional-unit conflict between
 * post-loop code and the epilog in all its SPLOOP(D) loops. */
#define PM_REFILL 6

typedef struct pend_branch { int remaining; uint32_t target; } pend_branch;
typedef struct pend_store { uint32_t a, v, pc; uint8_t n, in_isr; } pend_store;

/* A cached packet's shape lives in the entry, so the fast loop decides without
 * touching the instruction until it executes it. */
typedef struct pkc_ent {
    uint32_t pc, next;
    c66x_insn *first;
    /* A packet crossing a fetch-packet boundary is not consecutive slots of one
     * block: its instructions are listed here (NULL: walk from first). */
    c66x_insn **xins;
    uint8_t  n, special, xread, load, xnops, branched, allfop;
    uint64_t xmask;
    const c66x_jit_region *region;  /* a compiled region rooted here, verified */
    const c66x_jit_region *region0; /* a clean compiled state here: no writes pending */
    uint32_t hits;                  /* C66X_JIT_AUTO: fast-loop runs with no branch in flight */
    uint32_t runs;                  /* C66X_JIT_AUTO: all fast-loop runs (the generator's cold test) */
    const struct c66x_jit_qregion *qregion; /* a region entered with one branch in flight (ABI 12) */
} pkc_ent;

/* Instruction lists of cached packets that cross a fetch-packet boundary. */
#define XPK_SIZE 4096
typedef struct xpk_ent { c66x_insn *in[MAX_PK]; } xpk_ent;

typedef struct hook { uint32_t pc; c66x_hook_fn fn; void *opaque; } hook;

struct c66x_core {
    /* Touched every cycle: kept together at the front for cache locality. */
    uint64_t cycle;
    uint32_t pc;
    uint32_t exec_pc;
    int      mcnop;
    int      int_entry;
    int      idle;
    int      branch_block;          /* packets since the last branch, for interrupt masking */
    unsigned nbr;
    uint32_t ifr;
    int      spl_irq_pending;       /* a loop drained for an interrupt: return to it */
    unsigned nhooks;
    uint32_t idle_head;
    uint64_t pm_resume_at;          /* no program-memory packet before this cycle */
    c66x_trace_fn trace;
    struct c66x_jit *jit;           /* compiled regions and profile; NULL = off */
    pend_branch br[NBR];
    /* Writes readable next cycle (most of them) skip the latency ring; they are
     * applied after the ring's slot, which preserves scheduling order. */
    unsigned imm_n;
    wq_ent   imm[IMM_DEPTH];
    uint8_t  wqn[WQ_SLOTS];
    unsigned wq_base;               /* ring offset: one back per pipeline stall */
    uint64_t wmask;                 /* registers written by a non-load at this cycle's commit */
    uint32_t reg[C66X_NREGS];
    uint32_t cr[32];
    unsigned npst;
    uint8_t  store_now;             /* no load this cycle: a store lands at once */
    pend_store pst[PST_DEPTH];      /* this cycle's stores, applied at its end */
    c66x_stats st;
    ramreg   ram[8];
    unsigned nram;
    unsigned last_ram;
    spl_state spl;
    spl_steady steady[48];
    wq_ent   wq[WQ_SLOTS][WQ_DEPTH];

    c66x_bus bus;
    uint32_t efr;
    uint32_t irq_level;
    uint32_t int_vector;
    uint32_t cond_hist[8];          /* SPLOOPW condition register, by cycle */
    uint32_t spl_irq_ret;

    hook     hooks[NHOOK];
    uint32_t trap_pc;
    void    *trace_opaque;

    uint16_t htab[2256];            /* binutils opcodes 0..1023, extension table from 2000 */
    /* busy-wait detection (c66x_set_idle_loop) */
    uint32_t idle_slo, idle_shi;
    int      idle_allow_reads;
    int      idle_armed;
    uint8_t  idle_fx;               /* first side effect this iteration, reason code */
    uint32_t idle_fx_addr, idle_fx_pc;
    uint64_t idle_head_cycle, idle_head_irqs;
    uint32_t idle_reg[C66X_NREGS];
    c66x_idle_info idle_info;
    /* The fixed point survives an interrupt whose handler stores nothing the
     * loop reads: the loop's reads are kept as a set of words, handler stores
     * are checked against it, and the loop is idle again where the handler
     * returns to, once the registers are back. */
    unsigned isr_depth;
    uint8_t  idle_fp;               /* the last STOP_IDLE still holds */
    uint8_t  idle_isr_hit;          /* a handler stored to a word the loop reads */
    uint8_t  idle_ret_pending;      /* the handler returned: check at idle_resume_pc */
    uint8_t  idle_irq_captured;
    uint32_t idle_resume_pc;
    uint32_t idle_irq_reg[C66X_NREGS];
    uint32_t idle_rs[IDLE_RS_SLOTS];
    unsigned idle_rs_n;
    uint8_t  idle_rs_full;
    uint32_t idle_idem[IDLE_IDEM];  /* bus addresses whose writes the loop may repeat */
    unsigned idle_nidem;
    /* PC -> first instruction of a cached packet; cleared on any invalidation */
    pkc_ent  pkc[PKC_SIZE];
    xpk_ent  xpk[XPK_SIZE];
    unsigned nxpk;                  /* used xpk entries; reset with pkc */
    uint32_t watch_lo, watch_hi;
    c66x_watch_fn watch_fn;
    void    *watch_opaque;
    /* c66x_record_open */
    FILE    *rec;
    uint64_t rec_hash_at;
    /* compiled regions */
    const struct c66x_jit_api *api;
    uint64_t code_gen;              /* bumped when decoded code is dropped */
    uint64_t jit_exit[16];          /* region exits by reason (tools/c14_jitgen.py EXIT_*) */
    c66x_jit_cap *jit_cap;          /* non-NULL: sched() captures instead of queueing */
    uint8_t  jit_committed;         /* a region returned after this cycle's commit */
    /* A compiled state handed from one generated function to another: the
     * pending values and flags in the positions the target state names. */
    struct { uint32_t v[64]; uint8_t f[64]; uint64_t gen; } jit_xs;
    /* Appended, not inserted: modules built for this ABI read the fields above. */
    uint64_t xpk_flushes;           /* code_gen bumps from a full xpk table */
};

/* ------------------------------------------------------------------------ */
/* compiled regions                                                          */

/* What a region calls for the paths it does not inline. */
typedef struct c66x_jit_api {
    uint32_t (*mem_read)(c66x_core *c, uint32_t a, unsigned n);
    void     (*store_defer)(c66x_core *c, uint32_t a, uint32_t v, unsigned n);
    void     (*flush_stores)(c66x_core *c);
    void     (*invalidate_code)(c66x_core *c, uint32_t a, uint32_t n);
    void     (*ctrl_write)(c66x_core *c, unsigned crlo, uint32_t v);
    /* exec_insn with its writes captured; returns the stop code (0) */
    int      (*exec_capture)(c66x_core *c, c66x_insn *in, uint32_t next_pc, c66x_jit_cap *cap);
    c66x_insn *(*insn_at)(c66x_core *c, uint32_t addr);
    void     (*spl_end_cycle)(c66x_core *c);   /* the stage boundary of a running kernel */
    /* 1 when every fetch packet still holds its bytes; also decodes each, so a
     * later store into one drops decoded code and moves code_gen on */
    int      (*verify)(c66x_core *c, const uint32_t *addr, const uint8_t *bytes, uint32_t n);
    uint32_t (*ctrl_read)(c66x_core *c, unsigned crlo, uint32_t pce1);
} c66x_jit_api;

/* Entered from fast_cycles at the root packet's cycle, after its commit and
 * stall check, with no branch in flight; returns at the top of a later cycle
 * (before that cycle's commit), with every write, branch and NOP cycle still
 * in flight put back where the interpreter keeps it -- or right after a
 * cycle's commit with jit_committed set, when what follows the commit (an
 * interrupt made deliverable, a stall) is the interpreter's to run. */
typedef void (*c66x_region_fn)(c66x_core *c, uint64_t end);

/* A successful check of a set of fetch packets, shared by every region that
 * depends on them: the core and code generation (+ 1) it holds for. */
typedef struct c66x_jit_verified { void *core; uint64_t gen; } c66x_jit_verified;

struct c66x_jit_region {
    uint32_t pc;
    uint32_t idle_head;             /* the idle head the region stops at */
    c66x_region_fn fn;
    uint32_t ndeps;
    const uint32_t *dep_addr;       /* fetch packets it was built from ... */
    const uint8_t *dep_bytes;       /* ... and their 32 bytes each */
    c66x_jit_verified *verified;    /* shared memo for those deps, or NULL */
};

/* A compiled SPLOOP kernel: entered from spl_fast_cycles in steady state (every
 * overlapping iteration running, past the first three cycles), at any body
 * offset, after that cycle's commit; returns at the top of a later cycle. Valid
 * only for a loop whose recorded body is exactly this one. */
typedef struct c66x_jit_kernel {
    uint32_t addr;                  /* the SPLOOP instruction */
    uint16_t kind;                  /* its handler (H_SPLOOP/H_SPLOOPD/H_SPLOOPW) */
    uint8_t  ii, dynlen;
    int8_t   creg;
    uint8_t  cz;
    const uint8_t *body_n;          /* instructions per body offset, dynlen entries */
    const uint32_t *body_addr;      /* their addresses, offset by offset */
    c66x_region_fn fn;
    uint32_t ndeps;
    const uint32_t *dep_addr;
    const uint8_t *dep_bytes;
    /* The cycles after the last iteration started, while program-memory fetch is
     * still off; NULL when the loop has only one stage (ABI 10). */
    c66x_region_fn drain_fn;
} c66x_jit_kernel;

/* A whole SPLOOP invocation (ABI 11): entered from fast_cycles at the SPLOOP
 * execute packet's cycle, after its commit, with no branch in flight and no loop
 * interrupted (TSR.SPLX clear). It keeps c->spl exactly as the interpreter would,
 * so it can hand over at the top of any cycle; it returns without touching
 * anything when it declines (the core then sees the cycle unchanged). Loading,
 * ramp, steady state and drain run compiled up to where program fetch resumes. */
typedef struct c66x_jit_loop {
    uint32_t pc;                    /* the SPLOOP execute packet */
    c66x_region_fn fn;
    uint32_t ndeps;
    const uint32_t *dep_addr;
    const uint8_t *dep_bytes;
} c66x_jit_loop;

/* A region rooted at a packet reached with exactly one branch in flight (ABI 12):
 * a loop whose back-branch is issued a few packets before it lands is almost
 * never entered free (e.g. 0x8006CDE0), but once entered with the
 * branch it runs whole invocations. Entered from fast_cycles when c->br[0] holds
 * this remaining count and target; the region owns the branch from then on. */
typedef struct c66x_jit_qregion {
    c66x_jit_region r;
    int32_t  rem;                   /* c->br[0].remaining at the top of the cycle */
    uint32_t target;
} c66x_jit_qregion;

/* A compiled module exports one of these as c66x_jit_exports. */
typedef struct c66x_jit_module {
    uint32_t abi;                   /* C66X_JIT_ABI */
    uint32_t nregions;
    const c66x_jit_region *regions;
    uint32_t nkernels;
    const c66x_jit_kernel *kernels;
    /* Clean states (nothing in flight, nothing pending, no known values), entered
     * when the interpreter has no write pending either (ABI 9). */
    uint32_t nregions0;
    const c66x_jit_region *regions0;
    uint32_t nloops;                /* ABI 11 */
    const c66x_jit_loop *loops;
    uint32_t nqregions;             /* ABI 12 */
    const c66x_jit_qregion *qregions;
} c66x_jit_module;


/* Ring slot of the writes landing `ahead` cycles after this one. A cross-path
 * stall freezes the whole pipeline, so writes still in their stages land a
 * cycle later: wq_base moves back one per stall (0x8006D040 depends on it). */
static inline unsigned wq_slot(const c66x_core *c, int ahead)
{
    return (unsigned)(c->cycle + c->wq_base + ahead) & (WQ_SLOTS - 1);
}

/* No write scheduled in any ring slot. */
static inline int jit_wq_empty(const c66x_core *c)
{
    uint64_t a, b;
    __builtin_memcpy(&a, c->wqn, 8);
    __builtin_memcpy(&b, c->wqn + 8, 8);
    return !(a | b);
}

/* The interrupt the core would take now, 0 = none. */
static inline int pending_interrupt(c66x_core *c)
{
    uint32_t ifr = c->ifr;
    if ((ifr & (1u << C66X_INT_NMI)) && (c->cr[CR_IER] & IER_NMIE))
        return C66X_INT_NMI;
    if (!(c->cr[CR_TSR] & TSR_GIE) || !(c->cr[CR_IER] & IER_NMIE))
        return 0;
    uint32_t en = ifr & c->cr[CR_IER] & 0xfff0;
    if (!en)
        return 0;
    return __builtin_ctz(en);
}

/* A region's commit of the ring slot landing now, for writes scheduled before
 * the region was entered (commit_writes' ring half). Returns 1 when it
 * committed a control-register write, after which an interrupt may be
 * deliverable. */
static inline int c66x_jit_pre_commit(c66x_core *c)
{
    unsigned s = wq_slot(c, -1), n = c->wqn[s];
    int ctrl = 0;
    for (unsigned i = 0; i < n; i++) {
        if (c->wq[s][i].kind == WK_REG) {
            c->reg[c->wq[s][i].idx] = c->wq[s][i].val;
        } else {
            c->api->ctrl_write(c, c->wq[s][i].idx, c->wq[s][i].val);
            ctrl = 1;
        }
    }
    c->wqn[s] = 0;
    return ctrl;
}

#endif
