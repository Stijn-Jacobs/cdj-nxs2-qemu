/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Decoder cross-check: every instruction of a Ghidra M16C/60 listing
 * ("address hexbytes mnemonic operands" per line) against m16c_disas on the
 * same image, by length and mnemonic.
 *
 *   decode_check image.flat base listing.txt
 *
 * Ghidra's LDINTB is the two LDC instructions it is made of, so it is checked
 * as those.
 */
#include "../m16c.h"
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    static uint8_t img[0x100000 + 16];
    char line[512], name[32];
    unsigned long base, total = 0, bad = 0;
    FILE *f, *l;
    size_t n;

    if (argc < 4 || !(f = fopen(argv[1], "rb")) || !(l = fopen(argv[3], "r"))) {
        fprintf(stderr, "usage: %s image.flat base listing.txt\n", argv[0]);
        return 2;
    }
    base = strtoul(argv[2], NULL, 0);
    n = fread(img + base, 1, sizeof(img) - 16 - base, f);
    fclose(f);
    (void)n;
    while (fgets(line, sizeof(line), l)) {
        char hex[64], mnem[32];
        unsigned long pc;
        unsigned len, want;

        if (sscanf(line, "%lx %63s %31s", &pc, hex, mnem) != 3) {
            continue;
        }
        want = strlen(hex) / 2;
        total++;
        len = m16c_disas(img + pc, pc, name, sizeof(name));
        if (!strcmp(mnem, "LDINTB")) {
            unsigned len2 = len ? m16c_disas(img + pc + len, pc + len, name, sizeof(name)) : 0;

            if (len + len2 == want && !strcmp(name, "LDC")) {
                continue;
            }
        } else if (len == want && !strcmp(name, mnem)) {
            continue;
        }
        bad++;
        if (bad <= 40) {
            printf("%06lx %s: ghidra %s (%u), m16c %s (%u)\n", pc, hex, mnem,
                   want, len ? name : "-", len);
        }
    }
    printf("%lu instructions, %lu mismatches\n", total, bad);
    return bad != 0;
}
