/* SPDX-License-Identifier: GPL-2.0-or-later */
/* C66x native kernels: NXS2 MASTER TEMPO bin phase. */
#include "c66x_native_int.h"

/* ------------------------------------------------------------------------ */
/* MASTER TEMPO bin phase, one bin per pass (NXS2: the loops at 0x80042820  */
/* in 0x800423F8 and 0x800442AC in 0x80043E8C)                              */

/* Each bin forms x = back[0] + fwd[0] and y = fwd[1] - back'[0] (fwd walks up
 * two words, the two back pointers down two) and the angle of (x, y) as an
 * arctangent of min / max(|x|, |y|): a seed of 1 / max from six thresholds and
 * lines on max, three Newton steps, then the ratio itself below B27 or a cubic
 * in it less 0.002179053 in double, folded into its octant against three
 * constants (zero, a quarter turn, a half turn). The angle replaces a word of
 * an output array.
 *
 * The 28-packet loops overlap four bins and are entered with results still in
 * flight, some of them conditional. A pass is written per stage of the
 * schedule: each value reads the registers or in-flight results it reads on
 * the core, and the ones still in flight at the end go back to the pending
 * set at the same distance. The two twins differ in their register allocation
 * and where the schedule starts; the first also stores the angle it replaces
 * less a reference word. */

static inline double u2d(uint64_t v) { double d; memcpy(&d, &v, 8); return d; }
static inline uint64_t d2u(double d) { uint64_t v; memcpy(&v, &d, 8); return v; }

/* one step of 1 / max, with y max formed by the caller in the loop's order */
static inline float recip_step(float y, float y_max)
{
    return (y + y) - y * y_max;
}

/* the cubic's angle: the code's MVK/MVKH double subtracted after SPDP, then DPSP */
static inline uint32_t cubic_angle(float cubic)
{
    return f2u((float)((double)cubic - u2d((uint64_t)0x3F61D9CE << 32 | 0x2BE1491F)));
}

typedef struct octant {
    uint32_t turned;                /* the quarter turn less theta when |y| > |x|, else theta */
    uint32_t axis;                  /* the quarter turn when x == zero, else turned */
    uint32_t ang;
} octant;

static octant fold_octant(uint32_t theta, uint32_t swapped, float x, float y, uint32_t zero,
                          uint32_t quarter, uint32_t half, uint32_t sign)
{
    octant o;
    float z = u2f(zero);
    o.turned = swapped ? f2u(u2f(quarter) - u2f(theta)) : theta;
    o.axis = x == z ? quarter : o.turned;
    uint32_t a = y == z ? zero : o.axis;
    if (z > x)
        a = f2u(u2f(half) - u2f(a));
    o.ang = y < z ? a ^ (sign | 0x80000000) : a;
    return o;
}

/* the first twin: its loop head starts its code */
enum { PA_A3, PA_B6, PA_A4, PA_B5, PA_B19, PA_B0, PA_B4, PA_B18, PA_A9, PA_N };

static const native_pend mt_phase_a_pend[] = {
    { A(3), 0, 0 }, { B(6), 0, 0 }, { A(4), 0, 0 }, { B(5), 0, 0 }, { B(19), 0, 1 }, { B(0), 0, 0 },
    { B(4), 1, 0 }, { B(18), 1, 1 }, { A(9), 2, 0 },
};

static int mt_phase_a_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, *p = s->pend;
    const uint8_t *fwd = ram_ptr(c, r[A(31)], 8), *back_a = ram_ptr(c, r[A(30)], 4);
    const uint8_t *back_b = ram_ptr(c, r[B(13)], 4), *ref = ram_ptr(c, r[A(12)], 4);
    const uint8_t *angp = ram_ptr(c, r[A(11)], 4);

    if (!fwd || !back_a || !back_b || !ref || !angp || !ram_ptr(c, r[A(10)], 4) || directed_rounding(s))
        return 0;

    /* the newest bin: x, y, max, min and the lines of its seed */
    float x = u2f(ld32(back_b)) + u2f(ld32(fwd)), y = u2f(ld32(fwd + 4)) - u2f(ld32(back_a));
    uint32_t ax = f2u(x) & 0x7FFFFFFF, ay = f2u(y) & 0x7FFFFFFF;
    int swap = u2f(ay) > u2f(ax);
    uint32_t hi = swap ? ay : ax, lo = swap ? ax : ay;
    float m = u2f(hi);
    int over_a27 = u2f(r[A(27)]) < m, over_a28 = u2f(r[A(28)]) < m;
    int over_b27 = m > u2f(r[B(27)]), over_b29 = m > u2f(r[B(29)]);
    float a28m = u2f(r[A(28)]) * m;
    uint32_t b21 = f2u(over_a28 ? u2f(r[B(12)]) - u2f(r[A(29)]) * m : u2f(r[B(10)]) - u2f(r[B(11)]) * m);
    uint32_t b20 = over_a27 ? b21 : f2u(u2f(r[B(31)]) - u2f(r[B(3)]) * m);

    /* the bin before: its seed, picked from the last pass's lines, and two Newton steps */
    uint32_t b19 = r[B(1)] ? r[B(20)] : p[PA_B19];
    uint32_t b18 = r[B(2)] ? b19 : p[PA_B18];
    uint32_t b17 = r[B(0)] ? b18 : f2u(u2f(r[B(25)]) - u2f(r[B(5)]));
    float seed = u2f(p[PA_B0] ? b17 : f2u(u2f(r[B(24)]) - u2f(r[A(5)])));
    float rmax = u2f(r[B(15)]);
    float y1 = recip_step(seed, rmax * seed);
    float y2 = recip_step(y1, y1 * rmax);

    /* the one before: the last Newton step, the ratio and its cubic */
    float y3 = u2f(p[PA_B5]) - u2f(r[B(7)]) * u2f(p[PA_B4]);
    float ratio = u2f(r[B(8)]) * y3, r2 = ratio * ratio, r3 = r2 * ratio;

    /* and the oldest: its angle */
    uint32_t theta = u2f(r[B(9)]) < u2f(r[B(27)]) ? r[B(9)] : cubic_angle(u2f(p[PA_A4]) - u2f(r[A(21)]));
    float oy = u2f(p[PA_A3]);
    octant o = fold_octant(theta, r[A(6)], u2f(p[PA_A9]), oy, r[A(22)], r[A(23)], r[A(19)], r[B(16)]);
    uint32_t old = ld32(angp), last_x = p[PA_B6];

    put_st(s, r[A(10)], f2u(u2f(old) - u2f(ld32(ref))), s->at + 0x1BC);
    put_st(s, r[A(11)], o.ang, s->at + 0x1D4);

    p[PA_A3] = r[A(16)];
    p[PA_B6] = f2u(x);
    p[PA_A4] = f2u(u2f(r[A(24)]) * ratio - u2f(r[B(22)]) * r3);
    p[PA_B5] = f2u(y2 + y2);
    p[PA_B19] = over_b29 ? b19 : f2u(u2f(r[B(30)]) - m);
    p[PA_B0] = m > u2f(r[B(23)]);
    p[PA_B4] = f2u(rmax * y2);
    p[PA_B18] = over_b27 ? b18 : f2u(u2f(r[B(28)]) - a28m);
    p[PA_A9] = r[B(4)];
    s->pend_live = ((1u << PA_N) - 1) & ~(over_b29 << PA_B19) & ~(over_b27 << PA_B18);

    r[A(0)] = oy < u2f(r[A(22)]);
    r[A(1)] = over_a27;
    r[A(2)] -= r[A(2)] != 0;
    r[A(3)] = f2u(a28m);
    r[A(4)] = o.ang;
    r[A(5)] = f2u(u2f(r[A(26)]) * m);
    r[A(16)] = r[A(7)];
    r[A(6)] = r[A(20)];
    r[A(7)] = r[A(14)] = f2u(y);
    r[A(8)] = ld32(ref);
    r[A(9)] = old;
    r[A(10)] += 4;
    r[A(11)] += 4;
    r[A(12)] += 4;
    r[A(20)] = r[A(13)];
    r[A(13)] = swap;
    r[A(17)] = o.axis;
    r[A(18)] = o.turned;
    r[A(21)] = f2u(u2f(r[A(25)]) * r2);
    r[A(30)] -= 8;
    r[A(31)] += 8;
    r[B(0)] = m > u2f(r[B(3)]);
    r[B(1)] = over_b29;
    r[B(2)] = over_b27;
    r[B(4)] = last_x;
    r[B(5)] = f2u(u2f(r[B(26)]) * m);
    r[B(6)] = p[PA_B0];
    r[B(7)] = f2u(y2);
    r[B(8)] = r[B(14)];
    r[B(9)] = f2u(ratio);
    r[B(13)] -= 8;
    r[B(14)] = lo;
    r[B(15)] = hi;
    r[B(17)] = b17;
    r[B(18)] = b18;
    r[B(19)] = b19;
    r[B(20)] = b20;
    r[B(21)] = b21;
    s->cycles = 28;
    s->packets = 28;
    s->insns = 123;
    s->pc = r[A(2)] ? s->at : s->at + 0x1EC;
    s->last_pc = s->at + 0x1D4;
    s->branch_block = 0;
    return 1;
}

/* the second twin: its loop head is 0xC into its code */
enum { PB_A6, PB_B7, PB_B7_LATE, PB_A3, PB_B8, PB_B19, PB_B1, PB_N };

static const native_pend mt_phase_b_pend[] = {
    { A(6), 0, 0 }, { B(7), 0, 0 }, { B(7), 1, 0 }, { A(3), 1, 0 }, { B(8), 1, 0 }, { B(19), 1, 1 },
    { B(1), 2, 0 },
};

static int mt_phase_b_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, *p = s->pend, head = s->at + 0xC;
    const uint8_t *fwd = ram_ptr(c, r[A(12)], 8), *back_a = ram_ptr(c, r[A(11)], 4);
    const uint8_t *back_b = ram_ptr(c, r[B(13)], 4);

    if (!fwd || !back_a || !back_b || !ram_ptr(c, r[A(21)], 4) || directed_rounding(s))
        return 0;

    /* the newest bin */
    float x = u2f(ld32(back_b)) + u2f(ld32(fwd)), y = u2f(ld32(fwd + 4)) - u2f(ld32(back_a));
    uint32_t ax = f2u(x) & 0x7FFFFFFF, ay = f2u(y) & 0x7FFFFFFF;
    int swap = u2f(ay) > u2f(ax);
    uint32_t hi = swap ? ay : ax, lo = swap ? ax : ay;
    float m = u2f(hi);
    int over_a30 = u2f(r[A(30)]) < m, over_a31 = u2f(r[A(31)]) < m;
    int over_b27 = m > u2f(r[B(27)]), over_b29 = m > u2f(r[B(29)]);
    float a31m = u2f(r[A(31)]) * m;
    uint32_t b21 = f2u(over_a31 ? u2f(r[B(12)]) - u2f(r[A(10)]) * m : u2f(r[B(10)]) - u2f(r[B(11)]) * m);

    /* the bin before: its seed and two Newton steps */
    uint32_t b20 = r[A(1)] ? r[B(21)] : r[B(20)];
    uint32_t b19 = r[B(1)] ? b20 : p[PB_B19];
    uint32_t b18 = r[B(2)] ? b19 : f2u(u2f(r[B(28)]) - u2f(r[A(3)]));
    int over_b3 = u2f(r[B(5)]) > u2f(r[B(3)]);
    uint32_t b17 = over_b3 ? b18 : f2u(u2f(r[B(25)]) - u2f(p[PB_B7]));
    float seed = u2f(r[B(0)] ? b17 : f2u(u2f(r[B(24)]) - u2f(p[PB_A6])));
    float rmax = u2f(r[B(5)]);
    float y1 = recip_step(seed, rmax * seed);
    float y2 = recip_step(y1, y1 * rmax);

    /* the one before: the last Newton step, the ratio and its cubic */
    float y3 = u2f(p[PB_B8]) - u2f(r[B(9)]) * u2f(p[PB_B1]);
    float ratio = u2f(r[B(6)]) * y3, r2 = ratio * ratio, r3 = r2 * ratio;

    /* and the oldest: its angle */
    int small = u2f(r[B(4)]) < u2f(r[B(27)]);
    float cubic = u2f(p[PB_A3]) - u2f(r[A(7)]);
    uint32_t theta = small ? r[B(4)] : cubic_angle(cubic);
    float oy = u2f(r[A(22)]);
    octant o = fold_octant(theta, r[A(9)], u2f(r[B(7)]), oy, r[A(25)], r[A(26)], r[A(24)], r[B(16)]);
    uint32_t last_x = p[PB_B7_LATE];

    put_st(s, r[A(21)], o.ang, s->at + 0x1CC);

    p[PB_A6] = f2u(u2f(r[A(29)]) * m);
    p[PB_B7] = f2u(u2f(r[B(26)]) * m);
    p[PB_B7_LATE] = f2u(x);
    p[PB_A3] = f2u(u2f(r[A(27)]) * ratio - u2f(r[B(22)]) * r3);
    p[PB_B8] = f2u(y2 + y2);
    p[PB_B19] = over_b29 ? b19 : f2u(u2f(r[B(30)]) - m);
    p[PB_B1] = f2u(rmax * y2);
    s->pend_live = ((1u << PB_N) - 1) & ~(over_b29 << PB_B19);

    r[A(0)] = oy < u2f(r[A(25)]);
    r[A(1)] = over_a30;
    r[A(2)] -= r[A(2)] != 0;
    r[A(3)] = f2u(a31m);
    r[A(9)] = r[A(5)];
    r[A(5)] = r[A(4)];
    r[A(4)] = swap;
    r[A(6)] = o.ang;
    r[A(7)] = f2u(u2f(r[A(28)]) * r2);
    r[A(19)] = r[A(22)];
    r[A(22)] = r[A(16)];
    r[A(16)] = r[A(8)];
    r[A(8)] = f2u(y);
    r[A(11)] -= 8;
    r[A(12)] += 8;
    r[A(17)] = o.axis;
    r[A(18)] = f2u(u2f(r[A(27)]) * ratio);
    r[A(20)] = o.turned;
    r[A(21)] += 4;
    if (!small)
        r[A(23)] = (uint32_t)(d2u((double)cubic) >> 32);
    r[B(0)] = m > u2f(r[B(23)]);
    r[B(1)] = over_b29;
    r[B(2)] = over_b27;
    r[B(4)] = f2u(ratio);
    r[B(5)] = hi;
    r[B(6)] = r[B(14)];
    r[B(7)] = r[B(8)];
    r[B(8)] = last_x;
    r[B(9)] = f2u(y2);
    r[B(13)] -= 8;
    r[B(14)] = lo;
    r[B(17)] = b17;
    r[B(18)] = b18;
    r[B(19)] = b19;
    r[B(20)] = over_a30 ? b20 : f2u(u2f(r[B(31)]) - u2f(r[B(3)]) * m);
    r[B(21)] = b21;
    s->cycles = 28;
    s->packets = 28;
    s->insns = 118;
    s->pc = r[A(2)] ? head : s->at + 0x1E4;
    s->last_pc = s->at + 0x1CC;
    s->branch_block = 0;
    return 1;
}

/* ------------------------------------------------------------------------ */
/* The twins' other loops, taken when their first argument is nonzero: the  */
/* loop at 0x80042FFC in 0x800423F8 and the one at 0x80044BE4 in 0x80043E8C */

/* The same angle of a bin's (x, y), but x = back_b[0] + fwd[1] and y =
 * back_a[0] - fwd[0], the seed is formed whole in the bin's own pass, and the
 * octant fold compares the bin's |x| and |y| again instead of keeping the swap. */

/* the first twin's: 35 packets, its loop head 0x1C into its code */
enum { PA1_A1, PA1_A3, PA1_A5, PA1_N };

static const native_pend mt_phase_a1_pend[] = { { A(1), 0, 0 }, { A(3), 1, 0 }, { A(5), 2, 0 } };

static int mt_phase_a1_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, *p = s->pend;
    const uint8_t *fwd = ram_ptr(c, r[A(11)], 8), *back_a = ram_ptr(c, r[A(10)], 4);
    const uint8_t *back_b = ram_ptr(c, r[B(31)], 4), *ref = ram_ptr(c, r[B(11)], 4);
    const uint8_t *angp = ram_ptr(c, r[B(10)], 4);

    if (!fwd || !back_a || !back_b || !ref || !angp || !ram_ptr(c, r[B(3)], 4) || directed_rounding(s))
        return 0;

    /* the newest bin: x, y, max and its seed of 1 / max */
    float x = u2f(ld32(back_b)) + u2f(ld32(fwd + 4)), y = u2f(ld32(back_a)) - u2f(ld32(fwd));
    uint32_t ax = f2u(x) & 0x7FFFFFFF, ay = f2u(y) & 0x7FFFFFFF;
    int swap = u2f(ax) < u2f(ay);
    uint32_t hi = swap ? ay : ax;
    float m = u2f(hi);
    int over_a19 = m > u2f(r[A(19)]);
    uint32_t a17 = f2u(u2f(r[B(29)]) < m ? u2f(r[A(31)]) - u2f(r[B(30)]) * m : u2f(r[A(29)]) - u2f(r[A(30)]) * m);
    uint32_t a13 = u2f(r[B(28)]) < m ? a17 : f2u(u2f(r[A(27)]) - u2f(r[A(28)]) * m);
    uint32_t a8 = m > u2f(r[A(25)]) ? a13 : f2u(u2f(r[A(26)]) - m);
    uint32_t a2 = m > u2f(r[A(23)]) ? a8 : f2u(u2f(r[A(24)]) - u2f(r[B(29)]) * m);
    uint32_t a5 = m > u2f(r[A(28)]) ? a2 : f2u(u2f(r[A(21)]) - u2f(r[A(22)]) * m);
    float seed = over_a19 ? u2f(a5) : u2f(r[A(20)]) - u2f(r[B(27)]) * m;
    float m_seed = m * seed;

    /* the bin before: three Newton steps and its ratio */
    float rmax = u2f(r[A(6)]);
    float y1 = (u2f(r[A(4)]) + u2f(r[A(4)])) - u2f(p[PA1_A5]);
    float y2 = recip_step(y1, y1 * rmax);
    float y3 = recip_step(y2, rmax * y2);
    uint32_t lo = r[B(5)] ? r[B(17)] : r[A(7)];
    float ratio = u2f(lo) * y3;

    /* and the one before that: its angle */
    float r1 = u2f(r[A(9)]), r2 = u2f(p[PA1_A1]);
    int small = r1 < u2f(r[A(23)]);
    float cubic = (u2f(r[B(25)]) * r1 - u2f(r[A(18)]) * (r2 * r1)) - u2f(r[B(26)]) * r2;
    uint32_t theta = small ? r[A(9)] : cubic_angle(cubic);
    float oy = u2f(p[PA1_A3]);
    octant o = fold_octant(theta, u2f(r[B(16)]) < u2f(r[B(6)]), u2f(r[B(19)]), oy, r[B(23)], r[B(24)], r[B(22)],
                           r[A(16)]);

    put_st(s, r[B(3)], f2u(u2f(ld32(angp)) - u2f(ld32(ref))), s->at + 0x1E4);
    put_st(s, r[B(10)], o.ang, s->at + 0x1F0);

    p[PA1_A1] = f2u(ratio * ratio);
    p[PA1_A3] = r[B(7)];
    p[PA1_A5] = f2u(seed * m_seed);
    s->pend_live = (1u << PA1_N) - 1;

    r[A(0)] = over_a19;
    r[A(1)] = oy == u2f(r[B(23)]);
    r[A(2)] = a2;
    r[A(3)] = f2u(m_seed);
    r[A(4)] = f2u(seed);
    r[A(5)] = a5;
    r[A(6)] = hi;
    r[A(7)] = ay;
    r[A(8)] = a8;
    r[A(9)] = f2u(ratio);
    r[A(10)] -= 8;
    r[A(11)] += 8;
    r[A(12)] = lo;
    r[A(13)] = a13;
    r[A(14)] = r[B(1)];
    r[A(15)] = ay;
    r[A(17)] = a17;
    r[B(0)] = oy < u2f(r[B(23)]);
    r[B(2)] -= r[B(2)] != 0;
    r[B(3)] += 4;
    r[B(4)] = o.ang;
    r[B(5)] = swap;
    r[B(6)] = r[B(1)];
    r[B(1)] = ay;
    r[B(7)] = f2u(y);
    r[B(8)] = f2u(u2f(r[B(30)]) * m);
    if (!small)
        r[B(9)] = 0x3F61D9CE;
    r[B(10)] += 4;
    r[B(11)] += 4;
    r[B(12)] = f2u(x);
    r[B(16)] = r[B(17)];
    r[B(17)] = ax;
    r[B(19)] = r[B(18)];
    r[B(18)] = f2u(x);
    r[B(20)] = o.axis;
    r[B(21)] = o.turned;
    r[B(31)] -= 8;
    s->cycles = 35;
    s->packets = 35;
    s->insns = 119;
    s->pc = r[B(2)] ? s->at + 0x1C : s->at + 0x1F8;
    s->last_pc = s->at + 0x1F0;
    s->branch_block = 0;
    return 1;
}

/* the second twin's: 28 packets, its loop head 4 into its code; its seed
 * takes two passes and B6 has two results in flight */
enum { PB1_A16, PB1_B6, PB1_B5, PB1_A4, PB1_B6_LATE, PB1_B19, PB1_B4, PB1_N };

static const native_pend mt_phase_b1_pend[] = {
    { A(16), 0, 0 }, { B(6), 0, 0 }, { B(5), 1, 0 }, { A(4), 1, 0 }, { B(6), 1, 0 }, { B(19), 1, 1 },
    { B(4), 2, 0 },
};

static int mt_phase_b1_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, *p = s->pend;
    const uint8_t *fwd = ram_ptr(c, r[A(10)], 8), *back_a = ram_ptr(c, r[A(31)], 4);
    const uint8_t *back_b = ram_ptr(c, r[B(13)], 4);

    if (!fwd || !back_a || !back_b || !ram_ptr(c, r[A(19)], 4) || directed_rounding(s))
        return 0;

    /* the newest bin: x, y, max and the lines of its seed */
    float x = u2f(ld32(back_b)) + u2f(ld32(fwd + 4)), y = u2f(ld32(back_a)) - u2f(ld32(fwd));
    uint32_t ax = f2u(x) & 0x7FFFFFFF, ay = f2u(y) & 0x7FFFFFFF;
    int swap = u2f(ax) < u2f(ay);
    uint32_t hi = swap ? ay : ax, lo = swap ? ax : ay;
    float m = u2f(hi);
    int over_a28 = u2f(r[A(28)]) < m, over_b29 = m > u2f(r[B(29)]);

    /* the bin before: its seed, picked from the last pass's lines, and two Newton steps */
    uint32_t b20 = r[A(1)] ? r[B(21)] : r[B(20)];
    uint32_t b19 = r[B(1)] ? b20 : p[PB1_B19];
    uint32_t b18 = r[B(2)] ? b19 : f2u(u2f(r[B(28)]) - u2f(r[A(16)]));
    uint32_t b17 = u2f(r[B(7)]) > u2f(r[B(3)]) ? b18 : f2u(u2f(r[B(25)]) - u2f(p[PB1_B6]));
    float seed = u2f(r[B(0)] ? b17 : f2u(u2f(r[B(24)]) - u2f(p[PB1_A16])));
    float rmax = u2f(r[B(7)]);
    float y1 = recip_step(seed, rmax * seed);
    float y2 = recip_step(y1, y1 * rmax);

    /* the one before: the last Newton step, the ratio and its powers */
    float y3 = u2f(p[PB1_B6_LATE]) - u2f(r[B(8)]) * u2f(p[PB1_B4]);
    float ratio = u2f(r[B(5)]) * y3, r2 = ratio * ratio, r3 = r2 * ratio;

    /* and the oldest: its angle */
    int small = u2f(r[B(9)]) < u2f(r[B(27)]);
    uint32_t theta = small ? r[B(9)] : cubic_angle(u2f(p[PB1_A4]) - u2f(r[A(5)]));
    octant o = fold_octant(theta, r[A(7)], u2f(r[B(6)]), u2f(r[A(21)]), r[A(23)], r[A(24)], r[A(22)], r[B(16)]);
    uint32_t last_x = p[PB1_B5];

    put_st(s, r[A(19)], o.ang, s->at + 0x1D4);

    p[PB1_A16] = f2u(u2f(r[A(27)]) * m);
    p[PB1_B6] = f2u(u2f(r[B(26)]) * m);
    p[PB1_B5] = f2u(x);
    p[PB1_A4] = f2u(u2f(r[A(25)]) * ratio - u2f(r[B(22)]) * r3);
    p[PB1_B6_LATE] = f2u(y2 + y2);
    p[PB1_B19] = over_b29 ? b19 : f2u(u2f(r[B(30)]) - m);
    p[PB1_B4] = f2u(rmax * y2);
    s->pend_live = ((1u << PB1_N) - 1) & ~(over_b29 << PB1_B19);

    r[A(0)] = u2f(r[A(21)]) < u2f(r[A(23)]);
    r[A(1)] = over_a28;
    r[A(2)] -= r[A(2)] != 0;
    r[A(4)] = f2u(u2f(r[A(25)]) * ratio);
    r[A(5)] = f2u(u2f(r[A(26)]) * r2);
    r[A(17)] = r[A(21)];
    r[A(21)] = r[A(8)];
    r[A(8)] = r[A(3)];
    r[A(3)] = f2u(y);
    r[A(7)] = r[A(6)];
    r[A(6)] = r[A(11)];
    r[A(9)] = o.axis;
    r[A(10)] += 8;
    r[A(11)] = swap;
    r[A(12)] = r[B(14)];
    r[A(16)] = f2u(u2f(r[A(29)]) * m);
    r[A(18)] = o.turned;
    r[A(19)] += 4;
    r[A(20)] = f2u(u2f(r[A(30)]) * m);
    r[A(31)] -= 8;
    r[B(0)] = m > u2f(r[B(23)]);
    r[B(1)] = over_b29;
    r[B(2)] = m > u2f(r[B(27)]);
    r[B(6)] = r[B(4)];
    r[B(4)] = last_x;
    r[B(5)] = r[B(14)];
    r[B(7)] = hi;
    r[B(8)] = f2u(y2);
    r[B(9)] = f2u(ratio);
    r[B(13)] -= 8;
    r[B(14)] = lo;
    r[B(17)] = b17;
    r[B(18)] = b18;
    r[B(19)] = b19;
    r[B(20)] = over_a28 ? b20 : f2u(u2f(r[B(31)]) - u2f(r[B(3)]) * m);
    r[B(21)] = u2f(r[A(29)]) < m ? f2u(u2f(r[B(12)]) - u2f(r[A(30)]) * m) : f2u(u2f(r[B(10)]) - u2f(r[B(11)]) * m);
    s->cycles = 28;
    s->packets = 28;
    s->insns = 122;
    s->pc = r[A(2)] ? s->at + 4 : s->at + 0x1EC;
    s->last_pc = s->at + 0x1D4;
    s->branch_block = 0;
    return 1;
}

static const native_wild mt_phase_a_wild[] = { };
static const native_wild mt_phase_b_wild[] = { };
static const native_wild mt_phase_a1_wild[] = { };
static const native_wild mt_phase_b1_wild[] = { };

const native_kernel native_mt_phase_a = KERNEL_PEND("mt_phase_a", 0x021350f1, 0x200, 0x068bdabfa2ef24b6ull, 0, 0x1EC,
                                                    mt_phase_a_wild, mt_phase_a_pass, mt_phase_a_pend);
const native_kernel native_mt_phase_b = KERNEL_PEND("mt_phase_b", 0x00a62e03, 0x200, 0xd7c3954553405810ull, 0xC, 0x1E4,
                                                    mt_phase_b_wild, mt_phase_b_pass, mt_phase_b_pend);
const native_kernel native_mt_phase_a1 = KERNEL_PEND("mt_phase_a1", 0x031b40f1, 0x200, 0xf24a896bc2c7b49dull, 0x1C, 0x1F8,
                                                     mt_phase_a1_wild, mt_phase_a1_pass, mt_phase_a1_pend);
const native_kernel native_mt_phase_b1 = KERNEL_PEND("mt_phase_b1", 0x59d7ceda, 0x200, 0x1710863f76e88b0aull, 4, 0x1EC,
                                                     mt_phase_b1_wild, mt_phase_b1_pass, mt_phase_b1_pend);
