/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Decoder check: decode the DSP image at the addresses objdump visited and
 * print the text objdump would. Spans are the boot-table sections merged where
 * contiguous, matching how the objdump reference was produced (a fetch packet
 * that crosses a span edge is unreadable to it).
 *
 * usage: c6xdis <main_unpacked.bin> [offset]  < addresses (one decimal per line)
 *        c6xdis <main_unpacked.bin> [offset] -s   (print sections)
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../c66x_decode.h"
#include "../c66x_image.h"

typedef struct span { uint32_t addr, size; uint8_t *data; } span;
static span spans[64];
static unsigned nspans;

static int cmp_sec(const void *a, const void *b)
{
    const c66x_section *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static int reader(void *opaque, uint32_t fp_addr, uint8_t fp[32])
{
    (void)opaque;
    for (unsigned i = 0; i < nspans; i++)
        if (fp_addr >= spans[i].addr && fp_addr + 32 <= spans[i].addr + spans[i].size) {
            memcpy(fp, spans[i].data + (fp_addr - spans[i].addr), 32);
            return 0;
        }
    return -1;
}

int main(int argc, char **argv)
{
    c66x_image img;
    size_t off = argc > 2 && argv[2][0] != '-' ? strtoul(argv[2], NULL, 0) : 0x34E0;
    if (argc < 2 || c66x_image_load(&img, argv[1], off)) {
        fprintf(stderr, "cannot load boot table\n");
        return 1;
    }
    c66x_section sec[64];
    memcpy(sec, img.sections, sizeof sec);
    qsort(sec, img.nsections, sizeof sec[0], cmp_sec);
    for (unsigned i = 0; i < img.nsections; i++) {
        if (nspans && sec[i].addr == spans[nspans - 1].addr + spans[nspans - 1].size) {
            span *s = &spans[nspans - 1];
            s->data = realloc(s->data, s->size + sec[i].size);
            memcpy(s->data + s->size, sec[i].data, sec[i].size);
            s->size += sec[i].size;
        } else {
            span *s = &spans[nspans++];
            s->addr = sec[i].addr;
            s->size = sec[i].size;
            s->data = malloc(s->size);
            memcpy(s->data, sec[i].data, s->size);
        }
    }
    if (argc > 2 && !strcmp(argv[argc - 1], "-s")) {
        printf("entry 0x%08x, %u sections\n", img.entry, img.nsections);
        for (unsigned i = 0; i < img.nsections; i++)
            printf("  0x%08x %u\n", img.sections[i].addr, img.sections[i].size);
        for (unsigned i = 0; i < nspans; i++)
            printf("span 0x%08x %u\n", spans[i].addr, spans[i].size);
        return 0;
    }

    char line[64], text[128];
    while (fgets(line, sizeof line, stdin)) {
        uint32_t a = strtoul(line, NULL, 0);
        c66x_insn in;
        int n = c66x_decode(reader, NULL, a, &in, text, sizeof text);
        if (n < 0) {
            printf("%u\t<read error>\n", a);
            continue;
        }
        bool par = in.opc >= 0 && c66x_prev_parallel(reader, NULL, a);
        printf("%u\t%s%s\n", a, par ? "|| " : "", text);
    }
    return 0;
}
