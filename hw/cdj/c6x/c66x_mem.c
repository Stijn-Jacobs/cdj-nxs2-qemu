/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x core: memory, the idle-loop checks, instruction fetch.
 */
#include "c66x_priv.h"

/* ------------------------------------------------------------------------ */
/* memory                                                                    */


void invalidate_code(c66x_core *c, uint32_t a, uint32_t n)
{
    /* Every mapping of the same host bytes: L2 is mapped at two addresses. */
    for (unsigned i = 0; i < c->nram; i++) {
        ramreg *r = &c->ram[i];
        if (!r->fpp)
            continue;
        for (unsigned j = 0; j < c->nram; j++) {
            ramreg *w = &c->ram[j];
            if (a - w->base >= w->size || w->host != r->host || w->size != r->size)
                continue;
            uint32_t lo = (a - w->base) >> 5;
            uint32_t hi = (uint32_t)(((uint64_t)(a - w->base) + n - 1) >> 5);
            for (uint32_t f = lo; f <= hi && ((uint64_t)f << 5) < r->size; f++) {
                fpblk **pg = r->fpp[f >> (FP_PAGE_SHIFT - 5)];
                if (!pg) {
                    f |= (1u << (FP_PAGE_SHIFT - 5)) - 1;
                    continue;
                }
                fpblk *b = pg[f & ((1u << (FP_PAGE_SHIFT - 5)) - 1)];
                if (b && b->valid) {
                    b->valid = 0;
                    for (int k = 0; k < 16; k++)
                        b->insn[k].pk_n = 0;
                    memset(c->pkc, 0, sizeof c->pkc);
                    c->nxpk = 0;
                    c->code_gen++;
                }
            }
        }
    }
}

static void idle_dma_store(c66x_core *c, uint32_t addr, uint32_t len);

void c66x_invalidate(c66x_core *c, uint32_t addr, uint32_t len)
{
    if (c->rec && len) {
        uint8_t *p = ram_ptr(c, addr, len);
        if (p) {
            rec_head(c, REC_STORE);
            fwrite(&addr, 4, 1, c->rec);
            fwrite(&len, 4, 1, c->rec);
            fwrite(p, 1, len, c->rec);
        }
    }
    if (len)
        invalidate_code(c, addr, len);
    if (len && c->idle_armed)
        idle_dma_store(c, addr, len);
}



static inline unsigned idle_rs_slot(uint32_t key)
{
    return (key * 2654435761u) >> (32 - 11) & (IDLE_RS_SLOTS - 1);
}

static void idle_rs_add_word(c66x_core *c, uint32_t w)
{
    uint32_t key = w | 1;
    if (c->idle_rs_full)
        return;
    for (unsigned h = idle_rs_slot(key);; h = (h + 1) & (IDLE_RS_SLOTS - 1)) {
        if (c->idle_rs[h] == key)
            return;
        if (!c->idle_rs[h]) {
            if (c->idle_rs_n >= IDLE_RS_SLOTS * 3 / 4) {
                c->idle_rs_full = 1;
                return;
            }
            c->idle_rs[h] = key;
            c->idle_rs_n++;
            return;
        }
    }
}

static int idle_rs_has_word(const c66x_core *c, uint32_t w)
{
    uint32_t key = w | 1;
    if (c->idle_rs_full)
        return 1;
    for (unsigned h = idle_rs_slot(key);; h = (h + 1) & (IDLE_RS_SLOTS - 1)) {
        if (c->idle_rs[h] == key)
            return 1;
        if (!c->idle_rs[h])
            return 0;
    }
}

void idle_rs_clear(c66x_core *c)
{
    if (c->idle_rs_n || c->idle_rs_full)
        memset(c->idle_rs, 0, sizeof c->idle_rs);
    c->idle_rs_n = 0;
    c->idle_rs_full = 0;
}

static void idle_loop_read(c66x_core *c, uint32_t a, unsigned n, int ram)
{
    if (c->isr_depth || c->idle_fx || (ram && a >= c->idle_slo && a < c->idle_shi))
        return;
    idle_rs_add_word(c, a & ~3u);
    if ((a & 3) + n > 4)
        idle_rs_add_word(c, (a & ~3u) + 4);
}

static void idle_isr_store(c66x_core *c, uint32_t a, unsigned n)
{
    if (idle_rs_has_word(c, a & ~3u) || ((a & 3) + n > 4 && idle_rs_has_word(c, (a & ~3u) + 4))) {
        c->idle_isr_hit = 1;
        c->idle_fp = 0;
    }
}

/* A host DMA master stored guest RAM (reported through c66x_invalidate): the
 * loop may be waiting for exactly these bytes, as for a handler's store. */
static void idle_dma_store(c66x_core *c, uint32_t addr, uint32_t len)
{
    if (c->idle_rs_full || len > 64 * 1024) {
        c->idle_isr_hit = 1;
        c->idle_fp = 0;
        return;
    }
    for (uint32_t w = addr & ~3u; w < addr + len; w += 4) {
        if (idle_rs_has_word(c, w)) {
            c->idle_isr_hit = 1;
            c->idle_fp = 0;
            return;
        }
    }
}

static int idle_idempotent(const c66x_core *c, uint32_t a)
{
    for (unsigned i = 0; i < c->idle_nidem; i++)
        if (c->idle_idem[i] == a)
            return 1;
    return 0;
}

uint32_t mem_read(c66x_core *c, uint32_t a, unsigned n)
{
    uint8_t *p = ram_ptr(c, a, n);
    if (p) {
        if (__builtin_expect(c->idle_armed, 0))
            idle_loop_read(c, a, n, 1);
        switch (n) {
        case 1: return p[0];
        case 2: return p[0] | (p[1] << 8);
        default: return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
        }
    }
    if (!c->idle_allow_reads)
        idle_note(c, 3, a);
    else if (c->idle_armed)
        idle_loop_read(c, a, n, 0);
    uint32_t v = c->bus.read ? c->bus.read(c->bus.opaque, a, n) : 0;
    if (c->rec)
        rec_rw(c, REC_READ, a, n, v);
    return v;
}

/* A store landing at the end of its cycle; in_isr and pc are as they were when
 * the instruction executed. */
void mem_write(c66x_core *c, uint32_t a, uint32_t v, unsigned n, int in_isr, uint32_t pc)
{
    if (c->watch_fn && a <= c->watch_hi && a + n - 1 >= c->watch_lo)
        c->watch_fn(c, c->watch_opaque, a, v, n);
    uint8_t *p = ram_ptr(c, a, n);
    /* The app republishes its status block (0x008C8950..) on every pass, so
     * storing the value already there is not a side effect. */
    if (c->idle_armed && !c->idle_fx && !(p && a >= c->idle_slo && a < c->idle_shi)) {
        uint32_t old = p ? (p[0] | (n > 1 ? p[1] << 8 : 0)
                            | (n > 2 ? (p[2] << 16) | ((uint32_t)p[3] << 24) : 0)) : ~v;
        uint32_t m = n == 4 ? 0xffffffffu : (1u << (8 * n)) - 1;
        if (!p || ((old ^ v) & m)) {
            if (in_isr) {
                idle_isr_store(c, a, n);
            } else if (p || !idle_idempotent(c, a)) {
                c->idle_fx = p ? 1 : 2;
                c->idle_fx_addr = a;
                c->idle_fx_pc = pc;
                c->idle_fp = 0;
            }
        }
    }
    if (p) {
        p[0] = v;
        if (n > 1) p[1] = v >> 8;
        if (n > 2) { p[2] = v >> 16; p[3] = v >> 24; }
        ramreg *r = &c->ram[c->last_ram];
        if (r->codepage[(a - r->base) >> FP_PAGE_SHIFT])
            invalidate_code(c, a, n);
        return;
    }
    /* Before the call: a store the write causes synchronously follows it. */
    if (c->rec)
        rec_rw(c, REC_WRITE, a, n, v);
    if (c->bus.write)
        c->bus.write(c->bus.opaque, a, v, n);
}

/* Loads and stores both touch memory at E3 (SPRU732J 4.2.3): a load in the
 * same cycle as a store to its address reads the old value, whichever comes
 * first in the packet or loop buffer. Stores are therefore held until every
 * instruction of the cycle has executed. */







/* exec_insn reads sources through rmask, so a register counts only if it is there. */
static int src_reg32(const c66x_insn *in, unsigned i)
{
    return in->op[i].kind == C66X_OPK_REG && in->op[i].size == 4 && ((in->regmask >> i) & 1);
}

static int src_val32(const c66x_insn *in, unsigned i)
{
    return src_reg32(in, i) || (in->op[i].kind == C66X_OPK_CONST && ((in->constmask >> i) & 1)
                                && in->op[i].size != 5 && in->op[i].size != 8);
}

static int dst_reg32(const c66x_insn *in, unsigned i)
{
    return in->op[i].kind == C66X_OPK_REG && in->op[i].size == 4 && in->op[i].low_first == 1;
}

static uint8_t fop_class(const c66x_insn *in)
{
    int k0 = in->op[0].kind == C66X_OPK_CONST;
    switch (in->handler) {
    case H_NOP:
        return F_NOP;
    case H_LOAD:
        return F_LOAD;
    case H_STORE:
        return F_STORE;
    case H_BNOP:
        return F_BRANCH;
    case H_B:
        return in->op[0].kind == C66X_OPK_IRP || in->op[0].kind == C66X_OPK_NRP ? F_NONE : F_BRANCH;
    case H_CALLP:
        return F_CALLP;
    case H_MVC:
        return F_MVC;
    case H_MVK:
        return in->nops == 2 && k0 && dst_reg32(in, 1) ? F_MVK : F_NONE;
    case H_MVKH:
        return in->nops == 2 && k0 && dst_reg32(in, 1) ? F_MVKH : F_NONE;
    case H_ADDK:
        return in->nops == 2 && k0 && dst_reg32(in, 1) ? F_ADDK : F_NONE;
    case H_MV:
        return in->nops == 2 && src_reg32(in, 0) && dst_reg32(in, 1) ? F_MV : F_NONE;
    case H_ADDKPC:
        return in->nops == 3 && in->op[0].kind == C66X_OPK_ADDR && dst_reg32(in, 1) ? F_ADDKPC : F_NONE;
    case H_EXT: case H_EXTU:
        if (in->nops == 4 && src_reg32(in, 0) && in->op[1].kind == C66X_OPK_CONST
            && in->op[2].kind == C66X_OPK_CONST && dst_reg32(in, 3))
            return in->handler == H_EXT ? F_EXT : F_EXTU;
        return F_NONE;
    default:
        break;
    }
    if ((in->handler == H_ADDSP || in->handler == H_SUBSP || in->handler == H_MPYSP)
        && in->nops == 3 && src_reg32(in, 0) && src_reg32(in, 1)
        && in->op[2].kind == C66X_OPK_REG && in->op[2].size == 4 && in->op[2].low_first >= 1)
        return in->handler == H_ADDSP ? F_ADDSP : in->handler == H_SUBSP ? F_SUBSP : F_MPYSP;
    if (in->nops != 3 || !src_val32(in, 0) || !src_val32(in, 1) || !dst_reg32(in, 2))
        return F_NONE;
    switch (in->handler) {
    case H_ADD:    return F_ADD;
    case H_SUB:    return F_SUB;
    case H_AND:    return F_AND;
    case H_OR:     return F_OR;
    case H_XOR:    return F_XOR;
    case H_CMPEQ:  return F_CMPEQ;
    case H_CMPGT:  return F_CMPGT;
    case H_CMPLT:  return F_CMPLT;
    case H_CMPGTU: return F_CMPGTU;
    case H_CMPLTU: return F_CMPLTU;
    case H_SHL:    return src_reg32(in, 0) ? F_SHL : F_NONE;
    case H_SHR:    return src_reg32(in, 0) ? F_SHR : F_NONE;
    case H_SHRU:   return src_reg32(in, 0) ? F_SHRU : F_NONE;
    default:       return F_NONE;
    }
}

/* Handler variants resolved once per decode, so execution never looks at names. */
static void classify(c66x_insn *in)
{
    const char *nm = c66x_opcode_name(in->opc);
    in->sub = 0;
    in->xread = 0;
    in->rmask = 0;
    in->regmask = 0;
    in->constmask = 0;
    for (unsigned i = 0; i < in->nops; i++) {
        const c66x_operand *o = &in->op[i];
        if (o->xpath && o->rw != tic6x_rw_write)
            in->xread = 1;
        if (o->kind == C66X_OPK_CONST || o->kind == C66X_OPK_ADDR
            || ((o->rw == tic6x_rw_read || o->rw == tic6x_rw_read_write) && o->kind != C66X_OPK_MEM)) {
            in->rmask |= 1u << i;
            if (o->kind == C66X_OPK_REG)
                in->regmask |= 1u << i;
            else if (o->kind == C66X_OPK_CONST)
                in->constmask |= 1u << i;
        }
    }
    /* Multi-cycle NOPs are inserted whatever the predicate says. */
    in->xnops = 0;
    in->isbranch = 0;
    switch (in->handler) {
    case H_NOP:
        if (in->nops && in->op[0].val > 1)
            in->xnops = in->op[0].val - 1;
        break;
    case H_BNOP:   in->xnops = in->op[1].val; in->isbranch = 1; break;
    case H_ADDKPC: in->xnops = in->op[2].val; break;
    case H_CALLP:  in->xnops = 5; in->isbranch = 1; break;
    case H_B: case H_BDEC: case H_BPOS: in->isbranch = 1; break;
    case H_LOAD:   in->xnops = in->prot ? 4 : 0; break;
    }
    switch (in->handler) {
    case H_LOAD: case H_STORE:
        if (!strcmp(nm, "ldb") || !strcmp(nm, "stb")) in->sub = LD_B;
        else if (!strcmp(nm, "ldbu")) in->sub = LD_BU;
        else if (!strcmp(nm, "ldh") || !strcmp(nm, "sth")) in->sub = LD_H;
        else if (!strcmp(nm, "ldhu")) in->sub = LD_HU;
        /* exact names: "ldw" contains "dw" and must stay a 4-byte access */
        else if (!strcmp(nm, "lddw") || !strcmp(nm, "ldndw")
                 || !strcmp(nm, "stdw") || !strcmp(nm, "stndw")) in->sub = LD_DW;
        break;
    case H_ADDA: case H_SUBA:
        in->sub = nm[4] == 'b' ? 0 : nm[4] == 'h' ? 1 : nm[4] == 'w' ? 2 : 3;
        break;
    case H_MPY16: case H_SMPY16: {
        /* after "mpy": h/l field per operand, s/u signedness per operand */
        static const struct { const char *s; uint8_t v; } t[] = {
            { "", 0 }, { "u", 12 }, { "us", 4 }, { "su", 8 }, { "h", 3 }, { "hu", 15 },
            { "hus", 7 }, { "hsu", 11 }, { "hl", 1 }, { "hlu", 13 }, { "hslu", 9 },
            { "huls", 5 }, { "lh", 2 }, { "lhu", 14 }, { "lshu", 10 }, { "luhs", 6 },
        };
        const char *p = nm + (in->handler == H_SMPY16) + 3;
        for (unsigned i = 0; i < sizeof t / sizeof t[0]; i++)
            if (!strcmp(p, t[i].s))
                in->sub = t[i].v;
        break;
    }
    case H_MPY32:
        in->sub = !strcmp(nm + 5, "u") ? 1 : !strcmp(nm + 5, "su") ? 2 : !strcmp(nm + 5, "us") ? 3 : 0;
        break;
    case H_MPYHI: case H_MPYHIR: case H_MPYLI: case H_MPYLIR:
        in->sub = nm[3] == 'i';
        break;
    case H_DOTPRSU2: case H_DOTPNRSU2:
        in->sub = strstr(nm, "us2") != NULL;
        break;
    case H_DOTPSU4: case H_DOTPU4:
        in->sub = !strcmp(nm, "dotpus4");
        break;
    }
    in->fop = fop_class(in);
    /* Load/store at base register +/- constant with no write-back, 32-bit
     * register value/destination: the fast form reads and writes RAM inline. */
    in->mfast = 0;
    in->ea_delta = 0;
    /* Also the constant write-back modes (mem_ea 8-11: *++R, *--R, *R++, *R--)
     * and doubleword stores from a 64-bit pair (stdw). */
    if ((in->fop == F_LOAD || in->fop == F_STORE) && in->nops == 2) {
        const c66x_operand *m = &in->op[in->fop == F_LOAD ? 0 : 1];
        const c66x_operand *r = &in->op[in->fop == F_LOAD ? 1 : 0];
        unsigned mode = m->mem_mode;
        int dw = in->sub == LD_DW;
        int reg_ok = dw ? in->fop == F_STORE && r->kind == C66X_OPK_PAIR && r->size == 8
                        : r->kind == C66X_OPK_REG && r->size == 4
                          && (in->fop == F_STORE || r->low_first >= 1);
        if (m->kind == C66X_OPK_MEM && mode <= 11 && (mode <= 1 || mode >= 8) && reg_ok) {
            uint32_t off = (uint32_t)m->val * m->mem_scale;
            in->ea_delta = (int32_t)(mode & 1 ? off : 0u - off);
            unsigned sz = in->sub == LD_B || in->sub == LD_BU ? 1 : in->sub == LD_H || in->sub == LD_HU ? 2 : 4;
            unsigned sx = in->fop == F_LOAD && (in->sub == LD_B || in->sub == LD_H);
            unsigned wb = mode == 8 || mode == 9 ? 1 : mode == 10 || mode == 11 ? 2 : 0;
            in->mfast = (uint8_t)(sz | (sx << 3) | (wb << 4) | (dw << 6));
        }
    }
}

static int fp_reader(void *opaque, uint32_t fp_addr, uint8_t fp[32])
{
    c66x_core *c = opaque;
    uint8_t *p = ram_ptr(c, fp_addr, 32);
    if (!p)
        return -1;
    memcpy(fp, p, 32);
    return 0;
}

c66x_insn *fetch_insn(c66x_core *c, uint32_t a)
{
    uint8_t *p = ram_ptr(c, a & ~31u, 32);
    if (!p)
        return NULL;
    ramreg *r = &c->ram[c->last_ram];
    if (!r->fpp)
        r->fpp = calloc((r->size >> FP_PAGE_SHIFT) + 1, sizeof(fpblk **));
    uint32_t off = a - r->base;
    fpblk **pg = r->fpp[off >> FP_PAGE_SHIFT];
    if (!pg) {
        pg = r->fpp[off >> FP_PAGE_SHIFT] = calloc(1u << (FP_PAGE_SHIFT - 5), sizeof(fpblk *));
        r->codepage[off >> FP_PAGE_SHIFT] = 1;
    }
    fpblk **bp = &pg[(off >> 5) & ((1u << (FP_PAGE_SHIFT - 5)) - 1)];
    if (!*bp)
        *bp = calloc(1, sizeof(fpblk));
    fpblk *b = *bp;
    unsigned slot = (a & 31) >> 1;
    if (!(b->valid & (1u << slot))) {
        c66x_insn *in = &b->insn[slot];
        c66x_decode(fp_reader, c, a, in, NULL, 0);
        in->handler = (in->opc >= 0 && in->opc < 2256) ? c->htab[in->opc] : H_UNIMP;
        classify(in);
        b->valid |= 1u << slot;
        c->st.decodes++;
    }
    return &b->insn[slot];
}
