/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x core: native forms of hot pure DSP kernels.
 *
 * A kernel is a loop whose body reads and writes only RAM and registers. Its
 * native form runs whole passes of that loop from the loop head: each pass
 * produces exactly the registers, control registers, stores and cycle count
 * the interpreter would, so the core is in the interpreter's own state at the
 * pass boundary, and anything the native form does not cover (a pass that
 * would cross the end of the step, a pointer outside RAM, a directed rounding
 * mode) is simply run by the interpreter from there.
 *
 * Interrupts are raised only between c66x_step calls, and none of these loops
 * touches IER, ISR or the bus, so a pass that ends inside the step with no
 * interrupt deliverable at its start cannot have been interrupted.
 *
 * A kernel is found by its code, not by its address, so a firmware release
 * that moves it still runs natively: the decoded code is searched for the
 * kernel's bytes, leaving out the fields a relink changes (absolute addresses
 * built by MVK/MVKH, branches out of the kernel). The native form takes its
 * code addresses from where the match is and its data addresses from the
 * matched MVK/MVKH pairs. A kernel found nowhere, or in more than one place,
 * runs on the interpreter.
 */
#include "c66x_native_int.h"

static const native_kernel *const kernels[] = {
    &native_fir, &native_ring_walk, &native_interp, &native_filter_step, &native_ring_copy,
    &native_mt_env_a, &native_mt_env_b, &native_mt_mark, &native_peak_scan, &native_xdj_interp,
    &native_engine,
    &native_fifo_drop, &native_mt_extrema, &native_slope_flags, &native_mt_phase_a, &native_mt_phase_b,
    &native_fft_scaled, &native_fft_plain, &native_voc_window, &native_voc_frame, &native_mt_phase_a1,
    &native_mt_phase_b1, &native_voc_ratio, &native_voc_bytes, &native_voc_lock, &native_voc_peaks,
};
#define NKERNELS (sizeof kernels / sizeof kernels[0])
_Static_assert(NKERNELS <= 32, "native_verified keeps one bit a kernel");

/* ------------------------------------------------------------------------ */
/* the driver                                                                */

typedef struct native_check {
    const native_kernel *kernel;
    native_state s;
    native_store st[NATIVE_STORES];
    uint64_t at, end_cycle, irqs;
    uint64_t packets, insns, stalls, sploops, jit_cycles;
    unsigned wq_base;
    uint64_t checked, skipped, bad, overrun;
} native_check;

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

typedef struct native_found {
    uint32_t at[NKERNELS];
    uint32_t addr[NKERNELS][2];
    unsigned matches[NKERNELS];     /* in the last search */
} native_found;

/* The bytes at p match kernel k: fill its addresses. */
static int native_match(const native_kernel *k, const uint8_t *p, uint32_t addr[2])
{
    uint8_t code[NATIVE_SIZE_MAX];
    if (k->size > sizeof code)
        return 0;
    memcpy(code, p, k->size);
    addr[0] = addr[1] = 0;
    for (unsigned i = 0; i < k->nwild; i++) {
        const native_wild *w = &k->wild[i];
        uint32_t v = ld32(code + w->off);
        if (w->addr >= 0) {
            uint32_t cst = (v & MVK_CST) >> 7;
            addr[w->addr] |= w->high ? cst << 16 : cst;
        }
        v &= ~w->mask;
        memcpy(code + w->off, &v, 4);
    }
    return fnv(code, k->size) == k->hash;
}

/* Search every fetch packet on a page that holds decoded code. A mapping that
 * shows the same bytes as another one already searched is skipped, so an
 * alias of the kernel is not a second match. */
static void native_search(c66x_core *c, native_found *f)
{
    memset(f->matches, 0, sizeof f->matches);
    for (unsigned ri = 0; ri < c->nram; ri++) {
        const ramreg *r = &c->ram[ri];
        int alias = 0;
        for (unsigned j = 0; j < ri; j++)
            alias |= c->ram[j].host == r->host;
        if (!r->fpp || alias)
            continue;
        for (uint32_t pg = 0; pg <= (r->size - 1) >> FP_PAGE_SHIFT; pg++) {
            if (!r->fpp[pg])
                continue;
            uint32_t end = (pg + 1) << FP_PAGE_SHIFT;
            for (uint32_t off = pg << FP_PAGE_SHIFT; off < end && off < r->size; off += 32) {
                uint32_t w = ld32(r->host + off);
                for (unsigned i = 0; i < NKERNELS; i++) {
                    const native_kernel *k = kernels[i];
                    uint32_t addr[2];
                    if (w != k->first || k->size > r->size - off || !native_match(k, r->host + off, addr))
                        continue;
                    f->matches[i]++;
                    f->at[i] = r->base + off;
                    memcpy(f->addr[i], addr, sizeof addr);
                }
            }
        }
    }
}

/* Which kernels were found exactly once, searched again whenever decoded code
 * was dropped or reached a new page. The found code is decoded too, so a later
 * store into it drops decoded code and moves code_gen on. The pages are taken
 * before that decode: a page it reaches first has not been searched yet. */
static uint32_t native_verified(c66x_core *c)
{
    if (c->native_gen == c->code_gen + 1 && c->native_pages == c->code_pages)
        return c->native_ok;
    native_found *f = c->native_found;
    native_search(c, f);
    uint64_t pages = c->code_pages;
    uint32_t ok = 0;
    for (unsigned i = 0; i < NKERNELS; i++) {
        if (f->matches[i] != 1)
            continue;
        for (uint32_t a = f->at[i]; a < f->at[i] + kernels[i]->size; a += 32)
            fetch_insn(c, a);
        ok |= 1u << i;
    }
    c->native_ok = ok;
    c->native_gen = c->code_gen + 1;
    c->native_pages = pages;
    return ok;
}

/* The native wins at its loop head even over a compiled region rooted there:
 * the kernel runs a pass in a few host instructions where the region steps
 * every packet. */
int native_entry(c66x_core *c, uint32_t pc)
{
    if (!c->native_mode)
        return 0;
    uint32_t ok = native_verified(c);
    const native_found *f = c->native_found;
    for (unsigned i = 0; i < NKERNELS; i++)
        if ((ok >> i & 1) && (f->at[i] + kernels[i]->entry == pc
                              || (c->native_mode == 2 && kernels[i]->exit != RETURNS
                                  && f->at[i] + kernels[i]->exit == pc)))
            return 1;
    const native_check *chk = c->native_chk;
    return chk && chk->end_cycle && pc == chk->s.pc;
}

int native_find(c66x_core *c, const char *name, uint32_t *at, uint32_t addr[2])
{
    uint32_t ok = native_verified(c);
    const native_found *f = c->native_found;
    for (unsigned i = 0; i < NKERNELS; i++)
        if (!strcmp(kernels[i]->name, name) && (ok >> i & 1)) {
            *at = f->at[i];
            memcpy(addr, f->addr[i], sizeof f->addr[i]);
            return 1;
        }
    return 0;
}

/* The write ring holds the kernel's in-flight register results, each at most
 * once and all but the conditional ones: their values, the register's own for
 * an absent one, and which are there. */
static int pend_match(const c66x_core *c, const native_kernel *k, uint32_t *val, uint32_t *live)
{
    uint32_t seen = 0, need = 0;
    for (int a = 0; a < WQ_SLOTS - 1; a++) {
        unsigned sl = wq_slot(c, a);
        for (unsigned i = 0; i < c->wqn[sl]; i++) {
            const wq_ent *e = &c->wq[sl][i];
            unsigned j = 0;
            while (j < k->npend && (k->pend[j].reg != e->idx || k->pend[j].ahead != a))
                j++;
            if (j == k->npend || e->kind != WK_REG || e->load != k->pend[j].load || (seen >> j & 1))
                return 0;
            seen |= 1u << j;
            val[j] = e->val;
        }
    }
    for (unsigned j = 0; j < k->npend; j++) {
        if (!k->pend[j].cond)
            need |= 1u << j;
        if (!(seen >> j & 1))
            val[j] = c->reg[k->pend[j].reg];
    }
    *live = seen;
    return (seen & need) == need;
}

static void check_arrival(c66x_core *c, native_check *k)
{
    if (c->st.interrupts != k->irqs) {
        k->skipped++;
        return;
    }
    const native_state *s = &k->s;
    char why[256] = "";
    /* A module's compiled region can run straight through the pass's end, so
     * the core never stops there to be compared. */
    int compiled = c66x_jit_cycles(c) != k->jit_cycles;
    if (compiled && (c->cycle > k->end_cycle || c->pc != s->pc)) {
        k->overrun++;
        return;
    }
    if (c->cycle != k->end_cycle || c->pc != s->pc)
        snprintf(why, sizeof why, "cycle +%lld pc 0x%08x (predicted +%u 0x%08x)",
                 (long long)(c->cycle - k->at), c->pc, s->cycles, s->pc);
    for (unsigned i = 0; !*why && i < C66X_NREGS; i++)
        if (c->reg[i] != s->reg[i])
            snprintf(why, sizeof why, "reg %c%u 0x%08x (predicted 0x%08x)", i < 32 ? 'A' : 'B', i & 31,
                     c->reg[i], s->reg[i]);
    for (unsigned i = 0; !*why && i < 32; i++)
        if (c->cr[i] != s->cr[i])
            snprintf(why, sizeof why, "cr %u 0x%08x (predicted 0x%08x)", i, c->cr[i], s->cr[i]);
    /* compiled code does not count the packets and instructions it runs */
    if (!*why && !compiled && (c->st.packets - k->packets != s->packets || c->st.insns - k->insns != s->insns
                  || c->st.sploops - k->sploops != s->sploops))
        snprintf(why, sizeof why, "packets %llu insns %llu sploops %llu (predicted %u %u %u)",
                 (unsigned long long)(c->st.packets - k->packets), (unsigned long long)(c->st.insns - k->insns),
                 (unsigned long long)(c->st.sploops - k->sploops), s->packets, s->insns, s->sploops);
    if (!*why && (c->st.stalls != k->stalls || c->wq_base != k->wq_base || c->branch_block != (int)s->branch_block
                  || c->nbr || c->mcnop || c->exec_pc != s->last_pc))
        snprintf(why, sizeof why,
                 "stalls %llu wq_base %d branch_block %d nbr %u mcnop %d exec 0x%08x (predicted %u, 0x%08x)",
                 (unsigned long long)(c->st.stalls - k->stalls), (int)(c->wq_base - k->wq_base),
                 c->branch_block, c->nbr, c->mcnop, c->exec_pc, s->branch_block, s->last_pc);
    if (!*why && s->pm_resume && c->pm_resume_at != k->at + s->pm_resume)
        snprintf(why, sizeof why, "pm_resume_at +%lld (predicted +%u)",
                 (long long)(c->pm_resume_at - k->at), s->pm_resume);
    uint32_t pend[NATIVE_PENDS], live;
    if (!*why && k->kernel->npend && (!pend_match(c, k->kernel, pend, &live) || live != s->pend_live))
        snprintf(why, sizeof why, "in-flight writes 0x%x (predicted 0x%x)", live, s->pend_live);
    for (unsigned j = 0; !*why && j < k->kernel->npend; j++)
        if (pend[j] != s->pend[j])
            snprintf(why, sizeof why, "in-flight %c%u 0x%08x (predicted 0x%08x)", k->kernel->pend[j].reg < 32 ? 'A' : 'B',
                     k->kernel->pend[j].reg & 31, pend[j], s->pend[j]);
    if (!*why && s->mv.len) {
        const uint8_t *p = ram_ptr(c, s->mv.dst, s->mv.len);
        if (!p || fnv(p, s->mv.len) != s->mv.hash)
            snprintf(why, sizeof why, "copy of %u bytes to 0x%08x", s->mv.len, s->mv.dst);
    }
    for (unsigned i = 0; !*why && i < s->nbulk; i++) {
        const uint8_t *p = ram_ptr(c, s->bulk[i].dst, s->bulk[i].len);
        if (!p || fnv(p, s->bulk[i].len) != s->bulk[i].hash)
            snprintf(why, sizeof why, "%u bytes at 0x%08x", s->bulk[i].len, s->bulk[i].dst);
    }
    for (unsigned i = 0; !*why && i < s->nst; i++) {
        unsigned later = i + 1;
        while (later < s->nst && s->st[later].a != s->st[i].a)
            later++;
        if (later < s->nst)
            continue;           /* overwritten within the pass */
        unsigned n = s->st[i].n;
        uint32_t mask = n == 4 ? ~0u : (1u << 8 * n) - 1;
        const uint8_t *p = ram_ptr(c, s->st[i].a, n);
        uint32_t v = 0;
        if (p)
            memcpy(&v, p, n);
        if (!p || ((v ^ s->st[i].v) & mask))
            snprintf(why, sizeof why, "store 0x%08x 0x%08x (predicted 0x%08x)", s->st[i].a, v, s->st[i].v & mask);
    }
    k->checked++;
    if (*why && k->bad++ < 20)
        fprintf(stderr, "c66x native check: pass at cycle %llu: %s\n", (unsigned long long)k->at, why);
}

/* A span the pass wrote whole: copied straight into RAM unless a store there
 * has side effects (decoded code, a watch, the idle loop's tracking). */
static void bulk_write(c66x_core *c, const native_bulk *b, const uint8_t *src, uint32_t pc)
{
    uint8_t *p = ram_ptr(c, b->dst, b->len);
    int plain = p && !c->idle_armed && !c->idle_isr_disarmed
                && !(c->watch_fn && b->dst <= c->watch_hi && b->dst + b->len - 1 >= c->watch_lo);
    if (plain) {
        const ramreg *r = &c->ram[c->last_ram];
        for (uint32_t pg = (b->dst - r->base) >> FP_PAGE_SHIFT; pg <= (b->dst + b->len - 1 - r->base) >> FP_PAGE_SHIFT; pg++)
            plain &= !r->codepage[pg];
    }
    if (plain) {
        memcpy(p, src, b->len);
        return;
    }
    for (uint32_t i = 0; i < b->len; i += 4)
        mem_write(c, b->dst + i, ld32(src + i), 4, c->isr_depth != 0, pc);
}

int native_run(c66x_core *c, uint64_t end)
{
    const native_kernel *k = NULL;
    const native_found *f = c->native_found;
    uint32_t ok = native_verified(c);
    unsigned ki = 0;
    for (; ki < NKERNELS; ki++)
        if ((ok >> ki & 1) && f->at[ki] + kernels[ki]->entry == c->pc) {
            k = kernels[ki];
            break;
        }
    native_check *chk = c->native_chk;
    if (chk && chk->end_cycle && (k || c->pc == chk->s.pc || native_entry(c, c->pc))) {
        /* a kernel the pass calls, entered before the pass is over */
        if (c->cycle < chk->end_cycle && c->st.interrupts == chk->irqs)
            return 0;
        check_arrival(c, chk);
        chk->end_cycle = 0;
    }
    if (!k)
        return 0;
    native_state s, t;
    if (c->nbr || c->spl.active || (c->cr[CR_TSR] & TSR_SPLX) || c->idle_ret_pending || c->imm_n
        || !(k->npend ? pend_match(c, k, s.pend, &s.pend_live) : jit_wq_empty(c)) || c->npst || c->trace
        || (c->idle_armed && !c->idle_fx) || (c->ifr && pending_interrupt(c)))
        return 0;

    native_store st[NATIVE_STORES];
    s.st = st;
    s.bulk_buf = c->native_bulk;
    memcpy(s.reg, c->reg, sizeof s.reg);
    memcpy(s.cr, c->cr, sizeof s.cr);
    s.at = f->at[ki];
    memcpy(s.addr, f->addr[ki], sizeof s.addr);
    if (chk) {
        s.st = chk->st;
        t = s;
        t.cycles = t.packets = t.insns = t.sploops = t.pm_resume = t.nst = t.mv.len = t.nbulk = 0;
        t.branch_block = 5;
        if (k->pass(c, &t)) {
            if (t.mv.len)
                t.mv.hash = fnv(ram_ptr(c, t.mv.src, t.mv.len), t.mv.len);
            for (unsigned i = 0; i < t.nbulk; i++)
                t.bulk[i].hash = fnv(t.bulk_buf + t.bulk[i].off, t.bulk[i].len);
            chk->kernel = k;
            chk->s = t;
            chk->at = c->cycle;
            chk->end_cycle = c->cycle + t.cycles;
            chk->irqs = c->st.interrupts;
            chk->packets = c->st.packets;
            chk->insns = c->st.insns;
            chk->stalls = c->st.stalls;
            chk->sploops = c->st.sploops;
            chk->jit_cycles = c66x_jit_cycles(c);
            chk->wq_base = c->wq_base;
            /* A function's return, or a pass that stops short of the loop's
             * end, lands on a packet that may already be cached as an
             * ordinary one: hand it to the general loop for the compare. */
            pkc_ent *e = &c->pkc[PKC_SLOT(t.pc)];
            if (t.pc != t.at + k->entry && t.pc != t.at + k->exit && e->pc == t.pc)
                e->special = 1;
        }
        return 0;
    }

    uint64_t at = c->cycle, gen = c->code_gen;
    unsigned head_slot[NATIVE_PENDS];
    for (unsigned j = 0; j < k->npend; j++)
        head_slot[j] = wq_slot(c, k->pend[j].ahead);
    int ran = 0;
    for (;;) {
        t = s;
        t.cycles = t.packets = t.insns = t.sploops = t.pm_resume = t.nst = t.mv.len = t.nbulk = 0;
        t.branch_block = 5;
        if (!k->pass(c, &t) || c->cycle + t.cycles > end)
            break;
        for (uint32_t i = 0; i < t.mv.len; i += 4)
            mem_write(c, t.mv.dst + i, ld32(ram_ptr(c, t.mv.src + i, 4)), 4, c->isr_depth != 0, t.mv.pc);
        for (unsigned i = 0; i < t.nst; i++)
            mem_write(c, t.st[i].a, t.st[i].v, t.st[i].n, c->isr_depth != 0, t.st[i].pc);
        for (unsigned i = 0; i < t.nbulk; i++)
            bulk_write(c, &t.bulk[i], t.bulk_buf + t.bulk[i].off, t.last_pc);
        if (t.pm_resume)
            c->pm_resume_at = c->cycle + t.pm_resume;
        c->cycle += t.cycles;
        c->st.packets += t.packets;
        c->st.insns += t.insns;
        c->st.sploops += t.sploops;
        s = t;
        ran = 1;
        if (t.pc != s.at + k->entry || c->code_gen != gen)
            break;
    }
    if (!ran)
        return 0;
    /* The writes in flight at the head land at the same distance from the end. */
    for (unsigned j = 0; j < k->npend; j++)
        c->wqn[head_slot[j]] = 0;
    for (unsigned j = 0; j < k->npend; j++)
        if (s.pend_live >> j & 1) {
            unsigned sl = wq_slot(c, k->pend[j].ahead);
            c->wq[sl][c->wqn[sl]++] = (wq_ent){ WK_REG, k->pend[j].reg, k->pend[j].load, s.pend[j] };
        }
    memcpy(c->reg, s.reg, sizeof c->reg);
    memcpy(c->cr, s.cr, sizeof c->cr);
    c->pc = s.pc;
    c->exec_pc = s.last_pc;
    c->branch_block = s.branch_block;
    c->wmask = 0;
    c->native_entries++;
    c->native_cycles += c->cycle - at;
    return 1;
}

/* C66X_NATIVE=0 runs every kernel on the interpreter; =check runs them there
 * too, and compares each pass with what its native form predicted. */
void native_init(c66x_core *c)
{
    const char *e = getenv("C66X_NATIVE");
    c->native_mode = !e ? 1 : !strcmp(e, "0") ? 0 : !strcmp(e, "check") ? 2 : 1;
    c->native_found = calloc(1, sizeof(native_found));
    c->native_bulk = malloc(NATIVE_BULK_BYTES);
    if (c->native_mode == 2)
        c->native_chk = calloc(1, sizeof(native_check));
}

void native_free(c66x_core *c)
{
    native_check *k = c->native_chk;
    native_found *f = c->native_found;
    if (c->native_mode && c->native_gen) {
        fprintf(stderr, "c66x native: kernels");
        for (unsigned i = 0; i < NKERNELS; i++)
            if (f->matches[i] == 1)
                fprintf(stderr, " %s 0x%08x", kernels[i]->name, f->at[i]);
            else
                fprintf(stderr, " %s (%u matches)", kernels[i]->name, f->matches[i]);
        fputc('\n', stderr);
    }
    if (k)
        fprintf(stderr, "c66x native check: %llu passes checked, %llu differ, %llu interrupted, %llu past the end\n",
                (unsigned long long)k->checked, (unsigned long long)k->bad, (unsigned long long)k->skipped,
                (unsigned long long)k->overrun);
    else if (c->native_entries)
        fprintf(stderr, "c66x native: %llu entries, %llu cycles\n",
                (unsigned long long)c->native_entries, (unsigned long long)c->native_cycles);
    free(k);
    free(f);
    free(c->native_bulk);
}
