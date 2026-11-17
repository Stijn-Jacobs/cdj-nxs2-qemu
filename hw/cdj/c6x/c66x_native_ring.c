/* SPDX-License-Identifier: GPL-2.0-or-later */
/* C66x native kernels: NXS2 buffer movement. */
#include "c66x_native_int.h"

/* ------------------------------------------------------------------------ */
/* ring-descriptor walk, one step per pass (NXS2: 0x800091C4)              */

/* The loop walks B2 steps through a ring of four 40-byte descriptors (A5 the
 * current one, B5 its index, B6 the sample position in it, counting down). A
 * step moves to the previous descriptor when the position runs out (into 294
 * more samples), and once more when that descriptor's word +28 is 1 and its
 * word +32 is not above the position (to position 293). The pass covers steps
 * that end on a descriptor whose word +28 is non-zero; the other branch is
 * left to the interpreter. 37 cycles a step, measured on the interpreter. */

static int ring_walk_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;
    uint32_t a5 = r[A(5)], b5 = r[B(5)], b6 = r[B(6)];
    uint32_t a0 = (int32_t)b6 < 1, b0 = 0, b1 = 0;

    b6 -= 1;
    if (a0) {
        b6 += 294;
        a5 -= 40;
        b0 = (int32_t)b5 < 1;
        b5 -= 1;
    }
    if (b0) {
        a5 += 160;
        b5 += 4;
    }
    const uint8_t *d = ram_ptr(c, a5 + 28, 8);
    if (!d)
        return 0;
    uint32_t a6 = ld32(d + 4);
    uint32_t a3 = ld32(d) == 1;
    uint32_t b7 = !((int32_t)b6 > (int32_t)a6);
    b0 = b7 & a3;
    if (b0) {
        a5 -= 40;
        b6 = 293;
        b1 = (int32_t)b5 < 1;
        b5 -= 1;
    }
    if (b1) {
        a5 += 160;
        b5 += 4;
    }
    d = ram_ptr(c, a5 + 28, 4);
    if (!d || !ld32(d))
        return 0;
    r[A(0)] = ld32(d);
    r[A(3)] = a3;
    r[A(5)] = a5;
    r[A(6)] = a6;
    r[B(0)] = b0;
    r[B(1)] = b1;
    r[B(5)] = b5;
    r[B(6)] = b6;
    r[B(7)] = b7;
    r[B(2)] -= 1;
    s->pc = s->at + (r[B(2)] ? 0x18 : 0xC0);
    s->last_pc = s->at + 0xBC;
    s->cycles = 37;
    s->packets = 23;
    s->insns = 30;
    return 1;
}

/* the MVK/MVKH pair of the prologue's descriptor ring, which the pass does not use */
static const native_wild ring_walk_wild[] = { WILD(0x08, MVK_CST), WILD(0x0C, MVK_CST) };

const native_kernel native_ring_walk = KERNEL("ring_walk", 0x0380142a, 0xC0, 0xc62f9cdee2901fc9ull, 0x18, 0xC0, ring_walk_wild, ring_walk_pass);

/* ------------------------------------------------------------------------ */
/* ring copy, one 8-byte frame per pass (NXS2: 0x800093E0, loop at 0x80009430) */

/* The loop copies B1 frames from B4 to B5 while stepping the read position
 * through the descriptor ring (A4 + 40 A18; B7 the position in it, A7 the
 * descriptors left, *A19 the ring length). The second word of a frame is
 * copied at the top of a pass and the first word of the next frame in the
 * back-branch's delay slot. The pass covers the steps that stay inside a
 * descriptor; a wrap to the next one, or running out of descriptors, is left
 * to the interpreter, as is a frame stored over a word the pass reads. */

static int ring_copy_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg;
    uint32_t b4 = r[B(4)], b5 = r[B(5)], b7 = r[B(7)] + 1, a18 = r[A(18)], a7 = r[A(7)], a19 = r[A(19)];
    uint32_t a4 = r[A(4)], b6 = r[B(6)];

    if ((int32_t)b7 >= 294)
        return 0;
    const uint8_t *src = ram_ptr(c, b4 + 4, 4), *dst = ram_ptr(c, b5 + 4, 4);
    if (!src || !dst)
        return 0;
    uint32_t w1 = ld32(src);
    if ((b7 | a18) == 0)
        b4 = b6;
    else
        b4 += 8;
    uint32_t desc = a4 + 40 * a18;
    const uint8_t *d = ram_ptr(c, desc + 28, 8), *len = ram_ptr(c, a19, 4);
    if (!d || !len || same_word(b5 + 4, desc + 28) || same_word(b5 + 4, desc + 32) || same_word(b5 + 4, a19))
        return 0;
    uint32_t a0 = (ld32(d + 4) == b7) & (ld32(d) == 0);
    uint32_t b9 = r[B(9)];
    if (a0) {
        a18 = (a18 + 1) & (ld32(len) - 1);
        int64_t off = (int64_t)(int16_t)2352 * (int32_t)a18;
        b9 = (uint32_t)((uint64_t)off >> 32);
        b4 = b6 + (uint32_t)off;
        b7 = 0;
        a7 -= 1;
        if (!a7)
            return 0;
    }
    desc = a4 + 40 * a18;
    d = ram_ptr(c, desc + 28, 4);
    if (!d || same_word(b5 + 4, desc + 28))
        return 0;
    uint32_t a8 = ld32(d) == 1, b8 = b7 == 0;
    if (b8 & a8)
        return 0;
    uint32_t b1 = r[B(1)] - 1;
    int64_t m40 = (int64_t)40 * (int32_t)r[A(18)];

    put_st(s, b5 + 4, w1, s->at + 0x18);
    if (b1) {
        const uint8_t *next = ram_ptr(c, b4, 4);
        if (!next || same_word(b4, b5 + 4) || !ram_ptr(c, b5 + 8, 4))
            return 0;
        b8 = ld32(next);
        put_st(s, b5 + 8, b8, s->at + 0x14C);
    }
    r[A(0)] = a0;
    r[A(1)] = a0 ? a7 : 1;
    r[A(3)] = desc;
    r[A(5)] = 294;
    r[A(7)] = a7;
    r[A(8)] = a8;
    r[A(9)] += 1;
    r[A(16)] = (uint32_t)m40;
    r[A(17)] = (uint32_t)((uint64_t)m40 >> 32);
    r[A(18)] = a18;
    r[B(0)] = 0;
    r[B(1)] = b1;
    r[B(4)] = b4;
    r[B(5)] = b5 + 8;
    r[B(7)] = b7;
    r[B(8)] = b8;
    r[B(9)] = b9;
    s->pc = s->at + (b1 ? 0x10 : 0x150);
    s->branch_block = 4;
    s->last_pc = s->at + 0x14C;
    s->cycles = 85;
    s->packets = 56;
    s->insns = 60;
    return 1;
}

static const native_wild ring_copy_wild[] = { };

const native_kernel native_ring_copy = KERNEL("ring_copy", 0x041002e6, 0x160, 0xfd497173d8a6ba9full, 0x10, 0x150, ring_copy_wild, ring_copy_pass);

/* ------------------------------------------------------------------------ */
/* frame FIFO drop, one call per pass (NXS2: 0x80007220)                    */

/* drop(count A4, frames B4, fifo A6): the FIFO's words are its length, the
 * frames it holds, and two counters. Dropping count frames from the front
 * shifts the remaining length - count 8-byte frames down with a ldndw/stndw
 * SPLOOP (ii 6) and takes count off the frames held and off the last
 * counter; the first is stored back as it was. A call with nothing to drop,
 * too few frames or nothing left to shift is left to the interpreter, as is
 * a FIFO header inside the frames. 6 cycles a frame plus 59, measured on the
 * interpreter. */

static int fifo_drop_pass(c66x_core *c, native_state *s)
{
    uint32_t *r = s->reg, count = r[A(4)], dst = r[B(4)], fifo = r[A(6)];
    const uint8_t *hp = ram_ptr(c, fifo, 16);

    if (!hp || (int32_t)count <= 0)
        return 0;
    uint32_t len = ld32(hp), held = ld32(hp + 4), first = ld32(hp + 8), last = ld32(hp + 12);
    uint32_t n = len - count, src = dst + 8 * count;
    if ((int32_t)held < (int32_t)count || (int32_t)n <= 0)
        return 0;
    const uint8_t *from = ram_ptr(c, dst, 8 * (count + n));
    if (!from || (fifo + 16 > dst && fifo < src + 8 * n))
        return 0;

    s->mv.dst = dst;
    s->mv.src = src;
    s->mv.len = 8 * n;
    s->mv.pc = s->at + 0x50;
    put_st(s, fifo + 4, held - count, s->at + 0x28);
    put_st(s, fifo + 12, last - count, s->at + 0x64);
    put_st(s, fifo + 8, first, s->at + 0x6C);
    r[A(0)] = 1;
    r[A(3)] = first;
    r[A(4)] = ld32(from + 8 * (count + n - 1));
    r[A(5)] = ld32(from + 8 * (count + n - 1) + 4);
    r[A(6)] = dst + 8 * n;
    r[A(7)] = count;
    r[A(8)] = fifo;
    r[A(9)] = count + first;
    r[B(4)] = n - 1;
    s->cr[CR_ILC] = 0;
    s->sploops = 1;
    s->pc = r[B(3)];
    s->last_pc = s->at + 0x70;
    s->cycles = 6 * n + 59;
    s->packets = 28;
    s->insns = 2 * n + 31;
    return 1;
}

static const native_wild fifo_drop_wild[] = { };

const native_kernel native_fifo_drop = KERNEL("fifo_drop", 0xd24e0746, 0x80, 0x8e1de9d33745c1fbull, 0, RETURNS, fifo_drop_wild, fifo_drop_pass);
