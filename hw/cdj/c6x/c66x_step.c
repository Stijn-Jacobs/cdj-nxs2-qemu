/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x core: interrupts, the cycle loop, the fast loop and c66x_step.
 */
#include "c66x_priv.h"

/* ------------------------------------------------------------------------ */
/* interrupts                                                                */

static void take_interrupt(c66x_core *c, int n, uint32_t ret)
{
    uint32_t tsr = c->cr[CR_TSR];
    if (n == C66X_INT_NMI) {
        c->cr[CR_NTSR] = tsr;
        c->cr[CR_NRP] = ret;
        c->cr[CR_IER] &= ~IER_NMIE;
        c->cr[CR_TSR] = (tsr & ~(TSR_SGIE | TSR_XEN | TSR_CXM | TSR_EXC | TSR_SPLX)) | TSR_INT;
    } else {
        c->cr[CR_ITSR] = tsr;
        c->cr[CR_IRP] = ret;
        c->cr[CR_CSR] = (c->cr[CR_CSR] & ~CSR_PGIE) | ((tsr & TSR_GIE) ? CSR_PGIE : 0);
        c->cr[CR_TSR] = (tsr & ~(TSR_GIE | TSR_SGIE | TSR_XEN | TSR_CXM | TSR_EXC | TSR_SPLX)) | TSR_INT;
    }
    c->ifr &= ~(1u << n);
    c->int_entry = INT_ENTRY_CYCLES;
    c->int_vector = (c->cr[CR_ISTP] & ~0x3ffu) + 0x20 * n;
    c->idle = 0;
    c->st.interrupts++;
    if (c->isr_depth++ == 0) {
        c->idle_resume_pc = ret;
        c->idle_irq_captured = 0;
        c->idle_ret_pending = 0;
    }
}

/* Registers as they stand once every write in flight has landed. */
static void project_regs(const c66x_core *c, uint32_t *out)
{
    memcpy(out, c->reg, sizeof c->reg);
    for (unsigned d = 0; d < WQ_SLOTS; d++) {
        unsigned s = wq_slot(c, (int)d - 1);
        for (unsigned i = 0; i < c->wqn[s]; i++)
            if (c->wq[s][i].kind == WK_REG)
                out[c->wq[s][i].idx] = c->wq[s][i].val;
        if (d == 0)
            for (unsigned i = 0; i < c->imm_n; i++)
                if (c->imm[i].kind == WK_REG)
                    out[c->imm[i].idx] = c->imm[i].val;
    }
}

/* A return from interrupt (B IRP / B NRP) executed. */
void idle_isr_return(c66x_core *c, uint32_t target)
{
    if (!c->isr_depth || --c->isr_depth)
        return;
    if (c->idle_fp && !c->idle_isr_hit && c->idle_irq_captured && target == c->idle_resume_pc)
        c->idle_ret_pending = 1;
    else
        c->idle_fp = 0;
}

/* ------------------------------------------------------------------------ */
/* the cycle loop                                                            */


int gather_packet(c66x_core *c, uint32_t pc, c66x_insn **pk, unsigned *n,
                         uint32_t *next)
{
    unsigned hslot = PKC_SLOT(pc);
    c66x_insn *first = c->pkc[hslot].pc == pc ? c->pkc[hslot].first : NULL;
    HC(gather, first ? 0 : 1);
    if (first && c->pkc[hslot].xins) {
        const pkc_ent *pe = &c->pkc[hslot];
        memcpy(pk, pe->xins, pe->n * sizeof pk[0]);
        *n = pe->n;
        *next = pe->next;
        return 0;
    }
    if (!first) {
        first = fetch_insn(c, pc);
        if (first && first->pk_n) {
            c->pkc[hslot] = (pkc_ent){ pc, first->pk_next, first, NULL, first->pk_n,
                                       first->pk_special, first->pk_xread, first->pk_load,
                                       first->pk_xnops, first->pk_branched, first->pk_allfop,
                                       first->pk_xmask, jit_lookup(c, pc), jit_lookup0(c, pc) };
            c->pkc[hslot].qregion = jit_lookupq(c, pc);
        }
    }
    if (first && first->pk_n) {
        /* Cached: the packet lies inside this fetch packet, so its instructions
         * are consecutive slots of one block, all still decoded (invalidation
         * clears pk_n for the whole block). */
        c66x_insn *in = first;
        for (unsigned i = 0; i < first->pk_n; i++) {
            pk[i] = in;
            in += in->size >> 1;
        }
        *n = first->pk_n;
        *next = first->pk_next;
        return 0;
    }
    uint32_t a = pc;
    int in_block = 1;
    *n = 0;
    for (;;) {
        c66x_insn *in = fetch_insn(c, a);
        if (!in) {
            c->trap_pc = a;
            return C66X_STOP_FAULT;
        }
        if (in->opc == -2 || in->size == 1) {
            c->trap_pc = a;
            return C66X_STOP_FAULT;
        }
        if (in->opc == -1) {
            c->trap_pc = a;
            return C66X_STOP_UNDEF;
        }
        if ((a & ~31u) != (pc & ~31u))
            in_block = 0;
        if (*n < MAX_PK)
            pk[(*n)++] = in;
        a += in->size;
        if (in->compact && (a & 31) == 28) {
            c66x_insn *hd = fetch_insn(c, a);
            if (hd && hd->opc == -2)
                a += 4;
        }
        if (!in->p)
            break;
    }
    *next = a;
    HC(gather, in_block ? 3 : 2);
    if (*n >= MAX_PK)
        return 0;
    pkc_ent e = { pc, a, pk[0], NULL, *n, 0, 0, 0, 0, 0, 1, 0 };
    uint8_t mask = 0;
    for (unsigned i = 0; i < *n; i++) {
        unsigned h = pk[i]->handler;
        if (pk[i]->xnops > e.xnops)
            e.xnops = pk[i]->xnops;
        e.branched |= pk[i]->isbranch;
        e.allfop &= pk[i]->fop != 0;
        for (unsigned k = 0; k < pk[i]->nops; k++) {
            const c66x_operand *o = &pk[i]->op[k];
            if (!o->xpath || o->rw == tic6x_rw_write)
                continue;
            e.xmask |= 1ULL << o->reg;
            if (o->kind == C66X_OPK_PAIR)
                e.xmask |= 1ULL << o->reg_hi;
        }
        if (h == H_SPMASK && pk[i]->nops)
            mask |= pk[i]->op[0].val;
        e.xread |= pk[i]->xread;
        e.load |= h == H_LOAD;
        if (h == H_SPLOOP || h == H_SPLOOPD || h == H_SPLOOPW || h == H_SPKERNEL
            || h == H_SPKERNELR || h == H_SPMASK || h == H_SPMASKR)
            e.special = 1;
    }
    if (in_block) {
        first = pk[0];
        first->pk_next = a;
        first->pk_mask = mask;
        first->pk_xread = e.xread;
        first->pk_special = e.special;
        first->pk_load = e.load;
        first->pk_xnops = e.xnops;
        first->pk_branched = e.branched;
        first->pk_allfop = e.allfop;
        first->pk_xmask = e.xmask;
        first->pk_n = *n;
    } else {
        /* The general loop keeps computing SPMASK and stalls itself for these
         * (pk_n stays 0); only the fast loop runs them from the list. A SPLOOP
         * packet is cached too, so fast_cycles can reach its compiled invocation
         * (0x008003DC and 0x0080B01C cross a fetch packet); the fast loop
         * itself still hands every special packet back. */
        if (c->nxpk == XPK_SIZE) {
            memset(c->pkc, 0, sizeof c->pkc);
            c->nxpk = 0;
            c->code_gen++;
            c->xpk_flushes++;
        }
        xpk_ent *x = &c->xpk[c->nxpk++];
        memcpy(x->in, pk, *n * sizeof pk[0]);
        e.xins = x->in;
        e.region = jit_lookup(c, pc);
        e.region0 = jit_lookup0(c, pc);
        e.qregion = jit_lookupq(c, pc);
        c->pkc[hslot] = e;
    }
    return 0;
}

static int needs_stall(c66x_core *c, c66x_insn **pk, unsigned n)
{
    int any = 0;
    for (unsigned i = 0; i < n; i++)
        any |= pk[i]->xread;
    if (!any)
        return 0;
    for (unsigned i = 0; i < n; i++)
        for (unsigned k = 0; k < pk[i]->nops; k++) {
            const c66x_operand *o = &pk[i]->op[k];
            if (!o->xpath || o->rw == tic6x_rw_write)
                continue;
            if ((c->wmask >> o->reg) & 1)
                return 1;
            if (o->kind == C66X_OPK_PAIR && ((c->wmask >> o->reg_hi) & 1))
                return 1;
        }
    return 0;
}

static inline void land_branches(c66x_core *c)
{
    for (unsigned i = 0; i < c->nbr; i++)
        c->br[i].remaining--;
    if (c->br[0].remaining <= 0) {
        c->pc = c->br[0].target;
        c->mcnop = 0;
        if (c->spl.active)
            c->spl.active = 0, c->cr[CR_TSR] &= ~TSR_SPLX;
        memmove(c->br, c->br + 1, (c->nbr - 1) * sizeof c->br[0]);
        c->nbr--;
    }
}

enum { FAST_BUDGET, FAST_STOP, FAST_HANDOFF };

/* The common cycle, without the general loop's machinery: no loop buffer, no
 * pending interrupt flag, no interrupt entry or IDLE, no trace/hook/idle head,
 * program fetch enabled, and a cached packet with no SPLOOP-family instruction.
 * Each cycle commits its writes first; a cycle that fails a condition is handed
 * back to the general loop already committed (FAST_HANDOFF). */
static int fast_cycles(c66x_core *c, uint64_t end, c66x_stop *stop)
{
    for (;;) {
        if (c->cycle >= end)
            return FAST_BUDGET;
        if (__builtin_expect(c->jit_committed, 0))
            c->jit_committed = 0;
        else
            commit_writes(c);
        /* A flag for an interrupt that cannot be taken (masked, GIE off)
         * changes nothing the general loop would do: gate on a deliverable one. */
        if (c->spl.active | c->int_entry | c->idle | c->spl_irq_pending
            | (c->trace != NULL) | c->nhooks | (c->cycle < c->pm_resume_at)
            || (c->ifr && pending_interrupt(c)))
            return FAST_HANDOFF;
        if (c->mcnop > 0) {
            /* A run of NOP cycles whose commits would all be empty and in which
             * at most the last cycle lands a branch is one step. */
            uint64_t m = (uint64_t)c->mcnop;
            HC(path, 1);
            if (end - c->cycle < m)
                m = end - c->cycle;
            if (c->nbr && (uint64_t)c->br[0].remaining < m)
                m = c->br[0].remaining;
            uint64_t ok = 1;
            while (ok < m && c->wqn[wq_slot(c, (int)ok - 1)] == 0)
                ok++;
            m = ok;
            c->mcnop -= (int)m;
            if (m > 1) {
                c->wmask = 0;
                c->cycle += m - 1;
                for (unsigned i = 0; i < c->nbr; i++)
                    c->br[i].remaining -= (int)(m - 1);
            }
        } else {
            if (c->idle_head && (c->pc == c->idle_head
                                 || (c->idle_ret_pending && c->pc == c->idle_resume_pc)))
                return FAST_HANDOFF;
            uint32_t pc = c->pc;
            const pkc_ent *pe = &c->pkc[PKC_SLOT(pc)];
            if (pe->pc != pc || !pe->first)
                return FAST_HANDOFF;
            if (pe->special) {
                /* A SPLOOP whose whole invocation is compiled: the general
                 * loop would otherwise run its loading, ramp and drain. */
                if (!c->jit || !c->jit->nloops || c->nbr || c->idle_ret_pending
                    || (c->cr[CR_TSR] & TSR_SPLX) || (pe->xmask & c->wmask))
                    return FAST_HANDOFF;
                const c66x_jit_loop *lp = jit_loop_lookup(c, pc);
                if (!lp)
                    return FAST_HANDOFF;
                uint64_t at = c->cycle;
                lp->fn(c, end);
                if (c->cycle == at) {
                    c->jit->ldeclined++;
                    return FAST_HANDOFF;
                }
                c->jit->lentries++;
                c->jit->lcycles += c->cycle - at;
                continue;
            }
            c66x_insn *first = pe->first;
            unsigned n = pe->n;
            if (pe->xmask & c->wmask) {
                c->st.stalls++;
                c->wq_base--;
                c->cycle++;
                continue;
            }
            if (c->jit) {
                if (!c->nbr && !c->idle_ret_pending && (pe->region || pe->region0)) {
                    uint64_t at = c->cycle;
                    if (pe->region0 && !c->imm_n && jit_wq_empty(c)) {
                        c->jit->entries0++;
                        pe->region0->fn(c, end);
                    } else if (pe->region) {
                        c->jit->entries++;
                        pe->region->fn(c, end);
                    } else {
                        goto interpret;
                    }
                    c->jit->cycles += c->cycle - at;
                    continue;
                }
                if (c->nbr == 1 && pe->qregion && !c->idle_ret_pending
                    && c->br[0].remaining == pe->qregion->rem && c->br[0].target == pe->qregion->target) {
                    uint64_t at = c->cycle;
                    c->jit->qentries++;
                    pe->qregion->r.fn(c, end);
                    c->jit->qcycles += c->cycle - at;
                    continue;
                }
            interpret:
                if (c->jit->prof)
                    jit_prof(c, pc);
                else if (c->jit->auto_dir) {
                    ((pkc_ent *)pe)->runs++;
                    ((pkc_ent *)pe)->hits += !c->nbr;
                }
            }
            c->exec_pc = pc;
            c->pc = pe->next;
            c->st.packets++;
            HC(path, 0);
            HCPC(pc, 0, 0, 0);
            c66x_insn *in = first;
            c->store_now = !pe->load;
            if (pe->allfop) {
                /* The common packet: fast forms only, bookkeeping precomputed. */
                c->st.insns += n;
                for (unsigned i = 0; i < n; i++, in = pe->xins ? pe->xins[i] : in + (in->size >> 1)) {
                    int r = fop_run(c, in, pe->next);
                    if (r) {
                        c->store_now = 0;
                        flush_stores(c);
                        *stop = r;
                        c->cycle++;
                        return FAST_STOP;
                    }
                }
                c->store_now = 0;
                if (c->npst)
                    flush_stores(c);
                if (pe->xnops > c->mcnop)
                    c->mcnop = pe->xnops;
                c->branch_block = pe->branched ? 5 : (c->branch_block ? c->branch_block - 1 : 0);
                if (c->nbr)
                    land_branches(c);
                c->cycle++;
                continue;
            }
            xctx x = { .next_pc = pe->next };
            for (unsigned i = 0; i < n; i++, in = pe->xins ? pe->xins[i] : in + (in->size >> 1)) {
                if (in->fop) {
                    fop_exec(c, in, &x);
                    continue;
                }
                x.pce1 = in->addr & ~31u;
                exec_insn(c, in, &x);
                if (x.stop) {
                    c->store_now = 0;
                    flush_stores(c);
                    *stop = x.stop;
                    c->cycle++;
                    return FAST_STOP;
                }
            }
            c->store_now = 0;
            if (c->npst)
                flush_stores(c);
            if (x.extra_nops > c->mcnop)
                c->mcnop = x.extra_nops;
            c->branch_block = x.branched ? 5 : (c->branch_block ? c->branch_block - 1 : 0);
        }
        if (c->nbr)
            land_branches(c);
        c->cycle++;
    }
}

/* A loop kernel between its SPKERNEL and its exit: program fetch is off, so a
 * cycle is the buffered instructions of the iterations overlapping it plus a
 * stage-boundary test every ii cycles (spl_end_cycle). The general loop does the
 * same with a division per cycle and the program-memory machinery around it.
 * Entered with this cycle's writes committed; hands back committed too. */
/* A terminated loop still draining with program-memory fetch off runs nothing
 * the general loop would add: no packet is fetched, so no interrupt is taken and
 * no new loop starts. */
static inline int spl_drain_fast(const c66x_core *c)
{
    const spl_state *s = &c->spl;
    return s->terminated & !s->int_drain & !s->abrupt & (c->cycle < c->pm_resume_at
           || c->cycle < s->drain_start + s->fetch_delay);
}

static inline int spl_fast_ok(const c66x_core *c)
{
    const spl_state *s = &c->spl;
    return s->active & !s->loading & (!s->terminated | spl_drain_fast(c)) & !s->abrupt & !s->initial_term
           & (s->ii > 0) & (c->cycle >= s->t0) & !c->nbr & !c->int_entry & !c->idle & !c->spl_irq_pending
           & (c->trace == NULL) & !c->nhooks;
}

static int spl_fast_cycles(c66x_core *c, uint64_t end, c66x_stop *stop)
{
    spl_state *s = &c->spl;
    uint32_t ii = (uint32_t)s->ii;
    uint64_t r = c->cycle - s->t0;
    uint64_t k = r / ii;
    uint32_t off = (uint32_t)(r - k * ii);
    uint64_t nstage = ((uint32_t)s->dynlen + ii - 1) / ii;
    if (!s->steady_built && (uint32_t)s->dynlen > ii) {
        /* spl_buffer_insns' order: oldest iteration (largest body offset) first */
        for (uint32_t o = 0; o < ii; o++) {
            spl_steady *st = &c->steady[o];
            st->n = 0;
            for (int64_t j = (int64_t)nstage - 1; j >= 0; j--) {
                int64_t bo = o + j * (int64_t)ii;
                if (bo >= s->dynlen)
                    continue;
                const spl_ent *e = &s->body[bo];
                for (unsigned i = 0; i < e->n && st->n < MAX_PK; i++)
                    st->in[st->n++] = e->insn[i];
            }
        }
        s->steady_built = 1;
    }
    if (c->jit && !s->kr_checked) {
        s->kr_checked = 1;
        s->kregion = c->jit->nkern ? jit_kernel_lookup(c) : NULL;
        if (c->jit->kprof)
            jit_kprof(c);
    }
    uint64_t prof_at = c->cycle;
    for (;;) {
        /* The drain, compiled: one list per (offset, stages drained). It must stop
         * before program-memory fetch comes back, which is when the post-loop code
         * starts running beside the buffer. */
        if (s->kregion && s->kregion->drain_fn && s->terminated && !s->int_drain && !s->abrupt
            && !s->initial_term && c->mcnop == 0 && !c->nbr && k > (uint64_t)s->last_iter
            && k - (uint64_t)s->last_iter < nstage) {
            uint64_t lim = end;
            uint64_t pm = c->pm_resume_at, fd = s->drain_start + (uint64_t)s->fetch_delay;
            if (pm > c->cycle && pm < lim)
                lim = pm;
            if (fd > c->cycle && fd < lim)
                lim = fd;
            if (lim > c->cycle) {
                uint64_t at = c->cycle;
                c->jit->dentries++;
                s->kregion->drain_fn(c, lim);
                c->jit->dcycles += c->cycle - at;
                if (c->jit->kprof)
                    jit_kprof_cycles(c, c->cycle - prof_at);
                return FAST_BUDGET;
            }
        }
        if (s->kregion && k >= nstage && k <= (uint64_t)s->last_iter && c->mcnop == 0 && c->cycle - s->t0 >= 3) {
            /* the compiled kernel, from this offset; it returns before a commit */
            uint64_t at = c->cycle;
            c->jit->kentries++;
            s->kregion->fn(c, end);
            c->jit->kcycles += c->cycle - at;
            /* Carry on here while the loop is still ours, saving a round trip
             * through c66x_step and fast_cycles per kernel entry. */
            if (c->cycle >= end) {
                if (c->jit->kprof)
                    jit_kprof_cycles(c, c->cycle - prof_at);
                return FAST_BUDGET;
            }
            /* the commit fast_cycles would have done on the way back in */
            if (__builtin_expect(c->jit_committed, 0))
                c->jit_committed = 0;
            else
                commit_writes(c);
            if (!spl_fast_ok(c)) {
                if (c->jit->kprof)
                    jit_kprof_cycles(c, c->cycle - prof_at);
                return FAST_HANDOFF;
            }
            r = c->cycle - s->t0;
            k = r / ii;
            off = (uint32_t)(r - k * ii);
            continue;
        }
        HC(path, 2);
        HCPC(s->addr, 1, s->ii, s->dynlen);
        if (s->creg >= 0)
            c->cond_hist[c->cycle & 7] = c->reg[s->creg];
        if (c->mcnop > 0)
            c->mcnop--;
        xctx bx = { 0 };
        bx.is_buffer = 1;
        if ((uint32_t)s->dynlen <= ii) {
            if (k >= 1 && k <= (uint64_t)s->last_iter && off < (uint32_t)s->dynlen) {
                spl_ent *e = &s->body[off];
                for (unsigned i = 0; i < e->n; i++) {
                    c66x_insn *in = e->insn[i];
                    if (in->fop) {
                        fop_exec(c, in, &bx);
                        continue;
                    }
                    bx.pce1 = in->addr & ~31u;
                    exec_insn(c, in, &bx);
                }
            }
        } else if (k >= nstage && k <= (uint64_t)s->last_iter) {
            const spl_steady *st = &c->steady[off];
            for (unsigned i = 0; i < st->n; i++) {
                c66x_insn *in = st->in[i];
                if (in->fop) {
                    fop_exec(c, in, &bx);
                    continue;
                }
                bx.pce1 = in->addr & ~31u;
                exec_insn(c, in, &bx);
            }
        } else {
            c66x_insn *buf[MAX_PK];
            unsigned nbuf = spl_buffer_insns(c, buf, 0);
            for (unsigned i = 0; i < nbuf; i++) {
                if (buf[i]->fop) {
                    fop_exec(c, buf[i], &bx);
                    continue;
                }
                bx.pce1 = buf[i]->addr & ~31u;
                exec_insn(c, buf[i], &bx);
            }
        }
        if (c->npst)
            flush_stores(c);
        if (bx.stop) {
            *stop = bx.stop;
            c->cycle++;
            return FAST_STOP;
        }
        /* The general loop ends every cycle of a loop that is not loading, not
         * only a stage boundary: a drained loop goes inactive between boundaries. */
        if (off + 1 == ii || s->terminated)
            spl_end_cycle(c, NULL, 0, 0, NULL);
        if (c->nbr)
            land_branches(c);
        c->cycle++;
        if (++off == ii) {
            off = 0;
            k++;
        }
        if (c->cycle >= end) {
            if (c->jit && c->jit->kprof)
                jit_kprof_cycles(c, c->cycle - prof_at);
            return FAST_BUDGET;
        }
        commit_writes(c);
        if (!spl_fast_ok(c)) {
            if (c->jit && c->jit->kprof)
                jit_kprof_cycles(c, c->cycle - prof_at);
            return FAST_HANDOFF;
        }
    }
}

c66x_stop c66x_step(c66x_core *c, uint64_t budget, uint64_t *executed)
{
    uint64_t start = c->cycle;
    c66x_stop stop = C66X_STOP_BUDGET;
    c66x_insn *pm[MAX_PK], *buf[MAX_PK];

    if (c->jit && c->jit->auto_dir)
        jit_auto_poll(c);
    if (c->rec && c->cycle >= c->rec_hash_at) {
        uint64_t h = c66x_state_hash(c);
        rec_head(c, REC_HASH);
        fwrite(&c->pc, 4, 1, c->rec);
        fwrite(&h, 8, 1, c->rec);
        c->rec_hash_at = c->cycle + REC_HASH_EVERY;
    }

    while (c->cycle - start < budget) {
        /* Every cycle starts in the fast loop, which commits the cycle's writes
         * and hands back only a cycle it cannot run. */
        switch (fast_cycles(c, start + budget, &stop)) {
        case FAST_BUDGET:
            continue;
        case FAST_STOP:
            goto done;
        default:
            break;
        }
        if (spl_fast_ok(c)) {
            int r = spl_fast_cycles(c, start + budget, &stop);
            if (r == FAST_BUDGET)
                continue;
            if (r == FAST_STOP)
                goto done;
        }
        if (c->spl.active && c->spl.creg >= 0)
            c->cond_hist[c->cycle & 7] = c->reg[c->spl.creg];

        /* Every cycle that reaches the end of this loop is a branch delay slot;
         * stalls and interrupt entry leave it early. */
        int slot_cycle = 1;
        unsigned npm = 0, nbuf = 0;
        HC(path, 3);
        HCPC(c->spl.active ? c->spl.addr : c->pc, c->spl.active ? 2 : 3, c->spl.ii, c->spl.dynlen);
#ifdef C66X_HCOUNT
        /* why this cycle left the fast loops */
        if (c->spl.active) HC(path, 5);
        else if (c->int_entry | c->idle | c->spl_irq_pending) HC(path, 6);
        else if (c->ifr && !pending_interrupt(c)) HC(path, 7);
#endif
        uint32_t mask = 0;
        xctx x = { 0 };

        if (c->int_entry) {
            if (--c->int_entry == 0) {
                c->pc = c->int_vector;
                c->nbr = 0;
                c->mcnop = 0;
                if (c->isr_depth == 1 && c->idle_fp) {
                    project_regs(c, c->idle_irq_reg);
                    c->idle_irq_captured = 1;
                }
            }
            c->cycle++;
            continue;
        }

        int pm_ok = spl_pm_enabled(c);

        if (c->idle) {
            int n = pending_interrupt(c);
            if (!n) {
                stop = C66X_STOP_IDLE;
                break;
            }
            take_interrupt(c, n, c->pc);
            c->cycle++;
            continue;
        }

        if (c->mcnop > 0) {
            c->mcnop--;
            slot_cycle = 1;
        } else if (pm_ok) {
            /* Interrupts are taken only between packets, outside branch delay
             * slots and the five packets after a branch (SPRU732 5.5.2). */
            int n = c->ifr ? pending_interrupt(c) : 0;
            if (n) {
                if (c->spl.active)
                    c->st.irq_wait_sploop++;
                else if (c->nbr || c->branch_block)
                    c->st.irq_wait_branch++;
                else {
                    take_interrupt(c, n, c->spl_irq_pending ? c->spl_irq_ret : c->pc);
                    if (c->spl_irq_pending) {
                        c->cr[n == C66X_INT_NMI ? CR_NTSR : CR_ITSR] |= TSR_SPLX;
                        c->spl_irq_pending = 0;
                    }
                    c->cycle++;
                    continue;
                }
            }
            /* The interrupt that drained a loop went away meanwhile (7.13.6):
             * execution simply continues after the loop. */
            c->spl_irq_pending = 0;
            /* A pending interrupt waiting out branch slots is not idleness: the
             * packets until it is taken must run. */
            if (c->idle_ret_pending && c->pc == c->idle_resume_pc && !c->mcnop && !c->nbr) {
                uint32_t pr[C66X_NREGS];
                c->idle_ret_pending = 0;
                project_regs(c, pr);
                if (memcmp(pr, c->idle_irq_reg, sizeof pr))
                    c->idle_fp = 0;
                else if (c->idle_fp && !n) {
                    c->idle_info.resume_idles++;
                    stop = C66X_STOP_IDLE;
                    break;
                }
            }
            if (c->pc == c->idle_head && c->idle_head && !c->mcnop && !c->nbr && !n && idle_check(c)) {
                stop = C66X_STOP_IDLE;
                break;
            }
            hook *hk = c->nhooks ? find_hook(c, c->pc) : NULL;
            if (hk) {
                if (hk->fn(c, hk->opaque)) {
                    stop = C66X_STOP_HOOK;
                    c->cycle++;
                    break;
                }
                c->cycle++;
                c->st.packets++;
                continue;
            }
            uint32_t next;
            int err = gather_packet(c, c->pc, pm, &npm, &next);
            if (err) {
                stop = err;
                break;
            }
            int cached = pm[0]->pk_n && pm[0]->pk_next == next;
            if ((!cached || pm[0]->pk_xread) && needs_stall(c, pm, npm)) {
                c->st.stalls++;
                c->wq_base--;
                c->cycle++;
                continue;
            }
            c->exec_pc = c->pc;
            if (c->trace)
                c->trace(c, c->trace_opaque, c->pc);
            x.pce1 = c->pc & ~31u;
            x.next_pc = next;
            c->pc = next;
            slot_cycle = 1;
            c->st.packets++;
            if (cached)
                mask = pm[0]->pk_mask;
            else
                for (unsigned i = 0; i < npm; i++)
                    if (pm[i]->handler == H_SPMASK && pm[i]->nops)
                        mask |= pm[i]->op[0].val;
        }

        if (c->spl.active)
            nbuf = spl_buffer_insns(c, buf, mask);

        /* Loop-buffer instructions first: the program-memory packet may start
         * a new SPLOOP that replaces the buffer state. */
        xctx bx = { 0 };
        bx.is_buffer = 1;
        for (unsigned i = 0; i < nbuf; i++) {
            if (buf[i]->fop) {
                fop_exec(c, buf[i], &bx);
                continue;
            }
            bx.pce1 = buf[i]->addr & ~31u;
            exec_insn(c, buf[i], &bx);
        }

        int spl_was_loading = c->spl.active && c->spl.loading;
        int initial_term = c->spl.active && c->spl.initial_term && c->spl.loading;
        /* Pipe-up after an interrupt (SPRU732 7.13.5): the SPLOOP packet's other
         * instructions and SPMASKed program-memory instructions act as NOPs. */
        int resuming = (c->cr[CR_TSR] & TSR_SPLX) && !c->spl.active;
        int resumed_load = spl_was_loading && c->spl.resumed;
        for (unsigned i = 0; i < npm; i++) {
            c66x_insn *in = pm[i];
            if (initial_term && !(in->unit >= 0 && (mask & (1u << in->unit)))
                && in->handler != H_SPKERNEL && in->handler != H_SPMASK && in->handler != H_NOP)
                continue;           /* ILC was 0: the body executes as NOPs */
            if (resuming && in->handler != H_SPLOOP && in->handler != H_SPLOOPD
                && in->handler != H_SPLOOPW) {
                int has_loop = 0;
                for (unsigned k = 0; k < npm; k++)
                    has_loop |= pm[k]->handler == H_SPLOOP || pm[k]->handler == H_SPLOOPD
                                || pm[k]->handler == H_SPLOOPW;
                if (has_loop)
                    continue;
            }
            if (resumed_load && in->unit >= 0 && (mask & (1u << in->unit)))
                continue;
            x.pce1 = in->addr & ~31u;   /* a packet may span two fetch packets */
            if (in->fop)
                fop_exec(c, in, &x);
            else
                exec_insn(c, in, &x);
            if (x.stop) {
                stop = x.stop;
                break;
            }
        }
        if (c->npst)
            flush_stores(c);
        if (stop != C66X_STOP_BUDGET) {
            c->cycle++;
            break;
        }
        if (bx.stop) {
            stop = bx.stop;
            c->cycle++;
            break;
        }

        if (c->spl.active && (spl_was_loading || !c->spl.loading))
            spl_end_cycle(c, pm, spl_was_loading ? npm : 0, mask, NULL);

        if (npm) {
            if (x.extra_nops > c->mcnop)
                c->mcnop = x.extra_nops;
            c->branch_block = x.branched ? 5 : (c->branch_block ? c->branch_block - 1 : 0);
        }

        if (slot_cycle) {
            /* A branch recorded this cycle has remaining = 6: this cycle is its E1. */
            for (unsigned i = 0; i < c->nbr; i++)
                c->br[i].remaining--;
            if (c->nbr && c->br[0].remaining <= 0) {
                c->pc = c->br[0].target;
                c->mcnop = 0;
                if (c->spl.active)
                    c->spl.active = 0, c->cr[CR_TSR] &= ~TSR_SPLX;
                memmove(c->br, c->br + 1, (c->nbr - 1) * sizeof c->br[0]);
                c->nbr--;
            }
        }
        c->cycle++;
    }
done:
    c->st.cycles = c->cycle;
    if (executed)
        *executed = c->cycle - start;
    return stop;
}
