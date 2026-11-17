/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The native kernels are found by their code wherever a release links it. The
 * NXS2's DSP program is loaded as MAIN ships it, its FIR is found, and then
 * moved to another address with another centre-tap table, as a relink would.
 *   make -C hw/cdj/c6x O=~/build/c6x native-test
 * usage: native_test <NXS2 main_unpacked.bin> [boot table offset, 0x34E0]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../c66x_priv.h"
#include "../c66x_image.h"

/* the FIR's MVK/MVKH of its centre-tap table, from the start of its code */
#define FIR_MVK  0x210
#define FIR_MVKH 0x21C
#define MOVED_TABLE 0x0099C000u

static int fails;

static uint8_t l2[0x100000];
static uint8_t ddr[0x01000000];

static uint8_t *host(uint32_t a)
{
    if (a - 0x00800000u < sizeof l2)
        return l2 + (a - 0x00800000u);
    if (a - 0x80000000u < sizeof ddr)
        return ddr + (a - 0x80000000u);
    return NULL;
}

static void set_cst(uint32_t a, uint16_t cst)
{
    uint32_t w;
    memcpy(&w, host(a), 4);
    w = (w & ~0x007FFF80u) | (uint32_t)cst << 7;
    memcpy(host(a), &w, 4);
}

/* Decode the fetch packets of a..a+n, the way running them would, and drop
 * what was decoded before, the way a store into code does. */
static void decode(c66x_core *c, uint32_t a, uint32_t n)
{
    for (uint32_t p = a & ~31u; p < a + n; p += 32)
        fetch_insn(c, p);
    c->code_gen++;
}

static void expect(c66x_core *c, const char *what, int found, uint32_t at, uint32_t table)
{
    uint32_t got_at = 0, addr[2] = { 0, 0 };
    int got = native_find(c, "fir", &got_at, addr);
    int ok = got == found && (!found || (got_at == at && addr[0] == table));
    fails += !ok;
    printf("  %s %-34s found %d at 0x%08x table 0x%08x\n", ok ? "ok  " : "FAIL", what, got, got_at, addr[0]);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <NXS2 main_unpacked.bin> [boot table offset]\n", argv[0]);
        return 2;
    }
    c66x_image img;
    if (c66x_image_load(&img, argv[1], argc > 2 ? strtoul(argv[2], NULL, 0) : 0x34E0)) {
        fprintf(stderr, "%s: no DSP boot table\n", argv[1]);
        return 2;
    }
    c66x_core *c = c66x_new(NULL);
    c66x_map_ram(c, 0x00800000, sizeof l2, l2);
    c66x_map_ram(c, 0x80000000, sizeof ddr, ddr);
    for (unsigned i = 0; i < img.nsections; i++) {
        const c66x_section *s = &img.sections[i];
        uint8_t *to = host(s->addr);
        if (to && host(s->addr + s->size - 1) && s->size <= sizeof ddr) {
            memcpy(to, s->data, s->size);
            decode(c, s->addr, s->size);
        }
    }

    uint32_t at = 0, addr[2];
    if (!native_find(c, "fir", &at, addr)) {
        printf("  FAIL the FIR is not in this image\n");
        return 1;
    }
    expect(c, "in the image as shipped", 1, at, addr[0]);

    /* Move the FIR's whole 4 KB page to the top of DDR, so its branches keep
     * their targets, and point it at another table. */
    uint32_t page = at & ~0xFFFu, moved = 0x80000000u + sizeof ddr - 0x1000, moved_at = moved + (at - page);
    uint8_t orig[0x1000];
    memcpy(orig, host(page), sizeof orig);
    memcpy(host(moved), orig, sizeof orig);
    memset(host(page), 0, sizeof orig);
    set_cst(moved_at + FIR_MVK, MOVED_TABLE & 0xFFFF);
    set_cst(moved_at + FIR_MVKH, MOVED_TABLE >> 16);
    decode(c, page, sizeof orig);
    decode(c, moved, sizeof orig);
    expect(c, "moved, with another table", 1, moved_at, MOVED_TABLE);

    memcpy(host(page), orig, sizeof orig);
    decode(c, page, sizeof orig);
    expect(c, "in two places", 0, 0, 0);

    memset(host(moved), 0, sizeof orig);
    memcpy(host(moved), orig, sizeof orig);
    memset(host(moved_at + 0x100), 0, 4);
    memset(host(page), 0, sizeof orig);
    decode(c, page, sizeof orig);
    decode(c, moved, sizeof orig);
    expect(c, "moved with one word changed", 0, 0, 0);

    c66x_free(c);
    c66x_image_free(&img);
    printf("%s\n", fails ? "FAILED" : "all ok");
    return fails != 0;
}
