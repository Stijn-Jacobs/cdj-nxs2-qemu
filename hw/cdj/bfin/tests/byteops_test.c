/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The byte-oriented DSP32 ALU ops (aopcde 18 and 20-24) and A0 += A1, run one at a time on
 * a scratch core against results worked out by hand from the Programming
 * Reference.
 */
#include "bfin_priv.h"
#include <stdio.h>

static uint8_t ram[4096];
static int failures;

static uint32_t bus_read(void *opaque, uint32_t addr, unsigned size)
{
    return 0;
}

static void bus_write(void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
}

static uint16_t alu0(unsigned aopcde, unsigned hl, unsigned m)
{
    return 0xC400 | m << 11 | hl << 5 | aopcde;
}

static uint16_t alu1(unsigned aop, unsigned s, unsigned dst0, unsigned dst1,
                     unsigned src0, unsigned src1)
{
    return aop << 14 | s << 13 | dst0 << 9 | dst1 << 6 | src0 << 3 | src1;
}

static bfin_core *new_core(void)
{
    static const bfin_bus bus = { NULL, bus_read, bus_write };
    bfin_core *c = bfin_new(&bus);

    bfin_map_ram(c, 0, sizeof ram, ram);
    return c;
}

static void load(bfin_core *c, const uint16_t *words, unsigned n)
{
    memcpy(ram, words, n * 2);
    bfin_reset(c, 0);
}

static int run(bfin_core *c)
{
    uint64_t n;

    return bfin_step(c, 1, &n) != BFIN_STOP_UNDEF;
}

static void expect(const char *name, bfin_core *c, unsigned reg, uint32_t want)
{
    uint32_t got = bfin_get_reg(c, 0, reg);

    if (got != want) {
        printf("FAIL %s: R%u = 0x%08x, want 0x%08x\n", name, reg, got, want);
        failures++;
    }
}

static void expect_acc(const char *name, bfin_core *c, int n, uint32_t want)
{
    uint32_t w = bfin_get_reg(c, 4, 1 + 2 * n), x = bfin_get_reg(c, 4, 2 * n);

    if (w != want || x != 0) {
        printf("FAIL %s: A%d = 0x%02x:%08x, want 0x00:%08x\n", name, n, x, w, want);
        failures++;
    }
}

static void insn(bfin_core *c, uint16_t iw0, uint16_t iw1)
{
    uint16_t w[2] = { iw0, iw1 };

    load(c, w, 2);
}

static void set_r(bfin_core *c, unsigned reg, uint32_t v)
{
    bfin_set_reg(c, 0, reg, v);
}

/* GNU Blackfin simulator decode_dsp32alu_0: aopcde8/aop3/x0/HL0
 * copies AX+AW from !s to s without touching ASTAT. These are assembled
 * architectural instructions, including the JPEG trap C408 E03F. */
static void accumulator_copy(bfin_core *c)
{
    const uint64_t values[] = {0, 1, 0x7fffffffffull, 0x8000000000ull,
        0xff12345678ull, 0x0112345678ull, 0x0080000000ull};
    const uint32_t flags[] = {0, UINT32_MAX, 0xa55a5aa5};
    for (unsigned dst = 0; dst < 2; dst++) {
        for (unsigned v = 0; v < sizeof(values)/sizeof(values[0]); v++) {
            for (unsigned f = 0; f < sizeof(flags)/sizeof(flags[0]); f++) {
                insn(c, 0xc408, dst ? 0xe03f : 0xc03f);
                unsigned src = !dst;
                bfin_set_reg(c, 4, 2 * src, values[v] >> 32);
                bfin_set_reg(c, 4, 2 * src + 1, values[v]);
                bfin_set_reg(c, 4, 2 * dst, 0x42);
                bfin_set_reg(c, 4, 2 * dst + 1, 0xdeadbeef);
                for (unsigned r = 0; r < 8; r++) {set_r(c, r, 0x12340000 + r);}
                c->astat = flags[f];
                uint64_t before = bfin_cycles(c);
                if (!run(c)) {printf("FAIL A%u copy trapped\n", dst); failures++;}
                uint32_t x = (int32_t)(int8_t)(values[v] >> 32);
                for (unsigned a = 0; a < 2; a++) {
                    if (bfin_get_reg(c, 4, 2 * a) != x ||
                        bfin_get_reg(c, 4, 2 * a + 1) != (uint32_t)values[v]) {
                        printf("FAIL copy A%u: 40-bit value/source changed\n", dst); failures++;
                    }
                }
                if (c->astat != flags[f]) {printf("FAIL copy changed ASTAT\n"); failures++;}
                for (unsigned r = 0; r < 8; r++) {expect("copy data registers", c, r, 0x12340000 + r);}
                if (c->pc != 4 || bfin_cycles(c) - before != 1) {
                    printf("FAIL copy instruction extent/cycle\n"); failures++;
                }
            }
        }
        // Reserved copy forms are not silently treated as clear/move.
        insn(c, 0xc408, (dst ? 0xe03f : 0xc03f) | 0x1000);
        if (run(c)) {printf("FAIL copy X=1 accepted\n"); failures++;}
        insn(c, 0xc428, dst ? 0xe03f : 0xc03f);
        if (run(c)) {printf("FAIL copy HL=1 accepted\n"); failures++;}
    }
    // The three preexisting clear forms retain their exact accumulator effects.
    for (unsigned aop = 0; aop < 3; aop++) {
        insn(c, 0xc408, (aop << 14) | 0x3f);
        c->a[0] = 0x123456789a; c->a[1] = -0x123456789a; c->astat = 0xa55a5aa5;
        if (!run(c)) {printf("FAIL clear form trapped\n"); failures++;}
        if (c->a[0] != (aop == 1 ? 0x123456789a : 0) ||
            c->a[1] != (aop == 0 ? -0x123456789a : 0) || c->astat != 0xa55a5aa5) {
            printf("FAIL clear form changed effects\n"); failures++;
        }
    }
}

int main(void)
{
    bfin_core *c = new_core();
    accumulator_copy(c);

    /* R4 = BYTEPACK (R1, R2) */
    insn(c, alu0(24, 0, 0), alu1(0, 0, 4, 0, 1, 2));
    set_r(c, 1, 0xAABBCCDD);
    set_r(c, 2, 0x11223344);
    run(c);
    expect("BYTEPACK", c, 4, 0x2244BBDD);

    /* (R5, R4) = BYTEUNPACK R3:2 [(R)], byte offset from I0 */
    insn(c, alu0(24, 0, 0), alu1(1, 0, 4, 5, 2, 0));
    set_r(c, 2, 0x04030201);
    set_r(c, 3, 0x08070605);
    run(c);
    expect("BYTEUNPACK r4", c, 4, 0x00020001);
    expect("BYTEUNPACK r5", c, 5, 0x00040003);
    insn(c, alu0(24, 0, 0), alu1(1, 0, 4, 5, 2, 0));
    set_r(c, 2, 0x04030201);
    set_r(c, 3, 0x08070605);
    bfin_set_reg(c, 2, 0, 1);
    run(c);
    expect("BYTEUNPACK I0=1 r4", c, 4, 0x00030002);
    expect("BYTEUNPACK I0=1 r5", c, 5, 0x00050004);
    insn(c, alu0(24, 0, 0), alu1(1, 1, 4, 5, 2, 0));
    set_r(c, 2, 0x04030201);
    set_r(c, 3, 0x08070605);
    run(c);
    expect("BYTEUNPACK (R) r4", c, 4, 0x00060005);
    expect("BYTEUNPACK (R) r5", c, 5, 0x00080007);

    /* R4 = BYTEOP1P (R1:0, R3:2) [(T)]: average of bytes, rounded up or truncated */
    insn(c, alu0(20, 0, 0), alu1(0, 0, 4, 0, 0, 2));
    set_r(c, 0, 0x10203041);
    set_r(c, 2, 0x20406080);
    run(c);
    expect("BYTEOP1P", c, 4, 0x18304861);
    insn(c, alu0(20, 0, 0), alu1(1, 0, 4, 0, 0, 2));
    set_r(c, 0, 0x10203041);
    set_r(c, 2, 0x20406080);
    run(c);
    expect("BYTEOP1P (T)", c, 4, 0x18304860);

    /* (R5, R4) = BYTEOP16P / BYTEOP16M (R1:0, R3:2) */
    insn(c, alu0(21, 0, 0), alu1(0, 0, 4, 5, 0, 2));
    set_r(c, 0, 0x04030201);
    set_r(c, 2, 0x40302010);
    run(c);
    expect("BYTEOP16P r4", c, 4, 0x00220011);
    expect("BYTEOP16P r5", c, 5, 0x00440033);
    insn(c, alu0(21, 0, 0), alu1(1, 0, 4, 5, 0, 2));
    set_r(c, 0, 0x04030201);
    set_r(c, 2, 0x40302010);
    run(c);
    expect("BYTEOP16M r4", c, 4, 0xFFE2FFF1);
    expect("BYTEOP16M r5", c, 5, 0xFFC4FFD3);

    /* SAA (R1:0, R3:2): |difference| of byte lanes added into the accumulator halves */
    insn(c, alu0(18, 0, 0), alu1(0, 0, 0, 0, 0, 2));
    set_r(c, 0, 0x0A141E28);
    set_r(c, 2, 0x050A0F14);
    run(c);
    expect_acc("SAA", c, 0, 0x000F0014);
    expect_acc("SAA", c, 1, 0x0005000A);
    insn(c, alu0(18, 0, 0), alu1(0, 0, 0, 0, 0, 2));
    set_r(c, 0, 0x0A141E28);
    set_r(c, 2, 0x050A0F14);
    bfin_set_reg(c, 4, 1, 0x0001FFF0);
    run(c);
    expect_acc("SAA saturates", c, 0, 0x0010FFFF);

    /* R4 = BYTEOP2P (R1:0, R3:2) (RNDL / RNDH / TL) */
    insn(c, alu0(22, 0, 0), alu1(0, 0, 4, 0, 0, 2));
    set_r(c, 0, 0x04030201);
    set_r(c, 2, 0x40302010);
    run(c);
    expect("BYTEOP2P (RNDL)", c, 4, 0x001E000D);
    insn(c, alu0(22, 1, 0), alu1(0, 0, 4, 0, 0, 2));
    set_r(c, 0, 0x04030201);
    set_r(c, 2, 0x40302010);
    run(c);
    expect("BYTEOP2P (RNDH)", c, 4, 0x1E000D00);
    insn(c, alu0(22, 0, 0), alu1(1, 0, 4, 0, 0, 2));
    set_r(c, 0, 0x04030201);
    set_r(c, 2, 0x40302010);
    run(c);
    expect("BYTEOP2P (TL)", c, 4, 0x001D000C);

    /* R4 = BYTEOP3P (R1:0, R3:2) (LO / HI): signed halves plus the bytes of R3, clamped to 0..255 */
    insn(c, alu0(23, 0, 0), alu1(0, 0, 4, 0, 0, 2));
    set_r(c, 0, 0x00FF0010);
    set_r(c, 3, 0x01020304);
    run(c);
    expect("BYTEOP3P (LO)", c, 4, 0x00FF0013);
    insn(c, alu0(23, 1, 0), alu1(0, 0, 4, 0, 0, 2));
    set_r(c, 0, 0x00FF0010);
    set_r(c, 3, 0x01020304);
    run(c);
    expect("BYTEOP3P (HI)", c, 4, 0xFF001400);
    insn(c, alu0(23, 0, 0), alu1(0, 0, 4, 0, 0, 2));
    set_r(c, 0, 0x0000FFF0);
    set_r(c, 3, 0x00000300);
    run(c);
    expect("BYTEOP3P clamps at 0", c, 4, 0x00000000);

    /* DISALGNEXCPT is a no-op */
    insn(c, alu0(18, 0, 0), alu1(3, 0, 0, 0, 0, 0));
    if (!run(c)) {
        printf("FAIL DISALGNEXCPT rejected\n");
        failures++;
    }

    /* an aopcde 24 form that does not exist stays unimplemented */
    insn(c, alu0(24, 0, 0), alu1(2, 0, 4, 0, 0, 2));
    if (run(c)) {
        printf("FAIL aopcde 24 aop 2 accepted\n");
        failures++;
    }

    /* the bundle the display firmware stops on: (R1, R0) = BYTEUNPACK R1:0 || load || NOP */
    {
        uint16_t w[4] = { 0xCC18, 0x4040, 0xA1C3, 0x0000 };

        load(c, w, 4);
        set_r(c, 0, 0x04030201);
        set_r(c, 1, 0x08070605);
        if (!run(c)) {
            printf("FAIL firmware bundle rejected\n");
            failures++;
        }
        expect("bundle r0", c, 0, 0x00020001);
        expect("bundle r1", c, 1, 0x00040003);
    }

    /* A0 += A1, plain and (W32); the instruction the display firmware stops on while drawing a cue */
    insn(c, 0xC40B, 0x803F);
    bfin_set_reg(c, 4, 1, 100);
    bfin_set_reg(c, 4, 3, 23);
    run(c);
    expect_acc("A0 += A1", c, 0, 123);
    insn(c, 0xC40B, 0xA03F);
    bfin_set_reg(c, 4, 1, 0x7FFFFFF0);
    bfin_set_reg(c, 4, 3, 0x100);
    run(c);
    expect_acc("A0 += A1 (W32)", c, 0, 0x7FFFFFFF);

    bfin_free(c);
    if (!failures) {
        printf("byteops: all passed\n");
    }
    return failures != 0;
}
