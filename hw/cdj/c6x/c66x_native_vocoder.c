/* SPDX-License-Identifier: GPL-2.0-or-later */
/* C66x native kernels: whole stages of the NXS2 MASTER TEMPO vocoder. */
#include "c66x_native_int.h"

/* ------------------------------------------------------------------------ */
/* window(y A4, w B4) (NXS2 0x8003D828): the real parts of the 2048 complex */
/* samples of y times a 2048-point window built from the 512 words of w     */

/* The four quarters are w, 1 - w backwards, 1 - w and w backwards. Each
 * quarter first compares the distance between y and w with a few KB and picks
 * one of two loop forms by it. Buffers far apart, as the vocoder's are, take
 * a two-wide SPLOOP in the first quarter and plain loops in the other three;
 * the cycles and registers are those of that path only. */

#define WIN_N 2048
#define WIN_W 512

static int window_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;
    uint32_t y = r[A(4)], w = r[B(4)];
    const uint32_t bytes = 8 * WIN_N;

    if (directed_rounding(s) || ((y | w) & 7))
        return 0;
    /* far enough apart that every overlap test the code makes fails */
    const uint8_t *py = ram_ptr(c, y, bytes), *pw = ram_ptr(c, w, 4 * WIN_W);
    if (!py || !pw || w - y < 0x10000 || y - w < 0x10000)
        return 0;

    float *v = (float *)s->bulk_buf;
    memcpy(v, py, bytes);
    float win[WIN_W];
    memcpy(win, pw, sizeof win);
    const float one = 1.0f;
    for (unsigned k = 0; k < WIN_W; k++)
        v[2 * k] = v[2 * k] * win[k];
    for (unsigned k = 0; k < WIN_W; k++)
        v[2 * (WIN_W + k)] = (one - win[WIN_W - 1 - k]) * v[2 * (WIN_W + k)];
    for (unsigned k = 0; k < WIN_W; k++)
        v[2 * (2 * WIN_W + k)] = (one - win[k]) * v[2 * (2 * WIN_W + k)];
    for (unsigned k = 0; k < WIN_W; k++)
        v[2 * (3 * WIN_W + k)] = win[WIN_W - 1 - k] * v[2 * (3 * WIN_W + k)];
    s->bulk[0] = (native_bulk){ y, bytes, 0, 0 };
    s->nbulk = 1;

    /* what the last quarter's last pass and each quarter's setup leave */
    r[A(0)] = 0;
    r[B(0)] = 0;
    r[A(3)] = y + bytes - 16;
    r[A(4)] = ld32(py + bytes - 8);
    r[A(5)] = w + 4 * WIN_W - 8;
    r[A(6)] = w;
    r[A(7)] = -6145;
    r[A(8)] = y + 8 + 16 * WIN_W / 2;
    r[A(9)] = w;
    r[A(31)] = w - (y + 3 * bytes / 4);
    r[B(4)] = f2u(v[2 * (WIN_N - 1)]);
    r[B(5)] = w - 8;
    r[B(6)] = w + 4 * WIN_W - 8;
    r[B(7)] = w + 4 * WIN_W - 4;

    s->pc = r[B(3)];
    s->last_pc = s->at + 0x344;
    s->branch_block = 5;            /* the return's BNOP has all five delay slots to itself */
    s->cycles = 19272;
    s->packets = 12594;
    s->insns = 15469;
    s->sploops = 1;
    return 1;
}

static const native_wild window_wild[] = { };

const native_kernel native_voc_window = KERNEL("voc_window", 0x10006000, 0x360, 0x4b8e3d8354026362ull, 8, RETURNS, window_wild, window_pass);

/* ------------------------------------------------------------------------ */
/* frame window(x A4, w B4) (NXS2 0x8003D120): the 2048 complex samples of  */
/* x times a symmetric window, sample k by w[k] and sample 2047 - k by w[k] */
/* for the 1024 words of w                                                   */

/* As in the stage above, each half picks its loop form by the distance
 * between x and w; buffers far apart take the first half's SPLOOP and the
 * second half's plain loop. */

#define FRAME_W 1024

static int frame_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;
    uint32_t x = r[A(4)], w = r[B(4)];
    const uint32_t bytes = 8 * WIN_N;

    if (directed_rounding(s) || ((x | w) & 7))
        return 0;
    const uint8_t *px = ram_ptr(c, x, bytes), *pw = ram_ptr(c, w, 4 * FRAME_W);
    if (!px || !pw || w - x < 0x10000 || x - w < 0x10000)
        return 0;

    float *v = (float *)s->bulk_buf;
    memcpy(v, px, bytes);
    float win[FRAME_W];
    memcpy(win, pw, sizeof win);
    for (unsigned k = 0; k < FRAME_W; k++) {
        v[2 * k] = win[k] * v[2 * k];
        v[2 * k + 1] = win[k] * v[2 * k + 1];
    }
    for (unsigned k = 0; k < FRAME_W; k++) {
        float *p = v + 2 * (WIN_N - 1 - k);
        p[0] = win[k] * p[0];
        p[1] = win[k] * p[1];
    }
    s->bulk[0] = (native_bulk){ x, bytes, 0, 0 };
    s->nbulk = 1;

    /* the first half's last SPLOOP iteration (samples 1022, 1023), then the
     * second half's drained pipeline (samples 2046, 2047, by w[1] and w[0]) */
    const float *in = (const float *)px;
    r[A(0)] = 0;
    r[A(3)] = w - 8;
    r[A(4)] = f2u(v[2 * (WIN_N - 1)]);
    r[A(5)] = f2u(v[2 * (WIN_N - 1) + 1]);
    r[A(6)] = f2u(win[0]);
    r[A(7)] = f2u(v[2 * (WIN_N - 2) + 1]);
    r[A(8)] = x + bytes;
    r[A(9)] = f2u(win[1]);
    r[A(16)] = w;
    r[A(31)] = w - (x + bytes / 2);
    r[B(0)] = 0;
    r[B(4)] = FRAME_W;
    r[B(5)] = w + 4 * FRAME_W - 4;
    r[B(6)] = f2u(v[2 * (FRAME_W - 2)]);
    r[B(7)] = f2u(v[2 * (FRAME_W - 2) + 1]);
    r[B(8)] = w + 4 * FRAME_W;
    r[B(9)] = x + bytes / 2;
    r[B(16)] = f2u(in[2 * (FRAME_W - 2)]);
    r[B(17)] = f2u(in[2 * (FRAME_W - 2) + 1]);

    s->pc = r[B(3)];
    s->last_pc = s->at + 0x1E8;
    s->branch_block = 5;
    s->cycles = 13892;
    s->packets = 6704;
    s->insns = 12359;
    s->sploops = 1;
    return 1;
}

static const native_wild frame_wild[] = { };

const native_kernel native_voc_frame = KERNEL("voc_frame", 0xe24e1a46, 0x200, 0x08e12f989636ed99ull, 0, RETURNS, frame_wild, frame_pass);

/* ------------------------------------------------------------------------ */
/* The two loops of 0x80043610 (NXS2): a ratio per bin at 0x80043708, then  */
/* the bins marked into a byte map at 0x80043D00                            */

/* Both are scheduled by hand, not as SPLOOPs, and entered with results in
 * flight, a load's among them. A pass is written per cycle of the schedule
 * like the bin phase loops: each value reads the registers or in-flight
 * results it reads on the core. */

static inline uint32_t addsp(uint32_t a, uint32_t b) { return f2u(u2f(a) + u2f(b)); }
static inline uint32_t subsp(uint32_t a, uint32_t b) { return f2u(u2f(a) - u2f(b)); }
static inline uint32_t mpysp(uint32_t a, uint32_t b) { return f2u(u2f(a) * u2f(b)); }
static inline int fgt(uint32_t a, uint32_t b) { return u2f(a) > u2f(b); }

/* The ratio loop, 10 packets. Each bin's d = x - A10 * y, of a word x of the
 * B23 array and a word y of the A26 array, gets a seed of 1 / d from five
 * thresholds and lines on d and three Newton steps; y times the last one is
 * stored at A25 (A27 when x equals A27) once B1 has counted down the passes
 * that fill the pipeline. */
enum { VR_A5, VR_B4, VR_B10, VR_B7, VR_A3, VR_B2, VR_B22, VR_A20, VR_A8, VR_B4_LATE, VR_N };

static const native_pend ratio_pend[] = {
    { A(5), 0, 0 }, { B(4), 0, 0 }, { B(10), 0, 0 }, { B(7), 1, 0 }, { A(3), 1, 0 }, { B(2), 1, 0 },
    { B(22), 1, 1 }, { A(20), 2, 0, 1 }, { A(8), 2, 0 }, { B(4), 2, 0 },
};

static int ratio_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, *p = s->pend, in[VR_N];
    const uint8_t *px = ram_ptr(c, r[B(23)], 4), *py = ram_ptr(c, r[A(26)], 4);

    if (!px || !py || !ram_ptr(c, r[A(25)], 4) || directed_rounding(s))
        return 0;
    memcpy(in, p, sizeof in);

    /* the newest bin's d and the comparisons its seed's lines will need */
    uint32_t d = subsp(r[B(7)], r[A(5)]);
    int over_a30 = fgt(d, r[A(30)]), over_a31 = fgt(d, r[A(31)]);

    /* the last bin's seed, from the lines of earlier passes */
    int over_a21 = fgt(r[A(23)], r[A(21)]), over_a28 = fgt(r[A(23)], r[A(28)]);
    int over_a22 = fgt(r[A(23)], r[A(22)]);
    uint32_t b22 = r[A(1)] ? subsp(r[B(11)], in[VR_A5]) : in[VR_B22];
    uint32_t b21 = r[A(3)] ? b22 : subsp(r[B(28)], r[B(16)]);
    uint32_t b20 = over_a28 ? b21 : subsp(r[B(27)], in[VR_A3]);
    uint32_t b19 = over_a22 ? b20 : subsp(r[B(26)], r[B(8)]);
    uint32_t seed = over_a21 ? b19 : subsp(r[B(25)], in[VR_B4]);

    /* Newton steps of older bins and the stored ratio */
    uint32_t b3x2 = addsp(r[B(3)], r[B(3)]);
    uint32_t newton = subsp(b3x2, mpysp(r[B(3)], mpysp(r[B(3)], r[B(6)])));
    uint32_t a6 = mpysp(mpysp(r[A(4)], in[VR_B10]), in[VR_B10]);
    uint32_t ratio = r[B(0)] ? r[A(24)] : mpysp(r[A(19)], subsp(r[B(17)], r[A(6)]));
    int x_is_a27 = u2f(in[VR_B7]) == u2f(r[A(27)]);
    uint32_t b1 = r[B(1)] - (r[B(1)] != 0);
    int take = r[A(2)] && (int32_t)r[A(2)] >= 0;

    if (!b1)
        put_st(s, r[A(25)], ratio, s->at + 0xE4);

    p[VR_A5] = mpysp(r[A(11)], d);
    p[VR_B4] = mpysp(r[B(9)], d);
    p[VR_B10] = newton;
    p[VR_B7] = r[B(6)];
    p[VR_A3] = mpysp(r[A(29)], d);
    p[VR_B2] = addsp(seed, seed);
    p[VR_B22] = over_a31 ? b22 : subsp(r[B(30)], mpysp(r[B(31)], d));
    p[VR_A20] = ld32(py);
    p[VR_A8] = r[A(17)];
    p[VR_B4_LATE] = mpysp(seed, r[B(8)]);
    s->pend_live = ((1u << VR_N) - 1) & ~(over_a31 << VR_B22);

    r[A(0)] = over_a21;
    r[A(1)] = over_a31;
    r[A(2)] -= take;
    r[A(3)] = over_a30;
    r[A(4)] = r[A(7)];
    r[A(5)] = mpysp(r[A(10)], in[VR_A20]);
    r[A(6)] = a6;
    r[A(7)] = r[A(16)];
    r[A(16)] = r[A(23)];
    r[A(9)] = r[A(17)];
    r[A(17)] = r[A(20)];
    r[A(19)] = r[A(18)];
    r[A(18)] = r[A(8)];
    r[A(8)] = in[VR_A8];
    r[A(20)] = in[VR_A20];
    r[A(23)] = d;
    r[A(24)] = x_is_a27 ? r[A(27)] : ratio;
    r[A(25)] += b1 ? 0 : 4;
    r[A(26)] += 4;
    r[B(0)] = x_is_a27;
    r[B(1)] = b1;
    r[B(3)] = subsp(in[VR_B2], mpysp(r[B(24)], in[VR_B4_LATE]));
    r[B(2)] = in[VR_B2];
    r[B(4)] = r[B(5)];
    r[B(6)] = r[B(5)];
    r[B(5)] = r[B(8)];
    r[B(7)] = ld32(px);
    r[B(8)] = d;
    r[B(10)] = in[VR_B10];
    r[B(16)] = mpysp(r[B(29)], d);
    r[B(17)] = addsp(in[VR_B10], in[VR_B10]);
    r[B(18)] = b3x2;
    r[B(19)] = b19;
    r[B(20)] = b20;
    r[B(21)] = b21;
    r[B(22)] = b22;
    r[B(23)] += 4;
    r[B(24)] = seed;
    s->cycles = 10;
    s->packets = 10;
    s->insns = 62;
    s->pc = take ? s->at + 8 : s->at + 0x100;
    s->last_pc = s->at + 0xE4;
    s->branch_block = 0;
    return 1;
}

static const native_wild ratio_wild[] = { };

const native_kernel native_voc_ratio = KERNEL_PEND("voc_ratio", 0x0127a059, 0x100, 0x9b6d229d747dc6e0ull, 8, 0x100,
                                                   ratio_wild, ratio_pass, ratio_pend);

/* The byte map loop, 11 packets: a bin counter A5 (up to B19, against A16
 * and 6) and float positions from it mark bytes of a map around A17 and A19
 * with A18 or A20; while B2 holds it also walks three word arrays (B20, B21,
 * B22) and stores to a fourth (B28). Its stores come before some of its loads
 * in the same pass, so the loads read what the pass has stored so far. */
enum { VB_B6, VB_A4, VB_B1, VB_B27, VB_A4_LATE, VB_N };

static const native_pend bytes_pend[] = {
    { B(6), 0, 0 }, { A(4), 0, 0 }, { B(1), 0, 0 }, { B(27), 1, 1, 1 }, { A(4), 1, 0 },
};

static inline uint32_t intsp(uint32_t v) { return f2u((float)(int32_t)v); }

/* *v = the n bytes at a as the pass has left them, zero-extended; 0 when a
 * is not RAM */
static int staged_load(c66x_core *c, const native_state *s, uint32_t a, unsigned n, uint32_t *v)
{
    const uint8_t *m = ram_ptr(c, a, n);
    if (!m)
        return 0;
    uint8_t b[4] = { 0 };
    memcpy(b, m, n);
    for (unsigned i = 0; i < s->nst; i++)
        for (unsigned k = 0; k < s->st[i].n; k++)
            if (s->st[i].a + k - a < n)
                b[s->st[i].a + k - a] = (uint8_t)(s->st[i].v >> 8 * k);
    *v = ld32(b);
    return 1;
}

static int bytes_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, *p = s->pend, in[VB_N], at = s->at;
    uint32_t a17 = r[A(17)], a19 = r[A(19)], mark = r[A(18)], mark2 = r[A(20)];
    int walk = r[B(2)] != 0;

    if (!ram_ptr(c, a19, 2) || !ram_ptr(c, a17 - 2, 3) || (r[B(0)] && !ram_ptr(c, r[B(28)], 4))
        || directed_rounding(s))
        return 0;
    memcpy(in, p, sizeof in);

    /* cycles 0-2 */
    if (!r[B(1)])
        put_stn(s, a19, mark, 1, at);
    if (r[B(0)])
        put_st(s, r[B(28)], r[B(23)], at);
    uint32_t b24 = addsp(r[B(23)], r[B(24)]), b11b29 = mpysp(r[B(11)], r[B(29)]);
    int over_b30 = fgt(r[B(29)], r[B(30)]), below_a16 = (int32_t)r[A(5)] < (int32_t)r[A(16)];
    if (!in[VB_B1])
        put_stn(s, a17 - 2, mark, 1, at + 0x1C);
    int more = (int32_t)r[B(19)] > (int32_t)r[A(5)];
    uint32_t a3 = addsp(in[VB_A4], r[A(21)]), dist = r[A(8)] - r[A(5)];
    uint32_t b8 = walk ? over_b30 : 1, b9 = b8;
    if (walk) {
        a19++;
        put_stn(s, a19, mark2, 1, at + 0x34);
    }
    uint32_t pos = intsp(r[A(5)]), pos_dist = intsp(dist);

    /* cycles 3-5 */
    uint32_t b20 = r[B(20)], b23 = r[B(23)];
    if (walk && !staged_load(c, s, b20 += 4, 4, &b23))
        return 0;
    uint32_t b7 = walk ? u2f(in[VB_B27]) < u2f(r[B(10)]) : 1, a2 = walk ? r[A(2)] : 0;
    uint32_t b1 = r[B(0)] ? u2f(r[B(23)]) < u2f(r[A(3)]) : 0;
    uint32_t a0 = r[B(0)] ? u2f(in[VB_A4_LATE]) < u2f(r[B(16)]) : 1;
    uint32_t b5 = addsp(in[VB_B6], r[B(5)]), b16 = subsp(in[VB_B27], b11b29);
    if (a2)
        put_stn(s, a19, mark, 1, at + 0x64);
    uint32_t a3_scaled = mpysp(r[A(9)], a3);
    uint32_t b25 = r[A(4)] ? addsp(in[VB_B27], r[B(25)]) : r[B(25)];
    if (!a0)
        put_stn(s, a17 - 2, mark, 1, at + 0x84);
    uint32_t b18 = walk ? b25 : r[B(18)];
    uint32_t b2 = more ? r[B(2)] : 0;
    int walk_on = b2 != 0;

    /* cycles 6-10 */
    uint32_t b22 = r[B(22)], b29 = r[B(29)], b21 = r[B(21)], b27 = in[VB_B27];
    if (walk_on && !staged_load(c, s, b22 += 4, 4, &b29))
        return 0;
    if (b1)
        put_stn(s, a17 - 2, mark, 1, at + 0x94);
    a17++;
    uint32_t b17 = r[B(0)] ? b24 : r[B(17)];
    uint32_t a1 = walk_on ? below_a16 : 0;
    if (walk_on)
        put_stn(s, a17 - 1, mark2, 1, at + 0xA8);
    uint32_t b23_out = a2 ? r[B(30)] : b23;
    uint32_t b0 = r[B(4)] ? u2f(b16) > u2f(r[B(3)]) : 1;
    if (walk_on && !staged_load(c, s, b21 += 4, 4, &b27))
        return 0;
    if (a1)
        put_stn(s, a17 - 1, mark, 1, at + 0xC0);
    if (!b0) {
        put_stn(s, a19, mark, 1, at + 0xD8);
        b23_out = r[B(30)];
    }
    if (!b8)
        put_stn(s, a17 - 2, mark, 1, at + 0xF8);
    if (!b9)
        b23_out = r[B(30)];

    p[VB_B6] = mpysp(r[B(26)], pos_dist);
    p[VB_A4] = mpysp(r[A(7)], pos_dist);
    p[VB_B1] = b7;
    p[VB_B27] = b27;
    p[VB_A4_LATE] = mpysp(r[A(9)], b5);
    s->pend_live = ((1u << VB_N) - 1) & ~(!walk_on << VB_B27);

    r[A(0)] = a0;
    r[A(1)] = a1;
    r[A(2)] = 6 > (int32_t)r[A(5)];
    r[A(3)] = a3_scaled;
    r[A(4)] = 6 > (int32_t)r[A(5)];
    r[A(5)]++;
    r[A(17)] = a17;
    r[A(19)] = a19;
    r[A(21)] = mpysp(r[A(6)], pos);
    r[B(28)] += r[B(0)] ? 4 : 0;
    r[B(0)] = r[B(4)];
    r[B(1)] = b9;
    r[B(2)] = b2;
    r[B(4)] = b2;
    r[B(5)] = mpysp(r[B(31)], pos);
    r[B(6)] = in[VB_B6];
    r[B(7)] = b7;
    r[B(8)] = b8;
    r[B(9)] = b9;
    r[B(12)] = more;
    r[B(16)] = b16;
    r[B(17)] = b17;
    r[B(18)] = b18;
    r[B(20)] = b20;
    r[B(21)] = b21;
    r[B(22)] = b22;
    r[B(23)] = b23_out;
    r[B(24)] = b24;
    r[B(25)] = b25;
    r[B(27)] = in[VB_B27];
    r[B(29)] = b29;
    s->cycles = 11;
    s->packets = 11;
    s->insns = 65;
    s->pc = walk ? at : at + 0x104;
    s->last_pc = at + 0xF8;
    s->branch_block = 0;
    return 1;
}

static const native_wild bytes_wild[] = { };

const native_kernel native_voc_bytes = KERNEL_PEND("voc_bytes", 0x594c0235, 0x104, 0x66e101dba65bd73eull, 0, 0x104,
                                                   bytes_wild, bytes_pass, bytes_pend);

/* ------------------------------------------------------------------------ */
/* The phase lock loop of 0x8003FEEC (NXS2) at 0x80040284                    */

/* One pass per bin i (A6): a byte A9 to ++A12, then the bin's class *++A8
 * and the flag bytes B7[0..6] around the bin pick a path. Bins of class 3
 * and up keep their phase; a positive class, or a positive byte at A18 + i,
 * marks A12 with A19; the others take phase A16[i] = A17[i] + A16[j] - A17[j]
 * from a neighbour j between i - 3 and i + 3, the last path from i - 3 and
 * i + 3 under two more flag tests that can also mark A12. Each path waits out
 * its own loads and float results, so nothing is in flight at the head and a
 * path's cost is fixed: the costs below are its blocks' packets, cycles and
 * instructions. */

static inline void cost(native_state *s, unsigned packets, unsigned cycles, unsigned insns)
{
    s->packets += packets;
    s->cycles += cycles;
    s->insns += insns;
}

/* *v = the n-byte signed value at a as the pass has left it */
static int staged_load_s(c66x_core *c, const native_state *s, uint32_t a, unsigned n, uint32_t *v)
{
    if (!staged_load(c, s, a, n, v))
        return 0;
    *v = n == 1 ? (uint32_t)(int8_t)*v : (uint32_t)(int16_t)*v;
    return 1;
}

/* A16[i] = A17[i] + A16[i + d] - A17[i + d] as the core's two loops over a
 * neighbour form it: *sum = the sum, *phase = the stored value */
static int lock_phase(c66x_core *c, native_state *s, int d, uint32_t pc, uint32_t *x, uint32_t *sum, uint32_t *phase)
{
    uint32_t *r = s->reg, at = r[A(16)] + 4 * r[A(6)], y;
    if (!staged_load(c, s, r[A(17)] + 4 * r[A(6)], 4, x) || !staged_load(c, s, at + 4 * d, 4, sum)
        || !staged_load(c, s, r[A(17)] + 4 * r[A(6)] + 4 * d, 4, &y) || !ram_ptr(c, at, 4))
        return 0;
    *sum = addsp(*x, *sum);
    *phase = subsp(*sum, y);
    put_st(s, at, *phase, pc);
    return 1;
}

/* The bin's path, up to the loop's own counting at 0x800404C8 */
static int lock_bin(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, at = s->at, i = r[A(6)], b7 = r[B(7)], a4i = 4 * i;

    if (directed_rounding(s) || !ram_ptr(c, r[A(12)] + 1, 1))
        return 0;
    r[A(12)]++;
    put_stn(s, r[A(12)], r[A(9)], 1, at + 0x4);
    r[A(8)] += 2;
    if (!staged_load_s(c, s, r[A(8)], 2, &r[A(3)]))
        return 0;
    r[A(0)] = 3 > (int32_t)r[A(3)];
    cost(s, 4, 13, 4);
    if (!r[A(0)])
        return 1;

    int mark = 1;
    r[A(0)] = 0 < (int32_t)r[A(3)];
    cost(s, 3, 7, 4);
    if (!r[A(0)]) {
        if (!staged_load_s(c, s, i + r[A(18)], 1, &r[A(3)]))
            return 0;
        r[A(0)] = 0 < (int32_t)r[A(3)];
        cost(s, 1, 6, 1);
        mark = r[A(0)];
    }
    if (mark) {
        if (!ram_ptr(c, r[A(12)], 1))
            return 0;
        put_stn(s, r[A(12)], r[A(19)], 1, at + 0x244);
        cost(s, 1, 1, 1);
        return 1;
    }

    uint32_t f[7];
    for (int k = 0; k < 7; k++)
        if (!staged_load_s(c, s, b7 + k, 1, &f[k]))
            return 0;
    r[B(4)] = f[2];
    r[B(0)] = 2 & f[2];
    cost(s, 4, 12, 4);
    if (r[B(0)]) {
        r[A(3)] = a4i;
        r[A(4)] = r[A(16)] + a4i;
        if (!staged_load(c, s, r[A(4)] - 4, 4, &r[A(7)]) || !staged_load(c, s, r[A(17)] + a4i - 4, 4, &r[A(4)])
            || !staged_load(c, s, r[A(17)] + a4i, 4, &r[A(5)]) || !ram_ptr(c, r[A(16)] + a4i, 4))
            return 0;
        r[A(3)] = subsp(addsp(r[A(5)], r[A(7)]), r[A(4)]);
        put_st(s, r[A(16)] + a4i, r[A(3)], at + 0x23C);
        cost(s, 11, 21, 12);
        return 1;
    }

    r[B(5)] = f[4];
    r[A(3)] = !((int32_t)i < 650);
    r[B(6)] = !(f[4] >> 1 & 1);
    r[A(0)] = r[A(3)] | r[B(6)];
    cost(s, 10, 15, 10);
    if (!r[A(0)]) {
        if (!staged_load(c, s, r[A(17)] + a4i, 4, &r[A(4)]) || !staged_load(c, s, r[A(17)] + a4i + 4, 4, &r[A(5)])
            || !staged_load(c, s, r[A(16)] + a4i + 4, 4, &r[A(3)]) || !ram_ptr(c, r[A(16)] + a4i, 4))
            return 0;
        r[A(3)] = subsp(addsp(r[A(4)], r[A(3)]), r[A(5)]);
        put_st(s, r[A(16)] + a4i, r[A(3)], at + 0x20C);
        cost(s, 10, 20, 11);
        return 1;
    }

    r[B(6)] = f[1];
    r[B(0)] = 2 & f[1];
    if (r[B(0)]) {
        r[B(6)] = f[3];
        r[B(0)] = 1 & f[3];
    }
    cost(s, 6, 18, 7);
    if (r[B(0)]) {
        r[A(7)] = a4i;
        r[A(31)] = r[A(17)] + a4i;
        if (!lock_phase(c, s, -2, at + 0x1E0, &r[A(5)], &r[A(4)], &r[A(3)]))
            return 0;
        cost(s, 12, 20, 13);
        return 1;
    }

    r[B(6)] = f[5];
    r[A(3)] = !((int32_t)i < 649);
    r[B(8)] = !(r[B(9)] & ~(r[B(5)] | ~((int32_t)f[5] >> 1)));
    r[A(0)] = r[A(3)] | r[B(8)];
    cost(s, 13, 18, 13);
    if (!r[A(0)]) {
        r[A(7)] = a4i;
        r[A(31)] = r[A(17)] + a4i;
        if (!lock_phase(c, s, 2, at + 0x1AC, &r[A(5)], &r[A(4)], &r[A(3)]))
            return 0;
        cost(s, 12, 20, 13);
        return 1;
    }

    /* both neighbours three bins away, each under its own flags */
    r[B(8)] = f[0];
    r[A(3)] = ~r[B(4)];
    r[B(1)] = 2 & f[0];
    if (r[B(1)]) {
        r[B(8)] = ~f[3];
        r[A(3)] |= r[B(8)];
        r[A(0)] = 1 & r[A(3)];
    } else
        r[A(0)] = r[A(9)];
    r[A(3)] = a4i;
    r[A(4)] = r[A(16)] + a4i;
    if (!r[A(0)]) {
        if (!lock_phase(c, s, -3, at + 0x104, &r[A(5)], &r[A(4)], &r[A(3)]))
            return 0;
        r[B(0)] = 0;
    } else {
        r[B(4)] = r[B(5)] | ~((int32_t)f[6] >> 1);
        r[B(5)] = r[B(6)] | r[B(4)];
        r[B(6)] = r[B(9)];
        r[B(4)] = !(648 > (int32_t)i);
        r[B(5)] = !(r[B(6)] & ~r[B(5)]);
        r[B(0)] = r[B(5)] | r[B(4)];
    }
    if (r[B(0)]) {
        if (!ram_ptr(c, r[A(12)], 1))
            return 0;
        put_stn(s, r[A(12)], r[A(19)], 1, at + 0x140);
    }
    if (!r[A(0)])
        r[B(0)] = r[A(9)];
    if (!r[B(0)]) {
        r[A(3)] = a4i;
        if (!lock_phase(c, s, 3, at + 0x174, &r[A(5)], &r[A(4)], &r[A(3)]))
            return 0;
    }
    cost(s, 43, 62, 52);
    return 1;
}

static int lock_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, at = s->at;

    if (!lock_bin(c, s))
        return 0;
    r[A(6)]++;
    r[B(7)]++;
    r[B(2)]--;
    cost(s, 3, 8, 4);
    s->pc = r[B(2)] ? at + 0x4 : at + 0x254;
    s->last_pc = at + 0x250;
    return 1;
}

static const native_wild lock_wild[] = { };

const native_kernel native_voc_lock = KERNEL("voc_lock", 0x040fc058, 0x260, 0xb517ba81993b0842ull, 0x4, 0x254,
                                             lock_wild, lock_pass);

/* ------------------------------------------------------------------------ */
/* The peak bin loop of 0x8004062C (NXS2) at 0x80040654                      */

/* One pass per bin of the class array *++A5: a bin of positive class, or
 * whose class two on is positive, looks at its entry in the half array at
 * A26 + A3 (A3 counts up by 2) and when that entry is 1 writes the larger
 * of the bin's class and the class two on there, plus one. The pass after
 * the last also sets four registers in the loop branch's delay slots, the
 * last three packets of every pass, so the branch block it ends with is 2,
 * taken or not. Such a
 * bin whose next class is positive too takes a branch never seen on the
 * vocoder's data and is left to the core. */

static int peaks_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, at = s->at, a5 = r[A(5)] + 2;
    const uint8_t *cls = ram_ptr(c, a5, 6);

    if (!cls)
        return 0;
    r[A(5)] = a5;
    r[A(4)] = ld16s(cls);
    r[A(0)] = 0 < (int32_t)r[A(4)];
    if (!r[A(0)]) {
        r[A(7)] = ld16s(cls + 4);
        r[A(0)] = 0 < (int32_t)r[A(7)];
    }
    if (!r[A(0)])
        cost(s, 8, 18, 10);
    else {
        r[A(7)] = ld16s(cls + 2);
        if (0 < (int32_t)r[A(7)])
            return 0;
        r[A(7)] = r[A(3)] + r[A(26)];
        uint8_t *e = ram_ptr(c, r[A(7)], 2);
        if (!e)
            return 0;
        r[A(8)] = ld16s(e);
        r[A(1)] = r[A(8)] == 1;
        r[A(0)] = 1;
        if (r[A(1)]) {
            r[A(6)] = ld16s(cls + 4);
            r[A(0)] = (int32_t)r[A(4)] < (int32_t)r[A(6)];
        }
        if (!r[A(0)]) {
            r[B(5)] = r[A(4)] + 1;
            put_stn(s, r[A(7)], r[B(5)], 2, at + 0x58);
        }
        if (!r[A(1)])
            r[A(0)] = 0;
        if (r[A(0)]) {
            r[A(4)] = r[A(6)] + 1;
            put_stn(s, r[A(7)], r[A(4)], 2, at + 0x68);
        }
        cost(s, 19, 37, 22);
    }
    r[B(0)]--;
    r[A(3)] += 2;
    if (!r[B(0)]) {
        r[A(20)] = 0x40C90FDB;      /* 2 pi */
        r[A(23)] = r[B(4)];
        r[B(4)] = 65;
        r[A(22)] = r[B(7)];
    }
    cost(s, 5, 7, 8);
    s->pc = r[B(0)] ? at + 0x14 : at + 0x8C;
    s->last_pc = at + 0x88;
    s->branch_block = 2;
    return 1;
}

static const native_wild peaks_wild[] = { };

const native_kernel native_voc_peaks = KERNEL("voc_peaks", 0x0de46078, 0xA0, 0xd66b50e16963edf6ull, 0x14, 0x8C,
                                              peaks_wild, peaks_pass);
