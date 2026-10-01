/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Blackfin core internals, shared by bfin_core.c and bfin_exec.c. */
#ifndef BFIN_PRIV_H
#define BFIN_PRIV_H

#include "bfin.h"
#include <string.h>

/* ASTAT bits. CC is kept apart in bfin_core.cc and merged on read. */
enum {
    AS_AZ = 1u << 0, AS_AN = 1u << 1, AS_AC0_COPY = 1u << 2,
    AS_V_COPY = 1u << 3, AS_CC = 1u << 5, AS_AQ = 1u << 6,
    AS_RND_MOD = 1u << 8, AS_AC0 = 1u << 12, AS_AC1 = 1u << 13,
    AS_AV0 = 1u << 16, AS_AV0S = 1u << 17, AS_AV1 = 1u << 18,
    AS_AV1S = 1u << 19, AS_V = 1u << 24, AS_VS = 1u << 25,
};

/* Core events (IPEND/ILAT/IMASK bit numbers). */
enum { EV_EMU, EV_RST, EV_NMI, EV_EVX, EV_GLOBAL, EV_IVHW, EV_IVTMR };

#define BFIN_MAX_RAM 8

typedef struct bfin_ram {
    uint32_t base, size;
    uint8_t *host;
} bfin_ram;

struct bfin_core {
    uint32_t pc;
    uint32_t r[8];
    uint32_t p[8];              /* P0-P5, SP, FP */
    uint32_t i[4], m[4], b[4], l[4];
    int64_t  a[2];              /* 40-bit accumulators, kept sign-extended */
    uint32_t lc[2], lt[2], lb[2];
    uint32_t astat;
    int      cc;
    uint32_t rets, reti, retx, retn, rete;
    uint32_t usp, ksp;          /* the SP of the mode not running */
    uint32_t seqstat, syscfg;

    /* CEC */
    uint32_t evt[16];
    uint32_t imask, ipend, ilat, ivg_level;

    /* Core timer: the count is derived from the cycle it reaches zero. */
    uint32_t tcntl, tperiod, tscale;
    uint64_t tzero;             /* cycle TCOUNT reaches 0; UINT64_MAX stopped */

    /* The core MMRs nothing interprets (L1 memory control, CPLBs, test
     * registers), stored so they read back. */
    uint32_t cmmr[0x4000 / 4];

    uint64_t cycles;
    int      idle;
    int      yield;
    int      wake;              /* the next IDLE returns at once */
    int      irq_check;         /* an event may be ready to take */

    /* Execution of the current instruction. */
    uint32_t npc;
    int      in_bundle;
    int      nload;             /* loads a bundle holds back until its end */
    struct { uint8_t grp, reg; uint32_t val; } load[2];
    int      undef;

    bfin_ram ram[BFIN_MAX_RAM];
    int      nram;
    bfin_bus bus;

    uint32_t trap_pc;
    uint64_t trap_insn;
    uint32_t break_pc;
    FILE    *trace;
    uint64_t trace_from, trace_to;
};

static inline uint8_t *bfin_host(bfin_core *c, uint32_t addr, unsigned size)
{
    for (int n = 0; n < c->nram; n++) {
        bfin_ram *r = &c->ram[n];
        if (addr - r->base <= r->size - size) {
            return r->host + (addr - r->base);
        }
    }
    return NULL;
}

uint32_t bfin_mmr_read(bfin_core *c, uint32_t addr);
void     bfin_mmr_write(bfin_core *c, uint32_t addr, uint32_t val);

static inline uint32_t bfin_load(bfin_core *c, uint32_t addr, unsigned size)
{
    uint8_t *h = bfin_host(c, addr, size);
    uint32_t v = 0;

    if (h) {
        memcpy(&v, h, size);
        return v;
    }
    if (addr >= 0xFFE00000) {
        return bfin_mmr_read(c, addr);
    }
    return c->bus.read(c->bus.opaque, addr, size);
}

static inline void bfin_store(bfin_core *c, uint32_t addr, uint32_t val,
                              unsigned size)
{
    uint8_t *h = bfin_host(c, addr, size);

    if (h) {
        memcpy(h, &val, size);
        return;
    }
    if (addr >= 0xFFE00000) {
        bfin_mmr_write(c, addr, val);
        return;
    }
    c->bus.write(c->bus.opaque, addr, val, size);
}

static inline uint16_t bfin_fetch16(bfin_core *c, uint32_t addr)
{
    return bfin_load(c, addr, 2);
}

static inline int bfin_user_mode(const bfin_core *c)
{
    return !(c->ipend & ~(1u << EV_GLOBAL));
}

uint32_t bfin_reg(bfin_core *c, unsigned grp, unsigned reg);
void     bfin_set_reg(bfin_core *c, unsigned grp, unsigned reg, uint32_t v);
uint32_t bfin_astat(const bfin_core *c);
void     bfin_set_astat(bfin_core *c, uint32_t v);

/* Event controller operations the instructions reach. */
void bfin_raise(bfin_core *c, int ev);
void bfin_exception(bfin_core *c, int excause, uint32_t retx);
void bfin_return(bfin_core *c, int ev);
void bfin_cli(bfin_core *c, unsigned dreg);
void bfin_sti(bfin_core *c, uint32_t mask);
void bfin_reti_pushed(bfin_core *c, int pushed);

/* Executes the instruction at c->pc of len bytes; sets c->npc. */
void bfin_exec(bfin_core *c, uint16_t iw0, uint16_t iw1, unsigned len);

#endif
