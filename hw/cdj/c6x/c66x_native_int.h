/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x core: what the native kernels and the driver in c66x_native.c share.
 * Each kernel file ends its kernels with a native_<name> descriptor; the
 * driver lists them in its kernels[] table.
 */
#ifndef C66X_NATIVE_INT_H
#define C66X_NATIVE_INT_H

#include "c66x_priv.h"

#define A(n) (n)
#define B(n) (32 + (n))

typedef struct native_store {
    uint32_t a, v, pc;
    uint8_t n;                      /* bytes stored: 1, 2 or 4 */
} native_store;

#define NATIVE_STORES 256           /* the most stores one pass may stage */

/* A register result still in the pipeline at a loop head entered mid-pipeline:
 * it lands before the packet ahead + 1 cycles on. A conditional one may be
 * absent. A load's has read its memory already, at its issue. */
typedef struct native_pend {
    uint8_t reg, ahead, cond, load;
} native_pend;

#define NATIVE_PENDS 16

/* A span of RAM a pass writes whole, too large for single stores: len bytes
 * to dst from bulk_buf + off. */
typedef struct native_bulk {
    uint32_t dst, len, off;
    uint64_t hash;
} native_bulk;

#define NATIVE_BULKS 2
#define NATIVE_BULK_BYTES 0xC000    /* the pass's own scratch and staged spans */

typedef struct native_state {
    uint32_t reg[C66X_NREGS];
    uint32_t cr[32];
    uint32_t pc;                    /* where the pass ended */
    uint32_t last_pc;               /* the last packet it executed */
    uint32_t cycles, packets, insns, sploops;
    uint32_t pm_resume;             /* pm_resume_at, cycles after the pass start; 0 = untouched */
    uint32_t branch_block;          /* 5 unless the pass ends in a branch's delay slot */
    unsigned nst;
    native_store *st;               /* NATIVE_STORES long, owned by the driver: t = s copies only the pointer */
    struct { uint32_t dst, src, len, pc; uint64_t hash; } mv;   /* a forward copy, src above dst; len 0 = none */
    uint32_t at;                    /* in: where the kernel's code was found */
    uint32_t addr[2];               /* in: the data addresses its MVK/MVKH pairs build */
    uint32_t pend[NATIVE_PENDS];    /* in and out: the values of the kernel's in-flight writes */
    uint32_t pend_live;             /* which of them are in flight; an absent one holds the register */
    native_bulk bulk[NATIVE_BULKS];
    unsigned nbulk;
    uint8_t *bulk_buf;              /* NATIVE_BULK_BYTES, owned by the driver like st */
} native_state;

/* An instruction field a relink changes, left out of the comparison. The
 * 16-bit constant of an MVK or MVKH also gives the low or high half of addr[n]. */
typedef struct native_wild {
    uint16_t off;                   /* the instruction word, from the start of the code */
    int8_t addr;                    /* -1, or the addr[] it gives half of */
    uint8_t high;
    uint32_t mask;
} native_wild;

#define MVK_CST 0x007FFF80u         /* cst16 of a 32-bit MVK/MVKH */
#define ADDR_LO(off, n) { off, n, 0, MVK_CST }
#define ADDR_HI(off, n) { off, n, 1, MVK_CST }
#define WILD(off, mask) { off, -1, 0, mask }

/* tools/native_sig.py prints first, size and hash for a stretch of an image. */
typedef struct native_kernel {
    const char *name;
    uint32_t first;                 /* the first word of the code (never a wild one) */
    uint32_t size;                  /* bytes of code from a fetch packet start, wild fields included */
    uint64_t hash;                  /* FNV-1a of those bytes with the wild fields cleared */
    uint32_t entry;                 /* the loop head, from the start of the code */
    uint32_t exit;                  /* where the last pass ends, from the start of the code, or RETURNS */
    const native_wild *wild;
    unsigned nwild;
    int (*pass)(c66x_core *c, native_state *s);   /* 0: not natively (the code runs) */
    const native_pend *pend;        /* in flight at the head and again at the pass's end */
    unsigned npend;
} native_kernel;

#define NATIVE_SIZE_MAX 4096        /* the longest stretch a signature may cover */
#define RETURNS 0xFFFFFFFFu         /* a function: its one pass ends at B3 */

#define KERNEL(name, first, size, hash, entry, exit, wild, pass) \
    { name, first, size, hash, entry, exit, wild, sizeof wild / sizeof wild[0], pass }
#define KERNEL_PEND(name, first, size, hash, entry, exit, wild, pass, pend) \
    { name, first, size, hash, entry, exit, wild, sizeof wild / sizeof wild[0], pass, \
      pend, sizeof pend / sizeof pend[0] }

static inline float u2f(uint32_t v) { float f; memcpy(&f, &v, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t v; memcpy(&v, &f, 4); return v; }

static inline uint32_t ld32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static inline int ld16s(const uint8_t *p)
{
    return (int16_t)(p[0] | p[1] << 8);
}

static inline void put_stn(native_state *s, uint32_t a, uint32_t v, unsigned n, uint32_t pc)
{
    s->st[s->nst].a = a;
    s->st[s->nst].v = v;
    s->st[s->nst].pc = pc;
    s->st[s->nst].n = (uint8_t)n;
    s->nst++;
}

static inline void put_st(native_state *s, uint32_t a, uint32_t v, uint32_t pc)
{
    put_stn(s, a, v, 4, pc);
}

/* One of the pass's stores reaches [a, a + n). */
static inline int stored(const native_state *s, uint32_t a, unsigned n)
{
    for (unsigned i = 0; i < s->nst; i++)
        if (s->st[i].a < a + n && a < s->st[i].a + s->st[i].n)
            return 1;
    return 0;
}

/* The word at a, p = ram_ptr(c, a, 4), as the pass has left it so far: a
 * pass made of several stretches reads what an earlier one stored. */
static inline uint32_t ld32_staged(const native_state *s, uint32_t a, const uint8_t *p)
{
    uint32_t v = ld32(p);
    for (unsigned i = 0; i < s->nst; i++) {
        const native_store *w = &s->st[i];
        if (w->a >= a + 4 || a >= w->a + w->n)
            continue;
        for (unsigned k = 0; k < w->n; k++) {
            uint32_t byte = w->a + k - a;
            if (byte < 4)
                v = (v & ~(0xFFu << 8 * byte)) | (w->v >> 8 * k & 0xFF) << 8 * byte;
        }
    }
    return v;
}

static inline int directed_rounding(const native_state *s)
{
    return ((s->cr[CR_FADCR] | s->cr[CR_FMCR]) & (3u << 9 | 3u << 25)) != 0;
}

static inline int same_word(uint32_t a, uint32_t b)
{
    return a - b + 3 < 7;
}

extern const native_kernel native_fir, native_ring_walk, native_interp, native_filter_step,
    native_ring_copy, native_mt_env_a, native_mt_env_b, native_mt_mark, native_peak_scan,
    native_xdj_interp, native_engine, native_fifo_drop, native_mt_extrema, native_slope_flags,
    native_mt_phase_a, native_mt_phase_b, native_fft_scaled, native_fft_plain, native_voc_window, native_voc_frame,
    native_mt_phase_a1, native_mt_phase_b1, native_voc_ratio, native_voc_bytes, native_voc_lock, native_voc_peaks;

#endif
