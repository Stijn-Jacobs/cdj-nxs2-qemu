/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x core: a cycle-level interpreter.
 *
 * Timing model (see c66x_core.md). The C6000 exposes its pipeline to software
 * and the DSP image depends on it (load results are read exactly 5 cycles
 * later, 16x16 multiplies 2), so every register write lands at the cycle
 * binutils' operand table gives it
 * (E1 = cycle 1: single-cycle ops at 1, loads at 5, mpysp at 4, adddp 6/7) and
 * becomes readable the cycle after. A branch takes effect after five delay
 * slots, where a slot is one E1 advance: an execute packet or a NOP cycle
 * (NOP n, BNOP n, CALLP, ADDKPC n, PROT loads), never a stall.
 */
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

static const struct { const char *name; uint16_t h; } hmap[] = {
    { "nop", H_NOP }, { "idle", H_IDLE }, { "dint", H_DINT }, { "rint", H_RINT },
    { "swe", H_SWE }, { "swenr", H_SWE },
    { "abs", H_ABS }, { "abs2", H_ABS2 }, { "add", H_ADD }, { "addu", H_ADDU },
    { "sub", H_SUB }, { "subu", H_SUBU }, { "add2", H_ADD2 }, { "sub2", H_SUB2 },
    { "add4", H_ADD4 }, { "sub4", H_SUB4 },
    { "addab", H_ADDA }, { "addah", H_ADDA }, { "addaw", H_ADDA }, { "addad", H_ADDA },
    { "subab", H_SUBA }, { "subah", H_SUBA }, { "subaw", H_SUBA },
    { "addk", H_ADDK }, { "addkpc", H_ADDKPC },
    { "addsub", H_ADDSUB }, { "addsub2", H_ADDSUB2 }, { "saddsub", H_SADDSUB },
    { "saddsub2", H_SADDSUB2 },
    { "and", H_AND }, { "andn", H_ANDN }, { "or", H_OR }, { "xor", H_XOR },
    { "not", H_NOT }, { "neg", H_NEG }, { "mv", H_MV }, { "mvk", H_MVK },
    { "mvkl", H_MVK }, { "mvkh", H_MVKH }, { "mvklh", H_MVKH }, { "zero", H_ZERO },
    { "dmv", H_DMV }, { "mvc", H_MVC }, { "mvd", H_MVD },
    { "cmpeq", H_CMPEQ }, { "cmpgt", H_CMPGT }, { "cmpgtu", H_CMPGTU },
    { "cmplt", H_CMPLT }, { "cmpltu", H_CMPLTU },
    { "cmpeq2", H_CMPEQ2 }, { "cmpgt2", H_CMPGT2 }, { "cmplt2", H_CMPLT2 },
    { "cmpeq4", H_CMPEQ4 }, { "cmpgtu4", H_CMPGTU4 }, { "cmpltu4", H_CMPLTU4 },
    { "shl", H_SHL }, { "shr", H_SHR }, { "shru", H_SHRU }, { "sshl", H_SSHL },
    { "shr2", H_SHR2 }, { "shru2", H_SHRU2 }, { "sshvl", H_SSHVL }, { "sshvr", H_SSHVR },
    { "shlmb", H_SHLMB }, { "shrmb", H_SHRMB }, { "rotl", H_ROTL },
    { "clr", H_CLR }, { "set", H_SET }, { "ext", H_EXT }, { "extu", H_EXTU },
    { "sadd", H_SADD }, { "ssub", H_SSUB }, { "sat", H_SAT }, { "sadd2", H_SADD2 },
    { "ssub2", H_SSUB2 }, { "saddus2", H_SADDUS2 }, { "saddsu2", H_SADDSU2 },
    { "saddu4", H_SADDU4 },
    { "subc", H_SUBC }, { "subabs4", H_SUBABS4 }, { "norm", H_NORM }, { "lmbd", H_LMBD },
    { "bitc4", H_BITC4 }, { "bitr", H_BITR }, { "deal", H_DEAL }, { "shfl", H_SHFL },
    { "shfl3", H_SHFL3 }, { "xpnd2", H_XPND2 }, { "xpnd4", H_XPND4 },
    { "swap2", H_SWAP2 }, { "swap4", H_SWAP4 },
    { "avg2", H_AVG2 }, { "avgu4", H_AVGU4 }, { "max2", H_MAX2 }, { "maxu4", H_MAXU4 },
    { "min2", H_MIN2 }, { "minu4", H_MINU4 },
    { "pack2", H_PACK2 }, { "packh2", H_PACKH2 }, { "packhl2", H_PACKHL2 },
    { "packlh2", H_PACKLH2 }, { "packh4", H_PACKH4 }, { "packl4", H_PACKL4 },
    { "spack2", H_SPACK2 }, { "spacku4", H_SPACKU4 }, { "rpack2", H_RPACK2 },
    { "dpack2", H_DPACK2 }, { "dpackx2", H_DPACKX2 },
    { "unpkhu4", H_UNPKHU4 }, { "unpklu4", H_UNPKLU4 },
    { "mpy", H_MPY16 }, { "mpyu", H_MPY16 }, { "mpyus", H_MPY16 }, { "mpysu", H_MPY16 },
    { "mpyh", H_MPY16 }, { "mpyhu", H_MPY16 }, { "mpyhus", H_MPY16 }, { "mpyhsu", H_MPY16 },
    { "mpyhl", H_MPY16 }, { "mpyhlu", H_MPY16 }, { "mpyhslu", H_MPY16 },
    { "mpyhuls", H_MPY16 }, { "mpylh", H_MPY16 }, { "mpylhu", H_MPY16 },
    { "mpylshu", H_MPY16 }, { "mpyluhs", H_MPY16 },
    { "smpy", H_SMPY16 }, { "smpyh", H_SMPY16 }, { "smpyhl", H_SMPY16 }, { "smpylh", H_SMPY16 },
    { "mpy32", H_MPY32 }, { "mpy32u", H_MPY32 }, { "mpy32su", H_MPY32 }, { "mpy32us", H_MPY32 },
    { "mpyi", H_MPYI }, { "mpyid", H_MPYID },
    { "mpyhi", H_MPYHI }, { "mpyih", H_MPYHI }, { "mpyhir", H_MPYHIR }, { "mpyihr", H_MPYHIR },
    { "mpyli", H_MPYLI }, { "mpyil", H_MPYLI }, { "mpylir", H_MPYLIR }, { "mpyilr", H_MPYLIR },
    { "mpy2", H_MPY2 }, { "smpy2", H_SMPY2 }, { "mpysu4", H_MPYSU4 }, { "mpyu4", H_MPYU4 },
    { "mpyus4", H_MPYUS4 }, { "smpy32", H_SMPY32 }, { "mpy2ir", H_MPY2IR },
    { "dotp2", H_DOTP2 }, { "dotpn2", H_DOTPN2 }, { "dotprsu2", H_DOTPRSU2 },
    { "dotprus2", H_DOTPRSU2 }, { "dotpnrsu2", H_DOTPNRSU2 }, { "dotpnrus2", H_DOTPNRSU2 },
    { "dotpsu4", H_DOTPSU4 }, { "dotpus4", H_DOTPSU4 }, { "dotpu4", H_DOTPU4 },
    { "ddotp4", H_DDOTP4 }, { "ddotph2", H_DDOTPH2 }, { "ddotpl2", H_DDOTPL2 },
    { "ddotph2r", H_DDOTPH2R }, { "ddotpl2r", H_DDOTPL2R },
    { "cmpy", H_CMPY }, { "cmpyr", H_CMPYR }, { "cmpyr1", H_CMPYR1 },
    { "gmpy", H_GMPY }, { "gmpy4", H_GMPY4 }, { "xormpy", H_XORMPY },
    { "abssp", H_ABSSP }, { "absdp", H_ABSDP }, { "addsp", H_ADDSP }, { "adddp", H_ADDDP },
    { "subsp", H_SUBSP }, { "subdp", H_SUBDP }, { "mpysp", H_MPYSP }, { "mpydp", H_MPYDP },
    { "mpyspdp", H_MPYSPDP }, { "mpysp2dp", H_MPYSP2DP },
    { "cmpeqsp", H_CMPEQSP }, { "cmpgtsp", H_CMPGTSP }, { "cmpltsp", H_CMPLTSP },
    { "cmpeqdp", H_CMPEQDP }, { "cmpgtdp", H_CMPGTDP }, { "cmpltdp", H_CMPLTDP },
    { "intsp", H_INTSP }, { "intspu", H_INTSPU }, { "intdp", H_INTDP }, { "intdpu", H_INTDPU },
    { "spint", H_SPINT }, { "dpint", H_DPINT }, { "sptrunc", H_SPTRUNC }, { "dptrunc", H_DPTRUNC },
    { "spdp", H_SPDP }, { "dpsp", H_DPSP }, { "rcpsp", H_RCPSP }, { "rcpdp", H_RCPDP },
    { "rsqrsp", H_RSQRSP }, { "rsqrdp", H_RSQRDP },
    { "ldb", H_LOAD }, { "ldbu", H_LOAD }, { "ldh", H_LOAD }, { "ldhu", H_LOAD },
    { "ldw", H_LOAD }, { "ldnw", H_LOAD }, { "lddw", H_LOAD }, { "ldndw", H_LOAD },
    { "ll", H_LOAD }, { "cmtl", H_LOAD },
    { "stb", H_STORE }, { "sth", H_STORE }, { "stw", H_STORE }, { "stnw", H_STORE },
    { "stdw", H_STORE }, { "stndw", H_STORE }, { "sl", H_STORE },
    { "b", H_B }, { "bnop", H_BNOP }, { "callp", H_CALLP }, { "bdec", H_BDEC },
    { "bpos", H_BPOS },
    { "sploop", H_SPLOOP }, { "sploopd", H_SPLOOPD }, { "sploopw", H_SPLOOPW },
    { "spkernel", H_SPKERNEL }, { "spkernelr", H_SPKERNELR },
    { "spmask", H_SPMASK }, { "spmaskr", H_SPMASKR },
    { "faddsp", H_ADDSP }, { "fsubsp", H_SUBSP }, { "fadddp", H_ADDDP }, { "fsubdp", H_SUBDP },
    { "fmpydp", H_MPYDP }, { "dmvd", H_DMV },
    { "dadd", H_DADD }, { "dsub", H_DSUB }, { "dsadd", H_DSADD }, { "dssub", H_DSSUB },
    { "daddsp", H_DADDSP }, { "dsubsp", H_DSUBSP }, { "dmpysp", H_DMPYSP }, { "qmpysp", H_QMPYSP }, { "cmpysp", H_CMPYSP },
    { "mpyu2", H_MPYU2 }, { "land", H_LAND }, { "landn", H_LANDN }, { "lor", H_LOR },
    { "dinthsp", H_DINTHSP }, { "dinthspu", H_DINTHSPU }, { "dspint", H_DSPINT },
    { "dsadd2", H_DSADD2 }, { "davgnr2", H_DAVGNR2 }, { "unpkh2", H_UNPKH2 },
    { "dshl2", H_DSHL2 }, { "dcmpgt2", H_DCMPGT2 }, { "dmpy2", H_DMPY2 },
    { "qsmpy32r1", H_QSMPY32R1 },
    { "shl2", H_SHL2 }, { "dpackl4", H_DPACKL4 }, { "dccmpyr1", H_DCCMPYR1 },
    { "dintsp", H_DINTSP }, { "dintspu", H_DINTSPU },
};

/* ------------------------------------------------------------------------ */


static void kstats_dump_fwd(void);

/* C66X_HCOUNT builds only: where execution goes, printed by c66x_free. */
#ifdef C66X_HCOUNT
static uint64_t hc_handler[H_COUNT], hc_fop[32], hc_path[8], hc_gather[8];
#define HC(arr, i) (hc_##arr[i]++)
/* Cycles per (kind, pc): 0 fast packet, 1 fast loop kernel (SPLOOP address),
 * 2 general cycle inside a loop, 3 general cycle outside one. */
#define HC_PCS 65536
typedef struct hc_pc_ent { uint32_t pc; uint8_t kind; uint8_t ii, dynlen; uint64_t n; } hc_pc_ent;
static hc_pc_ent hc_pcs[HC_PCS];
static void hc_pc(uint32_t pc, uint8_t kind, int ii, int dynlen)
{
    uint32_t key = pc ^ ((uint32_t)kind << 30);
    unsigned s = (key * 2654435761u) >> 16;
    for (unsigned i = 0; i < HC_PCS; i++, s = (s + 1) & (HC_PCS - 1)) {
        hc_pc_ent *e = &hc_pcs[s];
        if (e->n && (e->pc != pc || e->kind != kind))
            continue;
        e->pc = pc; e->kind = kind; e->ii = ii; e->dynlen = dynlen; e->n++;
        return;
    }
}
#define HCPC(pc, kind, ii, dl) hc_pc(pc, kind, ii, dl)
/* Finished loops per SPLOOP address: invocations, iterations, cycles. */
typedef struct hc_loop_ent { uint32_t addr; uint64_t n, iters, cycles, hist[8]; } hc_loop_ent;
static hc_loop_ent hc_loops[4096];
static void hc_loop(uint32_t addr, uint64_t iters, uint64_t cycles)
{
    unsigned s = (addr * 2654435761u) >> 20;
    for (unsigned i = 0; i < 4096; i++, s = (s + 1) & 4095) {
        hc_loop_ent *e = &hc_loops[s];
        if (e->n && e->addr != addr)
            continue;
        e->addr = addr; e->n++; e->iters += iters; e->cycles += cycles;
        e->hist[iters < 2 ? 0 : iters < 4 ? 1 : iters < 8 ? 2 : iters < 16 ? 3 : iters < 32 ? 4 : iters < 64 ? 5 : iters < 256 ? 6 : 7]++;
        return;
    }
}
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

static void rec_head(c66x_core *c, uint8_t type)
{
    fputc(type, c->rec);
    fwrite(&c->cycle, sizeof c->cycle, 1, c->rec);
}

static void rec_rw(c66x_core *c, uint8_t type, uint32_t a, unsigned n, uint32_t v)
{
    uint8_t n8 = (uint8_t)n;
    rec_head(c, type);
    fwrite(&a, 4, 1, c->rec);
    fwrite(&n8, 1, 1, c->rec);
    fwrite(&v, 4, 1, c->rec);
}

/* ------------------------------------------------------------------------ */
/* memory                                                                    */

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

static void invalidate_code(c66x_core *c, uint32_t a, uint32_t n)
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

static inline void commit_writes(c66x_core *c);

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

static void idle_rs_clear(c66x_core *c)
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

static inline uint32_t mem_read(c66x_core *c, uint32_t a, unsigned n)
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
static void mem_write(c66x_core *c, uint32_t a, uint32_t v, unsigned n, int in_isr, uint32_t pc)
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
static void flush_stores(c66x_core *c);

static void mem_write(c66x_core *c, uint32_t a, uint32_t v, unsigned n, int in_isr, uint32_t pc);

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

static void flush_stores(c66x_core *c)
{
    unsigned n = c->npst;
    c->npst = 0;
    for (unsigned i = 0; i < n; i++)
        mem_write(c, c->pst[i].a, c->pst[i].v, c->pst[i].n, c->pst[i].in_isr, c->pst[i].pc);
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

/* The same names for c66x_jit_describe (tools/c14_jitgen.py matches on them). */
static const char *const fop_names[] = {
    "none", "nop", "mvk", "mvkh", "mv", "addk", "addkpc",
    "add", "sub", "and", "or", "xor", "cmpeq", "cmpgt", "cmplt", "cmpgtu", "cmpltu",
    "shl", "shr", "shru", "ext", "extu",
    "load", "store", "branch", "callp", "mvc",
    "addsp", "subsp", "mpysp",
};

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

static c66x_insn *fetch_insn(c66x_core *c, uint32_t a)
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

/* ------------------------------------------------------------------------ */
/* API                                                                       */

static void jit_init(c66x_core *c);
static void jit_free(c66x_core *c);

c66x_core *c66x_new(const c66x_bus *bus)
{
    c66x_core *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    if (bus)
        c->bus = *bus;
    unsigned n = c66x_opcode_count();
    for (unsigned i = 0; i < 2256; i++) {
        if (i >= n && i < 2000)
            continue;
        const char *name = c66x_opcode_name(i);
        c->htab[i] = H_UNIMP;
        for (unsigned k = 0; k < sizeof hmap / sizeof hmap[0]; k++)
            if (!strcmp(hmap[k].name, name)) {
                c->htab[i] = hmap[k].h;
                break;
            }
    }
    c66x_reset(c, 0);
    jit_init(c);
    return c;
}

void c66x_free(c66x_core *c)
{
    if (!c)
        return;
    c66x_record_close(c);
    jit_free(c);
    kstats_dump_fwd();
#ifdef C66X_HCOUNT
    fprintf(stderr, "paths: fast-packet %llu nop-run %llu spl-fast %llu general %llu | general with "
            "spl active %llu, entry/idle/drain %llu, ifr set but nothing deliverable %llu\n",
            (unsigned long long)hc_path[0], (unsigned long long)hc_path[1], (unsigned long long)hc_path[2],
            (unsigned long long)hc_path[3], (unsigned long long)hc_path[5], (unsigned long long)hc_path[6],
            (unsigned long long)hc_path[7]);
    for (unsigned i = 0; i < 32; i++)
        if (hc_fop[i])
            fprintf(stderr, "fop %2u %llu\n", i, (unsigned long long)hc_fop[i]);
    for (unsigned i = 0; i < H_COUNT; i++)
        if (hc_handler[i])
            fprintf(stderr, "handler %3u %llu\n", i, (unsigned long long)hc_handler[i]);
    fprintf(stderr, "gather: pkc hit %llu, block fallback %llu, uncached cross-block %llu, uncached long %llu\n",
            (unsigned long long)hc_gather[0], (unsigned long long)hc_gather[1],
            (unsigned long long)hc_gather[2], (unsigned long long)hc_gather[3]);
    for (unsigned i = 0; i < 4096; i++)
        if (hc_loops[i].n)
            fprintf(stderr, "loop 0x%08x runs %llu iters %llu cycles %llu hist <2:%llu <4:%llu <8:%llu <16:%llu "
                    "<32:%llu <64:%llu <256:%llu more:%llu\n", hc_loops[i].addr, (unsigned long long)hc_loops[i].n,
                    (unsigned long long)hc_loops[i].iters, (unsigned long long)hc_loops[i].cycles,
                    (unsigned long long)hc_loops[i].hist[0], (unsigned long long)hc_loops[i].hist[1],
                    (unsigned long long)hc_loops[i].hist[2], (unsigned long long)hc_loops[i].hist[3],
                    (unsigned long long)hc_loops[i].hist[4], (unsigned long long)hc_loops[i].hist[5],
                    (unsigned long long)hc_loops[i].hist[6], (unsigned long long)hc_loops[i].hist[7]);
    for (unsigned i = 0; i < HC_PCS; i++)
        if (hc_pcs[i].n)
            fprintf(stderr, "pc %u 0x%08x ii %u dynlen %u %llu\n", hc_pcs[i].kind, hc_pcs[i].pc,
                    hc_pcs[i].ii, hc_pcs[i].dynlen, (unsigned long long)hc_pcs[i].n);
#endif
    for (unsigned i = 0; i < c->nram; i++) {
        ramreg *r = &c->ram[i];
        if (r->fpp) {
            for (uint32_t p = 0; p <= r->size >> FP_PAGE_SHIFT; p++) {
                if (!r->fpp[p])
                    continue;
                for (unsigned f = 0; f < 1u << (FP_PAGE_SHIFT - 5); f++)
                    free(r->fpp[p][f]);
                free(r->fpp[p]);
            }
            free(r->fpp);
        }
        int shared = 0;
        for (unsigned j = 0; j < i; j++)
            shared |= c->ram[j].codepage == r->codepage;
        if (!shared)
            free(r->codepage);
    }
    free(c);
}

/* Host buffers are identified by the index of their first mapping, which is
 * stable within a core: L2 and its alias share one. */
static uint32_t rec_host_id(const c66x_core *c, const ramreg *r)
{
    for (unsigned i = 0; i < c->nram; i++)
        if (c->ram[i].host == r->host && c->ram[i].size == r->size)
            return i;
    return 0;
}

uint64_t c66x_state_hash(const c66x_core *c)
{
    uint64_t h = 1469598103934665603ull;
    const uint8_t *p = (const uint8_t *)c->reg;
    for (size_t i = 0; i < sizeof c->reg; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    p = (const uint8_t *)c->cr;
    for (size_t i = 0; i < sizeof c->cr; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    h = (h ^ c->pc) * 1099511628211ull;
    h = (h ^ c->ifr) * 1099511628211ull;
    return h;
}

int c66x_record_open(c66x_core *c, const char *path)
{
    if (!path || !*path)
        return 0;
    c->rec = fopen(path, "ab");
    if (!c->rec)
        return -1;
    setvbuf(c->rec, NULL, _IOFBF, 1 << 20);
    rec_head(c, REC_NEW);
    c->rec_hash_at = 0;
    return 0;
}

void c66x_record_close(c66x_core *c)
{
    if (c->rec)
        fclose(c->rec);
    c->rec = NULL;
}

void c66x_map_ram(c66x_core *c, uint32_t base, uint32_t size, uint8_t *host)
{
    if (c->nram >= 8)
        return;
    if (c->rec) {
        uint32_t id = c->nram;
        for (unsigned i = 0; i < c->nram; i++)
            if (c->ram[i].host == host && c->ram[i].size == size)
                id = i;
        rec_head(c, REC_MAP);
        fwrite(&base, 4, 1, c->rec);
        fwrite(&size, 4, 1, c->rec);
        fwrite(&id, 4, 1, c->rec);
    }
    uint8_t *cp = NULL;
    for (unsigned i = 0; i < c->nram; i++)
        if (c->ram[i].host == host && c->ram[i].size == size)
            cp = c->ram[i].codepage;
    if (!cp)
        cp = calloc((size >> FP_PAGE_SHIFT) + 1, 1);
    c->ram[c->nram++] = (ramreg){ base, size, host, NULL, cp };
}

void c66x_reset(c66x_core *c, uint32_t pc)
{
    if (c->rec) {
        /* The program is already in RAM when the chip is reset: keep it. */
        static const uint8_t zero[4096];
        for (unsigned i = 0; i < c->nram; i++) {
            ramreg *r = &c->ram[i];
            uint32_t id = rec_host_id(c, r);
            if (id != i)
                continue;
            for (uint32_t off = 0; off + 4096 <= r->size; off += 4096) {
                if (!memcmp(r->host + off, zero, 4096))
                    continue;
                rec_head(c, REC_PAGE);
                fwrite(&id, 4, 1, c->rec);
                fwrite(&off, 4, 1, c->rec);
                fwrite(r->host + off, 1, 4096, c->rec);
            }
        }
        rec_head(c, REC_RESET);
        fwrite(&pc, 4, 1, c->rec);
    }
    memset(c->reg, 0, sizeof c->reg);
    memset(c->cr, 0, sizeof c->cr);
    memset(c->wqn, 0, sizeof c->wqn);
    c->imm_n = 0;
    c->wmask = 0;
    memset(&c->spl, 0, sizeof c->spl);
    c->cr[CR_CSR] = 0x14000100;         /* C66x CPU ID 0x14, EN = little endian */
    c->cr[CR_TSR] = 0;                  /* supervisor, GIE off */
    c->cr[CR_IER] = 1;
    c->ifr = 0;
    c->efr = 0;
    c->pc = pc;
    c->mcnop = 0;
    c->pm_resume_at = 0;
    c->spl_irq_pending = 0;
    c->int_entry = 0;
    c->idle = 0;
    c->nbr = 0;
    c->branch_block = 0;
    c->npst = 0;
    c->isr_depth = 0;
    c->idle_fp = 0;
    c->idle_ret_pending = 0;
}

void c66x_set_irq(c66x_core *c, int line, int level)
{
    uint32_t bit = 1u << line;
    if (c->rec) {
        uint8_t l = (uint8_t)line, v = level != 0;
        rec_head(c, REC_IRQ);
        fwrite(&l, 1, 1, c->rec);
        fwrite(&v, 1, 1, c->rec);
    }
    if (level && !(c->irq_level & bit)) {
        c->ifr |= bit;
        c->irq_level |= bit;
        if (line >= 0 && line < 16)
            c->st.irq_edges[line]++;
    } else if (!level) {
        c->irq_level &= ~bit;
    }
}

void c66x_hook_pc(c66x_core *c, uint32_t pc, c66x_hook_fn fn, void *opaque)
{
    for (unsigned i = 0; i < NHOOK; i++) {
        unsigned k = (pc / 2 + i) & (NHOOK - 1);
        if (!c->hooks[k].fn || c->hooks[k].pc == pc) {
            if (!c->hooks[k].fn)
                c->nhooks++;
            c->hooks[k] = (hook){ pc, fn, opaque };
            return;
        }
    }
}

static hook *find_hook(c66x_core *c, uint32_t pc)
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

uint32_t c66x_get_pc(const c66x_core *c) { return c->pc; }
void c66x_set_pc(c66x_core *c, uint32_t pc) { c->pc = pc; c->nbr = 0; c->mcnop = 0; }
uint32_t c66x_get_reg(const c66x_core *c, unsigned r) { return r < C66X_NREGS ? c->reg[r] : 0; }
void c66x_set_reg(c66x_core *c, unsigned r, uint32_t v) { if (r < C66X_NREGS) c->reg[r] = v; }
uint32_t c66x_trap_pc(const c66x_core *c) { return c->trap_pc; }
void c66x_get_stats(const c66x_core *c, c66x_stats *out) { *out = c->st; }
void c66x_set_trace(c66x_core *c, c66x_trace_fn fn, void *opaque) { c->trace = fn; c->trace_opaque = opaque; }
uint32_t c66x_get_exec_pc(const c66x_core *c) { return c->exec_pc; }
uint64_t c66x_get_cycle(const c66x_core *c) { return c->cycle; }

void c66x_set_idle_loop(c66x_core *c, uint32_t head_pc, uint32_t stack_lo,
                        uint32_t stack_hi, int allow_bus_reads)
{
    c->idle_head = head_pc;
    c->idle_slo = stack_lo;
    c->idle_shi = stack_hi;
    c->idle_allow_reads = allow_bus_reads;
    c->idle_armed = 0;
    c->idle_fx = 0;
    c->idle_fp = 0;
    c->idle_isr_hit = 0;
    c->idle_ret_pending = 0;
    idle_rs_clear(c);
}

void c66x_idle_idempotent_write(c66x_core *c, uint32_t addr)
{
    if (c->idle_nidem < IDLE_IDEM)
        c->idle_idem[c->idle_nidem++] = addr;
}

void c66x_skip_cycles(c66x_core *c, uint64_t n)
{
    if (c->rec) {
        rec_head(c, REC_SKIP);
        fwrite(&n, 8, 1, c->rec);
    }
    /* Writes in flight land first, in order, as if the cycles had run. */
    for (unsigned d = 0; d < WQ_SLOTS && n; d++) {
        commit_writes(c);
        c->cycle++;
        n--;
    }
    c->cycle += n;
    c->st.cycles = c->cycle;
}

void c66x_get_idle_info(const c66x_core *c, c66x_idle_info *out) { *out = c->idle_info; }

/* At the loop head: close the iteration that just ended, open the next one.
 * Returns 1 when the ended iteration was a fixed point. */
static int idle_check(c66x_core *c)
{
    int idle = 0;
    if (c->idle_armed) {
        c66x_idle_info *ii = &c->idle_info;
        uint8_t why = c->idle_fx;
        if (!why && (c->idle_isr_hit || c->isr_depth))
            why = 4;
        if (c->idle_isr_hit)
            ii->isr_store_hits++;
        if (!why && memcmp(c->reg, c->idle_reg, sizeof c->reg))
            why = 5;
        if (!why)
            for (unsigned s = 0; s < WQ_SLOTS; s++)
                if (c->wqn[s])
                    why = 7;
        ii->iterations++;
        ii->last_iter_cycles = (uint32_t)(c->cycle - c->idle_head_cycle);
        ii->last_reason = why;
        ii->last_addr = c->idle_fx_addr;
        ii->last_pc = c->idle_fx_pc;
        if (!why) {
            ii->idle_hits++;
            idle = 1;
        }
    }
    /* The words a fixed-point pass reads stay in the set; a pass that did
     * something starts it over. */
    if (!idle)
        idle_rs_clear(c);
    c->idle_fp = idle;
    c->idle_armed = 1;
    c->idle_fx = 0;
    c->idle_isr_hit = 0;
    c->idle_head_cycle = c->cycle;
    c->idle_head_irqs = c->st.interrupts;
    memcpy(c->idle_reg, c->reg, sizeof c->reg);
    return idle;
}

void c66x_watch_writes(c66x_core *c, uint32_t lo, uint32_t hi, c66x_watch_fn fn, void *opaque)
{
    c->watch_lo = lo;
    c->watch_hi = hi;
    c->watch_fn = fn;
    c->watch_opaque = opaque;
}

static uint32_t ctrl_read(c66x_core *c, unsigned crlo, uint32_t pce1)
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

static void ctrl_write_now(c66x_core *c, unsigned crlo, uint32_t v)
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

uint32_t c66x_get_creg(const c66x_core *c, unsigned crlo)
{
    return ctrl_read((c66x_core *)c, crlo, c->pc & ~31u);
}

void c66x_set_creg(c66x_core *c, unsigned crlo, uint32_t v) { ctrl_write_now(c, crlo, v); }

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
/* arithmetic helpers                                                        */

static inline int64_t sext40(uint64_t v) { return (int64_t)(v << 24) >> 24; }
static inline int32_t s16(uint32_t v) { return (int16_t)v; }
static inline int32_t lsb16s(uint32_t v) { return (int16_t)v; }
static inline int32_t msb16s(uint32_t v) { return (int16_t)(v >> 16); }
static inline uint32_t lsb16u(uint32_t v) { return v & 0xffff; }
static inline uint32_t msb16u(uint32_t v) { return v >> 16; }

static void set_sat(c66x_core *c, const c66x_insn *in)
{
    c->cr[CR_CSR] |= CSR_SAT;
    static const int8_t ssr_bit[8] = { 0, 1, 2, 3, -1, -1, 4, 5 };
    if (in->unit >= 0 && ssr_bit[in->unit] >= 0)
        c->cr[CR_SSR] |= 1u << ssr_bit[in->unit];
}

static int64_t sat_to(c66x_core *c, const c66x_insn *in, int64_t v, int bits)
{
    int64_t hi = ((int64_t)1 << (bits - 1)) - 1, lo = -hi - 1;
    if (v > hi) { set_sat(c, in); return hi; }
    if (v < lo) { set_sat(c, in); return lo; }
    return v;
}

static inline int32_t sat16(int32_t v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : v; }

static unsigned rmode(c66x_core *c, const c66x_insn *in)
{
    uint32_t r = (in->unit >= 6) ? c->cr[CR_FMCR] : c->cr[CR_FADCR];
    return in->side == 2 ? (r >> 25) & 3 : (r >> 9) & 3;
}

static const int fe_mode[4] = { FE_TONEAREST, FE_TOWARDZERO, FE_UPWARD, FE_DOWNWARD };

static inline float  u2f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }
static inline double u2d(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline uint64_t d2u(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }

static int32_t f_to_i32(double x, int mode_trunc, int rm)
{
    if (x != x || x >= 2147483648.0 || x < -2147483648.0)
        return (int32_t)0x80000000;
    if (mode_trunc)
        return (int32_t)x;
    double r;
    switch (rm) {
    case 1: r = trunc(x); break;
    case 2: r = ceil(x); break;
    case 3: r = floor(x); break;
    default: r = nearbyint(x); break;
    }
    if (r >= 2147483648.0 || r < -2147483648.0)
        return (int32_t)0x80000000;
    return (int32_t)r;
}

static uint32_t bitrev32(uint32_t v)
{
    v = ((v >> 1) & 0x55555555) | ((v & 0x55555555) << 1);
    v = ((v >> 2) & 0x33333333) | ((v & 0x33333333) << 2);
    v = ((v >> 4) & 0x0F0F0F0F) | ((v & 0x0F0F0F0F) << 4);
    v = ((v >> 8) & 0x00FF00FF) | ((v & 0x00FF00FF) << 8);
    return (v >> 16) | (v << 16);
}

static uint32_t gf_mul(uint32_t a, uint32_t b, uint32_t poly, unsigned size)
{
    uint32_t r = 0, top = 1u << (size - 1), mask = (size == 32) ? 0xffffffff : ((1u << size) - 1);
    while (b) {
        if (b & 1)
            r ^= a;
        b >>= 1;
        int carry = (a & top) != 0;
        a = (a << 1) & mask;
        if (carry)
            a ^= poly & mask;
    }
    return r;
}

/* ------------------------------------------------------------------------ */
/* execution                                                                 */

typedef struct xctx {
    uint32_t pce1;          /* fetch packet of the instruction */
    uint32_t next_pc;       /* address after the execute packet */
    int      extra_nops;    /* NOP cycles this packet inserts after itself */
    int      branched;      /* packet contains a branch instruction */
    int      is_buffer;     /* instruction came from the loop buffer */
    int      stop;
} xctx;

static uint64_t opval(c66x_core *c, const c66x_operand *o, uint32_t pce1)
{
    switch (o->kind) {
    case C66X_OPK_REG:   return c->reg[o->reg];
    case C66X_OPK_PAIR:
        if (o->size == 5)
            return ((uint64_t)(c->reg[o->reg_hi] & 0xff) << 32) | c->reg[o->reg];
        return ((uint64_t)c->reg[o->reg_hi] << 32) | c->reg[o->reg];
    case C66X_OPK_CONST: return (uint64_t)(int64_t)o->val;
    case C66X_OPK_ADDR:  return (uint32_t)o->val;
    case C66X_OPK_CTRL:  return ctrl_read(c, c66x_ctrl_crlo(o->val), pce1);
    case C66X_OPK_ILC:   return c->cr[CR_ILC];
    case C66X_OPK_IRP:   return c->cr[CR_IRP];
    case C66X_OPK_NRP:   return c->cr[CR_NRP];
    default: return 0;
    }
}

static inline int64_t sv(const c66x_operand *o, uint64_t v)
{
    if (o->kind == C66X_OPK_CONST)
        return (int64_t)v;
    if (o->size == 5)
        return sext40(v);
    if (o->size == 8)
        return (int64_t)v;
    return (int32_t)v;
}

static inline uint64_t uv(const c66x_operand *o, uint64_t v)
{
    if (o->size == 5)
        return v & 0xffffffffffULL;
    if (o->size == 8)
        return v;
    return (uint32_t)v;
}

static inline void write_op(c66x_core *c, const c66x_operand *o, uint64_t v, uint8_t load)
{
    switch (o->kind) {
    case C66X_OPK_REG:
        sched(c, o->low_first - 1, WK_REG, o->reg, (uint32_t)v, load);
        break;
    case C66X_OPK_PAIR:
        sched(c, o->low_first - 1, WK_REG, o->reg, (uint32_t)v, load);
        if (o->size == 5)
            sched(c, o->high_first - 1, WK_REG, o->reg_hi, (uint32_t)(v >> 32) & 0xff, load);
        else
            sched(c, o->high_first - 1, WK_REG, o->reg_hi, (uint32_t)(v >> 32), load);
        break;
    case C66X_OPK_CTRL: {
        unsigned crlo = c66x_ctrl_crlo(o->val);
        /* ILC/RILC take 4 cycles to reach the loop buffer (SPRU732 7.4.3). */
        unsigned d = (crlo == CR_ILC || crlo == CR_RILC) ? 3 : (crlo == CR_IFR || crlo == CR_ICR) ? 1 : 0;
        sched(c, d, WK_CTRL, crlo, (uint32_t)v, 0);
        break;
    }
    case C66X_OPK_ILC:
        sched(c, 3, WK_CTRL, CR_ILC, (uint32_t)v, 0);
        break;
    default:
        break;
    }
}

static uint32_t mem_ea(c66x_core *c, const c66x_operand *o)
{
    uint32_t base = c->reg[o->reg];
    uint32_t off = (o->mem_mode & 4) ? c->reg[o->mem_offreg] * o->mem_scale
                                     : (uint32_t)o->val * o->mem_scale;
    switch (o->mem_mode & ~4u) {
    case 0:  return base - off;
    case 1:  return base + off;
    case 8:  sched(c, 0, WK_REG, o->reg, base - off, 0); return base - off;
    case 9:  sched(c, 0, WK_REG, o->reg, base + off, 0); return base + off;
    case 10: sched(c, 0, WK_REG, o->reg, base - off, 0); return base;
    case 11: sched(c, 0, WK_REG, o->reg, base + off, 0); return base;
    default: return base;
    }
}

/* Branches taken in the delay slots of other branches all stay in flight. TI's
 * divide (stage 1, 0x00800280) seeds five in consecutive packets so its
 * one-packet loop at 0x0080030C has a branch landing every cycle. */
static inline int take_branch_rc(c66x_core *c, uint32_t target)
{
    if (c->nbr < NBR) {
        c->br[c->nbr++] = (pend_branch){ 6, target };
        return 0;
    }
    c->trap_pc = c->exec_pc;
    return C66X_STOP_FAULT;
}

static void take_branch(c66x_core *c, uint32_t target, xctx *x)
{
    int r = take_branch_rc(c, target);
    if (r)
        x->stop = r;
}

static void spl_start(c66x_core *c, const c66x_insn *in, xctx *x);
static void idle_isr_return(c66x_core *c, uint32_t target);

static inline void do_load(c66x_core *c, const c66x_insn *in)
{
    uint32_t ea = mem_ea(c, &in->op[0]);
    uint64_t val;
    switch (in->sub) {
    case LD_B:  val = (uint32_t)(int8_t)mem_read(c, ea, 1); break;
    case LD_BU: val = mem_read(c, ea, 1); break;
    case LD_H:  val = (uint32_t)(int16_t)mem_read(c, ea, 2); break;
    case LD_HU: val = mem_read(c, ea, 2); break;
    case LD_DW: val = mem_read(c, ea, 4) | ((uint64_t)mem_read(c, ea + 4, 4) << 32); break;
    default:    val = mem_read(c, ea, 4); break;
    }
    write_op(c, &in->op[1], val, 1);
}

static inline void do_store(c66x_core *c, const c66x_insn *in, uint32_t pce1)
{
    uint64_t val = opval(c, &in->op[0], pce1);
    uint32_t ea = mem_ea(c, &in->op[1]);
    if (in->sub == LD_B) store_defer(c, ea, (uint32_t)val, 1);
    else if (in->sub == LD_H) store_defer(c, ea, (uint32_t)val, 2);
    else if (in->sub == LD_DW) {
        store_defer(c, ea, (uint32_t)val, 4);
        store_defer(c, ea + 4, (uint32_t)(val >> 32), 4);
    } else store_defer(c, ea, (uint32_t)val, 4);
}

/* exec_insn for the F_* forms, with the operand decoding resolved at classify. */
/* The effect of one fast-form instruction, without the packet bookkeeping
 * (NOP cycles, branch flag) that a cached packet carries precomputed. Returns a
 * stop code, 0 normally. */
/* F_STORE's inline RAM store for one 32-bit word, else store_defer. */
static inline void store_word_fast(c66x_core *c, uint32_t ea, uint32_t v)
{
    ramreg *rr = &c->ram[c->last_ram];
    if (c->store_now && !c->watch_fn && (!c->idle_armed || c->idle_fx)
        && ea - rr->base <= rr->size - 4 && rr->size >= 4) {
        uint8_t *m = rr->host + (ea - rr->base);
        m[0] = v; m[1] = v >> 8; m[2] = v >> 16; m[3] = v >> 24;
        if (rr->codepage[(ea - rr->base) >> FP_PAGE_SHIFT])
            invalidate_code(c, ea, 4);
        return;
    }
    store_defer(c, ea, v, 4);
}

static inline int fop_run(c66x_core *c, const c66x_insn *in, uint32_t next_pc)
{
    const c66x_operand *op = in->op;
    HC(fop, in->fop);
    if (in->fop == F_NOP)
        return 0;
    if (in->cond_reg >= 0 && ((c->reg[in->cond_reg] == 0) != in->cond_z))
        return 0;
    switch (in->fop) {
    case F_LOAD:
        if (in->mfast) {
            uint32_t base = c->reg[op[0].reg], ea = base + (uint32_t)in->ea_delta, v;
            unsigned n = in->mfast & 7;
            if (in->mfast & 0x30) {
                /* mem_ea's order: the base update is scheduled before the read */
                if (c->imm_n < IMM_DEPTH)
                    c->imm[c->imm_n++] = (wq_ent){ WK_REG, op[0].reg, 0, ea };
                if (in->mfast & 0x20)
                    ea = base;
            }
            ramreg *rr = &c->ram[c->last_ram];
            /* mem_read's RAM path, minus the idle tracker when it would act */
            if ((!c->idle_armed || c->isr_depth || c->idle_fx)
                && ea - rr->base <= rr->size - n && rr->size >= n) {
                const uint8_t *m = rr->host + (ea - rr->base);
                v = n == 4 ? m[0] | (m[1] << 8) | (m[2] << 16) | ((uint32_t)m[3] << 24)
                  : n == 2 ? (uint32_t)(m[0] | (m[1] << 8)) : m[0];
            } else {
                v = mem_read(c, ea, n);
            }
            if (in->mfast & 8)
                v = n == 1 ? (uint32_t)(int8_t)v : (uint32_t)(int16_t)v;
            sched(c, op[1].low_first - 1, WK_REG, op[1].reg, v, 1);
            return 0;
        }
        do_load(c, in);
        return 0;
    case F_STORE:
        if (in->mfast) {
            uint32_t base = c->reg[op[1].reg], ea = base + (uint32_t)in->ea_delta, v = c->reg[op[0].reg];
            unsigned n = in->mfast & 7;
            if (in->mfast & 0x30) {
                if (c->imm_n < IMM_DEPTH)
                    c->imm[c->imm_n++] = (wq_ent){ WK_REG, op[1].reg, 0, ea };
                if (in->mfast & 0x20)
                    ea = base;
            }
            if (in->mfast & 0x40) {
                /* stdw: low word at ea, high word at ea + 4 */
                uint32_t hi = c->reg[op[0].reg_hi];
                store_word_fast(c, ea, v);
                store_word_fast(c, ea + 4, hi);
                return 0;
            }
            ramreg *rr = &c->ram[c->last_ram];
            /* mem_write's RAM path when nothing else would look at the store */
            if (c->store_now && !c->watch_fn && (!c->idle_armed || c->idle_fx)
                && ea - rr->base <= rr->size - n && rr->size >= n) {
                uint8_t *m = rr->host + (ea - rr->base);
                m[0] = v;
                if (n > 1) m[1] = v >> 8;
                if (n > 2) { m[2] = v >> 16; m[3] = v >> 24; }
                if (rr->codepage[(ea - rr->base) >> FP_PAGE_SHIFT])
                    invalidate_code(c, ea, n);
                return 0;
            }
            store_defer(c, ea, v, n);
            return 0;
        }
        do_store(c, in, in->addr & ~31u);
        return 0;
    case F_BRANCH:
        return take_branch_rc(c, (uint32_t)opval(c, &op[0], in->addr & ~31u));
    case F_CALLP: {
        int r = take_branch_rc(c, (uint32_t)op[0].val);
        write_op(c, &op[1], next_pc, 0);
        return r;
    }
    case F_MVC:
        write_op(c, &op[1], opval(c, &op[0], in->addr & ~31u), 0);
        return 0;
    case F_ADDSP: case F_SUBSP: case F_MPYSP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        float p = u2f(c->reg[op[0].reg]), q = u2f(c->reg[op[1].reg]);
        float res = in->fop == F_ADDSP ? p + q : in->fop == F_SUBSP ? p - q : p * q;
        if (rm) fesetround(FE_TONEAREST);
        sched(c, op[2].low_first - 1, WK_REG, op[2].reg, f2u(res), 0);
        return 0;
    }
    default:
        break;
    }
    uint32_t a = op[0].kind == C66X_OPK_REG ? c->reg[op[0].reg] : (uint32_t)op[0].val;
    uint32_t b = op[1].kind == C66X_OPK_REG ? c->reg[op[1].reg] : (uint32_t)op[1].val;
    int32_t sa = op[0].kind == C66X_OPK_REG ? (int32_t)a : op[0].val;
    int32_t sb = op[1].kind == C66X_OPK_REG ? (int32_t)b : op[1].val;
    uint32_t r;
    unsigned dst = 2;
    switch (in->fop) {
    case F_MVK:    r = (uint32_t)op[0].val; dst = 1; break;
    case F_MVKH:   r = (b & 0xffff) | ((uint32_t)op[0].val & 0xffff0000); dst = 1; break;
    case F_ADDK:   r = b + (uint32_t)op[0].val; dst = 1; break;
    case F_MV:     r = a; dst = 1; break;
    case F_ADDKPC: r = (uint32_t)op[0].val; dst = 1; break;
    case F_ADD:    r = a + b; break;
    case F_SUB:    r = a - b; break;
    case F_AND:    r = a & b; break;
    case F_OR:     r = a | b; break;
    case F_XOR:    r = a ^ b; break;
    case F_CMPEQ:  r = sa == sb; break;
    case F_CMPGT:  r = sa > sb; break;
    case F_CMPLT:  r = sa < sb; break;
    case F_CMPGTU: r = a > b; break;
    case F_CMPLTU: r = a < b; break;
    case F_SHL:    r = (b & 0x3f) > 31 ? 0 : a << (b & 0x3f); break;
    case F_SHR:    r = (uint32_t)(sa >> ((b & 0x3f) > 31 ? 31 : (b & 0x3f))); break;
    case F_SHRU:   r = (b & 0x3f) > 31 ? 0 : a >> (b & 0x3f); break;
    case F_EXT:    r = (uint32_t)((int32_t)(a << (op[1].val & 31)) >> (op[2].val & 31)); dst = 3; break;
    case F_EXTU:   r = (a << (op[1].val & 31)) >> (op[2].val & 31); dst = 3; break;
    default:       return 0;
    }
    if (c->imm_n < IMM_DEPTH)
        c->imm[c->imm_n++] = (wq_ent){ WK_REG, op[dst].reg, 0, r };
    return 0;
}

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

/* One instruction. Reads happen now (E1); writes are scheduled at the cycles
 * the binutils operand table gives. */
static void exec_insn(c66x_core *c, c66x_insn *in, xctx *x)
{
    unsigned h = in->handler;
    const c66x_operand *op = in->op;
    uint64_t v[4] = { 0 };
    int64_t r;
    uint32_t a, b;

    c->st.insns++;
    HC(handler, h);

    if (in->xnops > x->extra_nops)
        x->extra_nops = in->xnops;
    x->branched |= in->isbranch;
    if (h == H_NOP)
        return;

    if (in->cond_reg >= 0 && ((c->reg[in->cond_reg] == 0) != in->cond_z)) {
        /* An SPLOOP family predicate is an exit or reload test, not an enable. */
        if (h == H_SPLOOPW || h == H_SPLOOP || h == H_SPLOOPD)
            spl_start(c, in, x);
        return;
    }

    for (unsigned m = in->rmask; m; m &= m - 1) {
        unsigned i = __builtin_ctz(m);
        v[i] = (in->regmask >> i) & 1 ? c->reg[op[i].reg]
             : (in->constmask >> i) & 1 ? (uint64_t)(int64_t)op[i].val
             : opval(c, &op[i], x->pce1);
    }

#define SV(i) sv(&op[i], v[i])
#define UV(i) uv(&op[i], v[i])
#define W(i, val) write_op(c, &op[i], (uint64_t)(val), 0)
#define LAST (in->nops - 1)

    switch (h) {
    case H_UNIMP:
        c->trap_pc = in->addr;
        x->stop = C66X_STOP_UNDEF;
        return;
    case H_IDLE:
        c->idle = 1;
        return;
    case H_DINT:
        /* DINT/RINT move GIE to/from SGIE (SPRU732 DINT description). */
        c->cr[CR_TSR] = (c->cr[CR_TSR] & ~(TSR_GIE | TSR_SGIE))
                        | ((c->cr[CR_TSR] & TSR_GIE) ? TSR_SGIE : 0);
        return;
    case H_RINT:
        c->cr[CR_TSR] = (c->cr[CR_TSR] & ~TSR_GIE)
                        | ((c->cr[CR_TSR] & TSR_SGIE) ? TSR_GIE : 0);
        return;
    case H_SWE:
        c->trap_pc = in->addr;
        x->stop = C66X_STOP_FAULT;
        return;

    case H_ABS: r = SV(0); W(1, r < 0 ? -r : r); return;
    case H_ABS2:
        a = (uint16_t)abs(lsb16s(v[0])); b = (uint16_t)abs(msb16s(v[0]));
        W(1, a | (b << 16)); return;
    case H_ADD: W(LAST, SV(0) + SV(1)); return;
    case H_ADDU: W(LAST, UV(0) + UV(1)); return;
    case H_SUB: W(LAST, SV(0) - SV(1)); return;
    case H_SUBU: W(LAST, UV(0) - UV(1)); return;
    case H_ADD2:
        W(2, ((uint32_t)(v[0] + v[1]) & 0xffff) | ((((uint32_t)v[0] >> 16) + ((uint32_t)v[1] >> 16)) << 16));
        return;
    case H_SUB2:
        W(2, ((uint32_t)(v[0] - v[1]) & 0xffff) | ((((uint32_t)v[0] >> 16) - ((uint32_t)v[1] >> 16)) << 16));
        return;
    case H_ADD4: case H_SUB4: {
        uint32_t o = 0;
        for (int k = 0; k < 32; k += 8) {
            uint32_t p = (uint32_t)v[0] >> k & 0xff, q = (uint32_t)v[1] >> k & 0xff;
            o |= ((h == H_ADD4 ? p + q : p - q) & 0xff) << k;
        }
        W(2, o);
        return;
    }
    case H_ADDA: case H_SUBA: {
        unsigned sh = in->sub;
        uint32_t base = (uint32_t)v[0];
        uint32_t off = (uint32_t)v[1];
        W(LAST, h == H_ADDA ? base + (off << sh) : base - (off << sh));
        return;
    }
    case H_ADDK: W(1, (uint32_t)c->reg[op[1].reg] + (int32_t)op[0].val); return;
    case H_ADDKPC: W(1, (uint32_t)op[0].val); return;
    case H_ADDSUB: case H_SADDSUB: {
        int64_t s1 = (int32_t)v[0], s2 = (int32_t)v[1];
        int64_t ad = s1 + s2, sb = s1 - s2;
        if (h == H_SADDSUB) { ad = sat_to(c, in, ad, 32); sb = sat_to(c, in, sb, 32); }
        W(2, ((uint64_t)(uint32_t)ad << 32) | (uint32_t)sb);
        return;
    }
    case H_ADDSUB2: case H_SADDSUB2: {
        int32_t p1 = lsb16s(v[0]), p2 = msb16s(v[0]), q1 = lsb16s(v[1]), q2 = msb16s(v[1]);
        int32_t al = p1 + q1, ah = p2 + q2, sl = p1 - q1, sh2 = p2 - q2;
        if (h == H_SADDSUB2) { al = sat16(al); ah = sat16(ah); sl = sat16(sl); sh2 = sat16(sh2); }
        uint32_t o = ((uint16_t)ah << 16) | (uint16_t)al;
        uint32_t e = ((uint16_t)sh2 << 16) | (uint16_t)sl;
        W(2, ((uint64_t)o << 32) | e);
        return;
    }
    case H_AND: case H_ANDN: case H_OR: case H_XOR: {
        uint64_t p = v[0], q = v[1];
        /* C66x pair forms apply a scst5 to both 32-bit halves. */
        if (op[0].kind == C66X_OPK_CONST && op[2].size == 8)
            p = ((uint64_t)(uint32_t)op[0].val << 32) | (uint32_t)op[0].val;
        W(2, h == H_AND ? p & q : h == H_ANDN ? p & ~q : h == H_OR ? p | q : p ^ q);
        return;
    }
    case H_NOT: W(1, ~v[0]); return;
    case H_NEG: W(1, -SV(0)); return;
    case H_MV: W(1, v[0]); return;
    case H_MVK: W(1, (int64_t)op[0].val); return;
    case H_MVKH:
        W(1, (c->reg[op[1].reg] & 0xffff) | ((uint32_t)op[0].val & 0xffff0000));
        return;
    case H_ZERO: W(0, 0); return;
    case H_DMV: W(2, ((uint64_t)(uint32_t)v[0] << 32) | (uint32_t)v[1]); return;
    /* binutils marks the compact forms' source (ORREG1B, ORREG1BNORS) as
     * written, so read those operands here rather than trusting rw. */
    case H_MVC: W(1, opval(c, &op[0], x->pce1)); return;
    case H_MVD: W(1, v[0]); return;

    case H_CMPEQ: W(2, SV(0) == SV(1)); return;
    case H_CMPGT: W(2, SV(0) > SV(1)); return;
    case H_CMPLT: W(2, SV(0) < SV(1)); return;
    case H_CMPGTU:
        W(2, (op[0].kind == C66X_OPK_CONST ? (uint64_t)(uint32_t)op[0].val : UV(0)) > UV(1));
        return;
    case H_CMPLTU:
        W(2, (op[0].kind == C66X_OPK_CONST ? (uint64_t)(uint32_t)op[0].val : UV(0)) < UV(1));
        return;
    case H_CMPEQ2: case H_CMPGT2: case H_CMPLT2: {
        int32_t l1 = lsb16s(v[0]), h1 = msb16s(v[0]), l2 = lsb16s(v[1]), h2 = msb16s(v[1]);
        unsigned d0, d1;
        if (h == H_CMPEQ2) { d0 = l1 == l2; d1 = h1 == h2; }
        else if (h == H_CMPGT2) { d0 = l1 > l2; d1 = h1 > h2; }
        else { d0 = l1 < l2; d1 = h1 < h2; }
        W(2, d0 | (d1 << 1));
        return;
    }
    case H_CMPEQ4: case H_CMPGTU4: case H_CMPLTU4: {
        uint32_t o = 0;
        for (int k = 0; k < 4; k++) {
            uint8_t p = (uint32_t)v[0] >> (8 * k), q = (uint32_t)v[1] >> (8 * k);
            unsigned t = h == H_CMPEQ4 ? (int8_t)p == (int8_t)q : h == H_CMPGTU4 ? p > q : p < q;
            o |= t << k;
        }
        W(2, o);
        return;
    }

    case H_SHL: {
        unsigned n = (uint32_t)v[1] & 0x3f;
        uint64_t s = UV(0);
        uint64_t res = n > 39 ? 0 : s << n;
        W(2, res);
        return;
    }
    case H_SHR: {
        unsigned n = (uint32_t)v[1] & 0x3f;
        int64_t s = SV(0);
        if (n > 39) n = 39;
        W(2, s >> n);
        return;
    }
    case H_SHRU: {
        unsigned n = (uint32_t)v[1] & 0x3f;
        uint64_t s = UV(0);
        W(2, n > 39 ? 0 : s >> n);
        return;
    }
    case H_SSHL: {
        unsigned n = (uint32_t)v[1] & 0x1f;
        W(2, sat_to(c, in, (int64_t)(int32_t)v[0] << n, 32));
        return;
    }
    case H_SHR2: case H_SHRU2: {
        unsigned n = (uint32_t)v[1] & 0x1f;
        uint32_t o;
        if (h == H_SHR2) {
            int32_t l = lsb16s(v[0]), m = msb16s(v[0]);
            if (n > 15) n = 15;
            o = ((uint16_t)(l >> n)) | ((uint16_t)(m >> n) << 16);
        } else {
            uint32_t l = lsb16u(v[0]), m = msb16u(v[0]);
            o = n > 15 ? 0 : ((l >> n) & 0xffff) | (((m >> n) & 0xffff) << 16);
        }
        W(2, o);
        return;
    }
    case H_SSHVL: case H_SSHVR: {
        /* src2 (op0) shifted by the signed 6-bit amount in src1 (op1). */
        int32_t s = (int32_t)v[0];
        int32_t n = (int32_t)((uint32_t)v[1] << 26) >> 26;
        if (h == H_SSHVR) n = -n;
        int64_t res;
        if (n >= 0) {
            res = n >= 32 ? (s ? (s > 0 ? INT64_MAX : INT64_MIN) : 0) : (int64_t)s << n;
            res = sat_to(c, in, res, 32);
        } else {
            res = s >> (-n > 31 ? 31 : -n);
        }
        W(2, res);
        return;
    }
    case H_SHLMB: W(2, ((uint32_t)v[1] << 8) | ((uint32_t)v[0] >> 24)); return;
    case H_SHRMB: W(2, ((uint32_t)v[1] >> 8) | (((uint32_t)v[0] & 0xff) << 24)); return;
    case H_ROTL: {
        unsigned n = (uint32_t)v[1] & 31;
        uint32_t s = (uint32_t)v[0];
        W(2, n ? (s << n) | (s >> (32 - n)) : s);
        return;
    }
    case H_CLR: case H_SET: case H_EXT: case H_EXTU: {
        unsigned csta, cstb;
        uint32_t s = (uint32_t)v[0];
        if (in->nops == 4) {
            csta = op[1].val & 31; cstb = op[2].val & 31;
        } else {
            /* register form: src1 (op1) holds csta in 9..5 and cstb in 4..0 */
            s = (uint32_t)v[0];
            csta = ((uint32_t)v[1] >> 5) & 31; cstb = (uint32_t)v[1] & 31;
        }
        uint32_t res;
        if (h == H_CLR || h == H_SET) {
            if (cstb < csta) { res = s; }
            else {
                unsigned w = cstb - csta + 1;
                uint32_t m = (w >= 32 ? 0xffffffff : ((1u << w) - 1)) << csta;
                res = h == H_CLR ? s & ~m : s | m;
            }
        } else {
            uint32_t t = s << csta;
            res = h == H_EXT ? (uint32_t)((int32_t)t >> cstb) : (t >> cstb);
        }
        W(LAST, res);
        return;
    }

    case H_SADD: {
        int bits = op[LAST].size == 5 ? 40 : 32;
        W(2, sat_to(c, in, SV(0) + SV(1), bits));
        return;
    }
    case H_SSUB: {
        int bits = op[LAST].size == 5 ? 40 : 32;
        W(2, sat_to(c, in, SV(0) - SV(1), bits));
        return;
    }
    case H_SAT: W(1, sat_to(c, in, SV(0), 32)); return;
    case H_SADD2: case H_SSUB2: {
        int32_t l = h == H_SADD2 ? lsb16s(v[0]) + lsb16s(v[1]) : lsb16s(v[0]) - lsb16s(v[1]);
        int32_t m = h == H_SADD2 ? msb16s(v[0]) + msb16s(v[1]) : msb16s(v[0]) - msb16s(v[1]);
        if (l != sat16(l) || m != sat16(m)) set_sat(c, in);
        W(2, (uint16_t)sat16(l) | ((uint32_t)(uint16_t)sat16(m) << 16));
        return;
    }
    case H_SADDUS2: case H_SADDSU2: {
        /* unsigned 16-bit fields of one operand plus signed fields of the other, saturated to u16 */
        uint32_t us = h == H_SADDUS2 ? (uint32_t)v[0] : (uint32_t)v[1];
        uint32_t ss = h == H_SADDUS2 ? (uint32_t)v[1] : (uint32_t)v[0];
        int32_t l = (int32_t)lsb16u(us) + lsb16s(ss), m = (int32_t)msb16u(us) + msb16s(ss);
        l = l < 0 ? 0 : l > 65535 ? 65535 : l;
        m = m < 0 ? 0 : m > 65535 ? 65535 : m;
        W(2, (uint32_t)l | ((uint32_t)m << 16));
        return;
    }
    case H_SADDU4: {
        uint32_t o = 0;
        for (int k = 0; k < 32; k += 8) {
            unsigned s = (((uint32_t)v[0] >> k) & 0xff) + (((uint32_t)v[1] >> k) & 0xff);
            o |= (s > 255 ? 255 : s) << k;
        }
        W(2, o);
        return;
    }
    case H_SUBC: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        W(2, s1 >= s2 ? ((s1 - s2) << 1) + 1 : s1 << 1);
        return;
    }
    case H_SUBABS4: {
        uint32_t o = 0;
        for (int k = 0; k < 32; k += 8) {
            int d = (int)(((uint32_t)v[0] >> k) & 0xff) - (int)(((uint32_t)v[1] >> k) & 0xff);
            o |= (uint32_t)(d < 0 ? -d : d) << k;
        }
        W(2, o);
        return;
    }
    case H_NORM: {
        int bits = op[0].size == 5 ? 40 : 32;
        int64_t s = SV(0);
        uint64_t t = (uint64_t)(s < 0 ? ~s : s);
        int n = 0;
        for (int k = bits - 2; k >= 0 && !((t >> k) & 1); k--)
            n++;
        W(1, n);
        return;
    }
    case H_LMBD: {
        unsigned bit = (uint32_t)v[0] & 1;
        uint32_t s = (uint32_t)v[1];
        unsigned n = 0;
        while (n < 32 && (((s >> (31 - n)) & 1) != bit))
            n++;
        W(2, n);
        return;
    }
    case H_BITC4: {
        uint32_t o = 0, s = (uint32_t)v[0];
        for (int k = 0; k < 32; k += 8)
            o |= (uint32_t)__builtin_popcount((s >> k) & 0xff) << k;
        W(1, o);
        return;
    }
    case H_BITR: W(1, bitrev32((uint32_t)v[0])); return;
    case H_DEAL: {
        uint32_t s = (uint32_t)v[0], o = 0;
        for (int k = 0; k < 16; k++) {
            o |= ((s >> (2 * k + 1)) & 1) << (16 + k);
            o |= ((s >> (2 * k)) & 1) << k;
        }
        W(1, o);
        return;
    }
    case H_SHFL: {
        uint32_t s = (uint32_t)v[0], o = 0;
        for (int k = 0; k < 16; k++) {
            o |= ((s >> (16 + k)) & 1) << (2 * k + 1);
            o |= ((s >> k) & 1) << (2 * k);
        }
        W(1, o);
        return;
    }
    case H_XPND2: {
        uint32_t s = (uint32_t)v[0];
        W(1, (s & 1 ? 0x0000ffff : 0) | (s & 2 ? 0xffff0000 : 0));
        return;
    }
    case H_XPND4: {
        uint32_t s = (uint32_t)v[0], o = 0;
        for (int k = 0; k < 4; k++)
            if (s & (1u << k)) o |= 0xffu << (8 * k);
        W(1, o);
        return;
    }
    case H_SWAP2: { uint32_t s = (uint32_t)v[0]; W(1, (s << 16) | (s >> 16)); return; }
    case H_SWAP4: {
        uint32_t s = (uint32_t)v[0];
        W(1, ((s & 0x00ff00ff) << 8) | ((s >> 8) & 0x00ff00ff));
        return;
    }
    case H_AVG2: {
        int32_t l = (lsb16s(v[0]) + lsb16s(v[1]) + 1) >> 1, m = (msb16s(v[0]) + msb16s(v[1]) + 1) >> 1;
        W(2, (uint16_t)l | ((uint32_t)(uint16_t)m << 16));
        return;
    }
    case H_AVGU4: {
        uint32_t o = 0;
        for (int k = 0; k < 32; k += 8)
            o |= (((((uint32_t)v[0] >> k) & 0xff) + (((uint32_t)v[1] >> k) & 0xff) + 1) >> 1) << k;
        W(2, o);
        return;
    }
    case H_MAX2: case H_MIN2: {
        int32_t l1 = lsb16s(v[0]), l2 = lsb16s(v[1]), m1 = msb16s(v[0]), m2 = msb16s(v[1]);
        int32_t l = h == H_MAX2 ? (l1 >= l2 ? l1 : l2) : (l1 <= l2 ? l1 : l2);
        int32_t m = h == H_MAX2 ? (m1 >= m2 ? m1 : m2) : (m1 <= m2 ? m1 : m2);
        W(2, (uint16_t)l | ((uint32_t)(uint16_t)m << 16));
        return;
    }
    case H_MAXU4: case H_MINU4: {
        uint32_t o = 0;
        for (int k = 0; k < 32; k += 8) {
            uint32_t p = ((uint32_t)v[0] >> k) & 0xff, q = ((uint32_t)v[1] >> k) & 0xff;
            o |= (h == H_MAXU4 ? (p >= q ? p : q) : (p <= q ? p : q)) << k;
        }
        W(2, o);
        return;
    }
    case H_PACK2:   W(2, (((uint32_t)v[0] & 0xffff) << 16) | ((uint32_t)v[1] & 0xffff)); return;
    case H_PACKH2:  W(2, (((uint32_t)v[0] >> 16) << 16) | ((uint32_t)v[1] >> 16)); return;
    case H_PACKHL2: W(2, (((uint32_t)v[0] >> 16) << 16) | ((uint32_t)v[1] & 0xffff)); return;
    case H_PACKLH2: W(2, (((uint32_t)v[0] & 0xffff) << 16) | ((uint32_t)v[1] >> 16)); return;
    case H_PACKH4: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        W(2, ((s1 >> 24) << 24) | (((s1 >> 8) & 0xff) << 16) | ((s2 >> 24) << 8) | ((s2 >> 8) & 0xff));
        return;
    }
    case H_PACKL4: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        W(2, (((s1 >> 16) & 0xff) << 24) | ((s1 & 0xff) << 16) | (((s2 >> 16) & 0xff) << 8) | (s2 & 0xff));
        return;
    }
    case H_SPACK2: {
        int32_t s1 = (int32_t)v[0], s2 = (int32_t)v[1];
        int32_t a1 = s1 > 32767 ? 32767 : s1 < -32768 ? -32768 : s1;
        int32_t a2 = s2 > 32767 ? 32767 : s2 < -32768 ? -32768 : s2;
        W(2, ((uint32_t)(uint16_t)a1 << 16) | (uint16_t)a2);
        return;
    }
    case H_SPACKU4: {
        uint32_t o = 0;
        int16_t f[4] = { msb16s(v[0]), lsb16s(v[0]), msb16s(v[1]), lsb16s(v[1]) };
        for (int k = 0; k < 4; k++) {
            int t = f[k] > 255 ? 255 : f[k] < 0 ? 0 : f[k];
            o |= (uint32_t)t << (8 * (3 - k));
        }
        W(2, o);
        return;
    }
    case H_RPACK2: {
        int32_t s1 = (int32_t)v[0], s2 = (int32_t)v[1];
        int32_t a1 = (int32_t)sat_to(c, in, (int64_t)s1 << 1, 32) + 0x8000;
        int32_t a2 = (int32_t)sat_to(c, in, (int64_t)s2 << 1, 32) + 0x8000;
        W(2, (((uint32_t)a1 >> 16) << 16) | ((uint32_t)a2 >> 16));
        return;
    }
    case H_DPACK2: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        uint32_t e = ((s1 & 0xffff) << 16) | (s2 & 0xffff);
        uint32_t o = ((s1 >> 16) << 16) | (s2 >> 16);
        W(2, ((uint64_t)o << 32) | e);
        return;
    }
    case H_DPACKX2: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        uint32_t e = ((s1 & 0xffff) << 16) | (s2 >> 16);
        uint32_t o = ((s2 & 0xffff) << 16) | (s1 >> 16);
        W(2, ((uint64_t)o << 32) | e);
        return;
    }
    case H_UNPKHU4: {
        uint32_t s = (uint32_t)v[0];
        W(1, (((s >> 24) & 0xff) << 16) | ((s >> 16) & 0xff));
        return;
    }
    case H_UNPKLU4: {
        uint32_t s = (uint32_t)v[0];
        W(1, (((s >> 8) & 0xff) << 16) | (s & 0xff));
        return;
    }

    case H_MPY16: case H_SMPY16: {
        int f1h = in->sub & 1, f2h = (in->sub >> 1) & 1;
        char sg1 = (in->sub & 4) ? 'u' : 's', sg2 = (in->sub & 8) ? 'u' : 's';
        uint32_t x1 = (uint32_t)v[0], x2 = (uint32_t)v[1];
        if (op[0].kind == C66X_OPK_CONST) x1 = (uint32_t)op[0].val;
        int64_t a1 = f1h ? (sg1 == 'u' ? msb16u(x1) : msb16s(x1)) : (sg1 == 'u' ? lsb16u(x1) : lsb16s(x1));
        int64_t a2 = f2h ? (sg2 == 'u' ? msb16u(x2) : msb16s(x2)) : (sg2 == 'u' ? lsb16u(x2) : lsb16s(x2));
        int64_t prod = a1 * a2;
        if (h == H_SMPY16) {
            prod <<= 1;
            if (prod == 0x80000000LL) { set_sat(c, in); prod = 0x7fffffff; }
        }
        W(2, prod);
        return;
    }
    case H_MPY32: {
        int64_t s1 = (int32_t)v[0], s2 = (int32_t)v[1];
        uint64_t u1 = (uint32_t)v[0], u2 = (uint32_t)v[1];
        uint64_t res;
        if (in->sub == 1) res = u1 * u2;
        else if (in->sub == 2) res = (uint64_t)(s1 * (int64_t)u2);
        else if (in->sub == 3) res = (uint64_t)((int64_t)u1 * s2);
        else res = (uint64_t)(s1 * s2);
        W(2, res);
        return;
    }
    case H_MPYI: case H_MPYID: {
        int64_t s1 = op[0].kind == C66X_OPK_CONST ? op[0].val : (int32_t)v[0];
        W(2, s1 * (int32_t)v[1]);
        return;
    }
    case H_MPYHI: case H_MPYHIR: case H_MPYLI: case H_MPYLIR: {
        /* 16-bit field of one operand times the 32-bit other; mpyih/mpyil list the 32-bit one first */
        int swapped = in->sub;
        uint32_t x16 = (uint32_t)(swapped ? v[1] : v[0]);
        int64_t x32 = (int32_t)(swapped ? v[0] : v[1]);
        int64_t f = (h == H_MPYHI || h == H_MPYHIR) ? msb16s(x16) : lsb16s(x16);
        int64_t prod = f * x32;
        if (h == H_MPYHIR || h == H_MPYLIR)
            W(2, (prod + 0x4000) >> 15);
        else
            W(2, prod);
        return;
    }
    case H_MPY2: case H_SMPY2: case H_MPYSU4: case H_MPYU4: case H_MPYUS4: {
        uint32_t x1 = (uint32_t)v[0], x2 = (uint32_t)v[1];
        int64_t e, o;
        if (h == H_MPY2 || h == H_SMPY2) {
            e = (int64_t)lsb16s(x1) * lsb16s(x2);
            o = (int64_t)msb16s(x1) * msb16s(x2);
            if (h == H_SMPY2) {
                e <<= 1; o <<= 1;
                if (e == 0x80000000LL) { e = 0x7fffffff; set_sat(c, in); }
                if (o == 0x80000000LL) { o = 0x7fffffff; set_sat(c, in); }
            }
        } else {
            /* four 8x8 products, packed 16 bits each into the pair */
            uint64_t res = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t p = (x1 >> (8 * k)) & 0xff, q = (x2 >> (8 * k)) & 0xff;
                int32_t t = h == H_MPYU4 ? (int32_t)(p * q)
                           : h == H_MPYSU4 ? (int8_t)p * (int32_t)q : (int32_t)p * (int8_t)q;
                res |= (uint64_t)(uint16_t)t << (16 * k);
            }
            W(2, res);
            return;
        }
        W(2, ((uint64_t)(uint32_t)o << 32) | (uint32_t)e);
        return;
    }
    case H_SMPY32: {
        int64_t p = (int64_t)(int32_t)v[0] * (int32_t)v[1];
        /* (src1 * src2 << 1) >> 32, saturating the single overflow case */
        if (p == 0x4000000000000000LL) { set_sat(c, in); W(2, 0x7fffffff); return; }
        W(2, (p << 1) >> 32);
        return;
    }
    case H_MPY2IR: {
        int64_t p1 = (int64_t)msb16s(v[0]) * (int32_t)v[1];
        int64_t p2 = (int64_t)lsb16s(v[0]) * (int32_t)v[1];
        W(2, ((uint64_t)(uint32_t)((p1 + 0x4000) >> 15) << 32) | (uint32_t)((p2 + 0x4000) >> 15));
        return;
    }
    case H_DOTP2: case H_DOTPN2: {
        int64_t lo = (int64_t)lsb16s(v[0]) * lsb16s(v[1]);
        int64_t hi = (int64_t)msb16s(v[0]) * msb16s(v[1]);
        int64_t res = h == H_DOTP2 ? lo + hi : hi - lo;
        W(2, res);
        return;
    }
    case H_DOTPRSU2: case H_DOTPNRSU2: {
        /* signed 16-bit fields of src1 x unsigned fields of src2; the "us" twins list src2 first */
        int us = in->sub;
        uint32_t s1 = (uint32_t)(us ? v[1] : v[0]), s2 = (uint32_t)(us ? v[0] : v[1]);
        int64_t hi = (int64_t)msb16s(s1) * msb16u(s2);
        int64_t lo = (int64_t)lsb16s(s1) * lsb16u(s2);
        int64_t t = (h == H_DOTPRSU2 ? hi + lo : hi - lo) + 0x8000;
        W(2, t >> 16);
        return;
    }
    case H_DOTPSU4: case H_DOTPU4: {
        int us = in->sub;
        uint32_t s1 = (uint32_t)(us ? v[1] : v[0]), s2 = (uint32_t)(us ? v[0] : v[1]);
        int64_t sum = 0;
        for (int k = 0; k < 32; k += 8) {
            uint32_t p = (s1 >> k) & 0xff, q = (s2 >> k) & 0xff;
            sum += h == H_DOTPU4 ? (int64_t)p * q : (int64_t)(int8_t)p * q;
        }
        W(2, sum);
        return;
    }
    case H_DDOTP4: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        int64_t e = (int64_t)msb16s(s1) * (int8_t)(s2 >> 8) + (int64_t)lsb16s(s1) * (int8_t)s2;
        int64_t o = (int64_t)msb16s(s1) * (int8_t)(s2 >> 24) + (int64_t)lsb16s(s1) * (int8_t)(s2 >> 16);
        W(2, ((uint64_t)(uint32_t)o << 32) | (uint32_t)e);
        return;
    }
    case H_DDOTPH2: case H_DDOTPL2: case H_DDOTPH2R: case H_DDOTPL2R: {
        uint32_t se = (uint32_t)v[0], so = (uint32_t)(v[0] >> 32), s2 = (uint32_t)v[1];
        int64_t first, second;
        if (h == H_DDOTPH2 || h == H_DDOTPH2R) {
            first = (int64_t)msb16s(so) * msb16s(s2) + (int64_t)lsb16s(so) * lsb16s(s2);
        } else {
            first = (int64_t)msb16s(se) * msb16s(s2) + (int64_t)lsb16s(se) * lsb16s(s2);
        }
        second = (int64_t)lsb16s(so) * msb16s(s2) + (int64_t)msb16s(se) * lsb16s(s2);
        if (h == H_DDOTPH2 || h == H_DDOTPL2) {
            int64_t f = sat_to(c, in, first, 32), s = sat_to(c, in, second, 32);
            W(2, ((uint64_t)(uint32_t)f << 32) | (uint32_t)s);
        } else {
            uint32_t f = (uint32_t)sat_to(c, in, first + 0x8000, 32) >> 16;
            uint32_t s = (uint32_t)sat_to(c, in, second + 0x8000, 32) >> 16;
            if (h == H_DDOTPH2R) W(2, (f << 16) | (s & 0xffff));
            else W(2, (s << 16) | (f & 0xffff));
        }
        return;
    }
    case H_CMPY: case H_CMPYR: case H_CMPYR1: {
        uint32_t s1 = (uint32_t)v[0], s2 = (uint32_t)v[1];
        int64_t e = (int64_t)lsb16s(s1) * msb16s(s2) + (int64_t)msb16s(s1) * lsb16s(s2);
        int64_t o = (int64_t)msb16s(s1) * msb16s(s2) - (int64_t)lsb16s(s1) * lsb16s(s2);
        e = sat_to(c, in, e, 32);
        if (h == H_CMPY) {
            W(2, ((uint64_t)(uint32_t)o << 32) | (uint32_t)e);
        } else if (h == H_CMPYR) {
            o = sat_to(c, in, o, 32);
            uint32_t lo = (uint32_t)sat_to(c, in, e + 0x8000, 32) >> 16;
            uint32_t hi = (uint32_t)sat_to(c, in, o + 0x8000, 32) >> 16;
            W(2, (hi << 16) | lo);
        } else {
            o = sat_to(c, in, o, 32);
            uint32_t lo = (uint32_t)sat_to(c, in, (e + 0x4000) << 1, 32) >> 16;
            uint32_t hi = (uint32_t)sat_to(c, in, (o + 0x4000) << 1, 32) >> 16;
            W(2, (hi << 16) | lo);
        }
        return;
    }
    case H_GMPY: W(2, gf_mul((uint32_t)v[0], (uint32_t)v[1], c->cr[CR_GPLYA], 32)); return;
    case H_XORMPY: {
        uint64_t p = 0, a1 = (uint32_t)v[0];
        uint32_t b1 = (uint32_t)v[1];
        for (int k = 0; k < 32; k++) if (b1 & (1u << k)) p ^= a1 << k;
        W(2, (uint32_t)p);
        return;
    }
    case H_GMPY4: {
        uint32_t poly = 0x100 | (c->cr[CR_GFPGFR] & 0xff), o = 0;
        for (int k = 0; k < 32; k += 8)
            o |= gf_mul(((uint32_t)v[0] >> k) & 0xff, ((uint32_t)v[1] >> k) & 0xff, poly, 8) << k;
        W(2, o);
        return;
    }

    case H_ABSSP: W(1, (uint32_t)v[0] & 0x7fffffff); return;
    case H_ABSDP: W(1, v[0] & 0x7fffffffffffffffULL); return;
    case H_ADDSP: case H_SUBSP: case H_MPYSP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        float p = u2f((uint32_t)v[0]), q = u2f((uint32_t)v[1]);
        float res = h == H_ADDSP ? p + q : h == H_SUBSP ? p - q : p * q;
        if (rm) fesetround(FE_TONEAREST);
        W(2, f2u(res));
        return;
    }
    case H_ADDDP: case H_SUBDP: case H_MPYDP: case H_MPYSPDP: case H_MPYSP2DP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        double p, q, res;
        if (h == H_MPYSPDP) { p = u2f((uint32_t)v[0]); q = u2d(v[1]); }
        else if (h == H_MPYSP2DP) { p = u2f((uint32_t)v[0]); q = u2f((uint32_t)v[1]); }
        else { p = u2d(v[0]); q = u2d(v[1]); }
        res = h == H_ADDDP ? p + q : h == H_SUBDP ? p - q : p * q;
        if (rm) fesetround(FE_TONEAREST);
        W(2, d2u(res));
        return;
    }
    case H_CMPEQSP: W(2, u2f((uint32_t)v[0]) == u2f((uint32_t)v[1])); return;
    case H_CMPGTSP: W(2, u2f((uint32_t)v[0]) > u2f((uint32_t)v[1])); return;
    case H_CMPLTSP: W(2, u2f((uint32_t)v[0]) < u2f((uint32_t)v[1])); return;
    case H_CMPEQDP: W(2, u2d(v[0]) == u2d(v[1])); return;
    case H_CMPGTDP: W(2, u2d(v[0]) > u2d(v[1])); return;
    case H_CMPLTDP: W(2, u2d(v[0]) < u2d(v[1])); return;
    case H_INTSP: case H_INTSPU: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        float f = h == H_INTSP ? (float)(int32_t)v[0] : (float)(uint32_t)v[0];
        if (rm) fesetround(FE_TONEAREST);
        W(1, f2u(f));
        return;
    }
    case H_INTDP: W(1, d2u((double)(int32_t)v[0])); return;
    case H_INTDPU: W(1, d2u((double)(uint32_t)v[0])); return;
    case H_SPINT: W(1, (uint32_t)f_to_i32(u2f((uint32_t)v[0]), 0, rmode(c, in))); return;
    case H_DPINT: W(1, (uint32_t)f_to_i32(u2d(v[0]), 0, rmode(c, in))); return;
    case H_SPTRUNC: W(1, (uint32_t)f_to_i32(u2f((uint32_t)v[0]), 1, 0)); return;
    case H_DPTRUNC: W(1, (uint32_t)f_to_i32(u2d(v[0]), 1, 0)); return;
    case H_SPDP: W(1, d2u((double)u2f((uint32_t)v[0]))); return;
    case H_DPSP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        float f = (float)u2d(v[0]);
        if (rm) fesetround(FE_TONEAREST);
        W(1, f2u(f));
        return;
    }
    /* The silicon returns an 8-bit-accurate seed for Newton iteration; the
     * exact value converges to the same result. */
    case H_RCPSP: W(1, f2u(1.0f / u2f((uint32_t)v[0]))); return;
    case H_RCPDP: W(1, d2u(1.0 / u2d(v[0]))); return;
    case H_RSQRSP: W(1, f2u(1.0f / sqrtf(u2f((uint32_t)v[0])))); return;
    case H_RSQRDP: W(1, d2u(1.0 / sqrt(u2d(v[0])))); return;

    /* C66x two-way SIMD: the same 32-bit operation on each half of a pair. */
    case H_DADD: case H_DSUB: case H_DSADD: case H_DSSUB: {
        uint32_t p[2], q[2], o[2];
        for (int k = 0; k < 2; k++) {
            p[k] = op[0].kind == C66X_OPK_CONST ? (uint32_t)op[0].val : (uint32_t)(v[0] >> (32 * k));
            q[k] = (uint32_t)(v[1] >> (32 * k));
            int64_t r2 = (h == H_DADD || h == H_DSADD) ? (int64_t)(int32_t)p[k] + (int32_t)q[k]
                                                        : (int64_t)(int32_t)p[k] - (int32_t)q[k];
            if (h == H_DSADD || h == H_DSSUB)
                r2 = sat_to(c, in, r2, 32);
            o[k] = (uint32_t)r2;
        }
        W(2, ((uint64_t)o[1] << 32) | o[0]);
        return;
    }
    case H_DADDSP: case H_DSUBSP: case H_DMPYSP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        uint32_t o[2];
        for (int k = 0; k < 2; k++) {
            float p = u2f((uint32_t)(v[0] >> (32 * k))), q = u2f((uint32_t)(v[1] >> (32 * k)));
            o[k] = f2u(h == H_DADDSP ? p + q : h == H_DSUBSP ? p - q : p * q);
        }
        if (rm) fesetround(FE_TONEAREST);
        W(2, ((uint64_t)o[1] << 32) | o[0]);
        return;
    }
    case H_QMPYSP: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        for (int k = 0; k < 4; k++) {
            float p = u2f(c->reg[op[0].reg + k]), q = u2f(c->reg[op[1].reg + k]);
            sched(c, op[2].low_first - 1, WK_REG, op[2].reg + k, f2u(p * q), 0);
        }
        if (rm) fesetround(FE_TONEAREST);
        return;
    }
    case H_CMPYSP: {
        /*
         * Single-precision complex multiply, 128-bit result (SPRUGH7 4.62).
         * Used by the MASTER TEMPO time-stretch. Products in manual order:
         *   dst_0 = src1_lo * src2_hi      dst_1 = -(src1_lo * src2_lo)
         *   dst_2 = src1_hi * src2_lo      dst_3 = src1_hi * src2_hi
         * so dst_1:dst_0 is the real part's two terms (subtract to combine)
         * and dst_3:dst_2 the imaginary part's.
         */
        unsigned rm = rmode(c, in);
        float a_lo = u2f(c->reg[op[0].reg]), a_hi = u2f(c->reg[op[0].reg + 1]);
        float b_lo = u2f(c->reg[op[1].reg]), b_hi = u2f(c->reg[op[1].reg + 1]);
        float r4[4];

        if (rm) fesetround(fe_mode[rm]);
        r4[0] = a_lo * b_hi;
        r4[1] = -(a_lo * b_lo);
        r4[2] = a_hi * b_lo;
        r4[3] = a_hi * b_hi;
        for (int k = 0; k < 4; k++)
            sched(c, op[2].low_first - 1, WK_REG, op[2].reg + k, f2u(r4[k]), 0);
        if (rm) fesetround(FE_TONEAREST);
        return;
    }
    case H_QSMPY32R1:
        for (int k = 0; k < 4; k++) {
            int64_t p = (int32_t)c->reg[op[0].reg + k], q = (int32_t)c->reg[op[1].reg + k];
            int64_t r2 = sat_to(c, in, (p * q + (1LL << 30)) >> 31, 32);
            sched(c, op[2].low_first - 1, WK_REG, op[2].reg + k, (uint32_t)r2, 0);
        }
        return;
    case H_SHL2: {
        /* SPRUGH7 4.261: only the low four bits of the amount count. */
        unsigned n = (uint32_t)v[1] & 0xf;
        uint32_t s = (uint32_t)v[0];
        W(2, (((s >> 16) << n) & 0xffff) << 16 | ((s << n) & 0xffff));
        return;
    }
    case H_DPACKL4: {
        uint64_t o = 0;
        for (int k = 0; k < 2; k++) {
            uint32_t s1 = (uint32_t)(v[0] >> (32 * k)), s2 = (uint32_t)(v[1] >> (32 * k));
            uint32_t w = (((s1 >> 16) & 0xff) << 24) | ((s1 & 0xff) << 16)
                       | (((s2 >> 16) & 0xff) << 8) | (s2 & 0xff);
            o |= (uint64_t)w << (32 * k);
        }
        W(2, o);
        return;
    }
    case H_DCCMPYR1: {
        /*
         * Two CMPYR1s of src1 against the CONJUGATE of src2 (SPRUGH7 4.74).
         * The manual's Execution block has the imaginary term's sign flipped;
         * its seven worked examples (saturating ones included) all agree with
         * src1 * conj(src2), so that is what this computes.
         */
        uint64_t o = 0;
        for (int k = 0; k < 2; k++) {
            uint32_t s1 = (uint32_t)(v[0] >> (32 * k)), s2 = (uint32_t)(v[1] >> (32 * k));
            int64_t im = (int64_t)lsb16s(s1) * msb16s(s2) - (int64_t)msb16s(s1) * lsb16s(s2);
            int64_t re = (int64_t)msb16s(s1) * msb16s(s2) + (int64_t)lsb16s(s1) * lsb16s(s2);
            im = sat_to(c, in, im, 32);
            re = sat_to(c, in, re, 32);
            uint32_t lo = (uint32_t)sat_to(c, in, (im + 0x4000) << 1, 32) >> 16;
            uint32_t hi = (uint32_t)sat_to(c, in, (re + 0x4000) << 1, 32) >> 16;
            o |= (uint64_t)((hi << 16) | lo) << (32 * k);
        }
        W(2, o);
        return;
    }
    case H_MPYU2: {
        uint32_t e = lsb16u(v[0]) * lsb16u(v[1]), o = msb16u(v[0]) * msb16u(v[1]);
        W(2, ((uint64_t)o << 32) | e);
        return;
    }
    case H_LAND: W(2, (uint32_t)v[0] != 0 && (uint32_t)v[1] != 0); return;
    case H_LANDN: W(2, (uint32_t)v[0] != 0 && (uint32_t)v[1] == 0); return;
    case H_LOR: W(2, (uint32_t)v[0] != 0 || (uint32_t)v[1] != 0); return;
    case H_DINTHSP: case H_DINTHSPU: {
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        float e = h == H_DINTHSP ? (float)lsb16s(v[0]) : (float)lsb16u(v[0]);
        float o = h == H_DINTHSP ? (float)msb16s(v[0]) : (float)msb16u(v[0]);
        if (rm) fesetround(FE_TONEAREST);
        W(1, ((uint64_t)f2u(o) << 32) | f2u(e));
        return;
    }
    case H_DINTSP: case H_DINTSPU: {
        /* SPRUGH7 4.93 (printed under the heading DINTHSP) and 4.95: each
         * 32-bit word of the src2 pair to single precision. */
        unsigned rm = rmode(c, in);
        if (rm) fesetround(fe_mode[rm]);
        uint32_t w_e = (uint32_t)v[0], w_o = (uint32_t)(v[0] >> 32);
        float e = h == H_DINTSP ? (float)(int32_t)w_e : (float)w_e;
        float o = h == H_DINTSP ? (float)(int32_t)w_o : (float)w_o;
        if (rm) fesetround(FE_TONEAREST);
        W(1, ((uint64_t)f2u(o) << 32) | f2u(e));
        return;
    }
    case H_DSPINT: {
        uint32_t e = (uint32_t)f_to_i32(u2f((uint32_t)v[0]), 0, rmode(c, in));
        uint32_t o = (uint32_t)f_to_i32(u2f((uint32_t)(v[0] >> 32)), 0, rmode(c, in));
        W(1, ((uint64_t)o << 32) | e);
        return;
    }
    case H_DSADD2: case H_DAVGNR2: case H_DCMPGT2: case H_DSHL2: case H_UNPKH2: case H_DMPY2: {
        uint64_t res = 0;
        for (int k = 0; k < 4; k++) {
            int32_t p = (int16_t)(v[0] >> (16 * k)), q = (int16_t)(v[1] >> (16 * k));
            int32_t t;
            switch (h) {
            case H_DSADD2: t = sat16(p + q); break;
            case H_DAVGNR2: t = (p + q) >> 1; break;
            case H_DCMPGT2: t = p > q; break;
            default: t = 0; break;
            }
            if (h == H_DCMPGT2)
                res |= (uint64_t)t << k;
            else if (h != H_UNPKH2 && h != H_DMPY2)
                res |= (uint64_t)(uint16_t)t << (16 * k);
        }
        if (h == H_UNPKH2)
            res = ((uint64_t)(uint32_t)msb16s(v[0]) << 32) | (uint32_t)lsb16s(v[0]);
        /* One site each in the image; left as traps until one executes. */
        if (h == H_DMPY2 || h == H_DSHL2) {
            c->trap_pc = in->addr;
            x->stop = C66X_STOP_UNDEF;
            return;
        }
        W(LAST, res);
        return;
    }

    case H_LOAD:
        do_load(c, in);
        return;
    case H_STORE:
        do_store(c, in, x->pce1);
        return;

    case H_B:
        if (op[0].kind == C66X_OPK_IRP) {
            uint32_t itsr = c->cr[CR_ITSR];
            c->cr[CR_TSR] = itsr;
            take_branch(c, c->cr[CR_IRP], x);
            idle_isr_return(c, c->cr[CR_IRP]);
        } else if (op[0].kind == C66X_OPK_NRP) {
            c->cr[CR_TSR] = c->cr[CR_NTSR];
            c->cr[CR_IER] |= IER_NMIE;
            take_branch(c, c->cr[CR_NRP], x);
            idle_isr_return(c, c->cr[CR_NRP]);
        } else {
            take_branch(c, (uint32_t)opval(c, &op[0], x->pce1), x);
        }
        return;
    case H_BNOP:
        take_branch(c, (uint32_t)opval(c, &op[0], x->pce1), x);
        return;
    case H_CALLP:
        take_branch(c, (uint32_t)op[0].val, x);
        W(1, x->next_pc);
        return;
    case H_BDEC:
        if ((int32_t)v[1] >= 0) {
            take_branch(c, (uint32_t)op[0].val, x);
            W(1, (uint32_t)v[1] - 1);
        }
        return;
    case H_BPOS:
        if ((int32_t)v[1] >= 0)
            take_branch(c, (uint32_t)op[0].val, x);
        return;

    case H_SPLOOP: case H_SPLOOPD: case H_SPLOOPW:
        spl_start(c, in, x);
        return;
    case H_SPKERNEL: case H_SPMASK:
        return;           /* handled at packet level */
    case H_SPKERNELR: case H_SPMASKR:
        c->trap_pc = in->addr;
        x->stop = C66X_STOP_UNDEF;   /* loop reload: not used by this image */
        return;
    }
    c->trap_pc = in->addr;
    x->stop = C66X_STOP_UNDEF;
#undef SV
#undef UV
#undef W
#undef LAST
}

/* ------------------------------------------------------------------------ */
/* SPLOOP loop buffer (SPRU732 chapter 7)                                    */

static void spl_start(c66x_core *c, const c66x_insn *in, xctx *x)
{
    spl_state *s = &c->spl;
    int resumed = (c->cr[CR_TSR] & TSR_SPLX) != 0;
    memset(s, 0, sizeof *s);
    s->active = 1;
    s->kind = in->handler;
    s->ii = in->op[0].val;
    s->creg = in->cond_reg;
    s->cz = in->cond_z;
    s->addr = in->addr;
    s->t0 = c->cycle + 1;
    s->loading = 1;
    s->resumed = resumed;
    if (resumed && s->kind == H_SPLOOPD)
        s->kind = H_SPLOOP;
    c->cr[CR_TSR] |= TSR_SPLX;
    c->st.sploops++;
    if (s->kind == H_SPLOOP) {
        if (c->cr[CR_ILC] == 0) {
            s->initial_term = 1;
            s->terminated = 1;
            s->drain_start = s->t0;
        } else {
            c->cr[CR_ILC]--;
        }
    }
}

static int spl_cond_true(c66x_core *c, uint64_t cyc)
{
    uint32_t r = c->cond_hist[cyc & 7];
    return (r == 0) == c->spl.cz;
}

/* The loop stopped starting iterations at drain_start: when the kernel had
 * disabled program fetch, post-loop code waits for the fetch delay (capped at
 * the epilog, SPKERNEL note) plus the pipeline refill. */
static void spl_schedule_resume(c66x_core *c, int delay)
{
    spl_state *s = &c->spl;
    if (s->loading)
        return;                         /* early exit: fetch was never disabled */
    int epilog = s->dynlen - s->ii;
    if (epilog < 0)
        epilog = 0;
    if (delay > epilog)
        delay = epilog;
    uint64_t enable = s->drain_start + delay;
    if (enable > s->kernel_cycle + 1)
        c->pm_resume_at = enable + PM_REFILL;
}

static int spl_pm_enabled(c66x_core *c)
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

/* r / ii and r % ii for r = cycle - t0 (cycle >= t0). */
static inline void spl_pos(c66x_core *c, uint64_t *k, uint32_t *off)
{
    spl_state *s = &c->spl;
    uint32_t ii = (uint32_t)s->ii;
    if (s->pos_t0 == s->t0 && s->pos_ii == ii && s->pos_cycle + 1 == c->cycle) {
        if (++s->pos_off == ii) {
            s->pos_off = 0;
            s->pos_k++;
        }
    } else if (!(s->pos_t0 == s->t0 && s->pos_ii == ii && s->pos_cycle == c->cycle)) {
        uint64_t r = c->cycle - s->t0;
        s->pos_k = r / ii;
        s->pos_off = (uint32_t)(r - s->pos_k * ii);
        s->pos_t0 = s->t0;
        s->pos_ii = ii;
    }
    s->pos_cycle = c->cycle;
    *k = s->pos_k;
    *off = s->pos_off;
}

/* Instructions the loop buffer issues this cycle, iterations k >= 1. */
static unsigned spl_buffer_insns(c66x_core *c, c66x_insn **out, uint32_t mask)
{
    spl_state *s = &c->spl;
    unsigned n = 0;
    if (!s->active || s->initial_term || c->cycle < s->t0)
        return 0;
    int64_t r = c->cycle - s->t0;
    int loaded = s->loading ? (int)r : s->dynlen;
    /* Only the iterations whose body offset falls in [0, loaded) this cycle;
     * a long SPLOOPW runs millions of iterations. */
    uint64_t kq;
    uint32_t oq;
    spl_pos(c, &kq, &oq);
    int64_t kmax = (int64_t)kq;
    if (kmax > s->last_iter) kmax = s->last_iter;
    int64_t kmin = kmax + 1;
    while (kmin > 1 && r - (kmin - 1) * s->ii < loaded)
        kmin--;
    for (int64_t k = kmin; k <= kmax; k++) {
        int64_t off = r - (int64_t)k * s->ii;
        spl_ent *e = &s->body[off];
        for (unsigned i = 0; i < e->n && n < MAX_PK; i++) {
            c66x_insn *in = e->insn[i];
            if (!s->resumed && in->unit >= 0 && (mask & (1u << in->unit)))
                continue;
            out[n++] = in;
        }
    }
    return n;
}

/* End of a cycle: record the program-memory packet, then the stage boundary. */
static void spl_end_cycle(c66x_core *c, c66x_insn **pm, unsigned npm, uint32_t mask, int *pm_exec_mask)
{
    spl_state *s = &c->spl;
    if (!s->active || c->cycle < s->t0)
        return;
    int64_t r = c->cycle - s->t0;

    if (s->loading && r < 48) {
        spl_ent *e = &s->body[r];
        e->n = 0;
        int kernel = 0, fstg = 0, fcyc = 0;
        for (unsigned i = 0; i < npm; i++) {
            c66x_insn *in = pm[i];
            if (in->handler == H_SPKERNEL) {
                kernel = 1;
                if (in->nops >= 2) { fstg = in->op[0].val; fcyc = in->op[1].val; }
                continue;
            }
            if (in->handler == H_SPMASK || in->handler == H_BNOP || in->handler == H_NOP)
                continue;
            if (in->unit >= 0 && (mask & (1u << in->unit)))
                continue;
            if (e->n < 8)
                e->insn[e->n++] = in;
        }
        if (kernel) {
            s->loading = 0;
            s->dynlen = r + 1;
            s->fetch_delay = fstg * s->ii + fcyc;
            s->kernel_cycle = c->cycle;
        }
    }
    (void)pm_exec_mask;

    uint64_t pk;
    uint32_t poff;
    spl_pos(c, &pk, &poff);
    if (poff + 1 == (uint32_t)s->ii && !s->terminated && !s->abrupt) {
        int early3 = (s->kind != H_SPLOOP) && (r + 1) <= 3;
        int term = s->kind == H_SPLOOPW ? (!early3 && !spl_cond_true(c, c->cycle - 3))
                                        : (!early3 && c->cr[CR_ILC] == 0);
        /* SPRU732 7.13.1: an interrupt drains the loop at a stage boundary. */
        if (!term && !early3 && !s->loading && c->ifr && pending_interrupt(c)
            && !c->nbr && !c->branch_block
            && (s->kind == H_SPLOOPW
                || c->cr[CR_ILC] >= (uint32_t)((s->dynlen + s->ii - 1) / s->ii))) {
            s->int_drain = 1;
            s->terminated = 1;
            s->drain_start = c->cycle + 1;
        } else if (s->kind == H_SPLOOPW) {
            if (!early3 && !spl_cond_true(c, c->cycle - 3)) {
                s->abrupt = 1;
                s->terminated = 1;
                s->drain_start = c->cycle + 1;
                spl_schedule_resume(c, 0);
            } else {
                s->last_iter = (r + 1) / s->ii;
            }
        } else if (early3) {
            s->last_iter = (r + 1) / s->ii;
        } else if (c->cr[CR_ILC] == 0) {
            s->terminated = 1;
            s->drain_start = c->cycle + 1;
            spl_schedule_resume(c, s->fetch_delay);
        } else {
            c->cr[CR_ILC]--;
            s->last_iter = (r + 1) / s->ii;
        }
    }

    /* Idle once nothing is loading and the last started iteration has finished. */
    if (!s->loading) {
        int done;
        if (s->abrupt)
            done = 1;
        else if (s->terminated) {
            int64_t end = (int64_t)s->last_iter * s->ii + s->dynlen;
            if (s->initial_term)
                end = ((s->dynlen + s->ii - 1) / s->ii) * s->ii;
            done = r + 1 >= end;
        } else
            done = 0;
        if (done) {
            HCLOOP(s->addr, (uint64_t)s->last_iter + 1, r + 1);
            s->active = 0;
            c->cr[CR_TSR] &= ~TSR_SPLX;
            if (s->int_drain) {
                c->spl_irq_pending = 1;
                c->spl_irq_ret = s->addr;
            }
        }
    }
}

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
static void idle_isr_return(c66x_core *c, uint32_t target)
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

static const c66x_jit_region *jit_lookup(c66x_core *c, uint32_t pc);
static const c66x_jit_region *jit_lookup0(c66x_core *c, uint32_t pc);
static const c66x_jit_qregion *jit_lookupq(c66x_core *c, uint32_t pc);

static int gather_packet(c66x_core *c, uint32_t pc, c66x_insn **pk, unsigned *n,
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

/* ------------------------------------------------------------------------ */
/* compiled regions: loading, lookup, profile                                */

#ifndef C66X_JIT_TOOLS
#define C66X_JIT_TOOLS "tools"      /* the Makefile passes the source tree's */
#endif
#ifndef C66X_JIT_LIBDIR
#define C66X_JIT_LIBDIR "."
#endif

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

static uint32_t api_mem_read(c66x_core *c, uint32_t a, unsigned n) { return mem_read(c, a, n); }
static void api_store_defer(c66x_core *c, uint32_t a, uint32_t v, unsigned n) { store_defer(c, a, v, n); }
static void api_flush_stores(c66x_core *c) { flush_stores(c); }
static void api_invalidate_code(c66x_core *c, uint32_t a, uint32_t n) { invalidate_code(c, a, n); }
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
static const c66x_jit_loop *jit_loop_lookup(c66x_core *c, uint32_t pc)
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

static const c66x_jit_region *jit_lookup(c66x_core *c, uint32_t pc)
{
    struct c66x_jit *j = c->jit;
    return j && j->nregions ? jit_lookup_in(c, j->map, pc) : NULL;
}

static const c66x_jit_region *jit_lookup0(c66x_core *c, uint32_t pc)
{
    struct c66x_jit *j = c->jit;
    return j && j->nregions0 ? jit_lookup_in(c, j->map0, pc) : NULL;
}

static const c66x_jit_qregion *jit_lookupq(c66x_core *c, uint32_t pc)
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

static void kstats_dump_fwd(void)
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

static const c66x_jit_kernel *jit_kernel_lookup(c66x_core *c)
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

static void jit_kprof(c66x_core *c)
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

static void jit_kprof_cycles(c66x_core *c, uint64_t n)
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

static void jit_prof(c66x_core *c, uint32_t pc)
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

static void jit_auto_poll(c66x_core *c)
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

/* C66X_JIT=<a.so>[:<b.so>...] loads compiled regions; C66X_JIT_PROFILE=<dir>
 * counts packets and writes the generator's input when the core is freed;
 * C66X_JIT_AUTO=<dir> compiles while running (jit_auto_poll). */
static void jit_init(c66x_core *c)
{
    c->api = &jit_api;
    const char *mods = getenv("C66X_JIT");
    if (mods && *mods) {
        char *list = strdup(mods);
        char *save = NULL;
        for (char *t = strtok_r(list, ":", &save); t; t = strtok_r(NULL, ":", &save))
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
    const char *adir = getenv("C66X_JIT_AUTO");
    if (adir && *adir) {
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

static void jit_free(c66x_core *c)
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
