/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Runs a piece of C6x code on the emulator's core, for the tests of the code
 * a DSP mod places:
 *
 *     c6x_run <entry> <stop> [file@addr]... [a4=value]... [call=addr]... [dump=addr:words]...
 *
 * RAM is the C6655's L2 (0x00800000, 1 MB) and the first 1 MB of DDR3
 * (0x80000000). The core starts at entry and runs until it reaches stop; a
 * call= address stands for a routine of the program that is not loaded: when
 * the core reaches it, "call <addr> <a4> <b4> <a6> <b6> <a8> <b8> <a10> <b10>"
 * is printed and the core goes on at B3. At the end every register is
 * printed, then each dump as "<addr> <word>..." lines.
 *
 * Build: gcc -Ihw/cdj/c6x tests/c6x_run.c hw/cdj/c6x/c66x_*.c -lm and glib's gmodule.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c66x.h"

static uint8_t l2[0x100000], ddr[0x100000];

static uint8_t *ram(uint32_t addr)
{
    if (addr - 0x00800000u < sizeof l2)
        return l2 + (addr - 0x00800000u);
    if (addr - 0x80000000u < sizeof ddr)
        return ddr + (addr - 0x80000000u);
    return NULL;
}

static int stop_hook(c66x_core *c, void *opaque)
{
    return 1;
}

static int call_hook(c66x_core *c, void *opaque)
{
    printf("call %08x", (uint32_t)(uintptr_t)opaque);
    for (unsigned r = 4; r <= 10; r += 2)
        printf(" %08x %08x", c66x_get_reg(c, r), c66x_get_reg(c, 32 + r));
    printf("\n");
    c66x_set_pc(c, c66x_get_reg(c, 32 + 3));
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3)
        return 2;
    c66x_core *c = c66x_new(NULL);
    c66x_map_ram(c, 0x00800000, sizeof l2, l2);
    c66x_map_ram(c, 0x80000000, sizeof ddr, ddr);
    uint32_t entry = strtoul(argv[1], NULL, 0), stop = strtoul(argv[2], NULL, 0);
    c66x_reset(c, entry);
    c66x_hook_pc(c, stop, stop_hook, NULL);
    for (int i = 3; i < argc; i++) {
        char *at = strchr(argv[i], '@'), *eq = strchr(argv[i], '=');
        if (at) {
            *at = 0;
            FILE *f = fopen(argv[i], "rb");
            uint8_t *dst = ram(strtoul(at + 1, NULL, 0));
            if (!f || !dst)
                return 1;
            fread(dst, 1, 0x40000, f);
            fclose(f);
        } else if (eq && !strncmp(argv[i], "call=", 5)) {
            uint32_t addr = strtoul(eq + 1, NULL, 0);
            c66x_hook_pc(c, addr, call_hook, (void *)(uintptr_t)addr);
        } else if (eq && !strncmp(argv[i], "dump=", 5)) {
            continue;
        } else if (eq) {
            unsigned reg = strtoul(argv[i] + 1, NULL, 10) + (argv[i][0] == 'b' ? 32 : 0);
            c66x_set_reg(c, reg, strtoul(eq + 1, NULL, 0));
        }
    }
    uint64_t ran;
    c66x_stop why = c66x_step(c, 100000000, &ran);
    printf("stop %d pc %08x cycles %llu\n", why, c66x_get_pc(c), (unsigned long long)c66x_get_cycle(c));
    for (unsigned r = 0; r < 64; r++)
        printf("%c%u %08x\n", r < 32 ? 'a' : 'b', r & 31, c66x_get_reg(c, r));
    for (int i = 3; i < argc; i++) {
        if (strncmp(argv[i], "dump=", 5))
            continue;
        char *e;
        uint32_t addr = strtoul(argv[i] + 5, &e, 0), n = strtoul(e + 1, NULL, 0);
        printf("%08x", addr);
        for (uint32_t k = 0; k < n; k++) {
            uint32_t w;
            memcpy(&w, ram(addr + 4 * k), 4);
            printf(" %08x", w);
        }
        printf("\n");
    }
    return why == C66X_STOP_HOOK ? 0 : 3;
}
