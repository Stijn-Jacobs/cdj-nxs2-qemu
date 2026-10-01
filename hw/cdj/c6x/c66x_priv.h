/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x core internals shared by the core's own files, and by nothing else:
 *
 *   c66x_core.c   setup, the handler table, the public API
 *   c66x_mem.c    memory, the idle-loop checks, instruction fetch
 *   c66x_exec.c   instruction execution and the SPLOOP loop buffer
 *   c66x_step.c   interrupts, the cycle loop, the fast loop, c66x_step
 *   c66x_jit.c    compiled regions: loading, lookup, profile, the generator's queries
 *
 * What the hot path calls across those files is here as static inline, so it
 * stays inlined as it was in one file: the register-write scheduling, the
 * store queue, the control registers, fop_exec. What a compiled region may
 * see is c66x_core_int.h, which this includes; that one is ABI.
 */
#ifndef C66X_PRIV_H
#define C66X_PRIV_H

#include "c66x.h"

#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* GModule rather than dlopen so the JIT module also loads on Windows. */
#include <gmodule.h>
#ifndef _WIN32
#include <unistd.h>            /* ftruncate; see c66x_set_file_size() */
#endif
#include <time.h>
#include <unistd.h>

#include "c66x_decode.h"
#include "c66x_core_int.h"
#include "binutils/tic6x.h"

enum hid {
    H_UNIMP = 0,
    H_NOP, H_IDLE, H_DINT, H_RINT, H_SWE,
    H_ABS, H_ABS2, H_ADD, H_ADDU, H_SUB, H_SUBU, H_ADD2, H_SUB2, H_ADD4, H_SUB4,
    H_ADDA, H_SUBA, H_ADDK, H_ADDKPC, H_ADDSUB, H_ADDSUB2, H_SADDSUB, H_SADDSUB2,
    H_AND, H_ANDN, H_OR, H_XOR, H_NOT, H_NEG, H_MV, H_MVK, H_MVKH, H_ZERO, H_DMV,
    H_MVC, H_MVD,
    H_CMPEQ, H_CMPGT, H_CMPGTU, H_CMPLT, H_CMPLTU,
    H_CMPEQ2, H_CMPGT2, H_CMPLT2, H_CMPEQ4, H_CMPGTU4, H_CMPLTU4,
    H_SHL, H_SHR, H_SHRU, H_SSHL, H_SHR2, H_SHRU2, H_SSHVL, H_SSHVR,
    H_SHLMB, H_SHRMB, H_ROTL,
    H_CLR, H_SET, H_EXT, H_EXTU,
    H_SADD, H_SSUB, H_SAT, H_SADD2, H_SSUB2, H_SADDUS2, H_SADDSU2, H_SADDU4,
    H_SUBC, H_SUBABS4, H_NORM, H_LMBD, H_BITC4, H_BITR, H_DEAL, H_SHFL, H_SHFL3,
    H_XPND2, H_XPND4, H_SWAP2, H_SWAP4,
    H_AVG2, H_AVGU4, H_MAX2, H_MAXU4, H_MIN2, H_MINU4,
    H_PACK2, H_PACKH2, H_PACKHL2, H_PACKLH2, H_PACKH4, H_PACKL4, H_SPACK2, H_SPACKU4,
    H_RPACK2, H_DPACK2, H_DPACKX2,
    H_UNPKHU4, H_UNPKLU4,
    H_MPY16, H_SMPY16, H_MPY32, H_MPYI, H_MPYID, H_MPYHI, H_MPYHIR, H_MPYLI, H_MPYLIR,
    H_MPY2, H_SMPY2, H_MPYSU4, H_MPYU4, H_MPYUS4, H_SMPY32, H_MPY2IR,
    H_DOTP2, H_DOTPN2, H_DOTPRSU2, H_DOTPNRSU2, H_DOTPSU4, H_DOTPU4,
    H_DDOTP4, H_DDOTPH2, H_DDOTPL2, H_DDOTPH2R, H_DDOTPL2R,
    H_CMPY, H_CMPYR, H_CMPYR1, H_GMPY, H_GMPY4, H_XORMPY,
    H_ABSSP, H_ABSDP, H_ADDSP, H_ADDDP, H_SUBSP, H_SUBDP, H_MPYSP, H_MPYDP,
    H_MPYSPDP, H_MPYSP2DP,
    H_CMPEQSP, H_CMPGTSP, H_CMPLTSP, H_CMPEQDP, H_CMPGTDP, H_CMPLTDP,
    H_INTSP, H_INTSPU, H_INTDP, H_INTDPU, H_SPINT, H_DPINT, H_SPTRUNC, H_DPTRUNC,
    H_SPDP, H_DPSP, H_RCPSP, H_RCPDP, H_RSQRSP, H_RSQRDP,
    H_LOAD, H_STORE,
    H_B, H_BNOP, H_CALLP, H_BDEC, H_BPOS,
    H_SPLOOP, H_SPLOOPD, H_SPLOOPW, H_SPKERNEL, H_SPKERNELR, H_SPMASK, H_SPMASKR,
    /* C66x additions (c66x_ext_table.h) */
    H_DADD, H_DSUB, H_DSADD, H_DSSUB, H_DADDSP, H_DSUBSP, H_DMPYSP, H_QMPYSP,
    H_CMPYSP,
    H_MPYU2, H_LAND, H_LANDN, H_LOR, H_DINTHSP, H_DINTHSPU, H_DSPINT, H_DSADD2,
    H_DAVGNR2, H_UNPKH2, H_DSHL2, H_DCMPGT2, H_DMPY2, H_QSMPY32R1,
    H_SHL2, H_DPACKL4, H_DCCMPYR1, H_DINTSP, H_DINTSPU,
    H_COUNT
};

/* C66X_HCOUNT builds only: where execution goes, printed by c66x_free. */
#ifdef C66X_HCOUNT
extern uint64_t hc_handler[H_COUNT], hc_fop[32], hc_path[8], hc_gather[8];
void hc_pc(uint32_t pc, uint8_t kind, int ii, int dynlen);
void hc_loop(uint32_t addr, uint64_t iters, uint64_t cycles);
#define HC(arr, i) (hc_##arr[i]++)
#define HCPC(pc, kind, ii, dl) hc_pc(pc, kind, ii, dl)
#define HCLOOP(addr, it, cy) hc_loop(addr, it, cy)
#else
#define HCLOOP(addr, it, cy) ((void)0)
#define HC(arr, i) ((void)0)
#define HCPC(pc, kind, ii, dl) ((void)0)
#endif

/* ------------------------------------------------------------------------ */
/* record (c66x_record_open; read back by c6xreplay.c)                       */

/* One record: type, cycle, then a type-specific payload. */
enum {
    REC_READ = 'R',     /* u32 addr, u8 size, u32 value                        */
    REC_WRITE = 'W',    /* u32 addr, u8 size, u32 value                        */
    REC_IRQ = 'I',      /* u8 line, u8 level                                   */
    REC_STORE = 'D',    /* u32 addr, u32 len, len bytes (host store into RAM)  */
    REC_SKIP = 'S',     /* u64 cycles                                          */
    REC_NEW = 'N',      /* nothing: a fresh core                               */
    REC_MAP = 'M',      /* u32 base, u32 size, u32 host id                     */
    REC_PAGE = 'P',     /* u32 host id, u32 offset, 4096 bytes                 */
    REC_RESET = 'Z',    /* u32 pc                                              */
    REC_HASH = 'H',     /* u32 pc, u64 state hash                              */
};
#define REC_HASH_EVERY (1ull << 22)

static inline void rec_head(c66x_core *c, uint8_t type)
{
    fputc(type, c->rec);
    fwrite(&c->cycle, sizeof c->cycle, 1, c->rec);
}

static inline void rec_rw(c66x_core *c, uint8_t type, uint32_t a, unsigned n, uint32_t v)
{
    uint8_t n8 = (uint8_t)n;
    rec_head(c, type);
    fwrite(&a, 4, 1, c->rec);
    fwrite(&n8, 1, 1, c->rec);
    fwrite(&v, 4, 1, c->rec);
}

/* ------------------------------------------------------------------------ */
/* across files                                                              */

typedef struct xctx {
    uint32_t pce1;          /* fetch packet of the instruction */
    uint32_t next_pc;       /* address after the execute packet */
    int      extra_nops;    /* NOP cycles this packet inserts after itself */
    int      branched;      /* packet contains a branch instruction */
    int      is_buffer;     /* instruction came from the loop buffer */
    int      stop;
} xctx;

/* c66x_core.c */
uint32_t rec_host_id(const c66x_core *c, const ramreg *r);
int idle_check(c66x_core *c);

/* c66x_mem.c */
void invalidate_code(c66x_core *c, uint32_t a, uint32_t n);
void page_stored(c66x_core *c, uint8_t cp, uint32_t a, uint32_t n);
void idle_rs_clear(c66x_core *c);
uint32_t mem_read(c66x_core *c, uint32_t a, unsigned n);
void mem_write(c66x_core *c, uint32_t a, uint32_t v, unsigned n, int in_isr, uint32_t pc);
c66x_insn *fetch_insn(c66x_core *c, uint32_t a);

/* c66x_exec.c */
int fop_run(c66x_core *c, const c66x_insn *in, uint32_t next_pc);
void exec_insn(c66x_core *c, c66x_insn *in, xctx *x);
unsigned spl_buffer_insns(c66x_core *c, c66x_insn **out, uint32_t mask);
void spl_end_cycle(c66x_core *c, c66x_insn **pm, unsigned npm, uint32_t mask, int *pm_exec_mask);

/* c66x_step.c */
void idle_isr_return(c66x_core *c, uint32_t target);
int gather_packet(c66x_core *c, uint32_t pc, c66x_insn **pk, unsigned *n, uint32_t *next);

/* c66x_jit.c */
const c66x_jit_loop *jit_loop_lookup(c66x_core *c, uint32_t pc);
const c66x_jit_region *jit_lookup(c66x_core *c, uint32_t pc);
const c66x_jit_region *jit_lookup0(c66x_core *c, uint32_t pc);
const c66x_jit_qregion *jit_lookupq(c66x_core *c, uint32_t pc);
void kstats_dump_fwd(void);
const c66x_jit_kernel *jit_kernel_lookup(c66x_core *c);
void jit_kprof(c66x_core *c);
void jit_kprof_cycles(c66x_core *c, uint64_t n);
void jit_prof(c66x_core *c, uint32_t pc);
void jit_auto_poll(c66x_core *c);
void jit_init(c66x_core *c);
void jit_free(c66x_core *c);

/* ------------------------------------------------------------------------ */
/* memory (c66x_mem.c)                                                        */

static inline uint8_t *ram_ptr(c66x_core *c, uint32_t a, unsigned n)
{
    ramreg *r = &c->ram[c->last_ram];
    if (a - r->base <= r->size - n && r->size >= n)
        return r->host + (a - r->base);
    for (unsigned i = 0; i < c->nram; i++) {
        r = &c->ram[i];
        if (a - r->base <= r->size - n && r->size >= n) {
            c->last_ram = i;
            return r->host + (a - r->base);
        }
    }
    return NULL;
}

/* A side effect of the loop itself; handlers are judged by idle_isr_store. */
static inline void idle_note(c66x_core *c, uint8_t why, uint32_t a)
{
    if (c->idle_armed && !c->idle_fx && !c->isr_depth) {
        c->idle_fx = why;
        c->idle_fx_addr = a;
        c->idle_fx_pc = c->exec_pc;
        c->idle_fp = 0;
    }
}

static inline void flush_stores(c66x_core *c)
{
    unsigned n = c->npst;
    c->npst = 0;
    for (unsigned i = 0; i < n; i++)
        mem_write(c, c->pst[i].a, c->pst[i].v, c->pst[i].n, c->pst[i].in_isr, c->pst[i].pc);
}

static inline void store_defer(c66x_core *c, uint32_t a, uint32_t v, unsigned n)
{
    /* The end-of-cycle order is only visible to a load in the same cycle. */
    if (c->store_now) {
        mem_write(c, a, v, n, c->isr_depth != 0, c->exec_pc);
        return;
    }
    if (c->npst == PST_DEPTH)
        flush_stores(c);
    c->pst[c->npst++] = (pend_store){ a, v, c->exec_pc, (uint8_t)n, c->isr_depth != 0 };
}

enum { LD_W = 0, LD_B, LD_BU, LD_H, LD_HU, LD_DW };

/* Instructions the fast loop runs inline: every operand a 32-bit register or a
 * constant, one register result readable next cycle. Anything wider, saturating,
 * cross-unit or delayed stays with exec_insn. */
enum {
    F_NONE = 0, F_NOP, F_MVK, F_MVKH, F_MV, F_ADDK, F_ADDKPC,
    F_ADD, F_SUB, F_AND, F_OR, F_XOR, F_CMPEQ, F_CMPGT, F_CMPLT, F_CMPGTU, F_CMPLTU,
    F_SHL, F_SHR, F_SHRU, F_EXT, F_EXTU,
    /* no operand decoding to save, only exec_insn's dispatch */
    F_LOAD, F_STORE, F_BRANCH, F_CALLP, F_MVC,
    /* single-precision float from two registers, written at the operand's stage */
    F_ADDSP, F_SUBSP, F_MPYSP,
};

/* ------------------------------------------------------------------------ */
/* control registers, hooks                                                  */

static inline uint32_t ctrl_read(c66x_core *c, unsigned crlo, uint32_t pce1)
{
    switch (crlo) {
    case CR_CSR: return (c->cr[CR_CSR] & ~CSR_GIE) | (c->cr[CR_TSR] & TSR_GIE);
    case CR_IFR: return c->ifr;
    case CR_EFR: return c->efr;
    case CR_PCE1: return pce1;
    case CR_DNUM: return 0;
    case CR_TSCL: idle_note(c, 6, 0); return (uint32_t)c->cycle;
    case CR_TSCH: idle_note(c, 6, 0); return (uint32_t)(c->cycle >> 32);
    case CR_TSR: return c->cr[CR_TSR];
    default: return c->cr[crlo & 31];
    }
}

static inline void ctrl_write_now(c66x_core *c, unsigned crlo, uint32_t v)
{
    switch (crlo) {
    case CR_CSR:
        /* CPU ID, revision and EN are read-only; GIE lives in TSR. */
        c->cr[CR_CSR] = (c->cr[CR_CSR] & 0xFFFF0100) | (v & ~0xFFFF0101u);
        c->cr[CR_TSR] = (c->cr[CR_TSR] & ~TSR_GIE) | (v & CSR_GIE);
        break;
    case CR_IFR:  c->ifr |= v & ~1u; break;     /* ISR */
    case CR_ICR:  c->ifr &= ~(v & ~1u); break;
    case CR_IER:  c->cr[CR_IER] = v | 1; break;
    case CR_EFR:  c->efr &= ~v; break;          /* ECR */
    case CR_PCE1: case CR_DNUM: case CR_TSCH: break;
    default: c->cr[crlo & 31] = v; break;
    }
}

static inline hook *find_hook(c66x_core *c, uint32_t pc)
{
    for (unsigned i = 0; i < NHOOK; i++) {
        hook *h = &c->hooks[(pc / 2 + i) & (NHOOK - 1)];
        if (!h->fn)
            return NULL;
        if (h->pc == pc)
            return h;
    }
    return NULL;
}

/* ------------------------------------------------------------------------ */
/* pipeline bookkeeping                                                      */

static inline void sched(c66x_core *c, unsigned delay, uint8_t kind, uint8_t idx,
                         uint32_t val, uint8_t load)
{
    if (__builtin_expect(c->jit_cap != NULL, 0)) {
        c66x_jit_cap *cap = c->jit_cap;
        if (cap->n < C66X_JIT_CAP)
            cap->e[cap->n] = (typeof(cap->e[0])){ (uint8_t)delay, kind, idx, val };
        cap->n++;
        return;
    }
    if (delay == 0) {
        if (c->imm_n < IMM_DEPTH)
            c->imm[c->imm_n++] = (wq_ent){ kind, idx, load, val };
        return;
    }
    unsigned s = wq_slot(c, (int)delay);
    if (c->wqn[s] < WQ_DEPTH)
        c->wq[s][c->wqn[s]++] = (wq_ent){ kind, idx, load, val };
}

/* Only a result written at E1 (an immediate write) can stall a cross-path read
 * next cycle: SPRU732J 3.7.4 exempts a load's data and a result read one cycle
 * after a later stage generates it. 0x8006CE34 relies on that: its E4 mpysp
 * results are read through 1X four packets later, in the same cycle as the
 * square written to A5. */
static inline void commit_one(c66x_core *c, const wq_ent *e, uint64_t *mask)
{
    if (e->kind == WK_REG) {
        c->reg[e->idx] = e->val;
        if (mask)
            *mask |= 1ULL << e->idx;
    } else {
        ctrl_write_now(c, e->idx, e->val);
    }
}

/* Writes scheduled for the previous cycle land now: first those that waited in
 * the ring (scheduled in earlier cycles), then last cycle's immediate ones. */
static inline void commit_writes(c66x_core *c)
{
    unsigned s = wq_slot(c, -1);
    uint64_t mask = 0;
    unsigned n = c->wqn[s];
    for (unsigned i = 0; i < n; i++)
        commit_one(c, &c->wq[s][i], NULL);
    c->wqn[s] = 0;
    n = c->imm_n;
    for (unsigned i = 0; i < n; i++)
        commit_one(c, &c->imm[i], &mask);
    c->imm_n = 0;
    c->wmask = mask;
}

/* ------------------------------------------------------------------------ */
/* execution (c66x_exec.c)                                                   */

static inline void fop_exec(c66x_core *c, const c66x_insn *in, xctx *x)
{
    c->st.insns++;
    if (in->xnops > x->extra_nops)
        x->extra_nops = in->xnops;
    x->branched |= in->isbranch;
    int r = fop_run(c, in, x->next_pc);
    if (r)
        x->stop = r;
}

static inline int spl_pm_enabled(c66x_core *c)
{
    spl_state *s = &c->spl;
    if (c->cycle < c->pm_resume_at)
        return 0;
    if (!s->active || s->loading)
        return 1;
    if (s->abrupt)
        return 1;
    if (s->int_drain || !s->terminated)
        return 0;
    return c->cycle >= s->drain_start + s->fetch_delay;
}

/* ------------------------------------------------------------------------ */
/* compiled regions (c66x_jit.c)                                             */

#define JIT_MAP  16384          /* regions by root pc */
#define JIT_PROF (1u << 18)     /* profile slots */
#define JIT_MODS 256
#define JIT_LOOPS 1024          /* compiled SPLOOP invocations, open addressing by pc */

struct c66x_jit {
    struct jit_map_ent { uint32_t pc; const c66x_jit_region *r; uint64_t gen; } map[JIT_MAP];
    struct jit_map_ent map0[JIT_MAP];   /* clean entries by pc */
    unsigned nregions0;
    uint64_t entries0;
    struct jit_map_ent mapq[JIT_MAP];   /* queued-branch entries by pc (r is a c66x_jit_qregion) */
    unsigned nqregions;
    uint64_t qentries, qcycles;
    void *mods[JIT_MODS];
    unsigned nmods, nregions;
    uint64_t entries, verify_fail, cycles;
    /* compiled kernels by SPLOOP address (a short list: a loop's body is checked) */
    struct { const c66x_jit_kernel *k; uint64_t gen; } kern[1024];
    unsigned nkern;
    uint64_t kentries, kcycles;
    uint64_t dentries, dcycles;     /* the compiled drain */
    /* whole compiled SPLOOP invocations by execute-packet pc (ABI 11) */
    struct { uint32_t pc; const c66x_jit_loop *l; uint64_t gen; } loops[JIT_LOOPS];
    unsigned nloops;
    uint64_t lentries, lcycles, ldeclined;
    /* C66X_JIT_PROFILE: kernels as the core loaded them, with their cycles */
    struct { uint32_t addr; uint16_t kind; uint8_t ii, dynlen; int8_t creg; uint8_t cz;
             uint8_t n[48]; uint32_t a[48][8]; uint64_t cycles; uint8_t tried; } *kprof;
    unsigned nkprof;
    /* C66X_JIT_PROFILE: packet cycles in the fast loop, and how many of them
     * had no branch in flight (only those can enter a region) */
    char *prof_dir;
    struct { uint32_t pc; uint64_t n, n_free; } *prof;
    /* Interpreted packets entered with exactly one branch in flight, by
     * (pc, remaining, target): the candidates for queued roots. */
    struct { uint32_t pc, target; int rem; uint64_t n; } *qprof;
    uint64_t prof_nbr[4];           /* interpreted packet runs by branches in flight (3 = 3+) */
    /* C66X_JIT_AUTO */
    char *auto_dir, *auto_gen, *auto_lib;
    double auto_interval, auto_last;
    unsigned auto_cached;
    unsigned auto_min, auto_cold, auto_fn, auto_roots, auto_batch, auto_modules, auto_failed;
    int auto_busy;
    volatile int auto_result;       /* set by the compile thread: 1 built, -1 failed */
    GThread *auto_thr;
    char auto_out[4096], auto_log[4200];
    GStrv auto_argv;                /* the generator's argv; no shell involved */
    uint32_t auto_tried[65536];     /* packets already offered to the generator (pc | 1) */
    unsigned auto_ntried;
};

static inline unsigned jit_slot(uint32_t pc) { return (pc * 2654435761u) >> 18; }

#endif
