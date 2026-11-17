/* SPDX-License-Identifier: GPL-2.0-or-later */
/* C66x native kernels: the NXS2 normal-play engine. */
#include "c66x_native_int.h"

/* ------------------------------------------------------------------------ */
/* the normal-play engine's iteration, 0x80010F60 to 0x80011940 (NXS2)      */

/* The first stretch of each of the engine's 16 iterations: it steps the
 * smoothed position and speed globals at B14+876..948 and reduces the phase
 * at B14+920 by B18 in a SPLOOPW. A census of the normal-play recording found
 * three paths only: the reduction runs exactly once (153 cycles, 2,694,303
 * times), it is skipped (137 cycles, 597), or A2 is set at the head (70
 * cycles, 15, left to the interpreter). Every branch the census never took
 * declines, and so does a reduction that would run twice. */

static int engine_head(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;

    if (r[A(2)] || directed_rounding(s))
        return 0;
    uint32_t b14 = r[B(14)], a21 = r[A(21)];
    const uint8_t *g = ram_ptr(c, b14 + 876, 76), *tab = ram_ptr(c, a21, 20), *h450 = ram_ptr(c, b14 + 450, 2);
    if (!g || !tab || !h450)
        return 0;
    for (unsigned i = 0; i < 5; i++)
        if (same_word(a21 + 16, b14 + (uint32_t[]){ 900, 904, 912, 928, 948 }[i]))
            return 0;
    float b20 = u2f(r[B(20)]), b18 = u2f(r[B(18)]);

    r[B(9)] = ld32(g + 20);
    r[A(10)] = ld32(tab);
    r[B(12)] = (int16_t)(g[56] | g[57] << 8);
    float b9 = u2f(r[B(9)]), b5 = b9 - u2f(r[B(6)]);
    float b7 = u2f(0x3B000000) * b5;
    r[B(4)] = b7 > b20;
    r[B(8)] = b7 < u2f(r[B(21)]);
    r[B(0)] = r[B(4)] & r[B(8)];
    r[B(5)] = ld32(g + 52);
    r[B(4)] = ld32(g + 24);
    r[B(7)] = f2u(b7);
    if (r[B(0)]) {
        r[B(7)] = r[B(21)];
        r[B(0)] = 0;
    } else {
        r[A(18)] = 0xB8000001;
        r[A(3)] = u2f(r[A(18)]) < b7;
        r[B(8)] = b7 < b20;
        r[B(0)] = r[B(8)] & r[A(3)];
        if (r[B(0)])
            r[B(7)] = r[A(18)];
    }
    b7 = u2f(r[B(7)]);
    if (u2f(r[B(5)]) != b9)
        return 0;
    float m900 = u2f(r[B(4)]);
    if (b7 > b20 && m900 < b7)
        return 0;
    r[A(3)] = r[B(20)];
    r[B(5)] = b7 < b20;
    r[B(8)] = m900 > b7;
    r[B(0)] = r[B(5)] & r[B(8)];
    if (r[B(0)]) {
        r[B(4)] = r[B(7)];
        put_st(s, b14 + 900, r[B(4)], s->at + 0x428);
        r[A(0)] = 0;
    } else {
        r[A(0)] = u2f(r[A(3)]) == b7;
        if (r[A(0)]) {
            r[B(4)] = r[B(20)];
            put_st(s, b14 + 900, r[B(20)], s->at + 0x432);
        }
    }
    put_st(s, b14 + 928, r[B(9)], s->at + 0x450);
    float b6 = u2f(r[B(4)]) + u2f(r[B(6)]);
    r[B(6)] = f2u(b6);
    put_st(s, b14 + 904, r[B(6)], s->at + 0x460);
    r[B(17)] = b9 == b6;
    b5 = u2f(r[B(4)]) * (b9 - b6);
    r[B(5)] = b5 > b20;
    r[B(0)] = r[B(17)] | r[B(5)];
    if (!r[B(0)]) {
        r[B(6)] = r[B(9)];
        put_st(s, b14 + 904, r[B(6)], s->at + 0x484);
    }
    r[B(0)] = u2f(r[B(6)]) < b20;
    if (!r[B(0)]) {
        r[B(5)] = r[B(6)];
        put_st(s, b14 + 912, r[B(5)], s->at + 0x490);
    } else {
        r[B(4)] = 0x80000000;
        r[B(5)] = r[B(6)] ^ r[B(4)];
        put_st(s, b14 + 912, r[B(5)], s->at + 0x4A0);
    }
    put_st(s, b14 + 948, 0, s->at + 0x4A8);
    r[B(4)] = ld32(g);
    r[A(6)] = r[B(4)];
    r[B(7)] = ld32(g + 44);
    r[B(1)] = r[B(7)];
    r[B(0)] = (int16_t)(g[64] | g[65] << 8);
    r[A(3)] = a21 + 16;
    r[B(8)] = (int16_t)(h450[0] | h450[1] << 8);
    r[A(4)] = 0xFFFFFFFF;
    if (r[B(0)]) {
        r[A(3)] = ld32(tab + 16);
        r[A(0)] = r[A(3)] == r[A(4)];
    } else {
        r[A(0)] = 0;
    }
    r[B(8)] = r[B(8)] == 0;
    r[B(26)] = !r[A(0)];
    r[B(0)] = r[B(26)] & r[B(8)];
    if (!r[B(0)])
        return 0;
    b7 = u2f(r[B(5)]) + u2f(r[B(7)]);
    r[B(7)] = f2u(b7);
    r[B(5)] = b7 == b18;
    uint32_t b6_was = r[B(6)];
    r[B(6)] = r[B(5)] ^ 1;
    r[B(5)] = u2f(b6_was) == b18;
    r[B(0)] = r[B(6)] & r[B(5)];
    r[B(6)] = 0x3D800000;
    b5 = u2f(0x3D800000) * (b7 - b18);
    r[B(5)] = f2u(b5);
    if (r[B(0)]) {
        r[A(3)] = 0x3F800347;
        r[A(0)] = u2f(r[A(3)]) < b7;
        if (r[A(0)])
            r[B(7)] = f2u(b7 - b5);
        else
            r[B(7)] = r[B(18)];
    } else {
        r[A(0)] = 1;
    }
    b7 = u2f(r[B(7)]);
    r[B(0)] = b7 < b18;
    s->pc = s->at + 0x5A0;
    if (r[B(0)]) {
        put_st(s, b14 + 920, r[B(7)], s->at + 0x544);
        s->last_pc = s->at + 0x544;
        s->cycles = 137;
        s->packets = 91;
        s->insns = 105;
        return 1;
    }
    /* One reduction step: the step that clears A1 still has two later
     * iterations in flight, whose unpredicated subtract and adds land. */
    r[A(5)] = r[B(18)];
    float a4 = b7 - b18;
    if (!(a4 < b18))
        return 0;
    r[A(3)] = f2u(a4);
    r[A(4)] = f2u(a4 - b18 - b18);
    r[B(6)] = r[B(27)] + 1;
    r[B(5)] = r[B(27)] + 3;
    r[B(4)] = r[B(4)] + 8;
    r[B(7)] = r[B(4)] + 16;
    r[A(0)] = 1;
    r[A(1)] = 0;
    put_st(s, b14 + 876, r[B(4)], s->at + 0x58C);
    r[B(4)] = r[A(3)];
    r[B(27)] = r[B(6)];
    r[B(18)] = r[A(5)];
    put_st(s, b14 + 920, r[B(4)], s->at + 0x598);
    s->last_pc = s->at + 0x598;
    s->branch_block = 3;
    s->cycles = 153;
    s->packets = 103;
    s->insns = 141;
    s->sploops = 1;
    return 1;
}

/* The second stretch, 0x80011160 to 0x8001144C: a SPLOOP copies 24 sample
 * pairs backwards from the ring at A6 to B24 and A20, the two halves of the
 * 50-word window at A19 (B24 is its last word), then three SPLOOPs of 11
 * taps run the window against the coefficients below B30, one word further
 * along per FIR, in four serial sums each; the first two also scale a centre
 * tap by A22. A ring-bounds check at the top that fails leads to a second
 * copy loop the recordings never run: that declines, and so does a copy
 * whose stores land on its own loads or on the coefficients. 264 cycles,
 * from a packet trace of the interpreter. */

static inline int apart(uint32_t x, uint32_t n, uint32_t y, uint32_t m)
{
    return x - y >= m && y - x >= n;
}

static int engine_fir(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;

    if (directed_rounding(s))
        return 0;
    uint32_t a19 = r[A(19)], a20 = r[A(20)], b24 = r[B(24)], b30 = r[B(30)], a6 = r[A(6)] + 4;
    int32_t lo = (int32_t)(a6 - a19), hi = (int32_t)(a19 - a6);
    int b5 = (int32_t)r[B(31)] < hi || !(lo < (int32_t)r[A(26)]);
    int b6 = -297 < hi || !(lo < (int32_t)r[A(27)]);
    if (!(b5 && b6))
        return 0;
    a6 += 8;
    if (b24 != a19 + 196 || ((a6 | a19 | a20 | b30) & 3))
        return 0;
    const uint8_t *ring = ram_ptr(c, a6 - 196, 192), *coef = ram_ptr(c, b30 - 44, 44);
    const uint8_t *win = ram_ptr(c, a19, 200);
    if (!ring || !coef || !win || !ram_ptr(c, a20 - 92, 96))
        return 0;
    if (!apart(b24 - 92, 96, a6 - 196, 192) || !apart(a20 - 92, 96, a6 - 196, 192)
        || !apart(b24 - 92, 96, b30 - 44, 44) || !apart(a20 - 92, 96, b30 - 44, 44)
        || !apart(b24 - 92, 96, a20 - 92, 96))
        return 0;
    if (stored(s, a6 - 196, 192) || stored(s, b30 - 44, 44) || stored(s, a19, 200))
        return 0;

    float w[50], cf[11];
    memcpy(w, win, sizeof w);
    memcpy(cf, coef, sizeof cf);
    for (unsigned i = 0; i < 24; i++) {
        uint32_t hi_v = ld32(ring + 188 - 8 * i), lo_v = ld32(ring + 184 - 8 * i);
        put_st(s, b24 - 4 * i, hi_v, s->at + 0x64C);
        put_st(s, a20 - 4 * i, lo_v, s->at + 0x64C);
        w[49 - i] = u2f(hi_v);
        if (a20 - 4 * i - a19 < 200)
            w[(a20 - 4 * i - a19) / 4] = u2f(lo_v);
    }

    float b20 = u2f(r[B(20)]), a22 = u2f(r[A(22)]);
    float sa, sb, sc, sd, pa, pb, pd;
    for (unsigned j = 0; j < 3; j++) {
        sa = sb = sc = sd = b20;
        for (unsigned k = 0; k < 11; k++) {
            pa = w[1 + j + k] * cf[k];
            pb = w[26 + j + k] * cf[k];
            pd = cf[k] * w[22 + j - k];
            sa = pa + sa;
            sb = pb + sb;
            sc = cf[k] * w[47 + j - k] + sc;
            sd = pd + sd;
        }
        if (j == 0) {
            r[A(16)] = f2u(sd + sa);
            r[B(13)] = f2u(sc + sb);
            r[B(2)] = f2u(w[12] * a22);
            r[A(28)] = f2u(a22 * w[37]);
        } else if (j == 1) {
            r[A(9)] = f2u(sd + sa);
            r[B(10)] = f2u(sc + sb);
            r[B(11)] = f2u(w[13] * a22);
            r[A(29)] = f2u(a22 * w[38]);
        }
    }
    r[A(3)] = f2u(pb);
    r[A(4)] = f2u(pa);
    r[A(5)] = a19 + 156;
    r[A(6)] = f2u(sa);
    r[A(7)] = f2u(sb);
    r[A(8)] = a19 + 56;
    r[A(30)] = f2u(sc);
    r[A(31)] = f2u(sb);
    r[B(0)] = b30 - 44;
    r[B(3)] = b24 - 104;
    r[B(4)] = f2u(pd);
    r[B(5)] = f2u(sc);
    r[B(6)] = b24 - 44;
    r[B(7)] = b24 - 144;
    r[B(8)] = b30;
    r[B(9)] = f2u(sd);
    r[B(16)] = f2u(cf[10]);
    r[B(17)] = f2u(w[14]);
    s->cr[CR_ILC] = 0;
    s->pc = s->at + 0x88C;
    s->last_pc = s->at + 0x886;
    s->cycles = 264;
    s->packets = 80;
    s->insns = 666;
    s->branch_block = 0;
    s->sploops = 4;
    return 1;
}

/* The third stretch, 0x8001144C to 0x800114D8: unless B1 has reached A23,
 * interp (at the start of this code) runs twice at x = 2 B1, once over each
 * FIR's outputs, and the results go to *B19 and *B22.
 * The other branch, taken 254 times in the normal-play recording, declines. */

static int engine_interp(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;

    if (!(u2f(r[B(1)]) < u2f(r[A(23)])) || directed_rounding(s))
        return 0;
    uint32_t b11 = r[B(11)], a9 = r[A(9)], b13 = r[B(13)], b10 = r[B(10)];
    uint32_t a28 = r[A(28)], a29 = r[A(29)], b19 = r[B(19)], b22 = r[B(22)];
    r[B(0)] = 1;
    r[B(7)] = f2u(u2f(r[B(1)]) + u2f(r[B(1)]));
    r[A(4)] = r[B(7)];
    r[A(6)] = r[B(2)];
    r[B(4)] = r[A(16)];
    r[A(8)] = b11;
    r[B(6)] = a9;
    r[B(3)] = s->at + 0x900;
    native_interp.pass(c, s);
    put_st(s, b19, r[A(4)], s->at + 0x900);
    uint32_t cycles = s->cycles, packets = s->packets, insns = s->insns;
    r[A(4)] = r[B(7)];
    r[B(4)] = b13;
    r[A(6)] = a28;
    r[B(6)] = b10;
    r[A(8)] = a29;
    r[B(3)] = s->at + 0x914;
    native_interp.pass(c, s);
    put_st(s, b22, r[A(4)], s->at + 0x914);
    s->pc = s->at + 0x918;
    s->last_pc = s->at + 0x914;
    s->cycles += cycles + 22;
    s->packets += packets + 12;
    s->insns += insns + 18;
    s->branch_block = 4;
    return 1;
}

/* The fourth stretch, 0x800114D8 to 0x80011624: x at B14+912 is copied to
 * B14+908 and shaped into the level target at B14+924 (B18 from x = B18 up,
 * x (x^2 + 3 (B18 - x)) down to 0.2835, 18.165 x^2 (B18 - 2 x) below), and
 * the level (B14+916 holds it doubled, A23 halves it) moves a sixteenth of
 * the way to it, snapping when the step is under 2^-18, clamped to
 * [B20, B18]. The branches taken when B12 is 1, A10 is set or A2 is set
 * (none, none and 15 times in the normal-play recording) decline. */

static int engine_level(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;

    if (r[B(12)] == 1 || r[A(10)] || r[A(2)] || directed_rounding(s))
        return 0;
    uint32_t b14 = r[B(14)];
    const uint8_t *g = ram_ptr(c, b14 + 896, 32);
    if (!g)
        return 0;
    uint32_t xw = ld32_staged(s, b14 + 912, g + 16);
    float b18 = u2f(r[B(18)]), b20 = u2f(r[B(20)]), x = u2f(xw);
    put_st(s, b14 + 908, xw, s->at + 0x920);
    float sq = x * x, target;
    if (!(x < b18)) {
        r[B(23)] = r[B(18)];
        put_st(s, b14 + 924, r[B(18)], s->at + 0x940);
    } else if (!(u2f(0x3E9124ED) > x)) {
        target = x * (sq + 3.0f * (b18 - x));
        r[B(23)] = f2u(target);
        put_st(s, b14 + 924, r[B(23)], s->at + 0x97A);
    } else {
        target = (b18 - (x + x)) * (u2f(0x4191523B) * sq);
        r[B(23)] = f2u(target);
        put_st(s, b14 + 924, r[B(23)], s->at + 0x9A8);
    }
    float level = u2f(ld32_staged(s, b14 + 916, g + 20)) * u2f(r[A(23)]);
    float step = 0.0625f * (u2f(r[B(23)]) - level);
    float next = step + level;
    r[A(3)] = u2f(0x36800000) > step && step > b20;
    r[B(6)] = step > b20;
    r[B(7)] = step > u2f(0xB6800000);
    r[B(8)] = step < b20;
    r[B(5)] = r[B(8)] & r[B(7)];
    r[A(0)] = r[A(3)] | r[B(5)];
    s->cycles = 100;
    s->packets = 59;
    s->insns = 70;
    if (r[A(0)]) {
        next = u2f(r[B(23)]);
        s->cycles += 1;
        s->packets += 1;
        s->insns += 1;
    }
    if (next > b18) {
        next = b18;
        r[B(0)] = 0;
    } else {
        r[B(0)] = next < b20;
        if (r[B(0)])
            next = b20;
    }
    r[B(4)] = f2u(next + next);
    put_st(s, b14 + 916, r[B(4)], s->at + 0xA30);
    s->pc = s->at + 0xA64;
    s->last_pc = s->at + 0xA30;
    s->branch_block = 4;
    return 1;
}

/* The fifth stretch, 0x80011624 to 0x80011840: the outputs at *B19 and *B22
 * are scaled by the doubled level (B4, then the word at B14+916), and the
 * two filter_step calls get their coefficients on the stack from x at
 * B14+912. Only x at or above B18 and A23 (and above B20) runs here; the
 * three other ranges of x, taken 254, 253 and 398 times in the normal-play
 * recording, decline. */

static int engine_coef(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;

    if (directed_rounding(s))
        return 0;
    uint32_t b14 = r[B(14)], b15 = r[B(15)], b19 = r[B(19)], b22 = r[B(22)];
    const uint8_t *g = ram_ptr(c, b14 + 912, 8), *o19 = ram_ptr(c, b19, 4), *o22 = ram_ptr(c, b22, 4);
    if (!g || !o19 || !o22 || !ram_ptr(c, b15 + 8, 48) || same_word(b19, b22) || same_word(b19, b14 + 916))
        return 0;
    float x = u2f(ld32_staged(s, b14 + 912, g)), b18 = u2f(r[B(18)]), a23 = u2f(r[A(23)]);
    if (x < b18 || a23 > x || !(x > u2f(r[B(20)])))
        return 0;
    put_st(s, b19, f2u(u2f(r[B(4)]) * u2f(ld32_staged(s, b19, o19))), s->at + 0xA74);
    put_st(s, b22, f2u(u2f(ld32_staged(s, b14 + 916, g + 4)) * u2f(ld32_staged(s, b22, o22))), s->at + 0xA8C);
    float a5 = a23 * (b18 + b18), a3 = a23 * (b18 + 1.0f);
    put_st(s, b15 + 8, r[B(18)], s->at + 0xACC);
    put_st(s, b15 + 12, r[B(18)], s->at + 0xACC);
    put_st(s, b15 + 32, 0xBF800000, s->at + 0xAD0);
    put_st(s, b15 + 36, 0xBF800000, s->at + 0xAD2);
    put_st(s, b15 + 16, f2u(a5), s->at + 0xAEA);
    put_st(s, b15 + 40, f2u(a3), s->at + 0xAFA);
    put_st(s, b15 + 20, f2u(a5), s->at + 0xB04);
    put_st(s, b15 + 24, f2u(a5), s->at + 0xB04);
    put_st(s, b15 + 28, f2u(a5), s->at + 0xB0A);
    put_st(s, b15 + 44, f2u(a3) ^ 0x80000000, s->at + 0xB18);
    put_st(s, b15 + 48, f2u(a3), s->at + 0xB18);
    put_st(s, b15 + 52, f2u(a3) ^ 0x80000000, s->at + 0xB26);
    r[A(0)] = 1;
    r[A(3)] = f2u(a3);
    r[A(4)] = f2u(a5);
    r[A(5)] = f2u(a5);
    r[B(0)] = 1;
    r[B(4)] = f2u(a3) ^ 0x80000000;
    r[B(5)] = f2u(a3);
    r[B(6)] = 44;
    s->pc = s->at + 0xC80;
    s->last_pc = s->at + 0xB26;
    s->branch_block = 4;
    s->cycles = 98;
    s->packets = 51;
    s->insns = 55;
    return 1;
}

/* The sixth and seventh stretches, 0x80011840 to 0x80011874 and on to
 * 0x800118A0: filter_step (0x80 into this code) on the left output and then
 * on the right, from the coefficients on the stack.
 * The second reads what the first stores. */

/* glue: the cycles, packets and instructions of the argument set-up */
static int engine_step_call(c66x_core *c, native_state *s, uint32_t ret, const uint32_t glue[3])
{
    uint32_t at = s->at;
    s->reg[B(3)] = at + ret;
    s->at = at + 0x80;
    int ok = native_filter_step.pass(c, s);
    s->at = at;
    s->pc = at + ret;
    s->cycles += glue[0];
    s->packets += glue[1];
    s->insns += glue[2];
    return ok;
}

static int engine_step_l(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, b15 = r[B(15)];

    r[A(17)] = b15 + 120;
    r[B(17)] = b15 + 24;
    r[B(16)] = b15 + 12;
    r[B(8)] = b15 + 24;
    r[B(10)] = b15 + 120;
    r[A(4)] = r[B(19)];
    r[A(6)] = b15 + 8;
    r[A(10)] = b15 + 12;
    r[B(6)] = b15 + 56;
    r[B(4)] = b15 + 16;
    r[A(12)] = r[A(24)];
    r[A(8)] = r[B(22)];
    r[B(12)] = 1;
    return engine_step_call(c, s, 0xCB4, (const uint32_t[]){ 6, 6, 15 });
}

static int engine_step_r(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, b15 = r[B(15)];

    r[A(16)] = b15 + 248;
    r[A(6)] = b15 + 8;
    r[B(4)] = b15 + 16;
    r[B(6)] = b15 + 184;
    r[A(4)] = b15 + 56;
    r[B(10)] = r[A(16)];
    r[A(8)] = r[A(17)];
    r[B(8)] = r[B(17)];
    r[A(10)] = r[B(16)];
    r[A(12)] = r[A(25)];
    return engine_step_call(c, s, 0xCE0, (const uint32_t[]){ 9, 4, 11 });
}

/* The last stretch, 0x800118A0 to the back branch: the filtered pair goes
 * out through *B19++ and *B22++ and B25 is chosen for the word the engine
 * stores at B14+948 on its way out. x at B14+912 below A23 runs a third
 * filter_step first (653 times in the normal-play recording): that
 * declines. */

static int engine_tail(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;
    uint32_t b14 = r[B(14)], b15 = r[B(15)], b19 = r[B(19)], b22 = r[B(22)];
    const uint8_t *x = ram_ptr(c, b14 + 912, 4), *lp = ram_ptr(c, b15 + 184, 4), *rp = ram_ptr(c, b15 + 248, 4);
    const uint8_t *g904 = ram_ptr(c, b14 + 904, 4), *g464 = ram_ptr(c, b14 + 464, 4);
    if (!x || !lp || !rp || !g904 || !g464 || !ram_ptr(c, b19, 4) || !ram_ptr(c, b22, 4))
        return 0;
    if (same_word(b19, b15 + 248) || same_word(b19, b14 + 904) || same_word(b22, b14 + 904)
        || same_word(b19, b14 + 464) || same_word(b22, b14 + 464))
        return 0;
    r[A(3)] = ld32_staged(s, b14 + 912, x);
    if (u2f(r[A(3)]) < u2f(r[A(23)]))
        return 0;
    r[A(0)] = 0;
    put_st(s, b19, ld32_staged(s, b15 + 184, lp), s->at + 0xD38);
    r[B(4)] = ld32_staged(s, b15 + 248, rp);
    put_st(s, b22, r[B(4)], s->at + 0xD48);
    r[B(19)] = b19 + 4;
    r[B(22)] = b22 + 4;
    r[B(6)] = ld32_staged(s, b14 + 904, g904);
    r[B(0)] = u2f(r[B(6)]) > u2f(r[B(20)]);
    if (r[B(0)]) {
        r[B(25)] = 0;
        r[B(1)] = 0;
        r[B(2)] = r[B(12)];
        if (!r[B(2)])
            r[B(25)] = r[B(12)];
    } else {
        r[B(29)] = ld32_staged(s, b14 + 464, g464);
        r[B(1)] = u2f(r[B(6)]) == u2f(r[B(20)]);
        r[B(2)] = r[B(1)];
        if (r[B(1)])
            r[B(25)] = r[B(29)];
        else
            r[B(25)] = r[B(12)];
    }
    r[B(0)] = r[B(19)] < r[B(28)];
    s->pc = s->at + (r[B(0)] ? 0x3A0 : 0xD84);
    s->last_pc = s->at + 0xD80;
    s->cycles = 58;
    s->packets = 25;
    s->insns = 27;
    return 1;
}

/* One iteration of the engine, 0x80010F60 back to itself or out at
 * 0x80011944, as the eight stretches above in turn. A stretch that declines
 * ends the pass where it starts, and the interpreter runs the rest. The
 * cycles, packets and instructions each stretch charges are the
 * interpreter's own on that path, and the float constants are the code's
 * MVK/MVKH literals, kept as bit patterns. */

static int (*const engine_stretches[])(c66x_core *, native_state *) = {
    engine_head, engine_fir, engine_interp, engine_level,
    engine_coef, engine_step_l, engine_step_r, engine_tail,
};

static int engine_pass(c66x_core *c, native_state *s)
{
    native_state t = *s;
    unsigned i = 0;
    for (; i < sizeof engine_stretches / sizeof engine_stretches[0]; i++) {
        native_state u = t;
        u.cycles = u.packets = u.insns = u.sploops = 0;
        u.branch_block = 5;
        if (!engine_stretches[i](c, &u))
            break;
        u.cycles += t.cycles;
        u.packets += t.packets;
        u.insns += t.insns;
        u.sploops += t.sploops;
        t = u;
    }
    if (!i)
        return 0;
    *s = t;
    return 1;
}

/* the engine 0x80010DB4 to its loop's back branch, with interp and
 * filter_step in front of it */
static const native_wild engine_wild[] = { };

const native_kernel native_engine = KERNEL("engine", 0x00006000, 0xDA0, 0x92ed26044817bc35ull, 0x3A0, 0xD84, engine_wild, engine_pass);
