/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x core: compiled regions (loading, lookup, profile, C66X_JIT_AUTO) and
 * what tools/c14_jitgen.py asks the core.
 */
#include "c66x_priv.h"

/* ------------------------------------------------------------------------ */
/* compiled regions: loading, lookup, profile                                */

#ifndef C66X_JIT_TOOLS
#define C66X_JIT_TOOLS "tools"      /* the Makefile passes the source tree's */
#endif
#ifndef C66X_JIT_LIBDIR
#define C66X_JIT_LIBDIR "."
#endif



/* The same names for c66x_jit_describe (tools/c14_jitgen.py matches on them). */
static const char *const fop_names[] = {
    "none", "nop", "mvk", "mvkh", "mv", "addk", "addkpc",
    "add", "sub", "and", "or", "xor", "cmpeq", "cmpgt", "cmplt", "cmpgtu", "cmpltu",
    "shl", "shr", "shru", "ext", "extu",
    "load", "store", "branch", "callp", "mvc",
    "addsp", "subsp", "mpysp",
};

static uint32_t api_mem_read(c66x_core *c, uint32_t a, unsigned n) { return mem_read(c, a, n); }
static void api_store_defer(c66x_core *c, uint32_t a, uint32_t v, unsigned n) { store_defer(c, a, v, n); }
static void api_flush_stores(c66x_core *c) { flush_stores(c); }
/* Compiled stores call this after landing in a page codepage marks. */
static void api_invalidate_code(c66x_core *c, uint32_t a, uint32_t n)
{
    const ramreg *r = &c->ram[c->last_ram];
    uint8_t cp = a - r->base < r->size ? r->codepage[(a - r->base) >> FP_PAGE_SHIFT] : CP_CODE;
    page_stored(c, cp, a, n);
}
static void api_ctrl_write(c66x_core *c, unsigned crlo, uint32_t v) { ctrl_write_now(c, crlo, v); }
static c66x_insn *api_insn_at(c66x_core *c, uint32_t addr) { return fetch_insn(c, addr); }

static int api_exec_capture(c66x_core *c, c66x_insn *in, uint32_t next_pc, c66x_jit_cap *cap)
{
    xctx x = { .pce1 = in->addr & ~31u, .next_pc = next_pc };
    cap->n = 0;
    c->jit_cap = cap;
    exec_insn(c, in, &x);
    c->jit_cap = NULL;
    return x.stop;
}

static void api_spl_end_cycle(c66x_core *c) { spl_end_cycle(c, NULL, 0, 0, NULL); }

static int api_verify(c66x_core *c, const uint32_t *addr, const uint8_t *bytes, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *p = ram_ptr(c, addr[i], 32);
        if (!p || memcmp(p, bytes + 32 * i, 32))
            return 0;
    }
    /* Decoded blocks are what invalidate_code watches: a region stays valid
     * exactly as long as decoded code from the same bytes would. */
    for (uint32_t i = 0; i < n; i++)
        fetch_insn(c, addr[i]);
    return 1;
}

static const c66x_jit_api jit_api = { api_mem_read, api_store_defer, api_flush_stores, api_invalidate_code,
                                      api_ctrl_write, api_exec_capture, api_insn_at, api_spl_end_cycle,
                                      api_verify, ctrl_read };

/* For the generator: the writes the instruction at addr schedules when its
 * condition holds, as "shape <n> stop <code>" and one "w <delay> <kind> <idx>"
 * per write, in order. The core it runs on is the generator's scratch core. */
int c66x_jit_shape(c66x_core *c, uint32_t addr, char *buf, size_t len)
{
    c66x_insn *in = fetch_insn(c, addr);
    if (!in)
        return -1;
    if (in->cond_reg >= 0)
        c->reg[in->cond_reg] = in->cond_z ? 0 : 1;
    c66x_jit_cap cap;
    c->store_now = 0;
    int stop = api_exec_capture(c, in, 0, &cap);
    c->npst = 0;
    c->nbr = 0;
    size_t o = (size_t)snprintf(buf, len, "shape %u stop %d\n", cap.n, stop);
    for (unsigned i = 0; i < cap.n && i < C66X_JIT_CAP && o < len; i++)
        o += (size_t)snprintf(buf + o, len - o, "w %u %u %u\n", cap.e[i].delay, cap.e[i].kind, cap.e[i].idx);
    return o < len ? (int)o : -1;
}

static struct c66x_jit *jit_get(c66x_core *c)
{
    if (!c->jit)
        c->jit = calloc(1, sizeof *c->jit);
    return c->jit;
}

static void jit_map_add(struct jit_map_ent *map, const c66x_jit_region *regions, uint32_t n, unsigned *count)
{
    for (uint32_t i = 0; i < n; i++) {
        const c66x_jit_region *r = &regions[i];
        unsigned s = jit_slot(r->pc);
        while (map[s].r && map[s].pc != r->pc)
            s = (s + 1) & (JIT_MAP - 1);
        if (!map[s].r)
            (*count)++;
        map[s].pc = r->pc;
        map[s].r = r;
        map[s].gen = 0;
    }
}

/* One place for the only file-size call in this file; see its use below. */
static int c66x_set_file_size(FILE *f, uint32_t size)
{
    fflush(f);
#ifdef _WIN32
    return _chsize_s(_fileno(f), size);
#else
    return ftruncate(fileno(f), size);
#endif
}

static int jit_load(c66x_core *c, const char *path)
{
    struct c66x_jit *j = jit_get(c);
    GModule *h = g_module_open(path, G_MODULE_BIND_LOCAL);
    gpointer sym = NULL;

    if (!h) {
        fprintf(stderr, "c66x jit: %s\n", g_module_error());
        return -1;
    }
    if (!g_module_symbol(h, "c66x_jit_exports", &sym)) {
        sym = NULL;
    }
    const c66x_jit_module *m = sym;
    if (!m || m->abi != C66X_JIT_ABI || j->nmods == JIT_MODS) {
        fprintf(stderr, "c66x jit: %s: no module for ABI %d\n", path, C66X_JIT_ABI);
        g_module_close(h);
        return -1;
    }
    j->mods[j->nmods++] = h;
    for (uint32_t i = 0; i < m->nkernels && j->nkern < 1024; i++) {
        j->kern[j->nkern].k = &m->kernels[i];
        j->kern[j->nkern++].gen = 0;
    }
    jit_map_add(j->map, m->regions, m->nregions, &j->nregions);
    jit_map_add(j->map0, m->regions0, m->nregions0, &j->nregions0);
    for (uint32_t i = 0; i < m->nqregions; i++) {
        const c66x_jit_region *r = &m->qregions[i].r;
        unsigned s = jit_slot(r->pc);
        while (j->mapq[s].r && j->mapq[s].pc != r->pc)
            s = (s + 1) & (JIT_MAP - 1);
        if (!j->mapq[s].r)
            j->nqregions++;
        j->mapq[s].pc = r->pc;
        j->mapq[s].r = r;
        j->mapq[s].gen = 0;
    }
    for (uint32_t i = 0; i < m->nloops && j->nloops < JIT_LOOPS / 2; i++) {
        const c66x_jit_loop *l = &m->loops[i];
        unsigned s = jit_slot(l->pc) & (JIT_LOOPS - 1);
        while (j->loops[s].l && j->loops[s].pc != l->pc)
            s = (s + 1) & (JIT_LOOPS - 1);
        if (!j->loops[s].l)
            j->nloops++;
        j->loops[s].pc = l->pc;
        j->loops[s].l = l;
        j->loops[s].gen = 0;
    }
    return 0;
}

/* The compiled invocation of the SPLOOP whose execute packet is at pc, if its
 * code bytes are still what it was built from (checked once per code_gen). */
const c66x_jit_loop *jit_loop_lookup(c66x_core *c, uint32_t pc)
{
    struct c66x_jit *j = c->jit;
    unsigned s = jit_slot(pc) & (JIT_LOOPS - 1);
    while (j->loops[s].l && j->loops[s].pc != pc)
        s = (s + 1) & (JIT_LOOPS - 1);
    const c66x_jit_loop *l = j->loops[s].l;
    if (!l)
        return NULL;
    if (j->loops[s].gen == c->code_gen + 1)
        return l;
    /* failures are not cached: the bytes may arrive later (DMA) without any
     * decoded code being dropped */
    if (!api_verify(c, l->dep_addr, l->dep_bytes, l->ndeps)) {
        j->verify_fail++;
        return NULL;
    }
    j->loops[s].gen = c->code_gen + 1;
    return l;
}

/* The region rooted at pc, if its code bytes are still what it was built from.
 * Checked once per code generation (code_gen moves on every drop of decoded
 * code): gen holds code_gen + 1 when it matched, ~code_gen when it did not. */
static int api_verify(c66x_core *c, const uint32_t *addr, const uint8_t *bytes, uint32_t n);

static int api_verify(c66x_core *c, const uint32_t *addr, const uint8_t *bytes, uint32_t n);

static const c66x_jit_region *jit_lookup_in(c66x_core *c, struct jit_map_ent *map, uint32_t pc)
{
    struct c66x_jit *j = c->jit;
    unsigned s = jit_slot(pc);
    while (map[s].r && map[s].pc != pc)
        s = (s + 1) & (JIT_MAP - 1);
    const c66x_jit_region *r = map[s].r;
    /* A region stops before the idle head it was built for; a core without
     * busy-wait skipping (the replay) runs that packet like any other. */
    if (!r || (r->idle_head != c->idle_head && c->idle_head))
        return NULL;
    c66x_jit_verified *v = r->verified;
    if (v ? v->core == c && v->gen == c->code_gen + 1 : map[s].gen == c->code_gen + 1)
        return r;
    /* Not cached as a failure: the bytes may be loaded later without any
     * decoded code being dropped. */
    if (!api_verify(c, r->dep_addr, r->dep_bytes, r->ndeps)) {
        j->verify_fail++;
        return NULL;
    }
    if (v) {
        v->core = c;
        v->gen = c->code_gen + 1;
    }
    map[s].gen = c->code_gen + 1;
    return r;
}

const c66x_jit_region *jit_lookup(c66x_core *c, uint32_t pc)
{
    struct c66x_jit *j = c->jit;
    return j && j->nregions ? jit_lookup_in(c, j->map, pc) : NULL;
}

const c66x_jit_region *jit_lookup0(c66x_core *c, uint32_t pc)
{
    struct c66x_jit *j = c->jit;
    return j && j->nregions0 ? jit_lookup_in(c, j->map0, pc) : NULL;
}

const c66x_jit_qregion *jit_lookupq(c66x_core *c, uint32_t pc)
{
    struct c66x_jit *j = c->jit;
    /* r is the first member of its c66x_jit_qregion */
    return j && j->nqregions ? (const c66x_jit_qregion *)jit_lookup_in(c, j->mapq, pc) : NULL;
}

/* The compiled kernel for the running loop, if one matches its recorded body
 * and its code bytes are unchanged. */
/* C66X_KSTATS: per SPLOOP address, how its kernel lookups ended. */
static struct { uint32_t addr; uint64_t hit, no_kernel, kind, shape, body, verify; } kstats[512];
static void kstat(uint32_t addr, int which)
{
    for (unsigned i = 0; i < 512; i++) {
        if (kstats[i].addr && kstats[i].addr != addr)
            continue;
        kstats[i].addr = addr;
        (&kstats[i].hit)[which]++;
        return;
    }
}

void kstats_dump_fwd(void)
{
    if (!getenv("C66X_KSTATS"))
        return;
    for (unsigned i = 0; i < 512; i++)
        if (kstats[i].addr)
            fprintf(stderr, "kstat 0x%08x hit %llu none %llu kind %llu shape %llu body %llu verify %llu\n",
                    kstats[i].addr, (unsigned long long)kstats[i].hit, (unsigned long long)kstats[i].no_kernel,
                    (unsigned long long)kstats[i].kind, (unsigned long long)kstats[i].shape,
                    (unsigned long long)kstats[i].body, (unsigned long long)kstats[i].verify);
}

const c66x_jit_kernel *jit_kernel_lookup(c66x_core *c)
{
    struct c66x_jit *j = c->jit;
    const spl_state *s = &c->spl;
    int why = 1;
    for (unsigned i = 0; i < j->nkern; i++) {
        const c66x_jit_kernel *k = j->kern[i].k;
        if (k->addr != s->addr)
            continue;
        if (k->kind != s->kind || k->cz != s->cz || k->creg != s->creg) {
            why = 2;
            continue;
        }
        if (k->ii != s->ii || k->dynlen != s->dynlen) {
            why = 3;
            continue;
        }
        int ok = 1;
        const uint32_t *a = k->body_addr;
        for (int r = 0; ok && r < s->dynlen; r++) {
            ok = k->body_n[r] == s->body[r].n;
            for (unsigned q = 0; ok && q < s->body[r].n; q++)
                ok = s->body[r].insn[q]->addr == *a++;
        }
        if (!ok) {
            why = 4;
            continue;
        }
        if (j->kern[i].gen != c->code_gen + 1) {
            if (!api_verify(c, k->dep_addr, k->dep_bytes, k->ndeps)) {
                j->verify_fail++;
                why = 5;
                continue;
            }
            j->kern[i].gen = c->code_gen + 1;
        }
        kstat(s->addr, 0);
        return k;
    }
    kstat(s->addr, why);
    return NULL;
}

void jit_kprof(c66x_core *c)
{
    struct c66x_jit *j = c->jit;
    const spl_state *s = &c->spl;
    if (s->dynlen > 48)
        return;
    for (unsigned i = 0; i < j->nkprof; i++) {
        typeof(j->kprof[0]) *e = &j->kprof[i];
        if (e->addr != s->addr || e->kind != s->kind || e->ii != s->ii || e->dynlen != s->dynlen)
            continue;
        int same = 1;
        for (int r = 0; same && r < s->dynlen; r++) {
            same = e->n[r] == s->body[r].n;
            for (unsigned q = 0; same && q < s->body[r].n && q < 8; q++)
                same = e->a[r][q] == s->body[r].insn[q]->addr;
        }
        if (same)
            return;
    }
    if (j->nkprof == 4096)
        return;
    typeof(j->kprof[0]) *e = &j->kprof[j->nkprof++];
    memset(e, 0, sizeof *e);
    e->addr = s->addr;
    e->kind = s->kind;
    e->ii = s->ii;
    e->dynlen = s->dynlen;
    e->creg = s->creg;
    e->cz = s->cz;
    for (int r = 0; r < s->dynlen; r++) {
        e->n[r] = s->body[r].n;
        for (unsigned q = 0; q < s->body[r].n && q < 8; q++)
            e->a[r][q] = s->body[r].insn[q]->addr;
    }
}

void jit_kprof_cycles(c66x_core *c, uint64_t n)
{
    struct c66x_jit *j = c->jit;
    const spl_state *s = &c->spl;
    for (unsigned i = j->nkprof; i-- > 0;)
        /* Match the kind too: one loop address can run as more than one
         * SPLOOP form. */
        if (j->kprof[i].addr == s->addr && j->kprof[i].kind == s->kind && j->kprof[i].ii == s->ii
            && j->kprof[i].dynlen == s->dynlen) {
            j->kprof[i].cycles += n;
            return;
        }
}

void jit_prof(c66x_core *c, uint32_t pc)
{
    struct c66x_jit *j = c->jit;
    unsigned s = (pc * 2654435761u) >> 14;
    for (unsigned i = 0; i < JIT_PROF; i++, s = (s + 1) & (JIT_PROF - 1)) {
        if (j->prof[s].n && j->prof[s].pc != pc)
            continue;
        j->prof[s].pc = pc;
        j->prof[s].n++;
        j->prof[s].n_free += c->nbr == 0;
        break;
    }
    j->prof_nbr[c->nbr < 3 ? c->nbr : 3]++;
    if (c->nbr != 1)
        return;
    uint32_t h = (pc ^ c->br[0].target * 2246822519u ^ (uint32_t)c->br[0].remaining * 3266489917u) * 2654435761u;
    for (unsigned i = 0, q = h >> 14; i < JIT_PROF; i++, q = (q + 1) & (JIT_PROF - 1)) {
        typeof(j->qprof[0]) *e = &j->qprof[q];
        if (e->n && (e->pc != pc || e->target != c->br[0].target || e->rem != c->br[0].remaining))
            continue;
        e->pc = pc;
        e->target = c->br[0].target;
        e->rem = c->br[0].remaining;
        e->n++;
        return;
    }
}

/* What tools/c14_jitgen.py reads: <dir>/profile.txt and the mapped RAM as
 * <dir>/ram_<id>.bin. The profile is the fast loop's packet counts (C66X_JIT_PROFILE)
 * or the packet cache's hit counters (C66X_JIT_AUTO); "have" names roots already
 * compiled. With sparse set, only 4 KB pages that hold decoded code (and their
 * neighbours) are written, into a file of the full size. */
static void jit_write_profile(c66x_core *c, const char *dir, int sparse)
{
    struct c66x_jit *j = c->jit;
    char path[4096];
    snprintf(path, sizeof path, "%s/profile.txt", dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "idle_head 0x%08x\n", c->idle_head);
    for (unsigned i = 0; i < c->nram; i++)
        fprintf(f, "map 0x%08x 0x%08x %u\n", c->ram[i].base, c->ram[i].size, rec_host_id(c, &c->ram[i]));
    if (j->prof) {
        for (unsigned i = 0; i < JIT_PROF; i++)
            if (j->prof[i].n)
                fprintf(f, "prof 0x%08x %llu %llu\n", j->prof[i].pc, (unsigned long long)j->prof[i].n,
                        (unsigned long long)j->prof[i].n_free);
    } else {
        for (unsigned i = 0; i < PKC_SIZE; i++)
            if (c->pkc[i].hits && c->pkc[i].first)
                fprintf(f, "prof 0x%08x %u %u\n", c->pkc[i].pc, c->pkc[i].hits, c->pkc[i].hits);
    }
    for (unsigned i = 0; i < JIT_MAP; i++)
        if (j->map[i].r)
            fprintf(f, "have 0x%08x\n", j->map[i].pc);
    for (unsigned i = 0; j->qprof && i < JIT_PROF; i++)
        if (j->qprof[i].n >= 1000)
            fprintf(f, "qprof 0x%08x %d 0x%08x %llu\n", j->qprof[i].pc, j->qprof[i].rem, j->qprof[i].target,
                    (unsigned long long)j->qprof[i].n);
    if (j->prof)
        fprintf(f, "nbr %llu %llu %llu %llu\n", (unsigned long long)j->prof_nbr[0], (unsigned long long)j->prof_nbr[1],
                (unsigned long long)j->prof_nbr[2], (unsigned long long)j->prof_nbr[3]);
    for (unsigned i = 0; j->kprof && i < j->nkprof; i++) {
        typeof(j->kprof[0]) *e = &j->kprof[i];
        int have = 0;
        for (unsigned k = 0; k < j->nkern; k++)
            have |= j->kern[k].k->addr == e->addr && j->kern[k].k->ii == e->ii && j->kern[k].k->dynlen == e->dynlen;
        if (have)
            continue;
        fprintf(f, "kern 0x%08x %u %u %u %d %u %llu", e->addr, e->kind, e->ii, e->dynlen, e->creg, e->cz,
                (unsigned long long)e->cycles);
        for (int r = 0; r < e->dynlen; r++) {
            fprintf(f, " |");
            for (unsigned q = 0; q < e->n[r]; q++)
                fprintf(f, " 0x%08x", e->a[r][q]);
        }
        fprintf(f, "\n");
    }
    fclose(f);
    for (unsigned i = 0; i < c->nram; i++) {
        const ramreg *r = &c->ram[i];
        if (rec_host_id(c, r) != i)
            continue;
        snprintf(path, sizeof path, "%s/ram_%u.bin", dir, i);
        f = fopen(path, "wb");
        if (!f)
            continue;
        if (!sparse) {
            fwrite(r->host, 1, r->size, f);
        } else {
            uint32_t pages = (r->size + (1u << FP_PAGE_SHIFT) - 1) >> FP_PAGE_SHIFT;
            for (uint32_t p = 0; p < pages; p++) {
                int near = r->codepage[p] || (p && r->codepage[p - 1]) || (p + 1 < pages && r->codepage[p + 1]);
                if (!near)
                    continue;
                uint32_t off = p << FP_PAGE_SHIFT, len = r->size - off < (1u << FP_PAGE_SHIFT) ? r->size - off
                                                                                              : 1u << FP_PAGE_SHIFT;
                fseek(f, off, SEEK_SET);
                fwrite(r->host + off, 1, len, f);
            }
            fflush(f);
            /* The sparse dump wrote only pages near code; set the full
               length so the offsets still line up. */
            if (c66x_set_file_size(f, r->size))
                perror("c66x jit: set file size");
        }
        fclose(f);
    }
}

/* C66X_JIT_AUTO=<dir>: compile while running. Packets the fast loop runs
 * interpreted count hits on their cache entries; every C66X_JIT_AUTO_S seconds
 * (default 5) of host time, when some packet reached C66X_JIT_AUTO_MIN hits, the
 * counts and the code pages go to <dir>/batch<N>, a thread runs the generator
 * (C66X_JIT_GEN, default the tools directory this core was built from) and gcc,
 * and the module is loaded at the next step. Its regions attach to cache
 * entries as they are verified. */
/* defined below; the argv build in jit_auto_start() needs it first. */
static const char *jit_env(const char *name, const char *dflt);

static void *jit_auto_thread(void *arg)
{
    struct c66x_jit *j = arg;
    g_autofree char *out = NULL, *errout = NULL;
    GError *err = NULL;
    int status = -1;
    char so[4200];

    /* An argv rather than system(): no shell quoting, works on Windows too. */
    gboolean ok = g_spawn_sync(NULL, j->auto_argv, NULL, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, &out, &errout, &status, &err);
    FILE *log = fopen(j->auto_log, "wb");
    if (log) {
        if (out)
            fputs(out, log);
        if (errout)
            fputs(errout, log);
        if (!ok && err)
            fprintf(log, "\nspawn failed: %s\n", err->message);
        fclose(log);
    }
    if (err)
        g_error_free(err);

    snprintf(so, sizeof so, "%s.so", j->auto_out);
    FILE *f = (ok && status == 0) ? fopen(so, "rb") : NULL;
    if (f)
        fclose(f);
    j->auto_result = f ? 1 : -1;
    return NULL;
}

static double jit_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

/* Finds pc in the tried set; with add, inserts it. Returns 1 if it was there. */
static int jit_auto_tried(struct c66x_jit *j, uint32_t pc, int add)
{
    uint32_t key = pc | 1;
    unsigned t = (pc * 2654435761u) >> 16;
    while (j->auto_tried[t] && j->auto_tried[t] != key)
        t = (t + 1) & 0xffff;
    if (j->auto_tried[t])
        return 1;
    if (add && j->auto_ntried < 0xc000) {
        j->auto_tried[t] = key;
        j->auto_ntried++;
    }
    return 0;
}

/* Marks what the generator examined (<out>.tried). Without that list (the batch
 * failed before writing it) every packet offered is marked, or a batch that
 * cannot succeed would restart every interval. */
static void jit_auto_mark(c66x_core *c)
{
    struct c66x_jit *j = c->jit;
    char path[4200];
    snprintf(path, sizeof path, "%s.tried", j->auto_out);
    FILE *f = fopen(path, "r");
    if (f) {
        unsigned long pc;
        while (fscanf(f, "%lx", &pc) == 1)
            jit_auto_tried(j, (uint32_t)pc, 1);
        fclose(f);
        return;
    }
    for (unsigned i = 0; i < PKC_SIZE; i++) {
        pkc_ent *e = &c->pkc[i];
        if (e->hits >= j->auto_min && !e->region && e->first)
            jit_auto_tried(j, e->pc, 1);
    }
}

void jit_auto_poll(c66x_core *c)
{
    struct c66x_jit *j = c->jit;
    if (j->auto_busy) {
        if (!j->auto_result)
            return;
        g_thread_join(j->auto_thr);
        j->auto_thr = NULL;
        j->auto_busy = 0;
        jit_auto_mark(c);
        if (j->auto_result > 0) {
            char so[4200];
            snprintf(so, sizeof so, "%s.so", j->auto_out);
            if (!jit_load(c, so)) {
                j->auto_modules++;
                for (unsigned i = 0; i < PKC_SIZE; i++) {
                    if (!c->pkc[i].first)
                        continue;
                    if (!c->pkc[i].region)
                        c->pkc[i].region = jit_lookup(c, c->pkc[i].pc);
                    if (!c->pkc[i].region0)
                        c->pkc[i].region0 = jit_lookup0(c, c->pkc[i].pc);
                    if (!c->pkc[i].qregion)
                        c->pkc[i].qregion = jit_lookupq(c, c->pkc[i].pc);
                }
            }
        } else {
            j->auto_failed++;
        }
        j->auto_last = jit_now();
        return;
    }
    double now = jit_now();
    if (now - j->auto_last < j->auto_interval)
        return;
    j->auto_last = now;
    /* A batch only for something the generator has not examined yet: a packet
     * or kernel it could not compile would otherwise start one every interval. */
    int any = 0;
    for (unsigned i = 0; i < PKC_SIZE && !any; i++) {
        pkc_ent *e = &c->pkc[i];
        if (e->hits >= j->auto_min && !e->region && e->first && j->auto_ntried < 0xc000)
            any = !jit_auto_tried(j, e->pc, 0);
    }
    for (unsigned i = 0; j->kprof && i < j->nkprof; i++)
        if (!j->kprof[i].tried && j->kprof[i].cycles >= (uint64_t)j->auto_min * 50) {
            j->kprof[i].tried = 1;
            any = 1;
        }
    if (!any)
        return;
    char dir[4096];
    snprintf(dir, sizeof dir, "%s/batch%u", j->auto_dir, j->auto_batch++);
    /* No shell: system("mkdir -p") does not work on Windows. */
    if (g_mkdir_with_parents(dir, 0755))
        return;
    jit_write_profile(c, dir, 1);
    snprintf(j->auto_out, sizeof j->auto_out, "%s/m", dir);
    snprintf(j->auto_log, sizeof j->auto_log, "%s/gen.log", dir);
    g_strfreev(j->auto_argv);
    j->auto_argv = g_new0(char *, 17);
    unsigned a = 0;
    j->auto_argv[a++] = g_strdup(jit_env("C66X_JIT_PYTHON", "python3"));
    j->auto_argv[a++] = g_strdup(j->auto_gen);
    j->auto_argv[a++] = g_strdup(dir);
    j->auto_argv[a++] = g_strdup(j->auto_out);
    j->auto_argv[a++] = g_strdup("--lib");
    j->auto_argv[a++] = g_strdup(j->auto_lib);
    j->auto_argv[a++] = g_strdup("--min");
    j->auto_argv[a++] = g_strdup_printf("%u", j->auto_min);
    j->auto_argv[a++] = g_strdup("--cold");
    j->auto_argv[a++] = g_strdup_printf("%u", j->auto_cold);
    j->auto_argv[a++] = g_strdup("--fn-nodes");
    j->auto_argv[a++] = g_strdup_printf("%u", j->auto_fn);
    j->auto_argv[a++] = g_strdup("--roots");
    j->auto_argv[a++] = g_strdup_printf("%u", j->auto_roots);
    j->auto_argv[a++] = g_strdup("--kmin");
    j->auto_argv[a++] = g_strdup_printf("%llu",
                                        (unsigned long long)j->auto_min * 50);
    j->auto_argv[a] = NULL;
    j->auto_result = 0;
    j->auto_busy = 1;
    j->auto_thr = g_thread_new("c66x-jit", jit_auto_thread, j);
    if (!j->auto_thr) {
        j->auto_busy = 0;
        j->auto_failed++;
    }
}

static const char *jit_env(const char *name, const char *dflt)
{
    const char *v = getenv(name);
    return v && *v ? v : dflt;
}

/* C66X_JIT=<a.so>[:<b.so>...] (';' on Windows, where ':' follows the drive
 * letter) loads compiled regions; C66X_JIT_PROFILE=<dir>
 * counts packets and writes the generator's input when the core is freed;
 * C66X_JIT_AUTO=<dir> compiles while running (jit_auto_poll). */
void jit_init(c66x_core *c)
{
    c->api = &jit_api;
    const char *mods = getenv("C66X_JIT");
    if (mods && *mods) {
        char *list = strdup(mods);
        char *save = NULL;
        for (char *t = strtok_r(list, G_SEARCHPATH_SEPARATOR_S, &save); t;
             t = strtok_r(NULL, G_SEARCHPATH_SEPARATOR_S, &save))
            jit_load(c, t);
        free(list);
    }
    const char *dir = getenv("C66X_JIT_PROFILE");
    if (dir && *dir) {
        struct c66x_jit *j = jit_get(c);
        j->prof_dir = strdup(dir);
        j->prof = calloc(JIT_PROF, sizeof j->prof[0]);
        j->qprof = calloc(JIT_PROF, sizeof j->qprof[0]);
        j->kprof = calloc(4096, sizeof j->kprof[0]);
    }
    /* "0" reads as off, as the core's other switches do, not as a directory. */
    const char *adir = getenv("C66X_JIT_AUTO");
    if (adir && *adir && strcmp(adir, "0")) {
        struct c66x_jit *j = jit_get(c);
        j->auto_dir = strdup(adir);
        if (!j->kprof)
            j->kprof = calloc(4096, sizeof j->kprof[0]);
        j->auto_gen = strdup(jit_env("C66X_JIT_GEN", C66X_JIT_TOOLS "/c14_jitgen.py"));
        j->auto_lib = strdup(jit_env("C66X_JIT_LIB", C66X_JIT_LIBDIR "/libc66x.so"));
        j->auto_interval = atof(jit_env("C66X_JIT_AUTO_S", "5"));
        j->auto_min = (unsigned)atoi(jit_env("C66X_JIT_AUTO_MIN", "20000"));
        j->auto_roots = (unsigned)atoi(jit_env("C66X_JIT_AUTO_ROOTS", "400"));
        /* A batch's profile covers seconds, not the offline profile's whole run. */
        j->auto_cold = (unsigned)atoi(jit_env("C66X_JIT_AUTO_COLD", "2000"));
        /* Larger functions build slower but run faster; modules are cached
         * across runs, so speed wins. */
        j->auto_fn = (unsigned)atoi(jit_env("C66X_JIT_AUTO_FN", "1500"));
        j->auto_last = jit_now();
        /* Modules earlier runs compiled into this directory start the run warm;
         * a region whose code bytes differ now fails verification and stays unused. */
        /* <adir>/batch<N>/m.so, found with GDir since mingw has no glob(). */
        GDir *d = g_dir_open(adir, 0, NULL);
        if (d) {
            const char *ent;
            while ((ent = g_dir_read_name(d))) {
                unsigned n;
                if (sscanf(ent, "batch%u", &n) != 1)
                    continue;
                g_autofree char *so = g_build_filename(adir, ent, "m.so", NULL);
                if (!g_file_test(so, G_FILE_TEST_EXISTS))
                    continue;
                if (!jit_load(c, so))
                    j->auto_cached++;
                if (n >= j->auto_batch)
                    j->auto_batch = n + 1;
            }
            g_dir_close(d);
        }
    }
}

void c66x_jit_report(const c66x_core *c, char *buf, size_t len)
{
    const struct c66x_jit *j = c->jit;
    if (!len)
        return;
    buf[0] = 0;
    if (!j || !(j->nregions || j->nkern || j->auto_dir))
        return;
    snprintf(buf, len, "%u regions + %u clean, %llu entries + %llu clean, %llu cycles compiled, %llu verify failures; "
             "%u kernels, %llu kernel cycles, %llu drain cycles; %u loops, %llu loop cycles; "
             "%u queued, %llu queued cycles; auto %u cached %u batches %u loaded %u failed",
             j->nregions, j->nregions0,
             (unsigned long long)j->entries, (unsigned long long)j->entries0, (unsigned long long)j->cycles,
             (unsigned long long)j->verify_fail,
             j->nkern, (unsigned long long)j->kcycles, (unsigned long long)j->dcycles,
             j->nloops, (unsigned long long)j->lcycles, j->nqregions, (unsigned long long)j->qcycles,
             j->auto_cached,
             j->auto_batch, j->auto_modules, j->auto_failed);
}

void jit_free(c66x_core *c)
{
    struct c66x_jit *j = c->jit;
    if (!j)
        return;
    if (j->auto_busy)
        g_thread_join(j->auto_thr);
        j->auto_thr = NULL;
    if (j->auto_dir)
        fprintf(stderr, "c66x jit auto: %u cached modules, %u batches, %u modules loaded, %u failed\n",
                j->auto_cached, j->auto_batch, j->auto_modules, j->auto_failed);
    fprintf(stderr, "c66x jit: code generations %llu (%llu from a full xpk table)\n",
            (unsigned long long)c->code_gen, (unsigned long long)c->xpk_flushes);
    if (j->nregions || j->nkern)
        fprintf(stderr, "c66x jit: %u regions + %u clean, %llu entries + %llu clean, %llu cycles in regions, "
                "%llu verify failures\n", j->nregions, j->nregions0, (unsigned long long)j->entries,
                (unsigned long long)j->entries0, (unsigned long long)j->cycles,
                (unsigned long long)j->verify_fail);
    if (j->nkern)
        fprintf(stderr, "c66x jit: %u kernels, %llu entries, %llu cycles in kernels; drain %llu entries, "
                "%llu cycles\n", j->nkern, (unsigned long long)j->kentries, (unsigned long long)j->kcycles,
                (unsigned long long)j->dentries, (unsigned long long)j->dcycles);
    if (j->nqregions)
        fprintf(stderr, "c66x jit: %u queued-branch regions, %llu entries, %llu cycles\n", j->nqregions,
                (unsigned long long)j->qentries, (unsigned long long)j->qcycles);
    if (j->nloops)
        fprintf(stderr, "c66x jit: %u loops, %llu entries, %llu cycles in loops, %llu declined\n", j->nloops,
                (unsigned long long)j->lentries, (unsigned long long)j->lcycles,
                (unsigned long long)j->ldeclined);
    if (j->nregions) {
        static const char *const why[] = { "stub", "dynamic pc", "uncompilable", "queue", "node limit",
                                           "budget", "code dropped", "interrupt", "interrupt after commit", "stall",
                                           "chain" };
        fprintf(stderr, "c66x jit exits:");
        for (unsigned i = 0; i < sizeof why / sizeof why[0]; i++)
            fprintf(stderr, " %s %llu", why[i], (unsigned long long)c->jit_exit[i]);
        fprintf(stderr, "\n");
    }
    if (j->prof_dir && c->nram)
        jit_write_profile(c, j->prof_dir, 0);
    for (unsigned i = 0; i < j->nmods; i++)
        g_module_close(j->mods[i]);
    free(j->prof);
    free(j->kprof);
    free(j->prof_dir);
    free(j->auto_dir);
    free(j->auto_gen);
    free(j->auto_lib);
    free(j);
    c->jit = NULL;
}

/* For the generator: the packet the interpreter would run at pc, as text. One
 * "pk" line (cacheable = the fast loop can run it), then one "in" line per
 * instruction, each followed by its "op" lines. Returns the length, or -1. */
#define JIT_OUT(...) do { int k_ = snprintf(buf + *o, *o < len ? len - *o : 0, __VA_ARGS__); \
                          *o += k_ > 0 ? (size_t)k_ : 0; } while (0)

static void jit_describe_one(const c66x_insn *in, char *buf, size_t len, size_t *o)
{
    JIT_OUT("in 0x%08x size %u fop %s cond_reg %d cond_z %u mfast %u ea_delta %d xnops %u isbranch %u "
            "unit %d side %u nops %u handler %u sub %u name %s\n", in->addr, in->size, fop_names[in->fop],
            in->cond_reg, in->cond_z, in->mfast, in->ea_delta, in->xnops, in->isbranch, in->unit, in->side,
            in->nops, in->handler, in->sub, in->opc >= 0 ? c66x_opcode_name(in->opc) : "?");
    for (unsigned k = 0; k < in->nops; k++) {
        const c66x_operand *op = &in->op[k];
        JIT_OUT("op kind %u size %u rw %u low_first %u high_first %u reg %u reg_hi %u mem_mode %u "
                "mem_offreg %u mem_scale %u xpath %u val %d crlo %u\n", op->kind, op->size, op->rw,
                op->low_first, op->high_first, op->reg, op->reg_hi, op->mem_mode, op->mem_offreg,
                op->mem_scale, op->xpath, op->val,
                op->kind == C66X_OPK_CTRL ? c66x_ctrl_crlo(op->val) : 0);
    }
}

/* For the generator: the packet the interpreter would run at pc, as text. One
 * "pk" line (cacheable = the fast loop can run it), then one "in" line per
 * instruction, each followed by its "op" lines. Returns the length, or -1. */
int c66x_jit_describe(c66x_core *c, uint32_t pc, char *buf, size_t len)
{
    c66x_insn *pk[MAX_PK];
    unsigned n;
    uint32_t next;
    if (gather_packet(c, pc, pk, &n, &next) || gather_packet(c, pc, pk, &n, &next))
        return -1;
    const pkc_ent *e = &c->pkc[PKC_SLOT(pc)];
    int cached = e->pc == pc && e->first;
    size_t o_ = 0, *o = &o_;
    JIT_OUT("pk 0x%08x next 0x%08x n %u cacheable %d special %d load %d xnops %d branched %d allfop %d xmask 0x%llx\n",
            pc, next, n, cached, cached ? e->special : 1, cached ? e->load : 0, cached ? e->xnops : 0,
            cached ? e->branched : 0, cached ? e->allfop : 0, cached ? (unsigned long long)e->xmask : 0ull);
    for (unsigned i = 0; i < n; i++)
        jit_describe_one(pk[i], buf, len, o);
    return o_ < len ? (int)o_ : -1;
}

/* For the generator: one instruction (a loop-buffer body entry), "in" + "op" lines. */
int c66x_jit_describe_insn(c66x_core *c, uint32_t addr, char *buf, size_t len)
{
    c66x_insn *in = fetch_insn(c, addr);
    if (!in || in->opc < 0)
        return -1;
    size_t o_ = 0;
    jit_describe_one(in, buf, len, &o_);
    return o_ < len ? (int)o_ : -1;
}
#undef JIT_OUT
