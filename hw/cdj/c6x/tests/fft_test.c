/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The native FFTs against the interpreter. The NXS2's DSP program is loaded as
 * MAIN ships it and each FFT routine is called the way the vocoder calls it,
 * on random and on signal-like input: once on the interpreter, once through
 * its native form alone, once through the driver natively and once in check
 * mode. Every word of RAM, every register, the control registers and the
 * cycle, packet and instruction counts must agree. With "bench" it then times
 * a call each way.
 *   make -C hw/cdj/c6x O=~/build/c6x fft-test
 * usage: fft_test <NXS2 main_unpacked.bin> [bench]
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../c66x_native_int.h"
#include "../c66x_image.h"

#define N 2048
#define X 0x00814800u               /* the vocoder's buffers */
#define Y 0x0081C800u
#define W 0x00824800u
#define BREV 0x008A4CC8u
#define SP 0x0089F000u
#define STOP 0x80000100u

static int fails;
static uint8_t l2[0x100000], ddr[0x100000];
static uint8_t l2_in[sizeof l2], ddr_in[sizeof ddr], l2_ref[sizeof l2], ddr_ref[sizeof ddr];
static c66x_image img;

typedef struct routine {
    const char *name;
    uint32_t entry;
    const native_kernel *kernel;
} routine;

static const routine routines[] = {
    { "fft_scaled", 0x00804AE0, &native_fft_scaled },
    { "fft_plain", 0x00805F40, &native_fft_plain },
};

typedef struct result {
    uint32_t reg[C66X_NREGS], cr[32], pc, exec_pc;
    int branch_block;
    c66x_stats st;
    uint64_t native_entries;
} result;

static uint8_t *host(uint32_t a)
{
    if (a - 0x00800000u < sizeof l2)
        return l2 + (a - 0x00800000u);
    if (a - 0x80000000u < sizeof ddr)
        return ddr + (a - 0x80000000u);
    return NULL;
}

static uint32_t rng;
static uint32_t rnd(void)
{
    rng = rng * 1664525u + 1013904223u;
    return rng;
}

static void putf(uint32_t a, float f)
{
    memcpy(host(a), &f, 4);
}

/* DSPLIB's gen_twiddle_fft_sp: six words a butterfly, a table per stage. */
static void dsplib_twiddles(void)
{
    const double pi = 3.141592654;
    uint32_t k = 0;
    for (int j = 1; j <= N >> 2; j <<= 2)
        for (int i = 0; i < N >> 2; i += j) {
            for (int m = 1; m <= 3; m++) {
                putf(W + 4 * k++, (float)cos(2 * m * pi * i / N));
                putf(W + 4 * k++, (float)sin(2 * m * pi * i / N));
            }
        }
}

/* kind 0: random words of every exponent; 1: two windowed tones packed as one
 * complex frame with DSPLIB twiddles, as the vocoder feeds it; 2: zeros,
 * denormals and huge values among small ones. */
static void setup(int kind, uint32_t seed)
{
    rng = seed;
    memset(l2, 0, sizeof l2);
    memset(ddr, 0, sizeof ddr);
    for (unsigned i = 0; i < img.nsections; i++) {
        uint8_t *to = host(img.sections[i].addr);
        if (to)
            memcpy(to, img.sections[i].data, img.sections[i].size);
    }
    for (uint32_t a = SP - 0x100; a < SP + 0x40; a += 4)
        memcpy(host(a), &(uint32_t){ rnd() }, 4);
    for (uint32_t i = 0; i < 2 * N; i++)
        memcpy(host(Y + 4 * i), &(uint32_t){ rnd() }, 4);
    for (int i = 0; i < 64; i++) {
        int r = 0;
        for (int b = 0; b < 6; b++)
            r |= (i >> b & 1) << (5 - b);
        host(BREV)[i] = r;
    }
    if (kind == 0) {
        for (uint32_t i = 0; i < 2 * N; i++) {
            uint32_t v = rnd();
            v = (v & 0x807FFFFFu) | (100 + (rnd() % 56)) << 23;
            memcpy(host(X + 4 * i), &v, 4);
        }
        for (uint32_t i = 0; i < 6 * 682; i++)
            putf(W + 4 * i, (float)((int32_t)rnd() / 2147483648.0));
        return;
    }
    dsplib_twiddles();
    for (uint32_t i = 0; i < N; i++) {
        double win = 0.5 - 0.5 * cos(2 * M_PI * i / N);
        float a = (float)(win * (0.6 * sin(0.0731 * i) + 0.01 * ((int32_t)rnd() / 2147483648.0)));
        float b = (float)(win * 0.3 * cos(0.2113 * i + 0.4));
        if (kind == 2 && i % 7 == 0)
            a = (i % 21 == 0) ? 0.0f : (i % 14 == 0) ? 1e-40f : 3e30f;
        putf(X + 8 * i, a);
        putf(X + 8 * i + 4, b);
    }
}

static c66x_core *new_core(const char *mode, const routine *rt, uint32_t seed)
{
#ifdef _WIN32
    char env[32];
    snprintf(env, sizeof env, "C66X_NATIVE=%s", mode);
    _putenv(env);
#else
    setenv("C66X_NATIVE", mode, 1);
#endif
    c66x_core *c = c66x_new(NULL);
    c66x_map_ram(c, 0x00800000, sizeof l2, l2);
    c66x_map_ram(c, 0x80000000, sizeof ddr, ddr);
    c66x_reset(c, rt->entry);
    rng = seed ^ 0x5A5A5A5A;
    for (unsigned r = 0; r < C66X_NREGS; r++)
        c66x_set_reg(c, r, rnd());
    c66x_set_reg(c, A(4), N);
    c66x_set_reg(c, B(4), X);
    c66x_set_reg(c, A(6), W);
    c66x_set_reg(c, B(6), Y);
    c66x_set_reg(c, A(8), BREV);
    c66x_set_reg(c, B(8), 2);
    c66x_set_reg(c, A(10), 0);
    c66x_set_reg(c, B(10), N);
    c66x_set_reg(c, B(15), SP);
    c66x_set_reg(c, B(3), STOP);
    return c;
}

static int stop_hook(c66x_core *c, void *opaque)
{
    return 1;
}

static void finish(c66x_core *c, result *r)
{
    memcpy(r->reg, c->reg, sizeof r->reg);
    memcpy(r->cr, c->cr, sizeof r->cr);
    r->pc = c->pc;
    r->exec_pc = c->exec_pc;
    r->branch_block = c->branch_block;
    c66x_get_stats(c, &r->st);
    r->native_entries = c->native_entries;
}

static void run_core(const char *mode, const routine *rt, uint32_t seed, result *r)
{
    c66x_core *c = new_core(mode, rt, seed);
    c66x_hook_pc(c, STOP, stop_hook, NULL);
    /* decoded, as by the vocoder's earlier calls: kernels are found in decoded code */
    for (uint32_t a = rt->entry; a < rt->entry + rt->kernel->size; a += 32)
        fetch_insn(c, a);
    uint64_t ran;
    c66x_step(c, 1000000, &ran);
    finish(c, r);
    c66x_free(c);
}

/* The native form alone: the pass on the state the call enters with, its
 * stores and spans applied to RAM. */
static void run_pass(const routine *rt, uint32_t seed, result *r)
{
    c66x_core *c = new_core("0", rt, seed);
    native_store st[NATIVE_STORES];
    native_state s = { .st = st, .bulk_buf = c->native_bulk, .at = rt->entry, .branch_block = 5 };
    memcpy(s.reg, c->reg, sizeof s.reg);
    memcpy(s.cr, c->cr, sizeof s.cr);
    memset(r, 0, sizeof *r);
    if (rt->kernel->pass(c, &s)) {
        for (unsigned i = 0; i < s.nst; i++)
            memcpy(host(s.st[i].a), &s.st[i].v, s.st[i].n);
        for (unsigned i = 0; i < s.nbulk; i++)
            memcpy(host(s.bulk[i].dst), s.bulk_buf + s.bulk[i].off, s.bulk[i].len);
        memcpy(r->reg, s.reg, sizeof r->reg);
        memcpy(r->cr, s.cr, sizeof r->cr);
        r->pc = s.pc;
        r->exec_pc = s.last_pc;
        r->branch_block = s.branch_block;
        r->st.cycles = s.cycles + 1;    /* the stop hook's cycle */
        r->st.packets = s.packets;
        r->st.insns = s.insns;
        r->st.sploops = s.sploops;
    }
    c66x_free(c);
}

static void compare(const char *what, const result *ref, const result *got)
{
    char why[160] = "";
    for (uint32_t a = 0; !*why && a < sizeof l2; a += 4)
        if (memcmp(l2 + a, l2_ref + a, 4))
            snprintf(why, sizeof why, "word 0x%08x 0x%08x (interpreter 0x%08x)", 0x00800000u + a,
                     *(uint32_t *)(l2 + a), *(uint32_t *)(l2_ref + a));
    for (uint32_t a = 0; !*why && a < sizeof ddr; a += 4)
        if (memcmp(ddr + a, ddr_ref + a, 4))
            snprintf(why, sizeof why, "word 0x%08x", 0x80000000u + a);
    for (unsigned i = 0; !*why && i < C66X_NREGS; i++)
        if (got->reg[i] != ref->reg[i])
            snprintf(why, sizeof why, "%c%u 0x%08x (interpreter 0x%08x)", i < 32 ? 'A' : 'B', i & 31,
                     got->reg[i], ref->reg[i]);
    for (unsigned i = 0; !*why && i < 32; i++)
        if (got->cr[i] != ref->cr[i])
            snprintf(why, sizeof why, "cr %u 0x%08x (interpreter 0x%08x)", i, got->cr[i], ref->cr[i]);
    if (!*why && (got->pc != ref->pc || got->exec_pc != ref->exec_pc || got->branch_block != ref->branch_block
                  || got->st.cycles != ref->st.cycles
                  || got->st.packets != ref->st.packets || got->st.insns != ref->st.insns
                  || got->st.sploops != ref->st.sploops))
        snprintf(why, sizeof why, "pc 0x%08x exec 0x%08x cycles %llu packets %llu insns %llu sploops %llu", got->pc,
                 got->exec_pc, (unsigned long long)got->st.cycles, (unsigned long long)got->st.packets,
                 (unsigned long long)got->st.insns, (unsigned long long)got->st.sploops);
    fails += !!*why;
    printf("    %s %-12s %s\n", *why ? "FAIL" : "ok  ", what, why);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* Calls on one core, the input restored before each; the first, which
 * decodes the code, is not timed. */
static double time_calls(const char *mode, const routine *rt, int calls)
{
    static uint8_t x_in[8 * N];
    setup(1, 1);
    memcpy(x_in, host(X), sizeof x_in);
    c66x_core *c = new_core(mode, rt, 1);
    c66x_hook_pc(c, STOP, stop_hook, NULL);
    uint32_t reg[C66X_NREGS];
    memcpy(reg, c->reg, sizeof reg);
    double t0 = 0;
    for (int i = 0; i <= calls; i++) {
        if (i == 1)
            t0 = now();
        memcpy(host(X), x_in, sizeof x_in);
        c66x_set_pc(c, rt->entry);
        for (unsigned r = 0; r < C66X_NREGS; r++)
            c66x_set_reg(c, r, reg[r]);
        uint64_t ran;
        c66x_step(c, 1000000, &ran);
    }
    double t = (now() - t0) / calls;
    c66x_free(c);
    return t;
}

static void bench(const routine *rt)
{
    double interp = time_calls("0", rt, 200), driver = time_calls("1", rt, 2000);
    c66x_core *c = new_core("0", rt, 1);
    native_store st[NATIVE_STORES];
    const int calls = 2000;
    double t0 = now();
    for (int i = 0; i < calls; i++) {
        native_state s = { .st = st, .bulk_buf = c->native_bulk, .at = rt->entry, .branch_block = 5 };
        memcpy(s.reg, c->reg, sizeof s.reg);
        memcpy(s.cr, c->cr, sizeof s.cr);
        rt->kernel->pass(c, &s);
    }
    double pass = (now() - t0) / calls;
    c66x_free(c);
    printf("  %s: interpreter %.1f us, native through the driver %.1f us, the pass alone %.1f us a call\n", rt->name,
           interp * 1e6, driver * 1e6, pass * 1e6);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <NXS2 main_unpacked.bin> [bench]\n", argv[0]);
        return 2;
    }
    if (c66x_image_load(&img, argv[1], 0x34E0)) {
        fprintf(stderr, "%s: no DSP boot table\n", argv[1]);
        return 2;
    }
    static const struct { int kind; uint32_t seed; const char *name; } cases[] = {
        { 0, 1, "random" }, { 0, 2, "random" }, { 0, 3, "random" }, { 1, 4, "tones" }, { 2, 5, "edge values" },
    };
    for (unsigned ri = 0; ri < sizeof routines / sizeof routines[0]; ri++) {
        const routine *rt = &routines[ri];
        for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
            printf("  %s, %s input (seed %u)\n", rt->name, cases[k].name, cases[k].seed);
            result ref, got;
            setup(cases[k].kind, cases[k].seed);
            memcpy(l2_in, l2, sizeof l2);
            memcpy(ddr_in, ddr, sizeof ddr);
            run_core("0", rt, cases[k].seed, &ref);
            memcpy(l2_ref, l2, sizeof l2);
            memcpy(ddr_ref, ddr, sizeof ddr);

            memcpy(l2, l2_in, sizeof l2);
            memcpy(ddr, ddr_in, sizeof ddr);
            run_pass(rt, cases[k].seed, &got);
            compare("pass", &ref, &got);

            memcpy(l2, l2_in, sizeof l2);
            memcpy(ddr, ddr_in, sizeof ddr);
            run_core("1", rt, cases[k].seed, &got);
            compare("driver", &ref, &got);
            if (got.native_entries != 1) {
                fails++;
                printf("    FAIL the driver ran it natively %llu times\n", (unsigned long long)got.native_entries);
            }

            memcpy(l2, l2_in, sizeof l2);
            memcpy(ddr, ddr_in, sizeof ddr);
            fflush(stdout);
            run_core("check", rt, cases[k].seed, &got);
            compare("check mode", &ref, &got);
        }
    }
    if (argc > 2 && !strcmp(argv[2], "bench"))
        for (unsigned ri = 0; ri < sizeof routines / sizeof routines[0]; ri++)
            bench(&routines[ri]);
    c66x_image_free(&img);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
