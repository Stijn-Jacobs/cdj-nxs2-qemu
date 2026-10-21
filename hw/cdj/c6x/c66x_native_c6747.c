/* SPDX-License-Identifier: GPL-2.0-or-later */
/* C66x native kernels: the C6747 decks. */
#include "c66x_native_int.h"

/* ------------------------------------------------------------------------ */
/* C6747 decks, cubic interpolator, one call per pass (XDJ-1000: 0xC000FC50) */

/* The same Lagrange cubic as the NXS2's, scheduled differently: interp(t A4,
 * x0 B4, x1 A6, x2 B6, x3 A8) returns in A4, evaluated as
 *   0.5 (x0 t^2 + (x1 (t^3 - t - 2t^2 + 2) - x2 (t^3 - t^2 - 2t)))
 *     + (x3 (t^3 - t) - x0 (t^3 + 2t)) / 6
 * in the firmware's operation order. Registers only: entered at the call,
 * left through B3, 50 cycles on every call. These decks run their float
 * units rounding toward zero, which the pass reproduces on the host. */

static int float_mode(const native_state *s)
{
    uint32_t f = s->cr[CR_FADCR], m = s->cr[CR_FMCR];
    unsigned rm = f >> 9 & 3;

    if ((f >> 25 & 3) != rm || (m >> 9 & 3) != rm || (m >> 25 & 3) != rm)
        return -1;
    return (int)rm;
}

static int xdj_interp_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;
    int rm = float_mode(s);

    if (rm < 0)
        return 0;
    if (rm)
        host_round(rm);
    float t = u2f(r[A(4)]), x0 = u2f(r[B(4)]), x1 = u2f(r[A(6)]), x2 = u2f(r[B(6)]), x3 = u2f(r[A(8)]);
    float t2 = t * t, t3 = t * t2;
    float tt2 = t2 + t2, x0t2 = x0 * t2, t3mt = t3 - t, tt = t + t, t3mt2 = t3 - t2;
    float a16 = t3mt - tt2, a5 = tt + t3, a18 = t3mt2 - tt;
    float a9 = 2.0f + a16;
    float a4 = x1 * a9, a16b = a18 * x2, a6 = x3 * t3mt, b6 = x0 * a5;
    a4 = a4 - a16b;
    float b4 = a6 - b6;
    b6 = x0t2 + a4;
    float b5 = 0.5f * b6;
    b4 = u2f(0x3E2AAAABu) * b4;
    b4 = b5 + b4;
    if (rm)
        host_round(0);
    r[A(3)] = f2u(t2);
    r[A(4)] = f2u(b4);
    r[A(5)] = f2u(a5);
    r[A(6)] = f2u(a6);
    r[A(7)] = f2u(t3mt);
    r[A(9)] = f2u(a9);
    r[A(16)] = f2u(a16b);
    r[A(17)] = 0x40000000;
    r[A(18)] = f2u(a18);
    r[B(4)] = f2u(b4);
    r[B(5)] = f2u(b5);
    r[B(6)] = f2u(b6);
    r[B(7)] = 0x3E2AAAAB;
    s->last_pc = s->at + 0xA0;
    s->pc = r[B(3)];
    s->cycles = 50;
    s->packets = 36;
    s->insns = 36;
    return 1;
}

static const native_wild xdj_interp_wild[] = { };

const native_kernel native_xdj_interp = KERNEL("xdj_interp", 0x02004b7e, 0xC0, 0x513ea38b2578b325ull, 0x10, RETURNS, xdj_interp_wild, xdj_interp_pass);
