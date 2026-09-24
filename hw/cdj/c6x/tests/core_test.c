/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Core semantics tests, each against a number SPRU732J states or a property
 * the machine integration depends on. Programs are assembled with tic6x-elf-as
 * to flat binaries loaded at 0x00800000.
 *   make -C hw/cdj/c6x O=~/build/c6x core-test
 * usage: core_test pipeline.bin sploop_irq.bin idle.bin idle_head_hex
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../c66x.h"
#include "../c66x_decode.h"

static int fails, checks;
#define CHECK(name, got, want) do { checks++; if ((got) != (want)) { fails++; \
    printf("  FAIL %-44s got 0x%08x want 0x%08x\n", name, (unsigned)(got), (unsigned)(want)); } \
    else printf("  ok   %-44s 0x%08x\n", name, (unsigned)(got)); } while (0)

static uint8_t l2[0x100000];

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static c66x_core *load(const char *path)
{
    memset(l2, 0, sizeof l2);
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    size_t n = fread(l2, 1, 0x10000, f);
    fclose(f);
    printf("%s: %zu bytes\n", path, n);
    c66x_core *c = c66x_new(NULL);
    c66x_map_ram(c, 0x00800000, sizeof l2, l2);
    return c;
}

static void test_pipeline(const char *path)
{
    c66x_core *c = load(path);
    for (int i = 0; i < 8; i++)
        wr32(l2 + 0x10000 + 4 * i, 0xA0000000u + i);
    wr32(l2 + 0x10100, 0x11111111);
    memcpy(l2 + 0x10300, "abc", 4);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    CHECK("pipeline: stopped at IDLE", c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    CHECK("load: dst unchanged 1 cycle later", c66x_get_reg(c, C66X_A(5)), 7);
    CHECK("load: dst readable 5 cycles later", c66x_get_reg(c, C66X_A(6)), 0x11111111);
    CHECK("B: 5 delay-slot packets ran", c66x_get_reg(c, C66X_A(7)), 5);
    CHECK("BNOP 3: 2 delay-slot packets ran", c66x_get_reg(c, C66X_A(8)), 2);
    int ok = 0;
    for (int i = 0; i < 8; i++)
        ok += rd32(l2 + 0x10200 + 4 * i) == 0xA0000000u + i;
    CHECK("SPLOOP copy: 8 words", ok, 8);
    CHECK("SPLOOP copy: 9th word untouched", rd32(l2 + 0x10220), 0);
    CHECK("SPLOOP copy: a1 after loop", c66x_get_reg(c, C66X_A(9)), 0x00810020);
    CHECK("SPLOOPW strcpy bytes", rd32(l2 + 0x10400), rd32((const uint8_t *)"abc"));
    CHECK("SPLOOPW strcpy: dst pointer", c66x_get_reg(c, C66X_B(10)), 0x00810403);
    c66x_free(c);
}

static unsigned ntrace;
static void trace_pc(c66x_core *c, void *opaque, uint32_t pc)
{
    if (ntrace++ < 2000)
        printf("    pc %08x a1 %08x b0 %08x a15 %u tsr %08x itsr %08x irp %08x ilc %u\n", pc,
               c66x_get_reg(c, C66X_A(1)), c66x_get_reg(c, C66X_B(0)), c66x_get_reg(c, C66X_A(15)),
               c66x_get_creg(c, 0x1a), c66x_get_creg(c, 0x1b), c66x_get_creg(c, 6),
               c66x_get_creg(c, 0xd));
}

/* One run of the interrupted loop; irq_at < 0 means no interrupt. */
static void run_irq(const char *path, int irq_at, int expect_isr)
{
    c66x_core *c = load(path);
    for (int i = 0; i < 64; i++)
        wr32(l2 + 0x10000 + 4 * i, 0xB0000000u + i);
    c66x_reset(c, 0x00800000);
    ntrace = 0;
    if (getenv("CORE_TRACE") && atoi(getenv("CORE_TRACE")) == irq_at)
        c66x_set_trace(c, trace_pc, NULL);
    uint64_t ran, total = 0;
    c66x_stop st;
    if (irq_at >= 0) {
        c66x_step(c, irq_at, &ran);
        total += ran;
        c66x_set_irq(c, 4, 1);
        c66x_set_irq(c, 4, 0);
    }
    st = c66x_step(c, 100000, &ran);
    total += ran;
    char name[80];
    snprintf(name, sizeof name, "irq@%d: stopped at IDLE", irq_at);
    CHECK(name, st, C66X_STOP_IDLE);
    int ok = 0;
    for (int i = 0; i < 40; i++)
        ok += rd32(l2 + 0x10200 + 4 * i) == 0xB0000000u + i;
    snprintf(name, sizeof name, "irq@%d: 40 words copied", irq_at);
    CHECK(name, ok, 40);
    snprintf(name, sizeof name, "irq@%d: 41st word untouched", irq_at);
    CHECK(name, rd32(l2 + 0x102A0), 0);
    snprintf(name, sizeof name, "irq@%d: source pointer +40 words", irq_at);
    CHECK(name, c66x_get_reg(c, C66X_A(9)), 0x008100A0);
    snprintf(name, sizeof name, "irq@%d: destination pointer +40 words", irq_at);
    CHECK(name, c66x_get_reg(c, C66X_B(10)), 0x008102A0);
    snprintf(name, sizeof name, "irq@%d: ISR ran", irq_at);
    CHECK(name, c66x_get_reg(c, C66X_A(15)), (uint32_t)expect_isr);
    c66x_stats s;
    c66x_get_stats(c, &s);
    if (expect_isr) {
        /* 7.13.1: the loop drains only while ILC >= ceil(dynlen / ii) = 7;
         * nearer the end the interrupt waits for the loop to finish. */
        uint32_t ilc = c66x_get_reg(c, C66X_B(13));
        int drained = s.sploops == 2;
        snprintf(name, sizeof name, "irq@%d: %s", irq_at,
                 drained ? "drained with ILC >= 7 left" : "taken after the loop (ILC 0)");
        CHECK(name, drained ? (ilc >= 7 && ilc <= 39) : ilc == 0, 1);
        printf("       %s, ISR saw ILC %u\n", drained ? "loop drained and re-entered" : "loop completed first", ilc);
    }
    c66x_free(c);
}

static void test_idle(const char *path, uint32_t head)
{
    for (int variant = 0; variant < 2; variant++) {
        c66x_core *c = load(path);
        wr32(l2 + 0x10500, 0x1234);
        wr32(l2 + 0x1050C, variant);
        c66x_reset(c, 0x00800000);
        c66x_set_idle_loop(c, head, 0, 0, 0);
        uint64_t ran;
        c66x_stop st = c66x_step(c, 100000, &ran);
        c66x_idle_info ii;
        c66x_get_idle_info(c, &ii);
        if (variant == 0) {
            CHECK("idle: republishing loop is a fixed point", st, C66X_STOP_IDLE);
            CHECK("idle: stopped at the declared head", c66x_get_pc(c), head);
            CHECK("idle: status word republished", rd32(l2 + 0x10504), 0x1234);
        } else {
            CHECK("idle: counting loop never idles", st, C66X_STOP_BUDGET);
            CHECK("idle: reason = registers changed", ii.last_reason, 5);
        }
        c66x_free(c);
    }
}

static void test_idle_irq(const char *path, uint32_t head)
{
    for (int variant = 0; variant < 2; variant++) {
        c66x_core *c = load(path);
        wr32(l2 + 0x1050C, variant);
        c66x_reset(c, 0x00800000);
        c66x_set_idle_loop(c, head, 0x00811000, 0x00813000, 0);
        uint64_t ran;
        char name[80];
        snprintf(name, sizeof name, "idle irq %d: fixed point before the interrupt", variant);
        CHECK(name, c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
        c66x_set_irq(c, 4, 1);
        c66x_set_irq(c, 4, 0);
        c66x_stop st = c66x_step(c, 100000, &ran);
        c66x_idle_info ii;
        c66x_get_idle_info(c, &ii);
        snprintf(name, sizeof name, "idle irq %d: handler ran once", variant);
        CHECK(name, rd32(l2 + 0x10508), 1);
        snprintf(name, sizeof name, "idle irq %d: idle again", variant);
        CHECK(name, st, C66X_STOP_IDLE);
        if (variant == 0) {
            CHECK("idle irq 0: idle where the handler returned", ii.resume_idles, 1);
            CHECK("idle irq 0: loop did no work", c66x_get_reg(c, C66X_A(6)), 0);
            CHECK("idle irq 0: registers restored", c66x_get_reg(c, C66X_B(5)), 0);
        } else {
            CHECK("idle irq 1: no idle at the return", ii.resume_idles, 0);
            CHECK("idle irq 1: loop consumed the polled word", c66x_get_reg(c, C66X_A(6)), 1);
            CHECK("idle irq 1: polled word cleared", rd32(l2 + 0x10500), 0);
            CHECK("idle irq 1: back at the head", c66x_get_pc(c), head);
        }
        c66x_free(c);
    }
}

static void test_brchain(const char *path)
{
    c66x_core *c = load(path);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    CHECK("brchain: stopped at IDLE", c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    CHECK("brchain: loop body ran 10 times", c66x_get_reg(c, C66X_A(3)), 10);
    CHECK("brchain: counter reached 0", c66x_get_reg(c, C66X_B(1)), 0);
    CHECK("brchain: post-loop code ran once", c66x_get_reg(c, C66X_A(5)), 1);
    c66x_free(c);
}

static void test_splexit(const char *path)
{
    c66x_core *c = load(path);
    static const uint16_t v[8] = { 0x5533, 0, 0x11, 0x22, 0x33, 0x44, 0, 0xAACC };
    for (int i = 0; i < 8; i++)
        memcpy(l2 + 0x10600 + 2 * i, &v[i], 2);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    CHECK("splexit: stopped at IDLE", c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    int ok = 0;
    for (int i = 0; i < 8; i++)
        ok += rd32(l2 + 0x10700 + 4 * i) == 0x04020000u + v[i];
    CHECK("splexit: 8 words = template + value", ok, 8);
    CHECK("splexit: last word keeps its trailer", rd32(l2 + 0x1071C), 0x0402AACC);
    CHECK("splexit: post-loop A3", c66x_get_reg(c, C66X_A(3)), 1);
    CHECK("splexit: output pointer +8 words", c66x_get_reg(c, C66X_B(11)), 0x00810720);
    c66x_free(c);
}

static char buslog[256];

static uint32_t log_read(void *o, uint32_t a, unsigned s)
{
    size_t n = strlen(buslog);
    snprintf(buslog + n, sizeof buslog - n, "r%02x/%u ", a & 0xff, s);
    return 0x11223344;
}

static void log_write(void *o, uint32_t a, uint32_t v, unsigned s)
{
    size_t n = strlen(buslog);
    snprintf(buslog + n, sizeof buslog - n, "w%02x/%u ", a & 0xff, s);
}

static void test_mmio(const char *path)
{
    memset(l2, 0, sizeof l2);
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fread(l2, 1, 0x10000, f);
    fclose(f);
    c66x_bus bus = { NULL, log_read, log_write };
    c66x_core *c = c66x_new(&bus);
    c66x_map_ram(c, 0x00800000, sizeof l2, l2);
    c66x_reset(c, 0x00800000);
    buslog[0] = 0;
    uint64_t ran;
    CHECK("mmio: stopped at IDLE", c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    const char *want = "r58/4 r58/2 r60/4 r64/4 w58/4 ";
    checks++;
    if (strcmp(buslog, want)) {
        fails++;
        printf("  FAIL mmio: bus accesses \"%s\" want \"%s\"\n", buslog, want);
    } else {
        printf("  ok   mmio: bus accesses exactly %s\n", buslog);
    }
    CHECK("mmio: ldw value", c66x_get_reg(c, C66X_A(4)), 0x11223344);
    CHECK("mmio: ldh sign-extends", c66x_get_reg(c, C66X_A(5)), 0x00003344);
    c66x_free(c);
}

static uint32_t extu(uint32_t x, int csta, int cstb) { return (x << csta) >> cstb; }

#define RING 0x10114

/* The decoder's ring: byte i of a write lands at ((w + i) XOR 3) & 0x7FF. */
static void run_ring(const char *path, uint32_t w, uint32_t n)
{
    c66x_core *c = load(path);
    static uint8_t want[0x800];
    memset(l2 + RING, 0xEE, 0x800);
    memset(want, 0xEE, sizeof want);
    for (uint32_t i = 0; i < n; i++) {
        l2[0x11000 + i] = i;
        want[((w + i) ^ 3) & 0x7FF] = i;
    }
    wr32(l2 + 0x13000, w);
    wr32(l2 + 0x13004, n);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    char name[64];
    snprintf(name, sizeof name, "ring w=%u n=%u: stopped at IDLE", w, n);
    CHECK(name, c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    int bad = 0;
    for (int i = 0; i < 0x800; i++)
        bad += l2[RING + i] != want[i];
    snprintf(name, sizeof name, "ring w=%u n=%u: bytes differing", w, n);
    CHECK(name, bad, 0);
    snprintf(name, sizeof name, "ring w=%u n=%u: index advanced", w, n);
    CHECK(name, c66x_get_reg(c, C66X_A(12)), w + n);
    if (w == 0 && n == 8) {
        static const uint8_t v[8] = { 3, 2, 1, 0, 7, 6, 5, 4 };
        CHECK("ring: c8's vector 03 02 01 00 07 06 05 04", memcmp(l2 + RING, v, 8), 0);
        const uint32_t x = 0x12345677;
        CHECK("extu 21,21", c66x_get_reg(c, C66X_A(5)), extu(x, 21, 21));
        CHECK("extu 30,27", c66x_get_reg(c, C66X_A(6)), extu(x, 30, 27));
        CHECK("extu 25,30", c66x_get_reg(c, C66X_A(7)), extu(x, 25, 30));
        CHECK("extu 28,28", c66x_get_reg(c, C66X_A(8)), extu(x, 28, 28));
        CHECK("extu 21,23", c66x_get_reg(c, C66X_A(9)), extu(x, 21, 23));
        CHECK("sub .L2 1,b5,b5 = 1 - b5", c66x_get_reg(c, C66X_A(15)), (uint32_t)(1 - 5));
    }
    c66x_free(c);
}

/* The main-data copy moves n bytes backwards: logical base+dst-k <- base+src-k. */
static void run_resv(const char *path, uint32_t base, uint32_t dst, uint32_t src, uint32_t n)
{
    c66x_core *c = load(path);
    static uint8_t want[0x800];
    for (int i = 0; i < 0x800; i++)
        l2[RING + i] = i * 7 + 1;
    memcpy(want, l2 + RING, sizeof want);
    for (uint32_t k = 0; k < n; k++)
        want[((base + dst - k) ^ 3) & 0x7FF] = l2[RING + (((base + src - k) ^ 3) & 0x7FF)];
    wr32(l2 + 0x13000, base);
    wr32(l2 + 0x13004, dst);
    wr32(l2 + 0x13008, src);
    wr32(l2 + 0x1300C, n);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    char name[64];
    snprintf(name, sizeof name, "resv base=%#x n=%u: stopped at IDLE", base, n);
    CHECK(name, c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    int bad = 0;
    for (int i = 0; i < 0x800; i++)
        bad += l2[RING + i] != want[i];
    snprintf(name, sizeof name, "resv base=%#x n=%u: bytes differing", base, n);
    CHECK(name, bad, 0);
    c66x_free(c);
}

static void test_rs(const char *path)
{
    c66x_core *c = load(path);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    CHECK("rs: stopped at IDLE", c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    CHECK("rs: mvto 5-bit src ignores RS (b18 = b6)", c66x_get_reg(c, C66X_B(18)), 0x06);
    CHECK("rs: mvto srcms=2, cross (a21 = b19)", c66x_get_reg(c, C66X_A(21)), 0x19);
    CHECK("rs: mvfr 5-bit dst ignores RS (a9 = a17)", c66x_get_reg(c, C66X_A(9)), 0x17);
    CHECK("rs: mvfr 3-bit src takes RS (b4 = a16)", c66x_get_reg(c, C66X_B(4)), 0x16);
    CHECK("rs: decoy a25 untouched", c66x_get_reg(c, C66X_A(25)), 0x25);
    c66x_free(c);
}

static void test_memord(const char *path)
{
    c66x_core *c = load(path);
    for (int i = 0; i < 8; i++)
        wr32(l2 + 0x10000 + 4 * i, 0xA0000000u + i);
    wr32(l2 + 0x10014, 0x7777AAAA);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    CHECK("memord: stopped at IDLE", c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    CHECK("memord: store then load sees the new value", c66x_get_reg(c, C66X_A(2)), 0x11111111);
    CHECK("memord: load then store sees the old value", c66x_get_reg(c, C66X_A(3)), 0xA0000001);
    CHECK("memord: parallel store first, load sees old", c66x_get_reg(c, C66X_A(4)), 0xA0000002);
    CHECK("memord: parallel store first, memory is new", rd32(l2 + 0x10008), 0x22222222);
    CHECK("memord: parallel load first, load sees old", c66x_get_reg(c, C66X_A(5)), 0xA0000003);
    CHECK("memord: parallel load first, memory is new", rd32(l2 + 0x1000C), 0x22222222);
    CHECK("memord: pre-increment base used next packet", c66x_get_reg(c, C66X_A(6)), 0x11111111);
    CHECK("memord: pre-increment stored at base+16", rd32(l2 + 0x10010), 0x11111111);
    CHECK("memord: store reads its source at E1", rd32(l2 + 0x10018), 0);
    CHECK("memord: stdw low word first", rd32(l2 + 0x10020), 0x55667788);
    CHECK("memord: stdw high word second", rd32(l2 + 0x10024), 0x11223344);
    CHECK("memord: lddw low register", c66x_get_reg(c, C66X_A(12)), 0x55667788);
    CHECK("memord: lddw high register", c66x_get_reg(c, C66X_A(13)), 0x11223344);
    c66x_free(c);
}

static void test_stall(const char *path)
{
    c66x_core *c = load(path);
    wr32(l2 + 0x10000, 0x1234);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    CHECK("stall: stopped at IDLE", c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    c66x_stats s;
    c66x_get_stats(c, &s);
    CHECK("stall: one stall", s.stalls, 1);
    CHECK("stall: 2X read saw the E1 write", c66x_get_reg(c, C66X_B(2)), 7);
    CHECK("stall: packet 3 before the load", c66x_get_reg(c, C66X_A(10)), 0);
    CHECK("stall: packet 4 before the load", c66x_get_reg(c, C66X_A(11)), 0);
    CHECK("stall: packet 5 sees the load", c66x_get_reg(c, C66X_A(12)), 0x1234);
    c66x_free(c);
}

static void test_splmem(const char *path)
{
    c66x_core *c = load(path);
    wr32(l2 + 0x10000, 0xAAAA);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    CHECK("splmem: stopped at IDLE", c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    static const uint32_t want[8] = { 0xAAAA, 0xAAAA, 0x100, 0x101, 0x102, 0x103, 0x104, 0x105 };
    for (int k = 0; k < 8; k++) {
        char name[64];
        snprintf(name, sizeof name, "splmem: iteration %d loads the pre-store word", k);
        CHECK(name, rd32(l2 + 0x10100 + 4 * k), want[k]);
    }
    CHECK("splmem: output pointer +8 words", c66x_get_reg(c, C66X_B(0)), 0x00810120);
    c66x_free(c);
}

static void test_wlat(const char *path)
{
    c66x_core *c = load(path);
    wr32(l2 + 0x10000, 0x3F800000);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    CHECK("wlat: stopped at IDLE", c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    CHECK("wlat: +3 still the old A5", c66x_get_reg(c, C66X_A(12)), 0);
    CHECK("wlat: +4 the E4 mpysp square", c66x_get_reg(c, C66X_A(13)), 0x41100000);
    CHECK("wlat: +5 the E5 load", c66x_get_reg(c, C66X_A(14)), 0x3F800000);
    c66x_free(c);
}

static void test_xstall(const char *path)
{
    c66x_core *c = load(path);
    wr32(l2 + 0x10000, 0x3F800000);
    c66x_reset(c, 0x00800000);
    uint64_t ran;
    CHECK("xstall: stopped at IDLE", c66x_step(c, 100000, &ran), C66X_STOP_IDLE);
    CHECK("xstall: E4 result read through 1X as it lands", c66x_get_reg(c, C66X_A(4)), 0x40C00000);
    CHECK("xstall: same packet sees the E4 square", c66x_get_reg(c, C66X_A(13)), 0x41100000);
    CHECK("xstall: next packet sees the E5 load", c66x_get_reg(c, C66X_A(14)), 0x3F800000);
    CHECK("xstall: E1 result through 2X next cycle", c66x_get_reg(c, C66X_B(2)), 7);
    CHECK("xstall: load data through 2X", c66x_get_reg(c, C66X_B(3)), 0x3F800000);
    c66x_stats s;
    c66x_get_stats(c, &s);
    CHECK("xstall: only the E1 read stalls", s.stalls, 1);
    c66x_free(c);
}

int main(int argc, char **argv)
{
    if (argc < 18) {
        fprintf(stderr, "usage: core_test pipeline.bin sploop_irq.bin idle.bin idle_head brchain.bin splexit.bin mmio.bin mp3.bin resv.bin rs.bin idleirq.bin idleirq_head xstall.bin wlat.bin memord.bin splmem.bin stall.bin\n");
        return 2;
    }
    test_stall(argv[17]);
    test_memord(argv[15]);
    test_splmem(argv[16]);
    test_xstall(argv[13]);
    test_wlat(argv[14]);
    test_rs(argv[10]);
    test_idle_irq(argv[11], 0x00800000 + strtoul(argv[12], NULL, 16));
    run_ring(argv[8], 0, 8);
    run_ring(argv[8], 0x7FB, 13);
    run_ring(argv[8], 0x123, 1);
    run_resv(argv[9], 0x100, 0x40, 0x30, 8);
    run_resv(argv[9], 0x7F8, 0x0C, 0x02, 20);
    run_resv(argv[9], 0x200, 0x10, 0x08, 1);
    test_mmio(argv[7]);
    test_brchain(argv[5]);
    test_splexit(argv[6]);
    test_pipeline(argv[1]);
    run_irq(argv[2], -1, 0);
    /* The loop runs from about cycle 18 to 64; pulse INT4 across that span. */
    for (int at = 20; at <= 52; at += 4)
        run_irq(argv[2], at, 1);
    test_idle(argv[3], 0x00800000 + strtoul(argv[4], NULL, 16));
    printf("%d/%d checks passed\n", checks - fails, checks);
    return fails != 0;
}
