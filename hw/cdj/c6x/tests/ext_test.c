/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C66x-only instructions against SPRUGH7's own worked examples. binutils cannot
 * assemble them, so the words are hand-encoded from the manual's opcode maps.
 *   make -C hw/cdj/c6x O=~/build/c6x ext-test
 */
#include <stdio.h>
#include <string.h>
#include "../c66x.h"
#include "../c66x_decode.h"

static uint8_t l2[0x100000];
static int fails;

#define S12U(op, dst, s2, s1) ((1u << 28) | (dst) << 23 | (s2) << 18 | (s1) << 13 | (op) << 6 | (0x8u << 2))
#define L12U(op, dst, s2, s1) ((1u << 28) | (dst) << 23 | (s2) << 18 | (s1) << 13 | (op) << 5 | (0x6u << 2))
#define MCRU(op, dst, s2, s1) ((1u << 28) | (dst) << 23 | (s2) << 18 | (s1) << 13 | (op) << 6 | (0xcu << 2))
#define NOP(n) (((n) - 1u) << 13)
#define IDLE 0x0001E000u

static void run(const char *name, uint32_t insn, const uint32_t *in, const unsigned *inreg, int nin,
                const unsigned *outreg, const uint32_t *want, int nout, uint32_t csr_want)
{
    uint32_t prog[8] = { insn, NOP(5), IDLE, NOP(1), NOP(1), NOP(1), NOP(1), NOP(1) };
    memset(l2, 0, sizeof l2);
    memcpy(l2, prog, sizeof prog);
    c66x_core *c = c66x_new(NULL);
    c66x_map_ram(c, 0x00800000, sizeof l2, l2);
    c66x_reset(c, 0x00800000);
    for (int i = 0; i < nin; i++) c66x_set_reg(c, inreg[i], in[i]);
    uint64_t ran;
    int st = c66x_step(c, 1000, &ran);
    for (int i = 0; i < nout; i++) {
        uint32_t g = c66x_get_reg(c, outreg[i]);
        int ok = g == want[i] && st == C66X_STOP_IDLE;
        fails += !ok;
        printf("  %s %-10s out%d got %08x want %08x (stop %d)\n", ok ? "ok  " : "FAIL", name, i, g, want[i], st);
    }
    uint32_t csr = c66x_get_creg(c, 1);
    int sat = (csr >> 9) & 1;
    if (sat != (int)csr_want) { fails++; printf("  FAIL %-10s SAT bit %d want %u\n", name, sat, csr_want); }
    c66x_free(c);
}

int main(void)
{
    const unsigned A0 = C66X_A(0);
    /* SHL2 .S1 A0,4,A15 ; SHL2 .S1 A0,A1,A5 ; B0 by 8 via A-side */
    run("shl2 #4", S12U(0x03, 15, 0, 4), (uint32_t[]){ 0x1234fedc }, (unsigned[]){ A0 }, 1,
        (unsigned[]){ C66X_A(15) }, (uint32_t[]){ 0x2340edc0 }, 1, 0);
    run("shl2 A1", S12U(0x13, 5, 0, 1), (uint32_t[]){ 0x1234fedc, 4 }, (unsigned[]){ A0, C66X_A(1) }, 2,
        (unsigned[]){ C66X_A(5) }, (uint32_t[]){ 0x2340edc0 }, 1, 0);
    run("shl2 8", S12U(0x13, 5, 0, 1), (uint32_t[]){ 0xabcd1234, 8 }, (unsigned[]){ A0, C66X_A(1) }, 2,
        (unsigned[]){ C66X_A(5) }, (uint32_t[]){ 0xcd003400 }, 1, 0);
    /* DPACKL4 .L1 A7:A6,A3:A2,A1:A0 -- the manual prints A1 as deafbacf, a
     * typo for deadbeef (its own Execution block and A0 both give deadbeef). */
    run("dpackl4", L12U(0x68, 0, 2, 6), (uint32_t[]){ 0x55de55ad, 0x89012345, 0x55be55ef, 0x01234567 },
        (unsigned[]){ C66X_A(7), C66X_A(6), C66X_A(3), C66X_A(2) }, 4,
        (unsigned[]){ C66X_A(1), C66X_A(0) }, (uint32_t[]){ 0xdeadbeef, 0x01452367 }, 2, 0);
    /* DCCMPYR1 .M1 A3:A2,A1:A0,A15:A14 -- all seven examples */
    static const uint32_t ex[][7] = {
        { 0x80008000, 0x80008000, 0x80007fff, 0x80007fff, 0x00017fff, 0x00017fff, 1 },
        { 0x80008000, 0x80008000, 0x80008000, 0x80008000, 0x7fff0000, 0x7fff0000, 1 },
        { 0x08000400, 0x09000200, 0x09000200, 0x08000400, 0x00a00028, 0x00a0ffd8, 0 },
        { 0x7FFF7FFF, 0x7FFF8000, 0x7FFF8000, 0x7FFF7FFF, 0xffff7fff, 0xffff8000, 1 },
        { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0x00000000, 0x00000000, 0 },
        { 0x55555555, 0x55555555, 0x55555555, 0x55555555, 0x71c60000, 0x71c60000, 0 },
        { 0x01234567, 0x89ABCDEF, 0x89ABCDEF, 0x01234567, 0xe3cec049, 0xe3ce3fb7, 0 },
    };
    for (unsigned i = 0; i < sizeof ex / sizeof ex[0]; i++)
        run("dccmpyr1", MCRU(0x0e, 14, 0, 2), ex[i],
            (unsigned[]){ C66X_A(3), C66X_A(2), C66X_A(1), C66X_A(0) }, 4,
            (unsigned[]){ C66X_A(15), C66X_A(14) }, &ex[i][4], 2, ex[i][6]);
    printf("%s: %d failure(s)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
