/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Experimental fusion of a two-cycle, two-value-latency RAM copy pipeline.
 * The generator must recognize the entire mature cycle pair, not its PC.
 * The core serializes setters and posted host writes between step budgets.
 * No callback, code invalidation or deliverable interrupt may occur here.
 * A miss performs no state mutation; the original generated states run.
 */
static uint64_t jit_copy_bursts, jit_copy_burst_cycles;
static uint64_t jit_copy_misses[10], jit_copy_page_misses[4];
static inline unsigned jit_copy_miss(unsigned reason) { jit_copy_misses[reason]++; return 0; }

static const ramreg *jit_copy_region(const c66x_core *c, uint32_t a,
                                     unsigned preferred, uint32_t n,
                                     unsigned *selected)
{
    const ramreg *r = &c->ram[preferred];
    if (r->size >= 4 && a - r->base <= r->size - 4) {
        if (r->size < n || a - r->base > r->size - n) return NULL;
        *selected = preferred;
        return r;
    }
    for (unsigned i = 0; i < c->nram; i++) {
        r = &c->ram[i];
        if (r->size >= 4 && a - r->base <= r->size - 4) {
            if (r->size < n || a - r->base > r->size - n) return NULL;
            *selected = i;
            return r;
        }
    }
    return NULL;
}

/* The exact no-effect case of idle_rs_maybe, evaluated once for the
 * union of stores. It indexes by mapping-relative word offset, so it covers
 * aliases too. A positive bloom result always keeps the original callbacks.
 */
static inline int jit_copy_idle_quiet(const c66x_core *c, uint32_t off, uint32_t n)
{
    if (!c->idle_rs_n || c->idle_rs_full) return 1;
    uint32_t w = off >> 2, last = (off + n - 1) >> 2;
    while (w <= last) {
        uint32_t stop = w | 31u;
        if (stop > last) stop = last;
        uint32_t mask = (UINT32_MAX << (w & 31)) & (UINT32_MAX >> (31 - (stop & 31)));
        if (c->idle_rs_bloom[(w >> 5) & (IDLE_RS_BLOOM / 32 - 1)] & mask) return 0;
        w = stop + 1;
    }
    return 1;
}

static __attribute__((noinline)) unsigned jit_copy_burst(
    c66x_core *c, uint64_t end, uint64_t gen,
    unsigned src_reg, unsigned dst_reg, unsigned out_reg,
    uint32_t dst, uint64_t q0, uint64_t q1, uint64_t tail[2])
{
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
    return 0;
#else
    if (c->cycle >= end || c->code_gen != gen) return jit_copy_miss(4);
    if (c->watch_fn || c->trace || c->nhooks) return jit_copy_miss(0);
    if (c->store_now || c->npst || c->imm_n || c->nbr || !jit_wq_empty(c)
        || c->mcnop || c->branch_block) return jit_copy_miss(1);
    if ((c->idle_armed && !c->idle_fx) || (c->idle_isr_disarmed && !c->idle_fx))
        return jit_copy_miss(2);
    if (irq_possible(c) && pending_interrupt(c)) return jit_copy_miss(3);
    uint64_t k = (end - c->cycle) / 2;
    if (k > c->cr[CR_ILC]) k = c->cr[CR_ILC];
    if (k > 256) k = 256;
    if (k < 4) return jit_copy_miss(4);
    uint32_t n = (uint32_t)k * 8, src = c->reg[src_reg];
    /* Do not wrap either architectural pointer, including its final update. */
    if (src > UINT32_MAX - n || dst > UINT32_MAX - n) return jit_copy_miss(5);
    unsigned si, di;
    const ramreg *sr = jit_copy_region(c, src, c->last_ram, n, &si);
    if (!sr) return jit_copy_miss(6);
    const ramreg *dr = jit_copy_region(c, dst, si, n, &di);
    if (!dr) return jit_copy_miss(7);
    uint8_t *s = sr->host + (src - sr->base), *d = dr->host + (dst - dr->base);
    uintptr_t sa = (uintptr_t)s, da = (uintptr_t)d;
    if (sa <= da ? da - sa < n : sa - da < n) return jit_copy_miss(8);
    unsigned p0 = (dst - dr->base) >> FP_PAGE_SHIFT;
    unsigned p1 = (dst + n - 1 - dr->base) >> FP_PAGE_SHIFT;
    int idle_checked = 0;
    for (unsigned p = p0; p <= p1; p++) {
        uint8_t cp = dr->codepage[p];
        if (cp & ~CP_IDLE) {
            jit_copy_page_misses[cp & 3]++;
            return jit_copy_miss(9);
        }
        if ((cp & CP_IDLE) && !idle_checked) {
            if (!jit_copy_idle_quiet(c, dst - dr->base, n)) {
                jit_copy_page_misses[cp & 3]++;
                return jit_copy_miss(9);
            }
            idle_checked = 1;
        }
    }
    /* The first two stores come from the live pipeline, not source RAM.
     * Reads precede stores, and source/destination are disjoint host ranges. */
    memcpy(&tail[0], s + n - 16, 8);
    memcpy(&tail[1], s + n - 8, 8);
    uint64_t last;
    memcpy(&last, s + n - 24, 8);
    memcpy(d, &q0, 8);
    memcpy(d + 8, &q1, 8);
    memcpy(d + 16, s, n - 16);
    c->reg[src_reg] = src + n;
    c->reg[dst_reg] = dst + n - 8;
    c->reg[out_reg] = (uint32_t)last;
    c->reg[out_reg + 1] = (uint32_t)(last >> 32);
    c->last_ram = di;
    c->cr[CR_ILC] -= (uint32_t)k;
    c->cycle += 2 * k;
    c->spl.last_iter = (int)((c->cycle - c->spl.t0) / 2);
    jit_copy_bursts++;
    jit_copy_burst_cycles += 2 * k;
    return (unsigned)k;
#endif
}

__attribute__((destructor)) static void jit_copy_burst_report(void)
{
    if (getenv("C66X_JIT_BULK_REPORT")) {
        fprintf(stderr, "c66x copy burst: %llu batches, %llu exact cycles\n",
                (unsigned long long)jit_copy_bursts,
                (unsigned long long)jit_copy_burst_cycles);
        fprintf(stderr, "c66x copy misses:");
        for (unsigned i = 0; i < 10; i++) fprintf(stderr, " %u=%llu", i, (unsigned long long)jit_copy_misses[i]);
        fprintf(stderr, " pages code=%llu idle=%llu both=%llu\n",
                (unsigned long long)jit_copy_page_misses[1],
                (unsigned long long)jit_copy_page_misses[2],
                (unsigned long long)jit_copy_page_misses[3]);
    }
}
