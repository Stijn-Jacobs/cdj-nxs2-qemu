/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Blackfin core: registers, the event controller, the core timer and the
 * step loop. Instruction semantics are in bfin_exec.c.
 *
 * Timing is one core cycle per instruction or parallel bundle; there is no
 * pipeline model. Stalls do not matter to the display firmware, whose only
 * time bases are the core timer and the SCLK timers, both derived from this
 * count.
 */
#include "bfin_priv.h"
#include <stddef.h>
#include <stdlib.h>

#define MMR_EVT0    0xFFE02000
#define MMR_IMASK   0xFFE02104
#define MMR_IPEND   0xFFE02108
#define MMR_ILAT    0xFFE0210C
#define MMR_IPRIO   0xFFE02110
#define MMR_TCNTL   0xFFE03000
#define MMR_TPERIOD 0xFFE03004
#define MMR_TSCALE  0xFFE03008
#define MMR_TCOUNT  0xFFE0300C

#define TCNTL_PWR   1u
#define TCNTL_EN    2u
#define TCNTL_AUTO  4u
#define TCNTL_INT   8u

/* IMASK bits 0-4 are the unmaskable events and always read as set. */
#define IMASK_FIXED 0x1Fu

bfin_core *bfin_new(const bfin_bus *bus)
{
    bfin_core *c = calloc(1, sizeof(*c));

    c->bus = *bus;
    c->blocks = calloc(BLOCK_SLOTS, sizeof(*c->blocks));
    c->code_lines = calloc(1, CODE_LINE(~0u) + 1);
    c->tzero = UINT64_MAX;
    bfin_decode_init();
    return c;
}

void bfin_free(bfin_core *c)
{
    free(c->blocks);
    free(c->code_lines);
    free(c);
}

void bfin_map_ram(bfin_core *c, uint32_t base, uint32_t size, uint8_t *host)
{
    if (c->nram < BFIN_MAX_RAM) {
        c->ram[c->nram++] = (bfin_ram){ base, size, host, 0 };
    }
}

void bfin_map_rom(bfin_core *c, uint32_t base, uint32_t size,
                  const uint8_t *host)
{
    if (c->nram < BFIN_MAX_RAM) {
        c->ram[c->nram++] = (bfin_ram){ base, size, (uint8_t *)host, 1 };
    }
}

void bfin_reset(bfin_core *c, uint32_t pc)
{
    memset(c->r, 0, offsetof(bfin_core, cmmr) - offsetof(bfin_core, r));
    c->pc = pc;
    c->ipend = 1u << EV_RST;
    c->imask = IMASK_FIXED;
    c->syscfg = 0x30;
    c->tzero = UINT64_MAX;
    c->idle = 0;
    c->irq_check = 1;
}

const uint8_t *bfin_code_page(bfin_core *c, uint32_t addr)
{
    for (int n = 0; n < c->nram; n++) {
        bfin_ram *r = &c->ram[n];

        if (r->size >= 8 && addr - r->base <= r->size - 8) {
            c->code_base = r->base;
            c->code_span = r->size - 7;
            c->code_host = r->host;
            return r->host + (addr - r->base);
        }
    }
    return NULL;
}

/* ---- registers ---------------------------------------------------------- */

uint32_t bfin_astat(const bfin_core *c)
{
    return (c->astat & ~AS_CC) | (c->cc ? AS_CC : 0);
}

void bfin_set_astat(bfin_core *c, uint32_t v)
{
    c->astat = v & ~AS_CC;
    c->cc = !!(v & AS_CC);
}

static uint32_t sp_read(const bfin_core *c)
{
    return c->p[6];
}

uint32_t bfin_reg(bfin_core *c, unsigned grp, unsigned reg)
{
    switch (grp) {
    case 0: return c->r[reg];
    case 1: return c->p[reg];
    case 2: return reg < 4 ? c->i[reg] : c->m[reg - 4];
    case 3: return reg < 4 ? c->b[reg] : c->l[reg - 4];
    case 4:
        switch (reg) {
        case 0: return (int32_t)(int8_t)(c->a[0] >> 32);
        case 1: return (uint32_t)c->a[0];
        case 2: return (int32_t)(int8_t)(c->a[1] >> 32);
        case 3: return (uint32_t)c->a[1];
        case 6: return bfin_astat(c);
        case 7: return c->rets;
        }
        break;
    case 6:
        switch (reg) {
        case 0: return c->lc[0];
        case 1: return c->lt[0];
        case 2: return c->lb[0];
        case 3: return c->lc[1];
        case 4: return c->lt[1];
        case 5: return c->lb[1];
        case 6: return (uint32_t)c->cycles;
        case 7: return (uint32_t)(c->cycles >> 32);
        }
        break;
    case 7:
        switch (reg) {
        case 0: return bfin_user_mode(c) ? sp_read(c) : c->usp;
        case 1: return c->seqstat;
        case 2: return c->syscfg;
        case 3: return c->reti;
        case 4: return c->retx;
        case 5: return c->retn;
        case 6: return c->rete;
        }
        break;
    }
    c->undef = 1;
    return 0;
}

static void set_acc_x(bfin_core *c, int n, uint32_t v)
{
    c->a[n] = (int64_t)((uint64_t)(int8_t)v << 32 | (uint32_t)c->a[n]);
}

void bfin_set_reg(bfin_core *c, unsigned grp, unsigned reg, uint32_t v)
{
    switch (grp) {
    case 0: c->r[reg] = v; return;
    case 1: c->p[reg] = v; return;
    case 2: if (reg < 4) c->i[reg] = v; else c->m[reg - 4] = v; return;
    case 3: if (reg < 4) c->b[reg] = v; else c->l[reg - 4] = v; return;
    case 4:
        switch (reg) {
        case 0: set_acc_x(c, 0, v); return;
        case 1: c->a[0] = (c->a[0] & ~0xFFFFFFFFll) | v; return;
        case 2: set_acc_x(c, 1, v); return;
        case 3: c->a[1] = (c->a[1] & ~0xFFFFFFFFll) | v; return;
        case 6: bfin_set_astat(c, v); return;
        case 7: c->rets = v; return;
        }
        break;
    case 6:
        switch (reg) {
        case 0: c->lc[0] = v; return;
        case 1: c->lt[0] = v & ~1u; return;
        case 2: c->lb[0] = v & ~1u; return;
        case 3: c->lc[1] = v; return;
        case 4: c->lt[1] = v & ~1u; return;
        case 5: c->lb[1] = v & ~1u; return;
        case 6: case 7: return;
        }
        break;
    case 7:
        switch (reg) {
        case 0:
            if (bfin_user_mode(c)) {
                c->p[6] = v;
            } else {
                c->usp = v;
            }
            return;
        case 1: c->seqstat = v; return;
        case 2: c->syscfg = v; return;
        case 3: c->reti = v; return;
        case 4: c->retx = v; return;
        case 5: c->retn = v; return;
        case 6: c->rete = v; return;
        }
        break;
    }
    c->undef = 1;
}

/* ---- event controller --------------------------------------------------- */

/* The event to take now, or -1: the highest-priority latched, enabled event
 * above everything in service. IPEND bit 4 holds off IVHW..IVG15 from event
 * entry until the handler saves RETI. */
static int irq_ready(const bfin_core *c)
{
    uint32_t pend = (c->ilat | c->ivg_level) & c->imask;
    uint32_t active = c->ipend & ~(1u << EV_GLOBAL);
    uint32_t above = active ? (active & -active) - 1 : 0xFFFFu;

    if (c->ipend & (1u << EV_GLOBAL)) {
        above &= (1u << EV_IVHW) - 1;
    }
    pend &= above & ~(1u << EV_GLOBAL);
    return pend ? __builtin_ctz(pend) : -1;
}

static void enter_mode(bfin_core *c)
{
    if (bfin_user_mode(c)) {
        c->usp = c->p[6];
        c->p[6] = c->ksp;
    }
}

static void take_event(bfin_core *c, int ev)
{
    enter_mode(c);
    c->ilat &= ~(1u << ev);
    c->ipend |= 1u << ev;
    switch (ev) {
    case EV_NMI:
        c->retn = c->pc;
        break;
    case EV_EMU:
    case EV_RST:
        break;
    default:
        c->reti = c->pc;
        c->ipend |= 1u << EV_GLOBAL;
        break;
    }
    c->pc = c->evt[ev];
    c->idle = 0;
    c->irq_check = 1;
}

void bfin_raise(bfin_core *c, int ev)
{
    c->ilat |= 1u << ev;
    c->irq_check = 1;
}

void bfin_exception(bfin_core *c, int excause, uint32_t retx)
{
    enter_mode(c);
    c->seqstat = (c->seqstat & ~0x3Fu) | excause;
    c->retx = retx;
    c->ipend |= 1u << EV_EVX;
    c->npc = c->evt[EV_EVX];
    c->irq_check = 1;
}

void bfin_return(bfin_core *c, int ev)
{
    uint32_t active = c->ipend & ~((1u << EV_GLOBAL) | (1u << EV_EVX) |
                                   (1u << EV_NMI) | (1u << EV_EMU));

    switch (ev) {
    case EV_IVHW:                               /* RTI */
        if (active) {
            c->ipend &= ~(active & -active);
        }
        c->ipend &= ~(1u << EV_GLOBAL);
        c->npc = c->reti;
        break;
    case EV_EVX:
        c->ipend &= ~(1u << EV_EVX);
        c->npc = c->retx;
        break;
    case EV_NMI:
        c->ipend &= ~(1u << EV_NMI);
        c->npc = c->retn;
        break;
    case EV_EMU:
        c->ipend &= ~1u;
        c->npc = c->rete;
        break;
    }
    if (bfin_user_mode(c)) {
        c->ksp = c->p[6];
        c->p[6] = c->usp;
    }
    c->irq_check = 1;
}

void bfin_cli(bfin_core *c, unsigned dreg)
{
    c->r[dreg] = c->imask;
    c->imask = IMASK_FIXED;
}

void bfin_sti(bfin_core *c, uint32_t mask)
{
    c->imask = mask | IMASK_FIXED;
    c->irq_check = 1;
}

void bfin_reti_pushed(bfin_core *c, int pushed)
{
    if (pushed) {
        c->ipend &= ~(1u << EV_GLOBAL);
        c->irq_check = 1;
    } else if (c->ipend & ~(1u << EV_GLOBAL)) {
        c->ipend |= 1u << EV_GLOBAL;
    }
}

void bfin_yield(bfin_core *c)
{
    c->yield = 1;
}

void bfin_wake(bfin_core *c)
{
    if (c->idle) {
        c->idle = 0;
    } else {
        c->wake = 1;
    }
}

void bfin_set_ivg(bfin_core *c, int ivg, int level)
{
    if (level) {
        c->ivg_level |= 1u << ivg;
        c->irq_check = 1;
    } else {
        c->ivg_level &= ~(1u << ivg);
    }
}

/* ---- core timer --------------------------------------------------------- */

static int timer_running(const bfin_core *c)
{
    return (c->tcntl & (TCNTL_PWR | TCNTL_EN)) == (TCNTL_PWR | TCNTL_EN);
}

static uint32_t timer_count(const bfin_core *c)
{
    if (c->tzero == UINT64_MAX || c->tzero <= c->cycles) {
        return c->tzero == UINT64_MAX ? c->cmmr[(MMR_TCOUNT & 0x3FFF) / 4] : 0;
    }
    return (c->tzero - c->cycles) / ((c->tscale & 0xFF) + 1);
}

static void timer_start(bfin_core *c, uint32_t count)
{
    c->tzero = timer_running(c) && count ?
        c->cycles + (uint64_t)count * ((c->tscale & 0xFF) + 1) : UINT64_MAX;
    c->cmmr[(MMR_TCOUNT & 0x3FFF) / 4] = count;
    c->irq_check = 1;                       /* ends the running block */
}

static void timer_expire(bfin_core *c)
{
    c->tcntl |= TCNTL_INT;
    bfin_raise(c, EV_IVTMR);
    timer_start(c, c->tcntl & TCNTL_AUTO ? c->tperiod : 0);
}

uint64_t bfin_timer_due(const bfin_core *c)
{
    return c->tzero == UINT64_MAX ? UINT64_MAX :
           c->tzero > c->cycles ? c->tzero - c->cycles : 0;
}

/* ---- core MMRs ---------------------------------------------------------- */

uint32_t bfin_mmr_read(bfin_core *c, uint32_t addr)
{
    if (addr - MMR_EVT0 < 16 * 4) {
        return c->evt[(addr - MMR_EVT0) / 4];
    }
    switch (addr) {
    case MMR_IMASK:   return c->imask;
    case MMR_IPEND:   return c->ipend;
    case MMR_ILAT:    return c->ilat;
    case MMR_TCNTL:   return c->tcntl;
    case MMR_TPERIOD: return c->tperiod;
    case MMR_TSCALE:  return c->tscale;
    case MMR_TCOUNT:  return timer_count(c);
    }
    if (addr - 0xFFE00000 < sizeof(c->cmmr)) {
        return c->cmmr[(addr - 0xFFE00000) / 4];
    }
    return 0;
}

void bfin_mmr_write(bfin_core *c, uint32_t addr, uint32_t val)
{
    if (addr - MMR_EVT0 < 16 * 4) {
        c->evt[(addr - MMR_EVT0) / 4] = val;
        return;
    }
    switch (addr) {
    case MMR_IMASK:
        bfin_sti(c, val);
        return;
    case MMR_ILAT:
        /* Write-one-to-clear. */
        c->ilat &= ~val;
        return;
    case MMR_IPEND:
        return;
    case MMR_TCNTL: {
        uint32_t count = timer_count(c);

        /* TINT is sticky and write-one-to-clear. */
        c->tcntl = (val & ~TCNTL_INT) | (c->tcntl & TCNTL_INT & ~val);
        timer_start(c, count ? count : c->tperiod);
        return;
    }
    case MMR_TPERIOD:
        c->tperiod = val;
        if (!timer_running(c)) {
            c->cmmr[(MMR_TCOUNT & 0x3FFF) / 4] = val;
        }
        return;
    case MMR_TSCALE:
        c->tscale = val;
        return;
    case MMR_TCOUNT:
        timer_start(c, val);
        return;
    }
    if (addr - 0xFFE00000 < sizeof(c->cmmr)) {
        c->cmmr[(addr - 0xFFE00000) / 4] = val;
    }
}

/* ---- step loop ---------------------------------------------------------- */

unsigned bfin_insn_len(uint16_t iw0)
{
    if ((iw0 & 0xC000) != 0xC000 || (iw0 & 0xFE00) == 0xF800) {
        return 2;
    }
    return (iw0 & 0xF800) == 0xC800 ? 8 : 4;
}

static void trace_insn(bfin_core *c, uint32_t pc, unsigned len)
{
    fprintf(c->trace, "%08x:", pc);
    for (unsigned n = 0; n < len; n += 2) {
        fprintf(c->trace, " %04x", bfin_fetch16(c, pc + n));
    }
    fputc('\n', c->trace);
}

/* A hardware loop's bottom: loop 1 is checked first, so it must be the inner
 * loop when both end on one instruction. */
static uint32_t loop_bottom(bfin_core *c, uint32_t pc, uint32_t npc)
{
    for (int n = 1; n >= 0; n--) {
        if (pc == c->lb[n] && c->lc[n]) {
            if (--c->lc[n]) {
                return c->lt[n];
            }
        }
    }
    return npc;
}

/* JUMP.S 0 or JUMP.L 0, the form the ThreadX idle thread waits in. */
static int self_jump(uint16_t iw0, uint16_t iw1)
{
    return iw0 == 0x2000 || (iw0 == 0xE200 && iw1 == 0);
}

static inline int same_bytes(const uint8_t *a, const uint8_t *b)
{
    uint64_t x = 0, p, q;

    for (int n = 0; n < BLOCK_BYTES; n += 8) {
        memcpy(&p, a + n, 8);
        memcpy(&q, b + n, 8);
        x |= p ^ q;
    }
    return x == 0;
}

/* Whether an active hardware loop ends on an instruction of b before its
 * last one, where the chain would run past the loop's bottom. */
static int loop_inside(const bfin_core *c, const bfin_block *b)
{
    return (c->lc[0] && c->lb[0] - b->pc < b->span) ||
           (c->lc[1] && c->lb[1] - b->pc < b->span);
}

/* The block at c->pc when the run can go on into it without the checks
 * between blocks in bfin_step: no stop flag is up, it was checked in this
 * step and it ends below run_limit. */
static inline const bfin_block *next_block(bfin_core *c)
{
    const bfin_block *b = &c->blocks[(c->pc >> 1) % BLOCK_SLOTS];

    if (c->stop_flags || b->pc != c->pc || !b->n || b->gen != c->code_gen ||
        c->cycles + b->n > c->run_limit || loop_inside(c, b) ||
        c->pc == c->break_pc) {
        return NULL;
    }
    return b;
}

static int loop_ends_at(const bfin_core *c, uint32_t pc)
{
    return (c->lc[0] && pc == c->lb[0]) || (c->lc[1] && pc == c->lb[1]);
}

/* The last entry of a chain: the instruction before it went on to the next
 * one, which may be a hardware loop's top. */
static int chain_end(bfin_core *c, const bfin_insn *i)
{
    uint32_t last = i[-1].pc, npc = i->pc;

    BFIN_SYNC(c, i);
    if (last == c->lb[0] || last == c->lb[1]) {
        npc = loop_bottom(c, last, npc);
    }
    c->pc = npc;
#ifdef BFIN_CHAINS
    const bfin_block *b = next_block(c);

    if (b) {
        c->cycles_at = c->cycles;
        BFIN_MUSTTAIL return b->op[0].fn(c, b->op);
    }
#endif
    return 0;
}

/* The block of instructions starting at pc, decoded now if the slot holds
 * something else, the code changed or a loop's bottom moved inside it; NULL
 * when pc is not in RAM. */
static bfin_block *block_at(bfin_core *c, uint32_t pc)
{
    const uint8_t *code = bfin_code(c, pc);
    bfin_block *b;
    unsigned off = 0, k = 0;

    if (!code || pc - c->code_base >= c->code_span - (BLOCK_BYTES - 8)) {
        return NULL;
    }
    b = &c->blocks[(pc >> 1) % BLOCK_SLOTS];
    if (b->n && b->pc == pc) {
        if (b->gen == c->code_gen) {
            if (!loop_inside(c, b)) {
                return b;
            }
        } else if (same_bytes(code, b->raw)) {
            b->gen = c->code_gen;
            if (!loop_inside(c, b)) {
                return b;
            }
        }
    }
    c->code_lines[CODE_LINE(pc)] = 1;
    c->code_lines[CODE_LINE(pc + BLOCK_BYTES - 1)] = 1;
    b->gen = c->code_gen;
    memcpy(b->raw, code, BLOCK_BYTES);
    b->pc = pc;
    b->n = 0;
    while (off < BLOCK_BYTES && b->n < BLOCK_MAX) {
        uint16_t iw0, iw1 = 0;
        unsigned len;

        memcpy(&iw0, code + off, 2);
        len = bfin_insn_len(iw0);
        if (off + len > BLOCK_BYTES || (off && pc + off == c->break_pc)) {
            break;
        }
        if (len > 2) {
            memcpy(&iw1, code + off + 2, 2);
        }
        off += len;
        /* A NOP only takes its cycle, which the idx of the instruction after
         * it counts, so it leaves the chain unless it ends the block. */
        if (k && b->op[k - 1].iw0 == 0) {
            k--;
        }
        b->op[k].idx = b->n++;
        if (bfin_decode(c, &b->op[k++], pc + off - len, iw0, iw1) ||
            loop_ends_at(c, pc + off - len)) {
            break;
        }
    }
    b->op[k].fn = chain_end;
    b->op[k].pc = pc + off;
    b->op[k].idx = b->n;
    b->span = b->op[k - 1].pc - pc;
    return b;
}

static void trap(bfin_core *c, uint32_t pc, uint16_t iw0, uint16_t iw1,
                 unsigned len)
{
    c->trap_pc = pc;
    c->trap_insn = (uint64_t)iw0 << 16 | iw1;
    if (len == 8) {
        c->trap_insn = c->trap_insn << 32 |
            (uint32_t)bfin_fetch16(c, pc + 4) << 16 | bfin_fetch16(c, pc + 6);
    }
}

/* Only an event leaves a jump to itself, and none can be taken before the
 * core timer expires or the caller's budget ends: the peripherals that raise
 * the others run between steps. Every pass is one cycle, so the passes are
 * counted, not run. */
static void skip_self_jump(bfin_core *c, uint64_t end)
{
    uint64_t until = end < c->tzero ? end : c->tzero;

    if (until > c->cycles) {
        c->cycles = until;
    }
}

int bfin_end(bfin_core *c, const bfin_insn *i)
{
    uint32_t pc = i->pc, npc = c->npc;

    if (c->undef) {
        BFIN_SYNC(c, i);
        c->pc = pc;
        trap(c, pc, i->iw0, i->iw1, i->len);
        return 1;
    }
    if (npc == pc + i->len && (pc == c->lb[0] || pc == c->lb[1])) {
        npc = loop_bottom(c, pc, npc);
    }
    c->pc = npc;
    c->cycles = c->cycles_at + i->idx + 1;
    if (npc == pc && self_jump(i->iw0, i->iw1) && !c->trace &&
        pc != c->break_pc) {
        skip_self_jump(c, c->step_end);
    }
#ifdef BFIN_CHAINS
    const bfin_block *b = next_block(c);

    if (b) {
        c->cycles_at = c->cycles;
        BFIN_MUSTTAIL return b->op[0].fn(c, b->op);
    }
#endif
    return 0;
}

/* Executes the instruction at pc on its own. Returns nonzero if it is not
 * implemented, with the trap recorded. */
static int run_insn(bfin_core *c, uint32_t pc)
{
    const uint8_t *code = bfin_code(c, pc);
    bfin_insn one[2];
    uint16_t iw0, iw1;
    unsigned len;

    if (code) {
        memcpy(&iw0, code, 2);
        memcpy(&iw1, code + 2, 2);
    } else {
        iw0 = bfin_load(c, pc, 2);
        iw1 = bfin_load(c, pc + 2, 2);
    }
    len = bfin_insn_len(iw0);
    if (len == 2) {
        iw1 = 0;
    }
    if (c->trace && c->cycles >= c->trace_from && c->cycles < c->trace_to) {
        trace_insn(c, pc, len);
    }
    bfin_decode(c, &one[0], pc, iw0, iw1);
    one[0].idx = 0;
    one[1].fn = chain_end;
    one[1].pc = pc + len;
    one[1].idx = 1;
    c->cycles_at = c->cycles;
    return one[0].fn(c, one);
}

bfin_stop bfin_step(bfin_core *c, uint64_t budget, uint64_t *executed)
{
    uint64_t start = c->cycles, end = c->cycles + budget;
    bfin_stop stop = BFIN_STOP_BUDGET;

    c->undef = 0;
    c->code_gen++;
    c->step_end = end;
    while (c->cycles < end) {
        if (c->cycles >= c->tzero) {
            timer_expire(c);
        }
        if (c->irq_check) {
            int ev = irq_ready(c);

            c->irq_check = 0;
            if (ev >= 0) {
                take_event(c, ev);
            }
        }
        if (c->idle) {
            stop = BFIN_STOP_IDLE;
            break;
        }

        uint32_t pc = c->pc;

        if (pc == c->break_pc && c->cycles != start) {
            stop = BFIN_STOP_BREAK;
            break;
        }
        bfin_block *blk = c->trace ? NULL : block_at(c, pc);
        /* A block runs whole or not at all: inside it nothing tests the
         * cycle limit, and an instruction that cannot stop the run does not
         * look at the flags, so an event just taken (irq_check) gets the
         * checks above again after one instruction. A change to tzero
         * raises irq_check, which also stops blocks running on into each
         * other below run_limit. */
        c->run_limit = c->trace ? 0 : end < c->tzero ? end : c->tzero;

        if (blk && c->cycles + blk->n <= c->run_limit && !c->irq_check) {
            c->cycles_at = c->cycles;
            if (blk->op[0].fn(c, blk->op)) {
                stop = BFIN_STOP_UNDEF;
                break;
            }
        } else if (run_insn(c, pc)) {
            stop = BFIN_STOP_UNDEF;
            break;
        }
        if (c->yield) {
            c->yield = 0;
            break;
        }
    }
    if (executed) {
        *executed = c->cycles - start;
    }
    return stop;
}

void bfin_skip_cycles(bfin_core *c, uint64_t n)
{
    c->cycles += n;
}

uint64_t bfin_cycles(const bfin_core *c)
{
    return c->cycles;
}

uint32_t bfin_get_pc(const bfin_core *c)
{
    return c->pc;
}

uint32_t bfin_get_reg(bfin_core *c, unsigned grp, unsigned reg)
{
    return bfin_reg(c, grp, reg);
}

uint32_t bfin_get_ipend(const bfin_core *c)
{
    return c->ipend;
}

uint32_t bfin_get_mmr(bfin_core *c, uint32_t addr)
{
    return bfin_mmr_read(c, addr);
}

uint32_t bfin_trap_pc(const bfin_core *c)
{
    return c->trap_pc;
}

uint64_t bfin_trap_insn(const bfin_core *c)
{
    return c->trap_insn;
}

void bfin_set_trace(bfin_core *c, FILE *f, uint64_t from, uint64_t to)
{
    c->trace = f;
    c->trace_from = from;
    c->trace_to = to;
}

void bfin_set_break(bfin_core *c, uint32_t pc)
{
    c->break_pc = pc;
    memset(c->blocks, 0, BLOCK_SLOTS * sizeof(*c->blocks));
}
