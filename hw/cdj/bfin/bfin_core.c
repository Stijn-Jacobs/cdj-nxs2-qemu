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
    c->tzero = UINT64_MAX;
    return c;
}

void bfin_free(bfin_core *c)
{
    free(c);
}

void bfin_map_ram(bfin_core *c, uint32_t base, uint32_t size, uint8_t *host)
{
    if (c->nram < BFIN_MAX_RAM) {
        c->ram[c->nram++] = (bfin_ram){ base, size, host };
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

bfin_stop bfin_step(bfin_core *c, uint64_t budget, uint64_t *executed)
{
    uint64_t start = c->cycles, end = c->cycles + budget;
    bfin_stop stop = BFIN_STOP_BUDGET;

    while (c->cycles < end) {
        if (c->cycles >= c->tzero) {
            timer_expire(c);
        }
        if (c->irq_check || c->ivg_level) {
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
        uint16_t iw0 = bfin_fetch16(c, pc);
        unsigned len = bfin_insn_len(iw0);
        uint16_t iw1 = len > 2 ? bfin_fetch16(c, pc + 2) : 0;

        if (c->trace && c->cycles >= c->trace_from && c->cycles < c->trace_to) {
            trace_insn(c, pc, len);
        }
        c->npc = pc + len;
        c->undef = 0;
        bfin_exec(c, iw0, iw1, len);
        if (c->undef) {
            c->trap_pc = pc;
            c->trap_insn = (uint64_t)iw0 << 16 | iw1;
            if (len == 8) {
                c->trap_insn = c->trap_insn << 32 |
                    (uint32_t)bfin_fetch16(c, pc + 4) << 16 |
                    bfin_fetch16(c, pc + 6);
            }
            stop = BFIN_STOP_UNDEF;
            break;
        }
        c->pc = c->npc == pc + len ? loop_bottom(c, pc, c->npc) : c->npc;
        c->cycles++;
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
}
