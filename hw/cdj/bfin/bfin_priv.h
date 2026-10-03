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

/* Executes one instruction; the handler of its first word does it all. The
 * handlers of 16-bit instructions ignore iw1. */
typedef void bfin_op(bfin_core *c, uint16_t iw0, uint16_t iw1);

typedef struct bfin_ram {
    uint32_t base, size;
    uint8_t *host;
    int      rom;               /* read here, written through the bus */
} bfin_ram;

/* A decoded instruction. Its handler runs it and, while the code stays
 * straight, jumps on to the handler of the next one, so a block runs as one
 * chain of tail calls that the block's last entry ends. Returns nonzero on an
 * unimplemented instruction. */
typedef struct bfin_insn bfin_insn;
typedef int bfin_xop(bfin_core *c, const bfin_insn *i);

struct bfin_insn {
    bfin_xop *fn;
    bfin_op  *op;               /* the handler of its first word */
    uint32_t pc;
    uint16_t iw0, iw1;
    bfin_op  *slot[2];          /* a bundle's 16-bit slots, NULL for a NOP */
    uint16_t sw[2];
    uint16_t idx;               /* cycles from the start of the chain */
    uint16_t len;
};

/* Straight-line code decoded once: up to 16 instructions from the 32 bytes at
 * pc, ending early after an active hardware loop's bottom or an instruction
 * that can move one. The bytes are kept so a block is valid only while the
 * code is unchanged. op[n] ends the chain; its pc is the address after the
 * last instruction. */
#define BLOCK_BYTES 32
#define BLOCK_MAX   16
#define BLOCK_SLOTS 2048

typedef struct bfin_block {
    uint32_t pc;
    unsigned n;
    uint32_t span;              /* from pc to the last instruction */
    uint64_t gen;               /* code_gen when the bytes were last checked */
    uint8_t  raw[BLOCK_BYTES];
    bfin_insn op[BLOCK_MAX + 1];
} bfin_block;

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
    /* Inside a chain the count is cycles_at + the running instruction's idx;
     * cycles itself is brought up to date only where something can read it:
     * a bus or MMR access, a general handler, the end of the chain. */
    uint64_t cycles_at;
    uint64_t step_end;          /* the cycle the running step stops at */
    uint64_t run_limit;         /* blocks follow each other below it */
    int      wake;              /* the next IDLE returns at once */
    /* What ends a run of straight-line code, tested as one word. */
    union {
        struct {
            uint8_t undef;
            uint8_t irq_check;  /* an event may be ready to take */
            uint8_t idle;
            uint8_t yield;
        };
        uint32_t stop_flags;
    };

    /* Execution of the current instruction. */
    uint32_t npc;
    int      in_bundle;
    int      nload;             /* loads a bundle holds back until its end */
    struct { uint8_t grp, reg; uint32_t val; } load[2];

    bfin_ram ram[BFIN_MAX_RAM];
    int      nram;
    /* The RAM bank instructions last came from, less its last 8 bytes so a
     * whole 64-bit bundle can be read without a bounds check. */
    uint32_t code_base, code_span;
    uint8_t *code_host;
    bfin_block *blocks;
    /* Blocks are trusted without comparing their bytes while code_gen is
     * unchanged. It moves on a store into a 256-byte line some block was
     * decoded from (code_lines, by address bits 8-27), a bus write (a DMA
     * may land in RAM) and every step (the SoC runs between steps). */
    uint8_t *code_lines;
    uint64_t code_gen;
    bfin_bus bus;

    uint32_t trap_pc;
    uint64_t trap_insn;
    uint32_t break_pc;
    FILE    *trace;
    uint64_t trace_from, trace_to;
};

/* The bank holding the size bytes at addr, or NULL. */
static inline const bfin_ram *bfin_bank(bfin_core *c, uint32_t addr,
                                        unsigned size)
{
    for (int n = 0; n < c->nram; n++) {
        const bfin_ram *r = &c->ram[n];

        if (addr - r->base <= r->size - size) {
            return r;
        }
    }
    return NULL;
}

uint32_t bfin_mmr_read(bfin_core *c, uint32_t addr);
void     bfin_mmr_write(bfin_core *c, uint32_t addr, uint32_t val);

static inline uint32_t bfin_load(bfin_core *c, uint32_t addr, unsigned size)
{
    const bfin_ram *r = bfin_bank(c, addr, size);
    uint32_t v = 0;

    if (r) {
        memcpy(&v, r->host + (addr - r->base), size);
        return v;
    }
    if (addr >= 0xFFE00000) {
        return bfin_mmr_read(c, addr);
    }
    return c->bus.read(c->bus.opaque, addr, size);
}

#define CODE_LINE(addr) (((addr) >> 8) & 0xFFFFF)

static inline void bfin_store(bfin_core *c, uint32_t addr, uint32_t val,
                              unsigned size)
{
    const bfin_ram *r = bfin_bank(c, addr, size);

    if (r && !r->rom) {
        memcpy(r->host + (addr - r->base), &val, size);
        if (c->code_lines[CODE_LINE(addr)] |
            c->code_lines[CODE_LINE(addr + size - 1)]) {
            c->code_gen++;
        }
        return;
    }
    if (addr >= 0xFFE00000) {
        bfin_mmr_write(c, addr, val);
        return;
    }
    c->code_gen++;
    c->bus.write(c->bus.opaque, addr, val, size);
}

const uint8_t *bfin_code_page(bfin_core *c, uint32_t addr);

/* Host bytes of the code at addr, at least 8 of them, or NULL when it is not
 * in RAM. The bank the last fetch came from is tried first. */
static inline const uint8_t *bfin_code(bfin_core *c, uint32_t addr)
{
    if (addr - c->code_base < c->code_span) {
        return c->code_host + (addr - c->code_base);
    }
    return bfin_code_page(c, addr);
}

static inline uint16_t bfin_fetch16(bfin_core *c, uint32_t addr)
{
    const uint8_t *h = bfin_code(c, addr);
    uint16_t v;

    if (!h) {
        return bfin_load(c, addr, 2);
    }
    memcpy(&v, h, 2);
    return v;
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

/* Fills the decode tables bfin_exec dispatches through; idempotent. */
void bfin_decode_init(void);

/* Decodes the instruction at pc whose first words are iw0 and iw1; returns
 * nonzero when it can move a hardware loop (LB, LC), which ends a block. */
int bfin_decode(bfin_core *c, bfin_insn *in, uint32_t pc, uint16_t iw0,
                uint16_t iw1);

/* Ends a chain after instruction i, which left c->npc as the next pc. */
int bfin_end(bfin_core *c, const bfin_insn *i);

/* Without guaranteed tail calls a chain is only as deep as its block, so
 * blocks do not run on into each other. */
#if defined(__has_attribute)
#if __has_attribute(musttail)
#define BFIN_MUSTTAIL __attribute__((musttail))
#define BFIN_CHAINS 1
#endif
#endif
#ifndef BFIN_MUSTTAIL
#define BFIN_MUSTTAIL
#endif

/* Brings the cycle count up to the start of instruction i. */
#define BFIN_SYNC(c, i) ((c)->cycles = (c)->cycles_at + (i)->idx)

/* The end of an instruction that goes on to the next one in its block. */
#define BFIN_NEXT(c, i) do {                                    \
        BFIN_MUSTTAIL return (i)[1].fn((c), (i) + 1);           \
    } while (0)

/* The same for an instruction that can raise a stop flag (an event, an IDLE,
 * a bus access, an unimplemented form) but never branches. */
#define BFIN_NEXT_CHECKED(c, i) do {                            \
        if ((c)->stop_flags) {                                  \
            (c)->npc = (i)->pc + (i)->len;                      \
            BFIN_MUSTTAIL return bfin_end((c), (i));            \
        }                                                       \
        BFIN_NEXT((c), (i));                                    \
    } while (0)

#endif
