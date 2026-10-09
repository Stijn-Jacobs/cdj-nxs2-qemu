/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Instruction semantics, one instruction class at a time: each case loads a
 * few bytes at the start of a scratch code page, sets the registers it needs
 * and compares the registers, flags and memory against what the Renesas
 * M16C/60, M16C/20 Series Software Manual says the instruction does. The
 * cases that matter most are the ones an instruction-set description is
 * easiest to get wrong: carry as "no borrow" after SUB and CMP, the bit,base
 * operand with An, the byte multiply into R1, the 2-byte PUSHM registers and
 * the 4-byte interrupt frame.
 */
#include "m16c_priv.h"
#include <stdio.h>

#define CODE 0xC0000u

static uint8_t low[0x10000];
static uint8_t high[0x2000];
static uint8_t vectors[0x400];
static m16c_core *cpu;
static int failures, checks, acks;

static uint32_t bus_read(void *opaque, uint32_t addr, unsigned size)
{
    return 0;
}

static void bus_write(void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
}

static void bus_ack(void *opaque, unsigned vec)
{
    acks++;
}

static void init(void)
{
    static const m16c_bus bus = { NULL, bus_read, bus_write, bus_ack };

    if (!cpu) {
        cpu = m16c_new(&bus);
        m16c_map_ram(cpu, 0, sizeof(low), low);
        m16c_map_ram(cpu, CODE, sizeof(high), high);
        m16c_map_ram(cpu, 0xFFC00, sizeof(vectors), vectors);
    }
    memset(low, 0, sizeof(low));
    memset(high, 0x04, sizeof(high));       /* NOP */
    memset(vectors, 0, sizeof(vectors));
    m16c_reset(cpu);
    m16c_set_reg(cpu, M16C_ISP, 0x8000);
    m16c_set_reg(cpu, M16C_USP, 0x9000);
    m16c_set_reg(cpu, M16C_PC, CODE);
    acks = 0;
}

static void load(const uint8_t *bytes, unsigned n)
{
    memcpy(high, bytes, n);
}

static void step(unsigned n)
{
    uint64_t done;

    m16c_step(cpu, n, &done);
}

/* Loads the bytes and runs one instruction. */
#define RUN(...) do { \
        static const uint8_t b_[] = { __VA_ARGS__ }; \
        load(b_, sizeof(b_)); \
        m16c_set_reg(cpu, M16C_PC, CODE); \
        step(1); \
    } while (0)

#define R(r)      m16c_get_reg(cpu, r)
#define SET(r, v) m16c_set_reg(cpu, r, v)
#define M8(a)     m16c_read(cpu, a, 1)
#define M16(a)    m16c_read(cpu, a, 2)

static void check(const char *name, const char *what, uint32_t got,
                  uint32_t want)
{
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %s: %s = 0x%x, want 0x%x\n", name, what, got, want);
    }
}

#define EQ(name, what, got, want) check(name, what, got, want)

/* The C, Z, S and O flags as a four-bit value in that order. */
static unsigned flags(void)
{
    unsigned f = R(M16C_FLG);

    return (f & M16C_C ? 8 : 0) | (f & M16C_Z ? 4 : 0) |
           (f & M16C_S ? 2 : 0) | (f & M16C_O ? 1 : 0);
}

#define C_ 8
#define Z_ 4
#define S_ 2
#define O_ 1

static void test_mov(void)
{
    init();
    SET(M16C_R1, 0x1200);
    RUN(0xd8, 0xf2);                        /* MOV.B:Q #-1,R1L */
    EQ("MOV.B:Q", "R1", R(M16C_R1), 0x12FF);
    EQ("MOV.B:Q", "flags", flags(), S_);

    init();
    SET(M16C_R0, 0x1234);
    RUN(0xb3);                              /* MOV.B:Z #0,R0H */
    EQ("MOV.B:Z", "R0", R(M16C_R0), 0x0034);
    EQ("MOV.B:Z", "flags", flags(), Z_);

    init();
    low[0x4297] = 0x5A;
    RUN(0x72, 0xf2, 0x97, 0x42);            /* MOV.B:G 0x4297,R1L */
    EQ("MOV.B:G", "R1", R(M16C_R1), 0x5A);

    init();
    RUN(0xa2, 0x99, 0x3c);                  /* MOV.W:S #0x3c99,A0 */
    EQ("MOV.W:S", "A0", R(M16C_A0), 0x3c99);

    init();
    SET(M16C_A0, 0xFFFF);
    RUN(0xe2, 0x80);                        /* MOV.B:S #0x80,A0 */
    EQ("MOV.B:S A0", "A0 zero-extended", R(M16C_A0), 0x0080);

    init();
    SET(M16C_FB, 0x4000);
    memset(low + 0x3FFD, 0xFF, 3);
    RUN(0xd9, 0x0b, 0xfd);                  /* MOV.W:Q #0,-3[FB] */
    EQ("MOV.W:Q FB", "mem", M16(0x3FFD), 0);
    EQ("MOV.W:Q FB", "neighbour", M8(0x3FFF), 0xFF);

    init();
    SET(M16C_FB, 0x1000);
    RUN(0xeb, 0x4b, 0xf0);                  /* MOVA -16[FB],A0 */
    EQ("MOVA", "A0", R(M16C_A0), 0x0FF0);

    init();
    SET(M16C_R0, 0x12A5);
    RUN(0x7c, 0x81);                        /* MOVLL R0L,R0H */
    EQ("MOVLL", "R0", R(M16C_R0), 0x15A5);
}

static void test_stack(void)
{
    init();
    SET(M16C_A0, 0xBEEF);
    RUN(0xc2);                              /* PUSH.W:S A0 */
    EQ("PUSH.W:S", "SP", R(M16C_SP), 0x7FFE);
    EQ("PUSH.W:S", "mem", M16(0x7FFE), 0xBEEF);
    RUN(0xda);                              /* POP.W:S A1 */
    EQ("POP.W:S", "A1", R(M16C_A1), 0xBEEF);
    EQ("POP.W:S", "SP", R(M16C_SP), 0x8000);

    init();
    SET(M16C_R0, 0x12AB);
    RUN(0x82);                              /* PUSH.B:S R0L */
    EQ("PUSH.B:S", "SP", R(M16C_SP), 0x7FFF);
    EQ("PUSH.B:S", "mem", M8(0x7FFF), 0xAB);
    RUN(0x9a);                              /* POP.B:S R0H */
    EQ("POP.B:S", "R0", R(M16C_R0), 0xAB00 | 0xAB);
}

static void test_pushm(void)
{
    init();
    SET(M16C_R0, 0x1111);
    SET(M16C_R2, 0x2222);
    RUN(0xec, 0xa0);                        /* PUSHM R0,R2 */
    EQ("PUSHM", "SP", R(M16C_SP), 0x7FFC);
    EQ("PUSHM R0 on top", "mem[SP]", M16(0x7FFC), 0x1111);
    EQ("PUSHM R2 below", "mem[SP+2]", M16(0x7FFE), 0x2222);

    /* A0, A1, SB and FB are 16 bits: two bytes each, not three. */
    init();
    SET(M16C_A0, 0xDDDD);
    SET(M16C_A1, 0xCCCC);
    SET(M16C_SB, 0xBBBB);
    SET(M16C_FB, 0xAAAA);
    RUN(0xec, 0x0f);                        /* PUSHM A0,A1,SB,FB */
    EQ("PUSHM A/SB/FB", "SP", R(M16C_SP), 0x7FF8);
    EQ("PUSHM A0", "mem[SP]", M16(0x7FF8), 0xDDDD);
    EQ("PUSHM A1", "mem[SP+2]", M16(0x7FFA), 0xCCCC);
    EQ("PUSHM SB", "mem[SP+4]", M16(0x7FFC), 0xBBBB);
    EQ("PUSHM FB", "mem[SP+6]", M16(0x7FFE), 0xAAAA);

    init();
    SET(M16C_SP, 0x7000);
    low[0x7000] = 0x34; low[0x7001] = 0x12;
    low[0x7002] = 0x78; low[0x7003] = 0x56;
    RUN(0xed, 0x81);                        /* POPM R0,FB */
    EQ("POPM R0 first", "R0", R(M16C_R0), 0x1234);
    EQ("POPM FB second", "FB", R(M16C_FB), 0x5678);
    EQ("POPM", "SP", R(M16C_SP), 0x7004);
}

static void test_ext_mem(void)
{
    init();
    SET(M16C_A0, 2);
    high[0x6] = 0xEF; high[0x7] = 0xBE;
    RUN(0x75, 0x90, 0x04, 0x00, 0x0c);      /* LDE.W 0xc0004[A0],R0 */
    EQ("LDE.W", "R0", R(M16C_R0), 0xBEEF);

    init();
    SET(M16C_R0, 0xCAFE);
    RUN(0x75, 0x00, 0xaa, 0x0b, 0x0c);      /* STE.W R0,0xc0baa */
    EQ("STE.W", "mem", m16c_read(cpu, 0xC0BAA, 2), 0xCAFE);

    init();
    SET(M16C_R0, 0x1234);
    RUN(0x7a, 0x01);                        /* XCHG.B R0L,R0H */
    EQ("XCHG.B", "R0", R(M16C_R0), 0x3412);

    init();
    SET(M16C_FLG, M16C_Z);
    RUN(0xdf, 0x01, 0x09, 0x42, 0x02);      /* STZX #1,#2,0x4209 */
    EQ("STZX Z=1", "mem", M8(0x4209), 1);
    init();
    RUN(0xdf, 0x01, 0x09, 0x42, 0x02);
    EQ("STZX Z=0", "mem", M8(0x4209), 2);
}

static void test_add_sub(void)
{
    init();
    SET(M16C_A0, 0xFFFF);
    SET(M16C_R1, 1);
    RUN(0xa1, 0x14);                        /* ADD.W:G R1,A0 */
    EQ("ADD.W carry", "A0", R(M16C_A0), 0);
    EQ("ADD.W carry", "flags", flags(), C_ | Z_);

    init();
    SET(M16C_R0, 0x017F);
    RUN(0xa0, 0x10);                        /* ADD.B:G R0H,R0L */
    EQ("ADD.B overflow", "R0", R(M16C_R0), 0x0180);
    EQ("ADD.B overflow", "flags", flags(), S_ | O_);

    /* 0x8000 - 1: no borrow, so C is set, and the signed result overflows. */
    init();
    SET(M16C_R0, 0x8000);
    SET(M16C_R1, 1);
    RUN(0xc1, 0x10);                        /* CMP.W:G R1,R0 */
    EQ("CMP.W", "R0 unchanged", R(M16C_R0), 0x8000);
    EQ("CMP.W", "flags", flags(), C_ | O_);

    init();
    SET(M16C_R0, 3);
    SET(M16C_R1, 5);
    RUN(0xc1, 0x10);
    EQ("CMP.W borrow", "flags", flags(), S_);

    init();
    SET(M16C_R0, 5);
    SET(M16C_R1, 3);
    RUN(0xb9, 0x10);                        /* SBB.W R1,R0 with C=0 */
    EQ("SBB.W", "R0", R(M16C_R0), 1);
    EQ("SBB.W", "flags", flags(), C_);

    init();
    SET(M16C_R0, 0xFFFF);
    SET(M16C_FLG, M16C_C);
    RUN(0xb1, 0x10);                        /* ADC.W R1,R0 with C=1 */
    EQ("ADC.W", "R0", R(M16C_R0), 0);
    EQ("ADC.W", "flags", flags(), C_ | Z_);

    init();
    SET(M16C_R0, 0x00FF);
    SET(M16C_FLG, M16C_C);
    RUN(0xa4);                              /* INC.B R0L */
    EQ("INC.B", "R0", R(M16C_R0), 0);
    EQ("INC.B", "carry kept", flags(), C_ | Z_);

    init();
    SET(M16C_R0, 1);
    RUN(0x75, 0x50);                        /* NEG.W R0 */
    EQ("NEG.W", "R0", R(M16C_R0), 0xFFFF);
    EQ("NEG.W", "flags", flags(), S_);
    init();
    RUN(0x75, 0x50);
    EQ("NEG.W 0", "flags", flags(), C_ | Z_);

    init();
    SET(M16C_R0, 0xFFFE);
    RUN(0x77, 0xf0);                        /* ABS.W R0 */
    EQ("ABS.W", "R0", R(M16C_R0), 2);

    init();
    RUN(0xc9, 0x10);                        /* ADD.W:Q #1,R0 */
    EQ("ADD.W:Q", "R0", R(M16C_R0), 1);

    init();
    RUN(0x7d, 0xbf);                        /* ADD.B:Q #-1,SP */
    EQ("ADD:Q SP", "SP", R(M16C_SP), 0x7FFF);
    RUN(0x7d, 0xeb, 0x10, 0x00);            /* ADD.W:G #0x10,SP */
    EQ("ADD:G SP", "SP", R(M16C_SP), 0x800F);
}

/* The conditional jumps after the same 0x8000 - 1 compare: unsigned
 * greater-or-equal and signed less-than are both true. */
static void test_conditions(void)
{
    init();
    SET(M16C_R0, 0x8000);
    SET(M16C_R1, 1);
    RUN(0xc1, 0x10);
    RUN(0x68, 0x05);                        /* JGEU */
    EQ("JGEU", "pc", R(M16C_PC), CODE + 1 + 5);

    init();
    SET(M16C_R0, 0x8000);
    SET(M16C_R1, 1);
    RUN(0xc1, 0x10);
    RUN(0x7d, 0xce, 0x05);                  /* JLT */
    EQ("JLT", "pc", R(M16C_PC), CODE + 2 + 5);

    init();
    SET(M16C_R0, 0x8000);
    SET(M16C_R1, 1);
    RUN(0xc1, 0x10);
    RUN(0x7d, 0xca, 0x05);                  /* JGE not taken */
    EQ("JGE", "pc", R(M16C_PC), CODE + 3);

    init();
    SET(M16C_R0, 3);
    SET(M16C_R1, 5);
    RUN(0xc1, 0x10);
    RUN(0x6c, 0x05);                        /* JLTU (no carry) */
    EQ("JLTU", "pc", R(M16C_PC), CODE + 1 + 5);

    init();
    RUN(0x6e, 0x0b);                        /* JNE with Z=0 */
    EQ("JNE", "pc", R(M16C_PC), CODE + 1 + 0x0b);
    init();
    SET(M16C_FLG, M16C_Z);
    RUN(0x6e, 0x0b);
    EQ("JNE not taken", "pc", R(M16C_PC), CODE + 2);

    init();
    RUN(0x65);                              /* JMP.S */
    EQ("JMP.S", "pc", R(M16C_PC), CODE + 2 + 5);
}

static void test_mul_div(void)
{
    init();
    SET(M16C_R1, 0xFFFF);
    SET(M16C_R2, 0x7777);
    RUN(0x7d, 0x51, 0x1e, 0x00);            /* MUL.W #30,R1 */
    EQ("MUL.W", "R1", R(M16C_R1), 0xFFE2);
    EQ("MUL.W", "R3", R(M16C_R3), 0xFFFF);
    EQ("MUL.W", "R2 untouched", R(M16C_R2), 0x7777);

    init();
    SET(M16C_R1, 0xFFFF);
    RUN(0x7d, 0x41, 0x02, 0x00);            /* MULU.W #2,R1 */
    EQ("MULU.W", "R1", R(M16C_R1), 0xFFFE);
    EQ("MULU.W", "R3", R(M16C_R3), 1);

    /* The 16-bit product of MUL.B to R1L lands in R1, not R2. */
    init();
    SET(M16C_R1, 0x0002);
    SET(M16C_R2, 0x7777);
    RUN(0x7c, 0x52, 0x5d);                  /* MUL.B #0x5d,R1L */
    EQ("MUL.B R1L", "R1", R(M16C_R1), 0x00BA);
    EQ("MUL.B R1L", "R2 untouched", R(M16C_R2), 0x7777);
    init();
    SET(M16C_R1, 0x34FF);
    RUN(0x7c, 0x52, 0x5d);
    EQ("MUL.B negative", "R1", R(M16C_R1), 0xFFA3);

    init();
    SET(M16C_R0, 0x00F0);
    RUN(0x7c, 0x40, 0x0a);                  /* MULU.B #10,R0L */
    EQ("MULU.B", "R0", R(M16C_R0), 0x0960);

    init();
    SET(M16C_R0, 1003);
    RUN(0x7d, 0xe0, 0x05, 0x00);            /* DIVU.W #5 */
    EQ("DIVU.W", "quotient R0", R(M16C_R0), 200);
    EQ("DIVU.W", "remainder R2", R(M16C_R2), 3);

    init();
    SET(M16C_R0, 0x0107);
    RUN(0x7c, 0xe0, 0x05);                  /* DIVU.B #5 */
    EQ("DIVU.B", "R0", R(M16C_R0), 0x0334);

    init();
    SET(M16C_R0, 0xFFF9);
    SET(M16C_R2, 0xFFFF);
    RUN(0x7d, 0xe1, 0x02, 0x00);            /* DIV.W #2: -7 / 2 */
    EQ("DIV.W", "quotient", R(M16C_R0), 0xFFFD);
    EQ("DIV.W", "remainder", R(M16C_R2), 0xFFFF);

    init();
    SET(M16C_R0, 0xFFF9);
    SET(M16C_R2, 0xFFFF);
    RUN(0x7d, 0xe3, 0x02, 0x00);            /* DIVX.W #2: remainder takes the divisor's sign */
    EQ("DIVX.W", "quotient", R(M16C_R0), 0xFFFC);
    EQ("DIVX.W", "remainder", R(M16C_R2), 1);

    init();
    SET(M16C_R0, 10);
    RUN(0x7d, 0xe0, 0x00, 0x00);            /* DIVU.W #0 */
    EQ("DIVU by 0", "R0", R(M16C_R0), 10);
    EQ("DIVU by 0", "flags", flags(), O_);

    init();
    SET(M16C_R0, 0x0080);
    RUN(0x7c, 0x60);                        /* EXTS.B R0L */
    EQ("EXTS.B", "R0", R(M16C_R0), 0xFF80);
    EQ("EXTS.B", "flags", flags(), S_);
    init();
    SET(M16C_R0, 0x8000);
    RUN(0x7c, 0xf3);                        /* EXTS.W R0 */
    EQ("EXTS.W", "R2", R(M16C_R2), 0xFFFF);
    EQ("EXTS.W", "R0", R(M16C_R0), 0x8000);
}

static void test_logic_shift(void)
{
    init();
    SET(M16C_R0, 0x00A7);
    RUN(0x94, 0x0f);                        /* AND.B:S #0xf,R0L */
    EQ("AND.B:S", "R0", R(M16C_R0), 0x07);

    init();
    m16c_write(cpu, 0x41c0, 0x0300, 2);
    RUN(0x77, 0x0f, 0xc0, 0x41, 0x00, 0x0f); /* TST.W #0xf00,0x41c0 */
    EQ("TST.W", "flags", flags(), 0);
    m16c_write(cpu, 0x41c0, 0x00FF, 2);
    SET(M16C_PC, CODE);
    step(1);
    EQ("TST.W zero", "flags", flags(), Z_);
    EQ("TST.W", "mem untouched", M16(0x41c0), 0x00FF);

    init();
    SET(M16C_R0, 0x00FF);
    RUN(0x75, 0x70);                        /* NOT.W R0 */
    EQ("NOT.W", "R0", R(M16C_R0), 0xFF00);

    init();
    SET(M16C_A1, 0x1280);
    RUN(0xe9, 0xf5);                        /* SHL.W #-8,A1 */
    EQ("SHL.W right", "A1", R(M16C_A1), 0x0012);
    EQ("SHL.W right", "carry", flags(), C_);

    init();
    SET(M16C_R0, 0x8001);
    RUN(0xe9, 0x00);                        /* SHL.W #1,R0 */
    EQ("SHL.W left", "R0", R(M16C_R0), 2);
    EQ("SHL.W left", "flags", flags(), C_);

    init();
    SET(M16C_R0, 0x8000);
    RUN(0xf1, 0xf0);                        /* SHA.W #-8,R0 */
    EQ("SHA.W right", "R0", R(M16C_R0), 0xFF80);

    init();
    SET(M16C_R2, 0x8000);
    SET(M16C_R0, 1);
    RUN(0xeb, 0x80);                        /* SHL.L #1,R2R0 */
    EQ("SHL.L", "R0", R(M16C_R0), 2);
    EQ("SHL.L", "R2", R(M16C_R2), 0);
    EQ("SHL.L", "carry", flags(), C_);

    init();
    SET(M16C_R3, 0xF000);
    RUN(0xeb, 0xbf);                        /* SHA.L #-8,R3R1 */
    EQ("SHA.L", "R3", R(M16C_R3), 0xFFF0);
    EQ("SHA.L", "R1", R(M16C_R1), 0);

    init();
    SET(M16C_R1, 0xFE00);
    SET(M16C_R0, 0x0012);
    RUN(0x75, 0xe0);                        /* SHL.W R1H,R0 with R1H=-2 */
    EQ("SHL.W R1H", "R0", R(M16C_R0), 4);
    EQ("SHL.W R1H", "carry", flags(), C_);

    init();
    SET(M16C_R0, 0x8001);
    RUN(0xe1, 0x10);                        /* ROT.W #2,R0 */
    EQ("ROT.W", "R0", R(M16C_R0), 6);

    init();
    SET(M16C_R0, 0x8000);
    SET(M16C_FLG, M16C_C);
    RUN(0x77, 0xa0);                        /* ROLC.W R0 */
    EQ("ROLC.W", "R0", R(M16C_R0), 1);
    EQ("ROLC.W", "carry", flags(), C_);
    init();
    SET(M16C_R0, 1);
    SET(M16C_FLG, M16C_C);
    RUN(0x77, 0xb0);                        /* RORC.W R0 */
    EQ("RORC.W", "R0", R(M16C_R0), 0x8000);
    EQ("RORC.W", "flags", flags(), C_ | S_);
}

static void test_bits(void)
{
    init();
    RUN(0x7e, 0x9f, 0xa5, 0x91);            /* BSET bit address 0x91a5 = 0x1234 bit 5 */
    EQ("BSET abs", "mem", M8(0x1234), 0x20);
    RUN(0x7e, 0xbf, 0xa5, 0x91);            /* BTST */
    EQ("BTST abs", "flags", flags(), C_);
    RUN(0x7e, 0xaf, 0xa5, 0x91);            /* BNOT */
    EQ("BNOT abs", "mem", M8(0x1234), 0);
    RUN(0x7e, 0xbf, 0xa5, 0x91);
    EQ("BTST abs clear", "flags", flags(), Z_);

    init();
    SET(M16C_R0, 0x0020);
    RUN(0x7e, 0xb0, 0x05);                  /* BTST 5,R0 */
    EQ("BTST reg", "flags", flags(), C_);
    RUN(0x7e, 0x80, 0x05);                  /* BCLR 5,R0 */
    EQ("BCLR reg", "R0", R(M16C_R0), 0);

    /* bit,base:16[A0]: base is a byte address and A0 a bit offset. */
    init();
    SET(M16C_A0, 10);
    RUN(0x7e, 0x9c, 0x00, 0x10);            /* BSET 0x1000[A0] */
    EQ("BSET base[A0]", "byte 0x1001", M8(0x1001), 0x04);
    EQ("BSET base[A0]", "byte 0x1000", M8(0x1000), 0);

    init();
    SET(M16C_SB, 0x2000);
    RUN(0x7e, 0x9a, 0x13);                  /* BSET 0x13[SB]: bit offset from SB */
    EQ("BSET base[SB]", "mem", M8(0x2002), 0x08);

    init();
    SET(M16C_A0, 0xFFFE);
    RUN(0x7e, 0x24, 0x00, 0xfa);            /* BMNE 0,A0 with Z=0 */
    EQ("BMNE", "A0", R(M16C_A0), 0xFFFF);
    init();
    SET(M16C_A0, 0xFFFF);
    SET(M16C_FLG, M16C_Z);
    RUN(0x7e, 0x24, 0x00, 0xfa);
    EQ("BMNE clear", "A0", R(M16C_A0), 0xFFFE);

    init();
    SET(M16C_FLG, M16C_Z);
    RUN(0x7d, 0xd2);                        /* BMEQ C */
    EQ("BMEQ C", "flags", flags(), C_ | Z_);

    init();
    SET(M16C_R1, 0x8000);
    RUN(0x7e, 0xc1, 0x0f);                  /* BXOR 15,R1 */
    EQ("BXOR", "flags", flags(), C_);

    init();
    RUN(0x7e, 0x1f, 0xa5, 0x91);            /* BTSTS */
    EQ("BTSTS", "flags", flags(), Z_);
    EQ("BTSTS", "mem", M8(0x1234), 0x20);
    RUN(0x7e, 0x0f, 0xa5, 0x91);            /* BTSTC */
    EQ("BTSTC", "flags", flags(), C_);
    EQ("BTSTC", "mem", M8(0x1234), 0);

    init();
    low[0x1234] = 0x20;
    SET(M16C_FLG, M16C_C);
    RUN(0x7e, 0x4f, 0xa5, 0x91);            /* BAND */
    EQ("BAND", "flags", flags(), C_);
    RUN(0x7e, 0x5f, 0xa5, 0x91);            /* BNAND */
    EQ("BNAND", "flags", flags(), 0);
}

static void test_flow(void)
{
    init();
    SET(M16C_SP, 0x8000);
    RUN(0xfd, 0x00, 0x10, 0x0c);            /* JSR.A 0xc1000 */
    EQ("JSR.A", "pc", R(M16C_PC), 0xC1000);
    EQ("JSR.A", "SP", R(M16C_SP), 0x7FFD);
    EQ("JSR.A", "low word", M16(0x7FFD), 0x0004);
    EQ("JSR.A", "high byte", M8(0x7FFF), 0x0C);
    m16c_write(cpu, 0xC1000, 0xf3, 1);
    step(1);                                /* RTS */
    EQ("RTS", "pc", R(M16C_PC), CODE + 4);
    EQ("RTS", "SP", R(M16C_SP), 0x8000);

    init();
    RUN(0xf5, 0x10, 0x00);                  /* JSR.W */
    EQ("JSR.W", "pc", R(M16C_PC), CODE + 1 + 0x10);

    init();
    SET(M16C_FB, 0x1234);
    {
        static const uint8_t prog[] = {
            0xf5, 0x04, 0x00,               /* JSR.W to the ENTER */
            0x04, 0x04,
            0x7c, 0xf2, 0x04,               /* ENTER #4 */
            0x7d, 0xf2,                     /* EXITD */
        };

        load(prog, sizeof(prog));
    }
    step(2);
    EQ("ENTER", "FB", R(M16C_FB), 0x7FFB);
    EQ("ENTER", "pushed FB", M16(0x7FFB), 0x1234);
    EQ("ENTER", "SP", R(M16C_SP), 0x7FF7);
    step(1);
    EQ("EXITD", "pc", R(M16C_PC), CODE + 3);
    EQ("EXITD", "SP", R(M16C_SP), 0x8000);
    EQ("EXITD", "FB", R(M16C_FB), 0x1234);

    init();
    SET(M16C_R0, 2);
    RUN(0xf8, 0xf0, 0x04);                  /* ADJNZ.B #-1,R0L */
    EQ("ADJNZ taken", "pc", R(M16C_PC), CODE + 2 + 4);
    EQ("ADJNZ", "R0", R(M16C_R0), 1);
    RUN(0xf8, 0xf0, 0x04);
    EQ("ADJNZ not taken", "pc", R(M16C_PC), CODE + 3);

    /* The table follows the instruction and is read from its own 64 KiB
     * page; entries are offsets from the JMPI itself. */
    init();
    SET(M16C_A0, 4);
    high[0x34] = 0x10;
    high[0x35] = 0;
    RUN(0x7d, 0x2c, 0x30, 0x00);            /* JMPI.W 0x30[A0] */
    EQ("JMPI.W", "pc", R(M16C_PC), CODE + 0x10);

    init();
    SET(M16C_R3, 3);
    SET(M16C_R0, 0x0055);
    SET(M16C_A1, 0x2000);
    RUN(0x7c, 0xea);                        /* SSTR.B */
    EQ("SSTR.B", "last", M8(0x2002), 0x55);
    EQ("SSTR.B", "past the end", M8(0x2003), 0);
    EQ("SSTR.B", "A1", R(M16C_A1), 0x2003);
    EQ("SSTR.B", "R3", R(M16C_R3), 0);

    init();
    SET(M16C_R3, 2);
    SET(M16C_R1, 0x0C00);
    SET(M16C_A0, 0x0010);
    SET(M16C_A1, 0x3000);
    high[0x10] = 0xAA; high[0x11] = 0xBB;
    RUN(0x7c, 0xe8);                        /* SMOVF.B from R1H:A0 */
    EQ("SMOVF.B", "first", M8(0x3000), 0xAA);
    EQ("SMOVF.B", "second", M8(0x3001), 0xBB);
    EQ("SMOVF.B", "A0", R(M16C_A0), 0x0012);
}

static void test_control(void)
{
    init();
    RUN(0xeb, 0x50, 0xc2, 0x4c);            /* LDC #0x4cc2,SP */
    EQ("LDC SP", "SP", R(M16C_SP), 0x4cc2);
    RUN(0xeb, 0x30, 0x80, 0x00);            /* LDC #0x80,FLG: U selects USP */
    EQ("LDC FLG", "flg", R(M16C_FLG), 0x80);
    EQ("LDC FLG", "SP is USP", R(M16C_SP), 0x9000);
    RUN(0x7b, 0xb0);                        /* STC FLG,R0 */
    EQ("STC FLG", "R0", R(M16C_R0), 0x80);

    init();
    SET(M16C_R0, 0x1234);
    RUN(0x7a, 0xe0);                        /* LDC R0,SB */
    EQ("LDC SB", "SB", R(M16C_SB), 0x1234);

    init();
    RUN(0xeb, 0x20, 0x0f, 0x00);            /* LDC #0xf,INTBH */
    RUN(0xeb, 0x10, 0x00, 0xfd);            /* LDC #0xfd00,INTBL */
    EQ("INTB", "value", R(M16C_INTB), 0xFFD00);

    init();
    RUN(0xeb, 0x64);                        /* FSET I */
    EQ("FSET I", "flg", R(M16C_FLG), M16C_I);
    RUN(0xeb, 0x65);                        /* FCLR I */
    EQ("FCLR I", "flg", R(M16C_FLG), 0);
    RUN(0x7d, 0xa5);                        /* LDIPL #5 */
    EQ("LDIPL", "flg", R(M16C_FLG), 0x5000);

    init();
    SET(M16C_R0, 0x1111);
    RUN(0xeb, 0x44);                        /* FSET B: second register bank */
    EQ("bank 1", "R0", R(M16C_R0), 0);
    SET(M16C_R0, 0x2222);
    RUN(0xeb, 0x45);                        /* FCLR B */
    EQ("bank 0", "R0", R(M16C_R0), 0x1111);
}

static void set_vector(uint32_t addr, uint32_t target)
{
    m16c_write(cpu, addr, target & 0xFFFF, 2);
    m16c_write(cpu, addr + 2, target >> 16, 2);
}

static void test_interrupts(void)
{
    /* Reset vector. */
    init();
    set_vector(0xFFFFC, 0xC0123);
    m16c_reset(cpu);
    EQ("reset", "pc", R(M16C_PC), 0xC0123);

    /* A maskable interrupt: I and U clear, IPL raised, four-byte frame on the
     * interrupt stack, REIT restores everything including PC bits 16-19. */
    init();
    SET(M16C_INTB, 0xFFD00);
    set_vector(0xFFD00 + 21 * 4, 0xC0200);
    SET(M16C_FLG, M16C_I | M16C_U | M16C_C | 0x2000);
    m16c_set_irq(cpu, 21, 4);
    RUN(0x04);                              /* NOP, after the interrupt is taken */
    EQ("irq", "pc", R(M16C_PC), 0xC0200 + 1);
    EQ("irq", "flg", R(M16C_FLG), 0x4000 | M16C_C);
    EQ("irq", "SP is ISP", R(M16C_SP), 0x7FFC);
    EQ("irq", "return pc low", M16(0x7FFC), 0x0000);
    EQ("irq", "flg low", M8(0x7FFE), M16C_I | M16C_U | M16C_C);
    EQ("irq", "flg high over pc high", M8(0x7FFF), 0x2C);
    EQ("irq", "ack", acks, 1);
    RUN(0xfb);                              /* REIT */
    EQ("REIT", "pc", R(M16C_PC), CODE);
    EQ("REIT", "flg", R(M16C_FLG), M16C_I | M16C_U | M16C_C | 0x2000);
    EQ("REIT", "SP is USP", R(M16C_SP), 0x9000);
    EQ("REIT", "ISP", R(M16C_ISP), 0x8000);

    /* Not above IPL, or with I clear: not taken. */
    init();
    SET(M16C_INTB, 0xFFD00);
    SET(M16C_FLG, M16C_I | 0x4000);
    m16c_set_irq(cpu, 21, 4);
    RUN(0x04);
    EQ("irq masked by IPL", "pc", R(M16C_PC), CODE + 1);
    init();
    m16c_set_irq(cpu, 21, 7);
    RUN(0x04);
    EQ("irq masked by I", "pc", R(M16C_PC), CODE + 1);

    /* WAIT stops until an interrupt arrives. */
    init();
    SET(M16C_INTB, 0xFFD00);
    set_vector(0xFFD00 + 3 * 4, 0xC0300);
    SET(M16C_FLG, M16C_I);
    {
        static const uint8_t prog[] = { 0x7d, 0xf3, 0x04 };
        uint64_t done;

        load(prog, sizeof(prog));
        EQ("WAIT", "stop reason", m16c_step(cpu, 10, &done), M16C_STOP_WAIT);
        EQ("WAIT", "executed", done, 1);
        m16c_set_irq(cpu, 3, 1);
        m16c_step(cpu, 1, &done);
        EQ("WAIT woken", "pc", R(M16C_PC), 0xC0300 + 1);
    }

    /* Software interrupts keep IPL; INT with a number of 32 or more keeps U. */
    init();
    SET(M16C_INTB, 0xFFD00);
    set_vector(0xFFD00 + 5 * 4, 0xC0400);
    SET(M16C_FLG, M16C_I | M16C_U | 0x3000);
    RUN(0xeb, 0xc5);                        /* INT #5 */
    EQ("INT #5", "pc", R(M16C_PC), 0xC0400);
    EQ("INT #5", "flg", R(M16C_FLG), 0x3000);
    EQ("INT #5", "frame pc", M16(0x7FFC), 2);

    init();
    SET(M16C_INTB, 0xFFD00);
    set_vector(0xFFD00 + 40 * 4, 0xC0500);
    SET(M16C_FLG, M16C_I | M16C_U);
    RUN(0xeb, 0xe8);                        /* INT #40 */
    EQ("INT #40", "keeps U", R(M16C_FLG), M16C_U);
    EQ("INT #40", "frame on USP", R(M16C_SP), 0x8FFC);

    init();
    set_vector(0xFFFE4, 0xC0600);
    RUN(0x00);                              /* BRK */
    EQ("BRK", "pc", R(M16C_PC), 0xC0600);
    EQ("BRK", "return", M16(0x7FFC), 1);

    init();
    set_vector(0xFFFDC, 0xC0700);
    RUN(0xff);                              /* UND */
    EQ("UND", "pc", R(M16C_PC), 0xC0700);

    init();
    set_vector(0xFFFE0, 0xC0800);
    RUN(0xf6);                              /* INTO with O clear */
    EQ("INTO off", "pc", R(M16C_PC), CODE + 1);
    SET(M16C_FLG, M16C_O);
    SET(M16C_PC, CODE);
    step(1);
    EQ("INTO on", "pc", R(M16C_PC), 0xC0800);
}

static void test_driver(void)
{
    static const uint8_t prog[] = { 0x04, 0x04, 0x04 };
    uint64_t done;

    init();
    load(prog, sizeof(prog));
    m16c_set_break(cpu, CODE + 2);
    EQ("break", "stop reason", m16c_step(cpu, 10, &done), M16C_STOP_BREAK);
    EQ("break", "executed", done, 2);
    EQ("break", "resumes", m16c_step(cpu, 1, &done), M16C_STOP_BUDGET);
    EQ("break", "past", R(M16C_PC), CODE + 3);

    init();
    high[0] = 0x7d; high[1] = 0x80;
    EQ("undefined", "stop reason", m16c_step(cpu, 1, &done), M16C_STOP_UNDEF);
    EQ("undefined", "pc", m16c_trap_pc(cpu), CODE);
}

int main(void)
{
    test_mov();
    test_stack();
    test_pushm();
    test_ext_mem();
    test_add_sub();
    test_conditions();
    test_mul_div();
    test_logic_shift();
    test_bits();
    test_flow();
    test_control();
    test_interrupts();
    test_driver();
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
