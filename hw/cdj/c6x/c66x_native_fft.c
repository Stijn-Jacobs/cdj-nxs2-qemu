/* SPDX-License-Identifier: GPL-2.0-or-later */
/* C66x native kernels: the NXS2 MASTER TEMPO vocoder's 2048-point FFTs. */
#include "c66x_native_int.h"

#ifdef __SSE2__
#include <emmintrin.h>
#endif

/* ------------------------------------------------------------------------ */
/* complex FFT, one call per pass (NXS2: 0x00804AE0 scaled by 1/N, and      */
/* 0x00805F40 unscaled)                                                     */

/* Both are TI DSPLIB's C66x radix-4 FFT, fft(N A4, x B4, w A6, y B6, ...):
 * radix-4 stages on x in place while the stride n / 4 is above one, each a
 * SPLOOP of two butterflies a pass with its own six twiddles per butterfly,
 * then one radix-2 stage that writes y in digit-reversed order (BITR). The
 * two routines differ only in the sign of the quarter turn in the butterfly
 * and in the 1 / N (RCPSP) the first applies in its last stage.
 *
 * Every product and sum is the one the code forms, CMPYSP as its four
 * products combined by DADDSP. Only N = 2048 is handled: the vocoder never
 * passes anything else, and the stage count, the cycles and the registers the
 * routine leaves are those of that size, read off the interpreter. */

#define FFT_N 2048
#define FFT_SHIFT 22                /* NORM(N) + 3: BITR of a pass count, shifted down to the output word */
#define FFT_TW_BYTES (6 * (FFT_N + FFT_N / 4 + FFT_N / 16 + FFT_N / 64 + FFT_N / 256))   /* 6 n bytes a stage */
#define FFT_FRAME 72                /* the routine's stack frame below the caller's B15 */

typedef struct fft_variant {
    int scaled;
    uint32_t cycles, packets, insns;
    uint32_t last_pc;               /* from the start of the code */
} fft_variant;

static inline void cmul(float *o, const float *a, const float *w)
{
    float r0 = a[0] * w[1], r1 = -(a[0] * w[0]), r2 = a[1] * w[0], r3 = a[1] * w[1];
    o[0] = r2 + r0;
    o[1] = r3 + r1;
}

/* The quarter-turn terms of a butterfly: v1 multiplies the first twiddle pair
 * and v2 the third. The scaled routine turns one way, the other the other. */
static inline void quarter(float *v1, float *v2, const float *d02, const float *d13, int scaled)
{
    float p_lo = d02[0] - d13[1], p_hi = d02[1] + d13[0];
    float m_lo = d02[0] + d13[1], m_hi = d02[1] - d13[0];
    v1[0] = scaled ? p_lo : m_lo;
    v1[1] = scaled ? p_hi : m_hi;
    v2[0] = scaled ? m_lo : p_lo;
    v2[1] = scaled ? m_hi : p_hi;
}

typedef struct butterfly {
    float s02[2], d02[2], s13[2], d13[2], t0[2], v1[2], v2[2];
    float out[4][2];
} butterfly;

static inline void radix4(butterfly *b, const float *x0, const float *x1, const float *x2, const float *x3,
                          const float *tw, int scaled)
{
    for (int k = 0; k < 2; k++) {
        b->s02[k] = x0[k] + x2[k];
        b->d02[k] = x0[k] - x2[k];
        b->s13[k] = x1[k] + x3[k];
        b->d13[k] = x1[k] - x3[k];
    }
    for (int k = 0; k < 2; k++) {
        b->t0[k] = b->s02[k] - b->s13[k];
        b->out[0][k] = b->s02[k] + b->s13[k];
    }
    quarter(b->v1, b->v2, b->d02, b->d13, scaled);
    cmul(b->out[1], b->t0, tw + 2);
    cmul(b->out[2], b->v1, tw);
    cmul(b->out[3], b->v2, tw + 4);
}

#ifdef __SSE2__
/* Two butterflies at once, j and j + 1, lane for lane the scalar expressions:
 * the same operations on the same operands in the same order. */

static inline __m128 swap_pairs(__m128 a)
{
    return _mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 3, 0, 1));
}

static inline __m128 cmul2(__m128 a, __m128 w)
{
    const __m128 neg_hi = _mm_castsi128_ps(_mm_set_epi32(INT32_MIN, 0, INT32_MIN, 0));
    __m128 lo = _mm_shuffle_ps(a, a, _MM_SHUFFLE(2, 2, 0, 0)), hi = _mm_shuffle_ps(a, a, _MM_SHUFFLE(3, 3, 1, 1));
    __m128 r0 = _mm_mul_ps(lo, swap_pairs(w)), r2 = _mm_mul_ps(hi, w);
    return _mm_add_ps(r2, _mm_xor_ps(r0, neg_hi));
}

/* the twiddle pair at word k of the sets of butterflies j and j + 1 */
static inline __m128 twiddles2(const float *tw, unsigned k)
{
    __m128 lo = _mm_castsi128_ps(_mm_loadl_epi64((const __m128i *)(tw + k)));
    return _mm_loadh_pi(lo, (const __m64 *)(tw + k + 6));
}

static void radix4_stage(float *v, const float *tw, unsigned n, int scaled)
{
    const __m128 even = _mm_castsi128_ps(_mm_set_epi32(0, -1, 0, -1));
    unsigned s = n / 4;
    for (unsigned i = 0; i < FFT_N / 4; i += 2) {
        unsigned j = i % s;
        float *x = v + 2 * ((i / s) * n + j);
        __m128 x0 = _mm_loadu_ps(x), x1 = _mm_loadu_ps(x + 2 * s);
        __m128 x2 = _mm_loadu_ps(x + 4 * s), x3 = _mm_loadu_ps(x + 6 * s);
        __m128 s02 = _mm_add_ps(x0, x2), d02 = _mm_sub_ps(x0, x2);
        __m128 s13 = _mm_add_ps(x1, x3), d13 = _mm_sub_ps(x1, x3);
        __m128 t0 = _mm_sub_ps(s02, s13), out0 = _mm_add_ps(s02, s13);
        __m128 sum = _mm_add_ps(d02, swap_pairs(d13)), diff = _mm_sub_ps(d02, swap_pairs(d13));
        __m128 p = _mm_or_ps(_mm_and_ps(even, diff), _mm_andnot_ps(even, sum));
        __m128 m = _mm_or_ps(_mm_and_ps(even, sum), _mm_andnot_ps(even, diff));
        _mm_storeu_ps(x, out0);
        _mm_storeu_ps(x + 2 * s, cmul2(t0, twiddles2(tw, 6 * j + 2)));
        _mm_storeu_ps(x + 4 * s, cmul2(scaled ? p : m, twiddles2(tw, 6 * j)));
        _mm_storeu_ps(x + 6 * s, cmul2(scaled ? m : p, twiddles2(tw, 6 * j + 4)));
    }
}
#else
static void radix4_stage(float *v, const float *tw, unsigned n, int scaled)
{
    unsigned s = n / 4;
    for (unsigned i = 0; i < FFT_N / 4; i++) {
        unsigned j = i % s;
        float *x = v + 2 * ((i / s) * n + j);
        butterfly b;
        radix4(&b, x, x + 2 * s, x + 4 * s, x + 6 * s, tw + 6 * j, scaled);
        for (int q = 0; q < 4; q++) {
            x[2 * q * s] = b.out[q][0];
            x[2 * q * s + 1] = b.out[q][1];
        }
    }
}
#endif

static inline uint32_t bitr(uint32_t v)
{
    v = (v >> 1 & 0x55555555u) | (v & 0x55555555u) << 1;
    v = (v >> 2 & 0x33333333u) | (v & 0x33333333u) << 2;
    v = (v >> 4 & 0x0F0F0F0Fu) | (v & 0x0F0F0F0Fu) << 4;
    v = (v >> 8 & 0x00FF00FFu) | (v & 0x00FF00FFu) << 8;
    return v >> 16 | v << 16;
}

typedef struct pair_sums {
    float s01[2], s23[2], d01[2], d23[2];
} pair_sums;

/* Four consecutive points. The upper half's sums take their operands the
 * other way round, as the code does (it shows only in a NaN's payload). */
static inline void radix2(pair_sums *p, const float *x, int upper)
{
    for (int k = 0; k < 2; k++) {
        p->s01[k] = upper ? x[2 + k] + x[k] : x[k] + x[2 + k];
        p->s23[k] = upper ? x[6 + k] + x[4 + k] : x[4 + k] + x[6 + k];
        p->d01[k] = x[k] - x[2 + k];
        p->d23[k] = x[4 + k] - x[6 + k];
    }
}

/* Each pass takes four points from each half of v and puts every result at
 * the pass count's digit reversal in y, a quarter of y apart. */
#ifdef __SSE2__
static inline void radix2_out(float *y, uint32_t rev, __m128 sums, __m128 diffs)
{
    _mm_storel_pi((__m64 *)(y + rev), sums);
    _mm_storeh_pi((__m64 *)(y + rev + FFT_N / 2), sums);
    _mm_storel_pi((__m64 *)(y + rev + FFT_N), diffs);
    _mm_storeh_pi((__m64 *)(y + rev + 3 * FFT_N / 2), diffs);
}

static void radix2_stage(float *y, const float *v, float scale, int scaled)
{
    const unsigned passes = FFT_N / 8;
    const __m128 sc = _mm_set1_ps(scale);
    for (unsigned i = 0; i < passes; i++) {
        const float *a = v + 8 * i, *b = a + FFT_N;
        __m128 a01 = _mm_loadu_ps(a), a23 = _mm_loadu_ps(a + 4), b01 = _mm_loadu_ps(b), b23 = _mm_loadu_ps(b + 4);
        __m128 a02 = _mm_movelh_ps(a01, a23), a13 = _mm_movehl_ps(a23, a01);
        __m128 b02 = _mm_movelh_ps(b01, b23), b13 = _mm_movehl_ps(b23, b01);
        __m128 sa = _mm_add_ps(a02, a13), da = _mm_sub_ps(a02, a13);
        __m128 sb = _mm_add_ps(b13, b02), db = _mm_sub_ps(b02, b13);
        if (scaled) {
            sa = _mm_mul_ps(sa, sc);
            da = _mm_mul_ps(da, sc);
            sb = _mm_mul_ps(sb, sc);
            db = _mm_mul_ps(db, sc);
        }
        radix2_out(y, bitr(i) >> FFT_SHIFT, sa, da);
        radix2_out(y, bitr(passes + i) >> FFT_SHIFT, sb, db);
    }
}
#else
static inline void radix2_out(float *y, uint32_t rev, const pair_sums *p, float scale, int scaled)
{
    const float *in[4] = { p->s01, p->s23, p->d01, p->d23 };
    for (int q = 0; q < 4; q++)
        for (int k = 0; k < 2; k++)
            y[rev + q * FFT_N / 2 + k] = scaled ? in[q][k] * scale : in[q][k];
}

static void radix2_stage(float *y, const float *v, float scale, int scaled)
{
    const unsigned passes = FFT_N / 8;
    for (unsigned i = 0; i < passes; i++) {
        pair_sums pa, pb;
        radix2(&pa, v + 8 * i, 0);
        radix2(&pb, v + FFT_N + 8 * i, 1);
        radix2_out(y, bitr(i) >> FFT_SHIFT, &pa, scale, scaled);
        radix2_out(y, bitr(passes + i) >> FFT_SHIFT, &pb, scale, scaled);
    }
}
#endif

static inline uint32_t pair_lo(const float *v) { return f2u(v[0]); }
static inline uint32_t pair_hi(const float *v) { return f2u(v[1]); }

static int fft_pass(c66x_core *c, native_state *s, const fft_variant *fv)
{
    uint32_t *r = s->reg;
    uint32_t x = r[B(4)], w = r[A(6)], y = r[B(6)], sp = r[B(15)];
    const uint32_t bytes = 8 * FFT_N;

    if (directed_rounding(s) || r[A(4)] != FFT_N || ((x | w | y | sp) & 7))
        return 0;
    const uint8_t *px = ram_ptr(c, x, bytes), *pw = ram_ptr(c, w, FFT_TW_BYTES);
    if (!px || !pw || !ram_ptr(c, y, bytes) || !ram_ptr(c, sp - FFT_FRAME, FFT_FRAME + 4))
        return 0;
    /* each span read or written apart from the others, as the code assumes */
    const uint32_t lo[4] = { x, y, w, sp - FFT_FRAME }, len[4] = { bytes, bytes, FFT_TW_BYTES, FFT_FRAME + 4 };
    for (int i = 0; i < 4; i++)
        for (int k = i + 1; k < 4; k++)
            if (lo[i] - lo[k] < len[k] || lo[k] - lo[i] < len[i])
                return 0;

    float *v = (float *)s->bulk_buf, *out = v + 2 * FFT_N, *tw = out + 2 * FFT_N;
    memcpy(v, px, bytes);
    memcpy(tw, pw, FFT_TW_BYTES);

    /* the stack frame: the callee-saved registers, then w, N and the twiddle offset */
    uint32_t pc = s->at;
    put_st(s, sp, r[A(11)], pc);
    put_st(s, sp - 8, r[A(10)], pc + 2);
    put_st(s, sp - 16, r[B(12)], pc + 4);
    put_st(s, sp - 12, r[B(13)], pc + 4);
    put_st(s, sp - 24, r[B(10)], pc + 6);
    put_st(s, sp - 20, r[B(11)], pc + 6);
    put_st(s, sp - 32, r[A(14)], pc + 8);
    put_st(s, sp - 28, r[A(15)], pc + 8);
    put_st(s, sp - 40, r[A(12)], pc + 0xA);
    put_st(s, sp - 36, r[A(13)], pc + 0xA);
    put_st(s, sp - 48, r[B(3)], pc + 0xC);
    put_st(s, sp - 68, w, pc + 0x10);
    put_st(s, sp - 64, FFT_N, pc + 0x12);

    unsigned off = 0, n = FFT_N;
    float last[8][2];
    for (; n > 4; n /= 4) {
        if (n == 8)
            memcpy(last, v + 2 * (FFT_N - 8), sizeof last);
        radix4_stage(v, tw + off / 4, n, fv->scaled);
        off += 6 * n;
    }
    put_st(s, sp - 60, off, s->at + 0x4A);

    /* the last pass of the last radix-4 stage: butterflies on points N-8.. and N-7.. */
    butterfly ba, bb;
    const float *tw8 = tw + (off - 6 * 8) / 4;
    radix4(&ba, last[0], last[2], last[4], last[6], tw8, fv->scaled);
    radix4(&bb, last[1], last[3], last[5], last[7], tw8 + 6, fv->scaled);

    float scale = 1.0f / (float)FFT_N;
    const unsigned h = FFT_N / 2, passes = FFT_N / 8;
    radix2_stage(out, v, scale, fv->scaled);
    pair_sums pa, pb;
    radix2(&pa, v + 8 * (passes - 1), 0);
    radix2(&pb, v + 2 * h + 8 * (passes - 1), 1);

    s->bulk[0] = (native_bulk){ x, bytes, 0, 0 };
    s->bulk[1] = (native_bulk){ y, bytes, bytes, 0 };
    s->nbulk = 2;

    /* the registers the last radix-2 pass (points 4 (N/8 - 1).. and N/2 + 4 (N/8 - 1)..) leaves */
    const float *xa = v + 8 * (passes - 1), *xb = v + 2 * h + 8 * (passes - 1);
    uint32_t rev_a = bitr(passes - 1) >> FFT_SHIFT, rev_b = bitr(2 * passes - 1) >> FFT_SHIFT;
    uint32_t end_a = y + 4 * rev_a + 3 * bytes / 4, end_b = y + 4 * rev_b + 3 * bytes / 4;
    uint32_t walked = 32 * passes;
    if (fv->scaled) {
        float sa01[2], sa23[2], da01[2], da23[2], sb01[2], sb23[2], db01[2], db23[2];
        for (int k = 0; k < 2; k++) {
            sa01[k] = pa.s01[k] * scale;
            sa23[k] = pa.s23[k] * scale;
            da01[k] = pa.d01[k] * scale;
            da23[k] = pa.d23[k] * scale;
            sb01[k] = pb.s01[k] * scale;
            sb23[k] = pb.s23[k] * scale;
            db01[k] = pb.d01[k] * scale;
            db23[k] = pb.d23[k] * scale;
        }
        float cm[2];
        cmul(cm, bb.t0, tw8 + 6 + 2);
        r[A(0)] = x + walked;
        r[A(1)] = end_a;
        r[A(2)] = bitr(passes - 1);
        r[A(3)] = x + 4 * FFT_N + 16 + walked;
        r[A(4)] = r[B(4)] = pair_lo(xb);
        r[A(5)] = r[B(5)] = pair_hi(xb);
        r[A(6)] = rev_a;
        r[A(7)] = pair_hi(pa.d23);
        r[A(8)] = pair_lo(sa01);
        r[A(9)] = pair_hi(sa01);
        r[A(16)] = pair_lo(xb + 4);
        r[A(17)] = pair_hi(xb + 4);
        r[A(18)] = pair_lo(da23);
        r[A(19)] = pair_hi(da23);
        r[A(20)] = pair_lo(da01);
        r[A(21)] = pair_hi(da01);
        r[A(22)] = pair_lo(sa23);
        r[A(23)] = pair_hi(sa23);
        r[A(24)] = r[A(25)] = f2u(scale);
        r[A(26)] = x + 16 + walked;
        r[A(27)] = y;
        r[A(28)] = passes;
        r[A(29)] = FFT_SHIFT;
        r[A(30)] = FFT_N / 4;
        r[A(31)] = x + 4 * FFT_N + walked;
        r[B(0)] = 1;
        r[B(1)] = 2;
        r[B(2)] = f2u(cm[0]);
        r[B(6)] = pair_lo(sb01);
        r[B(7)] = pair_hi(sb01);
        r[B(8)] = pair_lo(db01);
        r[B(9)] = pair_hi(db01);
        r[B(16)] = pair_lo(pb.d01);
        r[B(17)] = pair_hi(pb.d01);
        r[B(18)] = x + 4 * FFT_N + 24 + walked;
        r[B(19)] = x + 24 + walked;
        r[B(20)] = pair_lo(sb23);
        r[B(21)] = pair_hi(sb23);
        r[B(22)] = pair_lo(db23);
        r[B(23)] = pair_hi(db23);
        r[B(24)] = FFT_SHIFT;
        r[B(25)] = 2 * passes;
        r[B(26)] = FFT_N / 4;
        r[B(27)] = x + 4 * FFT_N + 8 + walked;
        r[B(28)] = x + 8 + walked;
        r[B(29)] = y;
        r[B(30)] = end_b;
        r[B(31)] = rev_b;
    } else {
        r[A(0)] = 1;
        r[A(1)] = x;
        r[A(2)] = x - 16 + 8 * FFT_N;
        r[A(3)] = end_a;
        r[A(4)] = pair_lo(pa.s01);
        r[A(5)] = pair_hi(pa.s01);
        r[A(6)] = x + 4 * FFT_N + 16 + walked;
        r[A(7)] = x + 16 + walked;
        r[A(8)] = pair_lo(pa.d01);
        r[A(9)] = pair_hi(pa.d01);
        r[A(16)] = pair_lo(pa.d23);
        r[A(17)] = pair_hi(pa.d23);
        r[A(18)] = pair_lo(xa + 4);
        r[A(19)] = pair_hi(xa + 4);
        r[A(20)] = pair_lo(pa.s23);
        r[A(21)] = pair_hi(pa.s23);
        r[A(22)] = y;
        r[A(23)] = passes;
        r[A(24)] = FFT_SHIFT;
        r[A(25)] = FFT_N / 4;
        r[A(26)] = x + 4 * FFT_N + walked;
        r[A(27)] = x + walked;
        r[A(28)] = w + off - 6 * 8;
        r[A(29)] = 2;
        r[A(30)] = f2u(ba.s02[1]);
        r[A(31)] = f2u(ba.d13[1]);
        r[B(0)] = 0;
        r[B(1)] = FFT_N / 4;
        r[B(2)] = x + 4 * FFT_N + 8;
        r[B(4)] = pair_lo(pb.d01);
        r[B(5)] = pair_hi(pb.d01);
        r[B(6)] = end_b;
        r[B(7)] = x + 4 * FFT_N + 24 + walked;
        r[B(8)] = pair_lo(xa + 2);
        r[B(9)] = pair_hi(xa + 2);
        r[B(16)] = pair_lo(xb + 2);
        r[B(17)] = pair_hi(xb + 2);
        r[B(18)] = pair_lo(pb.s23);
        r[B(19)] = pair_hi(pb.s23);
        r[B(20)] = pair_lo(xa + 6);
        r[B(21)] = pair_hi(xa + 6);
        r[B(22)] = pair_lo(pb.d23);
        r[B(23)] = pair_hi(pb.d23);
        r[B(24)] = x + 24 + walked;
        r[B(25)] = FFT_SHIFT;
        r[B(26)] = 2 * passes;
        r[B(27)] = FFT_N / 4;
        r[B(28)] = x + 4 * FFT_N + 8 + walked;
        r[B(29)] = x + 8 + walked;
        r[B(30)] = y;
        r[B(31)] = f2u(bb.v2[1]);
    }
    s->cr[CR_ILC] = 0;
    s->cr[CR_RILC] = FFT_N / 8;
    s->pc = r[B(3)];
    s->last_pc = s->at + fv->last_pc;
    s->branch_block = 3;            /* the return's BNOP, then two packets of its delay slots */
    s->cycles = fv->cycles;
    s->packets = fv->packets;
    s->insns = fv->insns;
    s->sploops = 6;
    return 1;
}

static const fft_variant fft_scaled = { 1, 16399, 333, 115222, 0x930 };
static const fft_variant fft_plain = { 0, 16391, 319, 113168, 0x474 };

static int fft_scaled_pass(c66x_core *c, native_state *s)
{
    return fft_pass(c, s, &fft_scaled);
}

static int fft_plain_pass(c66x_core *c, native_state *s)
{
    return fft_pass(c, s, &fft_plain);
}

static const native_wild fft_wild[] = { };

const native_kernel native_fft_scaled = KERNEL("fft_scaled", 0x257725f7, 0x940, 0xaeeaf038891535c9ull, 0, RETURNS, fft_wild, fft_scaled_pass);
const native_kernel native_fft_plain = KERNEL("fft_plain", 0x257725f7, 0x480, 0x78d52fdf2f5d8713ull, 0, RETURNS, fft_wild, fft_plain_pass);
