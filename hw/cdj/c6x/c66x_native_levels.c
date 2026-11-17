/* SPDX-License-Identifier: GPL-2.0-or-later */
/* C66x native kernels: NXS2 level and extrema analysis. */
#include "c66x_native_int.h"

/* ------------------------------------------------------------------------ */
/* MASTER TEMPO level envelope, one sample per pass (NXS2: 0x80045174)      */

/* Two twin loops (one per channel pair) of 650 iterations each, software
 * pipelined over two stages. Each pass forms the energy x of one sample pair
 * read from both ends of a 2048-frame stereo ring, ((a + b) / 2)^2 +
 * ((c - d) / 2)^2, seeds its reciprocal square root with 0x5F3759DF minus half
 * its bits (the bits go through the stack), and finishes the previous
 * sample's three Newton steps. That sample's scaled root replaces an output
 * word, and the difference to the word it replaces goes to a second array,
 * or a register's 0.0 when that array's old word d is negative and the
 * difference lies in [0, -2 d]. The loop runs under DINT; a pass is entered
 * at a packet with nothing in flight, so both stages' live values are plain
 * registers there. */

static inline void put_dp(uint32_t *r, unsigned lo, double d)
{
    uint64_t v;
    memcpy(&v, &d, 8);
    r[lo] = (uint32_t)v;
    r[lo + 1] = (uint32_t)(v >> 32);
}

/* One Newton step of 1/sqrt: y (1.5 - y (y h)) in the loop's own order. */
static inline float rsqrt_step(float y, float yyh, float three_halves)
{
    return y * (three_halves - yyh);
}

/* The first twin's loop head is the packet after its bdec target. */
static int mt_env_a_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, head = s->at + 8;

    if (directed_rounding(s) || r[A(1)] || (int32_t)r[B(0)] <= 0)
        return 0;
    const uint8_t *rb = ram_ptr(c, r[B(9)], 4), *lb = ram_ptr(c, r[B(8)], 4);
    const uint8_t *front = ram_ptr(c, r[A(18)], 8), *outp = ram_ptr(c, r[A(22)], 4);
    const uint8_t *difp = ram_ptr(c, r[A(21)], 4);
    if (!rb || !lb || !front || !outp || !difp || !ram_ptr(c, r[B(15)] + 4, 8))
        return 0;
    float half = u2f(r[A(16)]), th = u2f(r[B(6)]), h = u2f(r[A(19)]);

    float y1 = rsqrt_step(u2f(r[B(16)]), u2f(r[B(4)]), th);
    float y2 = rsqrt_step(y1, y1 * (y1 * h), th);
    float y3 = rsqrt_step(y2, y2 * (y2 * h), th);

    float sa = half * (u2f(ld32(front)) + u2f(ld32(lb)));
    float sd = (u2f(ld32(front + 4)) - u2f(ld32(rb))) * half;
    float x = sa * sa + sd * sd;
    uint32_t y0 = r[B(7)] - (uint32_t)((int32_t)f2u(x) >> 1);
    float hn = half * x;

    float out = u2f(r[A(23)]) * (u2f(r[A(3)]) * y3);
    float old = u2f(ld32(outp)), dold = u2f(ld32(difp));
    float diff = out - old, floor2 = u2f(r[A(20)]) * dold;
    uint32_t sel = r[A(17)];
    if (diff < 0.0f || !((double)dold < 0.0) || diff > floor2)
        sel = f2u(diff);

    put_st(s, r[B(15)] + 8, f2u(y3), head + 0x7C);
    put_st(s, r[B(15)] + 8, f2u(x), head + 0x80);
    put_st(s, r[B(15)] + 4, y0, head + 0xA6);
    put_st(s, r[A(22)], f2u(out), head + 0xA6);
    put_st(s, r[A(21)], sel, head + 0x100);

    r[A(0)] = diff > floor2;
    r[A(3)] = f2u(x);
    put_dp(r, A(4), (double)dold);
    r[A(6)] = f2u(floor2);
    r[A(7)] = sel;
    put_dp(r, A(8), (double)diff);
    r[A(18)] += 8;
    r[A(19)] = f2u(hn);
    r[A(21)] += 4;
    r[A(22)] += 4;
    r[A(24)] = f2u(diff);
    r[B(0)] -= 1;
    r[B(4)] = f2u(u2f(y0) * (u2f(y0) * hn));
    r[B(5)] = 0;
    r[B(8)] -= 8;
    r[B(9)] -= 8;
    r[B(16)] = y0;
    s->cycles = 60;
    s->packets = 51;
    s->insns = 68;
    s->pc = head;
    s->last_pc = head - 4;
    s->branch_block = 0;
    return 1;
}

/* The second twin's loop head is its bdec target. */
static int mt_env_b_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, head = s->at + 0xC;

    if (directed_rounding(s) || r[A(1)] || (int32_t)r[B(0)] <= 0)
        return 0;
    const uint8_t *lb = ram_ptr(c, r[B(18)], 4), *rb = ram_ptr(c, r[B(19)], 4);
    const uint8_t *front = ram_ptr(c, r[A(19)] + 8, 8), *outp = ram_ptr(c, r[A(17)], 4);
    const uint8_t *difp = ram_ptr(c, r[A(16)], 4);
    if (!rb || !lb || !front || !outp || !difp || !ram_ptr(c, r[B(15)] + 12, 8))
        return 0;
    float half = u2f(r[B(20)]), th = u2f(r[B(16)]), h = u2f(r[B(7)]);

    float y1 = rsqrt_step(u2f(r[B(8)]), u2f(r[B(4)]), th);
    float y2 = rsqrt_step(y1, y1 * (h * y1), th);
    float y3 = rsqrt_step(y2, y2 * (h * y2), th);

    float sd = half * (u2f(ld32(lb)) - u2f(ld32(front)));
    float sa = half * (u2f(ld32(rb)) + u2f(ld32(front + 4)));
    float x = sd * sd + sa * sa;
    uint32_t y0 = r[B(17)] - (uint32_t)((int32_t)f2u(x) >> 1);
    float hn = half * x;

    float out = u2f(r[A(18)]) * (y3 * u2f(r[B(6)]));
    float old = u2f(ld32(outp)), dold = u2f(ld32(difp));
    float diff = out - old, floor2 = u2f(r[A(3)]) * dold;
    uint32_t sel = r[A(20)];
    if (diff < 0.0f || !((double)dold < 0.0) || diff > floor2)
        sel = f2u(diff);

    put_st(s, r[B(15)] + 16, f2u(y3), head + 0x80);
    put_st(s, r[B(15)] + 16, f2u(x), head + 0x84);
    put_st(s, r[B(15)] + 12, y0, head + 0xA8);
    put_st(s, r[A(17)], f2u(out), head + 0xB4);
    put_st(s, r[A(16)], sel, head + 0x104);

    r[A(0)] = diff > floor2;
    put_dp(r, A(4), (double)dold);
    r[A(6)] = sel;
    r[A(7)] = f2u(floor2);
    put_dp(r, A(8), (double)diff);
    r[A(16)] += 4;
    r[A(17)] += 4;
    r[A(19)] += 8;
    r[A(21)] = f2u(diff);
    r[B(0)] -= 1;
    r[B(4)] = f2u(u2f(y0) * (hn * u2f(y0)));
    r[B(5)] = 0;
    r[B(6)] = f2u(x);
    r[B(7)] = f2u(hn);
    r[B(8)] = y0;
    r[B(9)] = f2u(hn * u2f(y0));
    r[B(18)] -= 8;
    r[B(19)] -= 8;
    s->cycles = 60;
    s->packets = 50;
    s->insns = 69;
    s->pc = head;
    s->last_pc = head + 0x104;
    s->branch_block = 0;
    return 1;
}

static const native_wild mt_env_wild[] = { };

const native_kernel native_mt_env_a = KERNEL("mt_env_a", 0x008000a8, 0x110, 0x6c7d92b20d46c2bdull, 0x8, 0x110, mt_env_wild, mt_env_a_pass);
const native_kernel native_mt_env_b = KERNEL("mt_env_b", 0x10004001, 0x120, 0xd6307a50e1686276ull, 0xC, 0x120, mt_env_wild, mt_env_b_pass);

/* ------------------------------------------------------------------------ */
/* MASTER TEMPO peak-neighbour marking, one index per pass (NXS2: 0x8004190C) */

/* For i = A3 below 650, two halfword arrays (h1 walked by *++B5, h2 = A10[i])
 * and one flag byte f = A16[i] decide whether i's neighbours get marked: when
 * 0 < h1 < 6, f == 1 and the float A8[i] > 0, or 0 < h2 < 6, f == 1 and the
 * float A9[i] > 0, the loop stores 1 to f[i - 1] and f[i + 1]. Playback never
 * takes those marking paths, so a pass declines at f == 1 and the interpreter
 * runs them; the two paths left (h2 out of range: 45 cycles; h2 in range,
 * f != 1: 52) store nothing. The last index is left to the interpreter too. */

static int mt_mark_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, i = r[A(3)], head = s->at + 0x18;

    if (i + 1 >= 650)
        return 0;
    const uint8_t *h1p = ram_ptr(c, r[B(5)] + 2, 2), *h2p = ram_ptr(c, r[A(10)] + 2 * i, 2);
    const uint8_t *fp = ram_ptr(c, r[A(16)] + i, 1);
    if (!h1p || !h2p || !fp)
        return 0;
    int32_t h1 = (int16_t)(h1p[0] | h1p[1] << 8), h2 = (int16_t)(h2p[0] | h2p[1] << 8);
    int in1 = 0 < h1 && h1 < 6, in2 = 0 < h2 && h2 < 6;
    if ((in1 || in2) && (int8_t)fp[0] == 1)
        return 0;

    if (in1 || in2)
        r[A(4)] = i + r[A(16)];
    r[A(0)] = 1;
    r[A(3)] = i + 1;
    r[A(5)] = 650;
    r[A(6)] = (uint32_t)h2;
    r[B(0)] = in2;
    r[B(1)] = 0;
    r[B(4)] = 6 > h2;
    r[B(5)] += 2;
    r[B(31)] = 0;
    s->cycles = in2 ? 52 : 45;
    s->packets = in2 ? 29 : 26;
    s->insns = in2 ? 36 : 31;
    s->pc = head;
    s->last_pc = head + 0x94;
    return 1;
}

static const native_wild mt_mark_wild[] = { };

const native_kernel native_mt_mark = KERNEL("mt_mark", 0x01942234, 0xC0, 0x72626aaaf20a4d06ull, 0x18, 0xB4, mt_mark_wild, mt_mark_pass);

/* ------------------------------------------------------------------------ */
/* peak scan, one sample per pass (NXS2: 0x80075920)                        */

/* Unoptimised code over a block of 294 floats: the loop keeps its state on
 * the stack, the halfword index at B15+4, the sample at B15+8, the running
 * maximum at B15+12 and minimum at B15+16 (both start at 0). A pass compares
 * the sample with both bounds (cmpltsp / cmpgtsp, so a NaN moves neither),
 * stores the index plus one and loads the next sample from *B20++. The last
 * sample, whose pass falls out of the loop, is left to the interpreter.
 * 44 cycles on every pass. */

static int peak_scan_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, head = s->at + 0xC, b15 = r[B(15)];

    if (!r[A(1)] || !r[A(5)])
        return 0;
    const uint8_t *fr = ram_ptr(c, b15 + 4, 16), *xp = ram_ptr(c, r[B(20)], 4);
    if (!fr || !xp || r[B(20)] - (b15 + 4) + 3 < 19)
        return 0;
    uint32_t x = ld32(fr + 4), hi = ld32(fr + 8), lo = ld32(fr + 12);
    int16_t i = (int16_t)(fr[0] | fr[1] << 8), i1 = (int16_t)(i + 1);
    if (i1 >= (int32_t)r[B(19)])
        return 0;

    r[B(16)] = x;
    r[B(17)] = hi;
    if (u2f(hi) < u2f(x)) {
        r[B(9)] = x;
        put_st(s, b15 + 12, x, head + 0x24);
    }
    r[B(7)] = x;
    r[B(8)] = lo;
    if (u2f(lo) > u2f(x)) {
        r[B(6)] = x;
        put_st(s, b15 + 16, x, head + 0x4C);
    }
    /* sth of the index, written as its whole word: the other half is padding */
    put_st(s, b15 + 4, (uint16_t)i1 | (ld32(fr) & 0xFFFF0000u), head + 0x64);
    r[B(18)] = ld32(xp);
    put_st(s, b15 + 8, r[B(18)], head + 0x86);
    r[A(0)] = r[A(5)];
    r[A(3)] = r[A(4)] = r[A(5)] = r[A(1)];
    r[B(0)] = 1;
    r[B(4)] = (uint32_t)(int32_t)i1;
    r[B(5)] = (uint32_t)(int32_t)i;
    r[B(20)] += 4;
    r[B(21)] = (uint32_t)((int32_t)i + 1);
    s->cycles = 48;
    s->packets = 34;
    s->insns = 37;
    s->pc = head;
    s->last_pc = head + 0x86;
    s->branch_block = 1;
    return 1;
}

static const native_wild peak_scan_wild[] = { };

const native_kernel native_peak_scan = KERNEL("peak_scan", 0x22040fd8, 0xA0, 0xc8c2da2eaedaadd2ull, 0xC, 0x98, peak_scan_wild, peak_scan_pass);

/* ------------------------------------------------------------------------ */
/* MASTER TEMPO extrema, one index per pass (NXS2: loop 0x800434E8 in 0x800433C8) */

/* For i = A4 below 651 the loop compares the pair x0 = p[0], x1 = p[1] at
 * p = B6 (one float further each pass) and tracks the slope's sign in B9.
 * A fall after a rise (B9 >= 0) records i - 1 as peak B5 in the halfword list
 * B31 and counts the halfword A10[i - 1] up from at least 1; a rise after a
 * fall records it as valley B4 in the list at B15 + 8, counts A10[i - 1] down
 * from at most -1, and from the second valley on stores to B11[peak] the
 * peak's height over its two valleys, B10[peak] - 0.5 (B10[v0] + B10[v1]).
 * A repeated direction clears A10[i - 1]; equal or unordered x leave it.
 * The halfword loads run in protected mode, so they read what the stores
 * before them left. A pass whose loads would read one of its own stores
 * (other than the valley it has just recorded) is left to the interpreter.
 * Cycles per path were measured on the interpreter. */

#define EXTREMA_END 651

enum { EX_LEVEL, EX_FALL_AGAIN, EX_PEAK, EX_RISE_AGAIN, EX_VALLEY, EX_VALLEY_HEIGHT };

static int mt_extrema_pass(c66x_core *c, native_state *s)
{
    static const uint8_t cycles[] = { 28, 41, 55, 30, 73, 73 }, packets[] = { 12, 16, 25, 13, 41, 41 };
    static const uint8_t insns[] = { 17, 22, 30, 17, 49, 49 };
    uint32_t *r = s->reg, i = r[A(4)], head = s->at + 8;
    const uint8_t *xp = ram_ptr(c, r[B(6)], 8);

    if (!xp || directed_rounding(s))
        return 0;

    uint32_t x0 = ld32(xp), x1 = ld32(xp + 4), cnt = r[A(10)] + 2 * i - 2;
    int path;
    r[B(16)] = r[B(7)] = x0;
    r[B(17)] = r[B(8)] = x1;
    if (u2f(x1) > u2f(x0) || u2f(x1) < u2f(x0)) {
        const uint8_t *cp = ram_ptr(c, cnt, 2);
        if (!cp)
            return 0;
        int rise = u2f(x1) > u2f(x0), b9 = (int32_t)r[B(9)];
        r[A(5)] = cnt;
        if (rise ? b9 > 0 : b9 < 0) {
            put_stn(s, cnt, 0, 2, s->at + (rise ? 0x10A : 0x72));
            r[B(0)] = 1;
            path = rise ? EX_RISE_AGAIN : EX_FALL_AGAIN;
        } else if (!rise) {
            uint32_t at = r[B(31)] + 2 * r[B(5)];
            if (!ram_ptr(c, at, 2))
                return 0;
            put_stn(s, at, i - 1, 2, s->at + 0x44);
            if (stored(s, cnt, 2))
                return 0;
            int n = ld16s(cp);
            put_stn(s, cnt, n > 0 ? n + 1 : 1, 2, s->at + (n > 0 ? 0x50 : 0x58));
            r[B(0)] = 0;
            r[B(5)] += 1;
            r[B(7)] = i - 1;
            r[B(9)] = -1;
            path = EX_PEAK;
        } else {
            uint32_t k = r[B(4)], vl = r[B(15)] + 8;
            if (!ram_ptr(c, vl + 2 * k, 2))
                return 0;
            put_stn(s, vl + 2 * k, i - 1, 2, s->at + 0x90);
            if (stored(s, cnt, 2))
                return 0;
            int n = ld16s(cp);
            put_stn(s, cnt, (uint32_t)(n < 0 ? n - 1 : -1), 2, s->at + (n < 0 ? 0xA0 : 0xA8));
            r[B(4)] = k + 1;
            r[B(7)] = 2 * k;
            r[B(8)] = vl - 4 + 2 * k;
            r[B(9)] = vl - 2 + 2 * k;
            r[B(0)] = (int32_t)(k + 1) < 2;
            path = EX_VALLEY;
            if (!r[B(0)]) {
                const uint8_t *v0p = ram_ptr(c, vl + 2 * k - 2, 2), *pkp = ram_ptr(c, r[B(31)] + 2 * k, 2);
                if (!v0p || !pkp || stored(s, vl + 2 * k - 2, 2) || stored(s, r[B(31)] + 2 * k, 2))
                    return 0;
                int v0 = ld16s(v0p), v1 = (int16_t)(i - 1), pk = ld16s(pkp);
                uint32_t b10 = r[B(10)], dst = r[B(11)] + 4 * pk;
                const uint8_t *y0 = ram_ptr(c, b10 + 4 * v0, 4), *y1 = ram_ptr(c, b10 + 4 * v1, 4);
                const uint8_t *yp = ram_ptr(c, b10 + 4 * pk, 4);
                if (!y0 || !y1 || !yp || !ram_ptr(c, dst, 4) || stored(s, b10 + 4 * v0, 4)
                    || stored(s, b10 + 4 * v1, 4) || stored(s, b10 + 4 * pk, 4))
                    return 0;
                float sum = u2f(ld32(y1)) + u2f(ld32(y0));
                float h = u2f(ld32(yp)) - 0.5f * sum;
                put_st(s, dst, f2u(h), s->at + 0xFC);
                r[B(7)] = f2u(h);
                r[B(8)] = ld32(yp);
                r[B(16)] = f2u(sum);
                path = EX_VALLEY_HEIGHT;
            }
            r[B(9)] = 1;
        }
    } else {
        r[B(0)] = 0;
        path = EX_LEVEL;
    }
    r[A(3)] = EXTREMA_END;
    r[A(4)] = i + 1;
    r[A(0)] = (int32_t)(i + 1) < EXTREMA_END;
    r[B(6)] += 4;
    s->cycles = cycles[path];
    s->packets = packets[path];
    s->insns = insns[path];

    s->pc = r[A(0)] ? head : s->at + 0x120;
    s->last_pc = s->at + 0x118;
    return 1;
}

static const native_wild mt_extrema_wild[] = { };

const native_kernel native_mt_extrema = KERNEL("mt_extrema", 0x82c7a487, 0x120, 0xa49cf32e0e99590cull, 0x8, 0x120, mt_extrema_wild, mt_extrema_pass);

/* ------------------------------------------------------------------------ */
/* slope flags, one call per pass (NXS2: 0x8003FEEC)                        */

/* The function's SPLOOP (ii 4) walks 650 indices: for x = B8 and h = A10 + 2
 * it forms d = x[i + 1] - x[i], passes d's bits through the stack, and
 * stores to the byte array at the MVK'd address first p = ((2 - h[i]) >> 14)
 * & 2, then p | the sign of d, so the second store decides each byte. The
 * loop is entered with nothing in flight; the pass runs from the function's
 * push of A12 to the first load after the epilog, which takes in the stb of 1
 * to A12 + 1. The bytes are staged as whole words, the edge words keeping
 * their other bytes. */

#define SLOPE_N 650

static int slope_flags_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, sp = r[B(15)], a12 = r[A(12)], flags = s->addr[0];
    uint32_t x = r[B(8)], h = r[A(10)] + 2, w0 = flags & ~3u, w1 = (flags + SLOPE_N + 3) & ~3u;
    const uint8_t *xp = ram_ptr(c, x, 4 * (SLOPE_N + 1)), *hp = ram_ptr(c, h, 2 * SLOPE_N);
    const uint8_t *fp = ram_ptr(c, w0, w1 - w0);

    if (!xp || !hp || !fp || !ram_ptr(c, sp - 4, 8) || !ram_ptr(c, a12 + 1, 1) || directed_rounding(s))
        return 0;
    /* the loads must not see the pass's own stores */
    uint32_t lo[] = { x, h }, len[] = { 4 * (SLOPE_N + 1), 2 * SLOPE_N };
    for (int i = 0; i < 2; i++)
        if ((lo[i] < sp + 4 && sp - 4 < lo[i] + len[i]) || (lo[i] < w1 && w0 < lo[i] + len[i]))
            return 0;
    if ((sp - 4 < w1 && w0 < sp + 4) || a12 + 1 - w0 < w1 - w0)
        return 0;

    uint8_t out[SLOPE_N + 8];
    memcpy(out, fp, w1 - w0);
    uint32_t d = 0, p = 0;
    for (unsigned i = 0; i < SLOPE_N; i++) {
        d = f2u(u2f(ld32(xp + 4 * i + 4)) - u2f(ld32(xp + 4 * i)));
        p = (uint32_t)((2 - ld16s(hp + 2 * i)) >> 14);
        out[flags - w0 + i] = (uint8_t)((p & 2) | d >> 31);
    }

    put_st(s, sp, a12, s->at + 0xC);
    for (uint32_t a = w0; a < w1; a += 4)
        put_st(s, a, ld32(out + (a - w0)), s->at + 0x90);
    put_st(s, sp - 4, d, s->at + 0x66);
    put_stn(s, a12 + 1, 1, 1, s->at + 0xB8);

    r[A(16)] = r[A(8)];
    r[A(17)] = r[B(6)];
    r[A(18)] = r[B(4)];
    r[A(19)] = 0;
    r[A(20)] = r[A(4)];
    r[A(3)] = (p & 2) | d >> 31;
    r[A(4)] = d;
    r[A(5)] = p;
    r[A(6)] = 1;
    r[A(7)] = flags + SLOPE_N;
    r[A(8)] = x + 4 * (SLOPE_N + 1);
    r[A(9)] = 1;
    r[A(12)] = a12 + 1;
    r[B(4)] = s->addr[1];
    r[B(5)] = flags + SLOPE_N;
    r[B(6)] = s->addr[1] - 3;
    r[B(7)] = flags - 3;
    r[B(8)] = s->addr[1];
    r[B(9)] = ld32(xp + 4 * SLOPE_N);
    r[B(15)] = sp - 8;
    r[B(16)] = flags;
    s->cr[CR_ILC] = 0;
    s->sploops = 1;
    s->pc = s->at + 0xC0;
    s->last_pc = s->at + 0xB8;
    s->branch_block = 0;
    s->cycles = 4 * SLOPE_N + 34;
    s->packets = 34;
    s->insns = 14 * SLOPE_N + 38;
    return 1;
}

static const native_wild slope_flags_wild[] = { ADDR_LO(0x10, 0), ADDR_HI(0x14, 0), ADDR_LO(0xA8, 1), ADDR_HI(0xAC, 1) };

const native_kernel native_slope_flags = KERNEL("slope_flags", 0x02111079, 0xC0, 0x26c285f4bc67e71full, 0xC, 0xC0, slope_flags_wild, slope_flags_pass);
