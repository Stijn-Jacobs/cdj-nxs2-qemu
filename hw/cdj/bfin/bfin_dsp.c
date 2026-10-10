/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Blackfin DSP32 groups: the ALU (16-bit, vector and 32-bit arithmetic, the
 * accumulator moves), the shifter and the two multiply/accumulate units.
 *
 * Accumulators are 40 bits, kept sign-extended in an int64_t. Flags follow
 * the Programming Reference per instruction where the firmware can observe
 * them (AZ, AN, V, AC0, CC); the accumulator overflow flags are set on
 * saturation only.
 */
#include "bfin_dsp.h"
#include <stdlib.h>

enum {
    MM_DEFAULT = 0, MM_S2RND = 1, MM_T = 2, MM_W32 = 3, MM_FU = 4,
    MM_TFU = 6, MM_IS = 8, MM_ISS2 = 9, MM_IH = 11, MM_IU = 12,
};

#define ACC_MAX  ((int64_t)0x7FFFFFFFFF)
#define ACC_MIN  (-(int64_t)0x8000000000)

static inline uint16_t half(uint32_t v, int hi)
{
    return hi ? v >> 16 : v;
}

static inline void set_half(uint32_t *d, int hi, uint16_t v)
{
    *d = hi ? (*d & 0xFFFF) | (uint32_t)v << 16 : (*d & 0xFFFF0000) | v;
}

static int64_t clamp(int64_t v, int64_t lo, int64_t hi, int *ov)
{
    if (v < lo || v > hi) {
        *ov = 1;
        return v < lo ? lo : hi;
    }
    return v;
}

static inline int64_t sext40(int64_t v)
{
    return (int64_t)((uint64_t)v << 24) >> 24;
}

/* ---- divide primitives -------------------------------------------------- */

/* DIVS/DIVQ, one quotient bit per step: the dividend register holds the
 * partial remainder in its high half and collects quotient bits from the
 * bottom; AQ carries the sign relation between remainder and divisor. */
void bfin_divs(bfin_core *c, unsigned dst, unsigned src)
{
    uint32_t v = c->r[dst];
    int aq = ((v >> 31) ^ (c->r[src] >> 15)) & 1;

    c->astat = (c->astat & ~AS_AQ) | (aq ? AS_AQ : 0);
    c->r[dst] = v << 1 | aq;
}

void bfin_divq(bfin_core *c, unsigned dst, unsigned src)
{
    uint32_t v = c->r[dst];
    uint16_t div = c->r[src], rem = v >> 16;
    int aq;

    rem = c->astat & AS_AQ ? rem + div : rem - div;
    aq = ((rem ^ div) >> 15) & 1;
    c->astat = (c->astat & ~AS_AQ) | (aq ? AS_AQ : 0);
    c->r[dst] = ((uint32_t)rem << 16 | (v & 0xFFFF)) << 1 | !aq;
}

/* ---- ALU ---------------------------------------------------------------- */

static uint16_t add16(uint16_t a, uint16_t b, int sub, int sat, int *ov)
{
    int32_t r = sub ? (int16_t)a - (int16_t)b : (int16_t)a + (int16_t)b;

    if (r > 0x7FFF || r < -0x8000) {
        *ov = 1;
        if (sat) {
            r = r < 0 ? -0x8000 : 0x7FFF;
        }
    }
    return r;
}

static void flags16(bfin_core *c, uint16_t hi, uint16_t lo, int both, int ov)
{
    int z = !lo || (both && !hi), n = (lo >> 15) || (both && (hi >> 15));

    c->astat &= ~(AS_AZ | AS_AN);
    c->astat |= (z ? AS_AZ : 0) | (n ? AS_AN : 0);
    bfin_flags_v(c, ov);
}

/* Rd = Rs +|+ Rt and its siblings: op bit 1 is the high half's operation,
 * bit 0 the low half's; cross swaps the result halves. */
static uint32_t vadd(uint32_t a, uint32_t b, unsigned op, int sat, int cross,
                     int *ov)
{
    uint16_t hi = add16(a >> 16, b >> 16, op >> 1, sat, ov);
    uint16_t lo = add16(a, b, op & 1, sat, ov);

    return cross ? (uint32_t)lo << 16 | hi : (uint32_t)hi << 16 | lo;
}

static int32_t minmax(int32_t a, int32_t b, int max)
{
    return max ? (a > b ? a : b) : (a < b ? a : b);
}

/* The four bytes of a register pair that the byte ops read, starting at byte
 * I0 (or I1) & 3 of the pair. rev swaps which register holds the low bytes. */
static uint32_t pair_bytes(bfin_core *c, unsigned pair, int rev, unsigned ireg)
{
    uint32_t lo = c->r[rev ? pair + 1 : pair], hi = c->r[rev ? pair : pair + 1];
    unsigned shift = 8 * (c->i[ireg] & 3);

    return shift ? lo >> shift | hi << (32 - shift) : lo;
}

static inline unsigned byte_at(uint32_t v, unsigned n)
{
    return v >> 8 * n & 0xFF;
}

static uint16_t sat_u16(uint32_t v)
{
    return v > 0xFFFF ? 0xFFFF : v;
}

static uint8_t sat_u8(int32_t v)
{
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* The byte-oriented ops of the ALU group (aopcde 18 and 20-24), which take
 * register pairs R1:0 and R3:2. Returns 0 for an encoding that is not one. */
static int byte_ops(bfin_core *c, unsigned aopcde, unsigned aop, unsigned hl,
                    unsigned s, unsigned dst0, unsigned dst1, unsigned src0,
                    unsigned src1)
{
    uint32_t s0, s1;
    int pairs_ok = (src0 == 0 || src0 == 2) && (src1 == 0 || src1 == 2);

    if (aopcde == 24 && aop == 0 && !s && !hl) {        /* BYTEPACK */
        c->r[dst0] = byte_at(c->r[src0], 0) | byte_at(c->r[src0], 2) << 8 |
                     byte_at(c->r[src1], 0) << 16 | byte_at(c->r[src1], 2) << 24;
        return 1;
    }
    if (aopcde == 18 && aop == 3 && !s && !hl) {        /* DISALGNEXCPT */
        return 1;
    }
    if (!pairs_ok || (hl && aopcde != 22 && aopcde != 23)) {
        return 0;
    }
    switch (aopcde) {
    case 18:                                            /* SAA */
        if (aop || hl) {
            return 0;
        }
        s0 = pair_bytes(c, src0, s, 0);
        s1 = pair_bytes(c, src1, s, 1);
        {
            uint32_t acc[2] = { c->a[0], c->a[1] };
            uint32_t sum[2] = { 0, 0 };

            for (unsigned n = 0; n < 4; n++) {
                int diff = byte_at(s0, n) - byte_at(s1, n);
                unsigned half = n & 1;
                uint32_t old = acc[n >> 1] >> (16 * half) & 0xFFFF;

                sum[n >> 1] |= (uint32_t)sat_u16(abs(diff) + old) << (16 * half);
            }
            c->a[0] = sum[0];
            c->a[1] = sum[1];
        }
        return 1;
    case 20:                                            /* BYTEOP1P */
        if (aop > 1) {
            return 0;
        }
        s0 = pair_bytes(c, src0, s, 0);
        s1 = pair_bytes(c, src1, s, 1);
        c->r[dst0] = 0;
        for (unsigned n = 0; n < 4; n++) {
            c->r[dst0] |= ((byte_at(s0, n) + byte_at(s1, n) + !aop) >> 1) << 8 * n;
        }
        return 1;
    case 21:                                            /* BYTEOP16P/M */
        if (aop > 1) {
            return 0;
        }
        if (dst0 == dst1) {
            c->undef = 1;
            return 1;
        }
        s0 = pair_bytes(c, src0, s, 0);
        s1 = pair_bytes(c, src1, s, 1);
        {
            int sign = aop ? -1 : 1;
            uint32_t r[4];

            for (unsigned n = 0; n < 4; n++) {
                r[n] = byte_at(s0, n) + sign * (int)byte_at(s1, n);
            }
            c->r[dst0] = (r[0] & 0xFFFF) | r[1] << 16;
            c->r[dst1] = (r[2] & 0xFFFF) | r[3] << 16;
        }
        return 1;
    case 22:                                            /* BYTEOP2P */
        if (aop > 1) {
            return 0;
        }
        s0 = pair_bytes(c, src0, s, 0);
        s1 = pair_bytes(c, src1, s, 0);
        {
            unsigned round = !aop * 2;
            uint32_t lo = (byte_at(s1, 1) + byte_at(s1, 0) + byte_at(s0, 1) +
                           byte_at(s0, 0) + round) >> 2 & 0xFF;
            uint32_t hi = (byte_at(s1, 3) + byte_at(s1, 2) + byte_at(s0, 3) +
                           byte_at(s0, 2) + round) >> 2 & 0xFF;

            c->r[dst0] = hi << (16 + 8 * hl) | lo << 8 * hl;
        }
        return 1;
    case 23:                                            /* BYTEOP3P */
        if (aop) {
            return 0;
        }
        /* Unlike the other byte ops, no I0/I1 byte alignment: the JPEG IDCT
         * stage runs this with I1 odd and the +128 bias only in R3 (R2 is
         * stale), and with the alignment applied every 8th and 9th pixel of
         * each block is biased wrong. */
        s0 = c->r[src0 + s];
        s1 = c->r[src1 + 1];
        {
            int lo = (int16_t)s0 + (int)byte_at(s1, hl ? 0 : 1);
            int hi = (int16_t)(s0 >> 16) + (int)byte_at(s1, hl ? 2 : 3);

            c->r[dst0] = (uint32_t)sat_u8(hi) << (16 + 8 * hl) |
                         (uint32_t)sat_u8(lo) << 8 * hl;
        }
        return 1;
    case 24:                                            /* BYTEUNPACK */
        if (aop != 1 || hl) {
            return 0;
        }
        if (dst0 == dst1) {
            c->undef = 1;
            return 1;
        }
        {
            uint64_t pair = s ? (uint64_t)c->r[src0] << 32 | c->r[src0 + 1]
                              : (uint64_t)c->r[src0 + 1] << 32 | c->r[src0];

            pair >>= 8 * (c->i[0] & 3);
            c->r[dst0] = (pair & 0xFF) | (pair >> 8 & 0xFF) << 16;
            c->r[dst1] = (pair >> 16 & 0xFF) | (pair >> 24 & 0xFF) << 16;
        }
        return 1;
    }
    return 0;
}

void bfin_dsp32alu(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned aopcde = iw0 & 0x1F, hl = (iw0 >> 5) & 1;
    unsigned src1 = iw1 & 7, src0 = (iw1 >> 3) & 7;
    unsigned dst1 = (iw1 >> 6) & 7, dst0 = (iw1 >> 9) & 7;
    unsigned x = (iw1 >> 12) & 1, s = (iw1 >> 13) & 1, aop = iw1 >> 14;
    uint32_t a = c->r[src0], b = c->r[src1];
    int ov = 0;

    switch (aopcde) {
    case 0: {                                   /* Rd = Rs +|+ Rt ... */
        uint32_t r = vadd(a, b, aop, s, x, &ov);

        c->r[dst0] = r;
        flags16(c, r >> 16, r, 1, ov);
        return;
    }
    case 1: {                                   /* Rd = +|+, Re = -|- */
        if (aop) {
            break;
        }
        uint32_t r1 = vadd(a, b, hl ? 1 : 0, s, x, &ov);
        uint32_t r0 = vadd(a, b, hl ? 2 : 3, s, x, &ov);

        c->r[dst1] = r1;
        c->r[dst0] = r0;
        flags16(c, r0 >> 16, r0, 1, ov);
        return;
    }
    case 2:
    case 3: {                                   /* Rd.h = Rs.h +/- Rt.h */
        uint16_t r = add16(half(a, aop >> 1), half(b, aop & 1), aopcde == 3, s, &ov);

        set_half(&c->r[dst0], hl, r);
        flags16(c, 0, r, 0, ov);
        return;
    }
    case 4:
        if (aop == 2) {
            uint32_t sum = bfin_add32(c, a, b, 0, s);

            c->r[dst0] = bfin_add32(c, a, b, 1, s);
            c->r[dst1] = sum;
            return;
        }
        if (aop < 2) {
            c->r[dst0] = bfin_add32(c, a, b, aop, s);
            return;
        }
        break;
    case 6: {                                   /* MAX/MIN/ABS (V) */
        uint16_t hi, lo;

        if (aop == 2) {
            hi = (int16_t)(a >> 16) == -0x8000 ? 0x7FFF : abs((int16_t)(a >> 16));
            lo = (int16_t)a == -0x8000 ? 0x7FFF : abs((int16_t)a);
        } else if (aop < 2) {
            hi = minmax((int16_t)(a >> 16), (int16_t)(b >> 16), !aop);
            lo = minmax((int16_t)a, (int16_t)b, !aop);
        } else {
            break;
        }
        c->r[dst0] = (uint32_t)hi << 16 | lo;
        flags16(c, hi, lo, 1, 0);
        return;
    }
    case 7: {
        uint32_t r;

        switch (aop) {
        case 0: r = minmax(a, b, 1); break;
        case 1: r = minmax(a, b, 0); break;
        case 2:
            ov = a == 0x80000000u;
            r = ov ? 0x7FFFFFFFu : (int32_t)a < 0 ? -a : a;
            break;
        default:
            ov = a == 0x80000000u;
            r = ov && s ? 0x7FFFFFFFu : -a;
            break;
        }
        c->r[dst0] = r;
        bfin_flags_nz(c, r);
        bfin_flags_v(c, ov);
        return;
    }
    case 8:                                     /* A0 = 0 ... */
        if (s) {
            break;
        }
        switch (aop) {
        case 0: c->a[0] = 0; return;
        case 1: c->a[1] = 0; return;
        case 2: c->a[0] = c->a[1] = 0; return;
        case 3: c->a[0] = c->a[1]; return;
        }
        break;
    case 9:                                     /* A0 = Rs, A0.L = Rs.L ... */
        if (s) {
            c->a[aop >> 1] = (int32_t)a;
        } else if (aop & 1) {
            c->a[aop >> 1] = sext40(((int64_t)(int8_t)a << 32) |
                                    (uint32_t)c->a[aop >> 1]);
        } else {
            uint32_t w = c->a[aop >> 1];

            set_half(&w, hl, half(a, hl));
            c->a[aop >> 1] = (c->a[aop >> 1] & ~0xFFFFFFFFll) | w;
        }
        return;
    case 10:                                    /* Rd.L = A0.X */
        if (aop > 1) {
            break;
        }
        set_half(&c->r[dst0], 0, (int8_t)(c->a[aop] >> 32));
        return;
    case 11:                                    /* A0 += A1 [(W32)] */
        if (aop != 2) {
            break;
        }
        c->a[0] = s ? clamp(c->a[0] + c->a[1], INT32_MIN, INT32_MAX, &ov)
                    : clamp(c->a[0] + c->a[1], ACC_MIN, ACC_MAX, &ov);
        if (ov) {
            c->astat |= AS_AV0 | AS_AV0S;
        }
        return;
    case 14:                                    /* A1 = -A0 ... */
        if (s) {
            break;
        }
        switch (aop) {
        case 0: c->a[0] = sext40(-c->a[0]); return;
        case 1: c->a[1] = sext40(-c->a[0]); return;
        case 2: c->a[0] = sext40(-c->a[1]); return;
        case 3: c->a[1] = sext40(-c->a[1]); return;
        }
        break;
    case 16:                                    /* A0 = ABS A0 ... */
        if (s) {
            break;
        }
        switch (aop) {
        case 0: c->a[0] = c->a[0] < 0 ? sext40(-c->a[0]) : c->a[0]; return;
        case 1: c->a[1] = c->a[0] < 0 ? sext40(-c->a[0]) : c->a[0]; return;
        case 2: c->a[0] = c->a[1] < 0 ? sext40(-c->a[1]) : c->a[1]; return;
        case 3: c->a[1] = c->a[1] < 0 ? sext40(-c->a[1]) : c->a[1]; return;
        }
        break;
    }
    if (!x && byte_ops(c, aopcde, aop, hl, s, dst0, dst1, src0, src1)) {
        return;
    }
    c->undef = 1;
}

/* ---- shifter ------------------------------------------------------------ */

/* An arithmetic or logical shift by a signed count: positive is left. */
static uint32_t shift32(bfin_core *c, uint32_t v, int n, int logical, int sat)
{
    uint32_t r;
    int ov = 0;

    if (n >= 0) {
        r = n > 31 ? 0 : v << n;
        if (!logical && n && ((int32_t)r >> n != (int32_t)v || n > 31)) {
            ov = 1;
            if (sat) {
                r = (int32_t)v < 0 ? 0x80000000u : 0x7FFFFFFFu;
            }
        }
    } else if (logical) {
        r = -n > 31 ? 0 : v >> -n;
    } else {
        r = (int32_t)v >> (-n > 31 ? 31 : -n);
    }
    bfin_flags_nz(c, r);
    bfin_flags_v(c, ov);
    return r;
}

static uint16_t shift16(bfin_core *c, uint16_t v, int n, int logical, int sat)
{
    int32_t r;
    int ov = 0;

    if (n >= 0) {
        r = n > 15 ? 0 : (uint16_t)(v << n);
        if (!logical && n && ((int16_t)r >> n != (int16_t)v || n > 15)) {
            ov = 1;
            if (sat) {
                r = (int16_t)v < 0 ? 0x8000 : 0x7FFF;
            }
        }
    } else if (logical) {
        r = -n > 15 ? 0 : v >> -n;
    } else {
        r = (int16_t)v >> (-n > 15 ? 15 : -n);
    }
    flags16(c, 0, r, 0, ov);
    return r;
}

/* ROT: a 33-bit rotate through CC, positive counts to the left. Every form
 * gives the count as six signed bits, -32 to 31. */
static uint32_t rot32(bfin_core *c, uint32_t v, int n)
{
    uint64_t x = (uint64_t)c->cc << 32 | v;
    unsigned k = n < 0 ? n + 33 : n;

    if (k) {
        x = ((x << k) | (x >> (33 - k))) & 0x1FFFFFFFFull;
    }
    c->cc = x >> 32;
    return x;
}

static int64_t shift_acc(bfin_core *c, int64_t a, int n, int logical)
{
    uint64_t u = (uint64_t)a & 0xFFFFFFFFFFull;

    if (n >= 0) {
        return sext40(n > 39 ? 0 : (int64_t)(u << n));
    }
    if (logical) {
        return sext40(-n > 39 ? 0 : (int64_t)(u >> -n));
    }
    return a >> (-n > 39 ? 39 : -n);
}

static int count_signbits32(uint32_t v)
{
    int n = 0;

    while (n < 31 && ((v >> (30 - n)) & 1) == (v >> 31)) {
        n++;
    }
    return n;
}

static int count_signbits16(uint16_t v)
{
    int n = 0;

    while (n < 15 && ((v >> (14 - n)) & 1) == (v >> 15)) {
        n++;
    }
    return n;
}

static uint32_t extract(bfin_core *c, uint32_t v, uint16_t ctl, int x)
{
    unsigned pos = (ctl >> 8) & 0x1F, len = ctl & 0x1F;
    uint32_t r = pos ? v >> pos : v;

    if (len < 32) {
        r &= (1u << len) - 1;
        if (x && len && (r >> (len - 1)) & 1) {
            r |= ~0u << len;
        }
    }
    bfin_flags_nz(c, r);
    return r;
}

static uint32_t deposit(bfin_core *c, uint32_t bg, uint32_t fg, int x)
{
    unsigned pos = (fg >> 8) & 0x1F, len = fg & 0x1F;
    uint32_t field = fg >> 16, mask = len ? ((1u << len) - 1) : 0;
    uint32_t r;

    if (x && len && ((field >> (len - 1)) & 1)) {
        field |= ~0u << len;
        mask = ~0u;
    }
    field &= mask;
    r = (bg & ~(mask << pos)) | (field << pos);
    bfin_flags_nz(c, r);
    return r;
}

void bfin_dsp32shift(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned sopcde = iw0 & 0x1F;
    unsigned src1 = iw1 & 7, src0 = (iw1 >> 3) & 7, dst0 = (iw1 >> 9) & 7;
    unsigned hls = (iw1 >> 12) & 3, sop = iw1 >> 14;
    uint32_t v = c->r[src1], ctl = c->r[src0];

    switch (sopcde) {
    case 0:
        if (sop == 3) {
            break;
        }
        set_half(&c->r[dst0], hls >> 1,
                 shift16(c, half(v, hls & 1), (int8_t)(ctl << 3) >> 3, sop == 2, sop == 1));
        return;
    case 2:
        if (sop == 3) {
            c->r[dst0] = rot32(c, v, (int8_t)(ctl << 2) >> 2);
        } else {
            c->r[dst0] = shift32(c, v, (int8_t)(ctl << 2) >> 2, sop == 2, sop == 1);
        }
        return;
    case 3:
        if (sop < 2) {
            c->a[hls & 1] = shift_acc(c, c->a[hls & 1], (int8_t)(ctl << 2) >> 2, sop);
            return;
        }
        break;
    case 4: {                                   /* PACK */
        c->r[dst0] = (uint32_t)half(v, sop >> 1) << 16 | half(ctl, sop & 1);
        return;
    }
    case 5: {                                   /* SIGNBITS */
        int n = sop == 0 ? count_signbits32(v) : sop == 1 ? count_signbits16(v)
              : sop == 2 ? count_signbits16(v >> 16) : -1;

        if (n < 0) {
            break;
        }
        set_half(&c->r[dst0], 0, n);
        return;
    }
    case 6:
        if (sop == 3) {                         /* ONES */
            set_half(&c->r[dst0], 0, __builtin_popcount(v));
            return;
        }
        break;
    case 10:                                    /* EXTRACT, DEPOSIT */
        if (sop < 2) {
            c->r[dst0] = extract(c, v, ctl, sop);
        } else {
            c->r[dst0] = deposit(c, v, ctl, sop == 3);
        }
        return;
    }
    c->undef = 1;
}

void bfin_dsp32shiftimm(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned sopcde = iw0 & 0x1F;
    unsigned src1 = iw1 & 7, dst0 = (iw1 >> 9) & 7;
    unsigned hls = (iw1 >> 12) & 3, sop = iw1 >> 14;
    int n = (int8_t)(((iw1 >> 3) & 0x3F) << 2) >> 2;
    uint32_t v = c->r[src1];

    switch (sopcde) {
    case 0:
        if (sop == 3) {
            break;
        }
        set_half(&c->r[dst0], hls >> 1,
                 shift16(c, half(v, hls & 1), n, sop == 2, sop == 1));
        return;
    case 1: {                                   /* (V) */
        if (sop == 3) {
            break;
        }
        uint16_t hi = shift16(c, v >> 16, n, sop == 2, sop == 1);
        uint16_t lo = shift16(c, v, n, sop == 2, sop == 1);

        c->r[dst0] = (uint32_t)hi << 16 | lo;
        return;
    }
    case 2:
        if (sop == 3) {
            c->r[dst0] = rot32(c, v, n);
        } else {
            c->r[dst0] = shift32(c, v, n, sop == 2, sop == 1);
        }
        return;
    case 3:
        if (sop < 2) {
            c->a[hls & 1] = shift_acc(c, c->a[hls & 1], n, sop);
            return;
        }
        break;
    }
    c->undef = 1;
}

/* The 32-bit form (sopcde 2), which bundles use most. */
void bfin_dsp32shiftimm32(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    unsigned src1 = iw1 & 7, dst0 = (iw1 >> 9) & 7, sop = iw1 >> 14;
    int n = (int8_t)(((iw1 >> 3) & 0x3F) << 2) >> 2;
    uint32_t v = c->r[src1];

    if (sop == 3) {
        c->r[dst0] = rot32(c, v, n);
    } else {
        c->r[dst0] = shift32(c, v, n, sop == 2, sop == 1);
    }
}

/* ---- multiply/accumulate ------------------------------------------------ */

static int mm_unsigned(unsigned mmod)
{
    return mmod == MM_FU || mmod == MM_TFU || mmod == MM_IU;
}

static int mm_integer(unsigned mmod)
{
    return mmod == MM_IS || mmod == MM_ISS2 || mmod == MM_IH || mmod == MM_IU;
}

static int64_t product(uint16_t a, uint16_t b, unsigned mmod, int mixed, int *ov)
{
    int64_t pa = mm_unsigned(mmod) ? a : (int16_t)a;
    int64_t pb = mm_unsigned(mmod) || mixed ? b : (int16_t)b;
    int64_t p = pa * pb;

    if (!mm_unsigned(mmod) && !mm_integer(mmod) && !mixed) {
        if (a == 0x8000 && b == 0x8000) {
            *ov = 1;
            return 0x7FFFFFFF;
        }
        p <<= 1;
    }
    return p;
}

/* Round a value at bit 16: unbiased (to even) unless ASTAT.RND_MOD. */
static int64_t round16(bfin_core *c, int64_t v)
{
    if (c->astat & AS_RND_MOD) {
        return (v + 0x8000) >> 16;
    }
    if ((v & 0x1FFFF) == 0x8000) {
        return v >> 16;
    }
    return (v + 0x8000) >> 16;
}

static uint16_t extract16(bfin_core *c, int64_t v, unsigned mmod, int *ov)
{
    switch (mmod) {
    case MM_IS:   return clamp(v, -0x8000, 0x7FFF, ov);
    case MM_ISS2: return clamp(v * 2, -0x8000, 0x7FFF, ov);
    case MM_IU:   return clamp(v, 0, 0xFFFF, ov);
    case MM_FU:   return clamp(round16(c, v), 0, 0xFFFF, ov);
    case MM_TFU:  return clamp(v >> 16, 0, 0xFFFF, ov);
    case MM_T:    return clamp(v >> 16, -0x8000, 0x7FFF, ov);
    case MM_S2RND: return clamp(round16(c, v * 2), -0x8000, 0x7FFF, ov);
    default:      return clamp(round16(c, v), -0x8000, 0x7FFF, ov);
    }
}

static uint32_t extract32(int64_t v, unsigned mmod, int *ov)
{
    if (mm_unsigned(mmod)) {
        return clamp(v, 0, 0xFFFFFFFFll, ov);
    }
    if (mmod == MM_S2RND || mmod == MM_ISS2) {
        v *= 2;
    }
    return clamp(v, INT32_MIN, INT32_MAX, ov);
}

static int64_t accumulate(bfin_core *c, int n, unsigned op, int64_t p,
                          unsigned mmod)
{
    int64_t a = op == 0 ? p : op == 1 ? c->a[n] + p : c->a[n] - p;
    int ov = 0;

    if (mmod == MM_W32) {
        a = clamp(a, INT32_MIN, INT32_MAX, &ov);
    } else if (mm_unsigned(mmod)) {
        a = clamp(a, 0, ACC_MAX, &ov);
    } else {
        a = clamp(a, ACC_MIN, ACC_MAX, &ov);
    }
    if (ov) {
        c->astat |= n ? AS_AV1 | AS_AV1S : AS_AV0 | AS_AV0S;
    }
    return a;
}

/* dsp32mac (mult = 0) and dsp32mult (mult = 1) share one layout: MAC1 feeds
 * A1 and the high half (or the odd register of a pair), MAC0 A0 and the low
 * half (or the even register). */
void bfin_dsp32mac(bfin_core *c, uint16_t iw0, uint16_t iw1, int mult)
{
    unsigned op1 = iw0 & 3, w1 = (iw0 >> 2) & 1, pair = (iw0 >> 3) & 1;
    unsigned mm = (iw0 >> 4) & 1, mmod = (iw0 >> 5) & 0xF;
    unsigned src1 = iw1 & 7, src0 = (iw1 >> 3) & 7, dst = (iw1 >> 6) & 7;
    unsigned h10 = (iw1 >> 9) & 1, h00 = (iw1 >> 10) & 1;
    unsigned op0 = (iw1 >> 11) & 3, w0 = (iw1 >> 13) & 1;
    unsigned h11 = (iw1 >> 14) & 1, h01 = (iw1 >> 15) & 1;
    uint32_t a = c->r[src0], b = c->r[src1];
    uint32_t out[2] = { c->r[dst], c->r[pair ? dst + 1 : dst] };
    int ov = 0;

    if (pair && (dst & 1)) {
        c->undef = 1;
        return;
    }
    for (int n = 1; n >= 0; n--) {
        unsigned op = n ? op1 : op0, w = n ? w1 : w0;
        int64_t v;

        if (mult) {
            if (!w) {
                continue;
            }
            v = product(half(a, n ? h01 : h00), half(b, n ? h11 : h10), mmod,
                        n && mm, &ov);
        } else {
            if (op != 3) {
                c->a[n] = accumulate(c, n, op, product(half(a, n ? h01 : h00),
                                     half(b, n ? h11 : h10), mmod, n && mm, &ov),
                                     mmod);
            }
            if (!w) {
                continue;
            }
            v = c->a[n];
        }
        if (pair) {
            out[n] = extract32(v, mmod, &ov);
        } else {
            set_half(&out[0], n, extract16(c, v, mmod, &ov));
        }
    }
    if (pair) {
        if (w0) {
            c->r[dst] = out[0];
        }
        if (w1) {
            c->r[dst + 1] = out[1];
        }
    } else if (w0 || w1) {
        c->r[dst] = out[0];
    }
    if (w0 || w1) {
        bfin_flags_v(c, ov);
    }
}
