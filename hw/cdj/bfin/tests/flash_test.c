/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The BF531's flash: AMD command set driven through bus accesses as firmware
 * would, status polling, read routing, flash boot, FLASH_OUT, snapshots.
 */
#include "bf531.h"
#include "bfin_priv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BASE 0x20000000u
#define SIZE (2u << 20)
#define CHECK(cond, ...) do {                                           \
        if (!(cond)) {                                                  \
            printf("FAIL line %d: ", __LINE__);                         \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
            failures++;                                                 \
        }                                                               \
    } while (0)

static int failures;
static char *logbuf;
static size_t logsz;
static FILE *logstream;
static bf531 *chip;
static bf531_host host;

static bf531 *fresh(void)
{
    if (chip) {
        bf531_free(chip);
    }
    /* Close the memstream before freeing its buffer: glibc only publishes
     * the text on fflush/fclose of that stream, and fflush(NULL) does not
     * reach it (the Linux failures of this test). */
    if (logstream) {
        fclose(logstream);
        logstream = NULL;
    }
    free(logbuf);
    logbuf = NULL;
    logsz = 0;
    logstream = open_memstream(&logbuf, &logsz);
    chip = bf531_new(0x100000, &host, logstream);
    return chip;
}

static const char *logtext(void)
{
    if (logstream) {
        fflush(logstream);
    }
    return logbuf ? logbuf : "";
}

static uint32_t rd(uint32_t a) { return bf531_bus_read(chip, BASE + a, 2); }
static void wr(uint32_t a, uint32_t v) { bf531_bus_write(chip, BASE + a, v, 2); }
static void cmd(uint32_t v) { wr(0xAAA, 0xAA); wr(0x554, 0x55); wr(0xAAA, v); }
static int mapped(void) { return bfin_bank(bf531_core(chip), BASE, 2) != NULL; }

static void program(uint32_t a, uint16_t v)
{
    cmd(0xA0);
    wr(a, v);
}

/* Polls as a driver would: DQ6 until two reads agree. Returns the reads it
 * took, negated when DQ6 never toggled. */
static int poll_dq6(uint32_t a)
{
    int n = 0, toggled = 0;
    uint32_t prev = rd(a) & 0x40;

    while (n++ < 100000) {
        uint32_t cur = rd(a) & 0x40;

        toggled |= cur != prev;
        if (cur == prev) {
            break;
        }
        prev = cur;
    }
    return toggled ? n : -n;
}

static void put_ldr(uint8_t *img)
{
    /* One last block into the boot entry: 300 NOPs, then JUMP.S . */
    uint8_t *p = img + 0x10000;
    uint32_t dest = 0xFFA08000u, count = 602;
    uint16_t flags = 0x8000, jmp = 0x2000;

    memcpy(p, &dest, 4);
    memcpy(p + 4, &count, 4);
    memcpy(p + 8, &flags, 2);
    memset(p + 10, 0, 600);
    memcpy(p + 10 + 600, &jmp, 2);
}

/* Scratch is a real LDR destination as well as mapped CPU RAM. Exercise the
 * public loader, including zero-fill blocks with no payload to bound count. */
static int scratch_ldr(uint32_t dest, uint32_t count, int zero)
{
    uint8_t img[0x20 + 10 + 0x1000] = { 0 };
    uint16_t flags = 0x8000 | (zero ? 1 : 0);

    memcpy(img + 0x20, &dest, 4);
    memcpy(img + 0x24, &count, 4);
    memcpy(img + 0x28, &flags, 2);
    for (size_t n = 0; n < sizeof(img) - 0x2a; n++) {
        img[0x2a + n] = (uint8_t)(0x93 ^ n);
    }
    return bf531_load_update(chip, img, zero ? 0x2a : 0x2a + count);
}

static void scratch_loader(void)
{
    const uint32_t base = 0xFFB00000u, size = 0x1000;
    const bfin_ram *bank;
    uint8_t *scratch;

    fresh();
    bank = bfin_bank(bf531_core(chip), base, size);
    scratch = bank ? bank->host + (base - bank->base) : NULL;
    CHECK(scratch != NULL, "scratch is mapped CPU RAM");
    if (!scratch) {
        return;
    }
    CHECK(bf531_mem(chip, base, size) == scratch, "full scratch host view");
    memset(scratch, 0xA5, size);
    CHECK(scratch_ldr(base, 64, 0) == 0, "LDR loads the 64-byte fatal record reservation");
    for (unsigned n = 0; n < 64; n++) {
        CHECK(scratch[n] == (uint8_t)(0x93 ^ n), "scratch payload byte %u", n);
    }
    CHECK(scratch[64] == 0xA5 && scratch[size - 1] == 0xA5, "payload respects adjacent bytes");
    CHECK(scratch_ldr(base + size - 1, 1, 0) == 0, "last scratch byte is a valid LDR target");
    CHECK(scratch[size - 1] == 0x93 && scratch[size - 2] == 0xA5, "last-byte payload content/guard");
    CHECK(scratch_ldr(base, size, 1) == 0, "full scratch zero-fill");
    for (unsigned n = 0; n < size; n++) {
        CHECK(scratch[n] == 0, "scratch zero-fill byte %u", n);
    }
    memset(scratch, 0x5A, size);
    CHECK(scratch_ldr(base + size, 1, 1) == -1, "end address is not scratch");
    CHECK(scratch_ldr(base + size, 0, 1) == -1, "empty block at end is outside mapped bank");
    CHECK(scratch_ldr(base + size - 1, 2, 1) == -1, "cannot cross scratch end");
    CHECK(scratch_ldr(base, size + 1, 1) == -1, "oversize scratch block refused");
    CHECK(scratch_ldr(base - 1, 2, 1) == -1, "cannot enter scratch from below");
    CHECK(scratch_ldr(base, UINT32_MAX, 1) == -1, "count cannot wrap destination range");
    CHECK(scratch_ldr(UINT32_MAX, 2, 1) == -1, "address cannot wrap into another bank");
    CHECK(bf531_mem(chip, base, SIZE_MAX) == NULL, "host length cannot narrow to u32");
    for (unsigned n = 0; n < size; n++) {
        CHECK(scratch[n] == 0x5A, "refused LDR preserves scratch byte %u", n);
    }
}

int main(void)
{
    uint8_t *img = malloc(SIZE);
    char tmp[] = "/tmp/flash_test_XXXXXX";
    int fd;

    memset(img, 0xFF, SIZE);

    /* array mode is ROM-mapped; reset with nothing pending is harmless */
    fresh();
    CHECK(mapped(), "array mode maps the ROM");
    CHECK(rd(0) == 0xFFFF, "erased array");
    wr(0, 0xF0);
    CHECK(mapped(), "F0 in array mode");

    /* autoselect */
    cmd(0x90);
    CHECK(!mapped(), "autoselect unmaps");
    CHECK(rd(0) == 0x0089 && rd(2) == 0x00AD, "ids %x %x", rd(0), rd(2));
    CHECK(bfin_load_nw(bf531_core(chip), BASE + 2, 2) == 0x00AD,
          "core read goes through the model");
    wr(0x20000, 0xF0);          /* reset at any address */
    CHECK(mapped() && rd(0) == 0xFFFF, "reset to array");

    /* CFI query */
    wr(0xAA, 0x98);
    CHECK(!mapped(), "CFI unmaps");
    CHECK(rd(0x20) == 'Q' && rd(0x22) == 'R' && rd(0x24) == 'Y', "QRY");
    CHECK(rd(0x26) == 2 && rd(0x28) == 0, "cmdset 2");
    CHECK(rd(0x4E) == 0x15, "size 2^21");
    CHECK(rd(0x50) == 1, "x16");
    CHECK(rd(0x58) == 2 && rd(0x5A) == 0x1E && rd(0x5C) == 0 &&
          rd(0x5E) == 0 && rd(0x60) == 1, "region 1: 31 x 64K");
    CHECK(rd(0x62) == 7 && rd(0x64) == 0 && rd(0x66) == 0x20 &&
          rd(0x68) == 0, "region 2: 8 x 8K (top boot)");
    wr(0, 0xF0);
    CHECK(mapped(), "reset leaves CFI");

    /* program: AND semantics, DQ6 and DQ7 polling */
    fresh();
    program(0x100, 0x1234);
    CHECK(!mapped(), "busy unmaps");
    CHECK((rd(0x100) & 0x80) == (~0x1234u & 0x80), "DQ7 is the complement while busy");
    {
        uint32_t a = rd(0x100), b = rd(0x100);

        CHECK(((a ^ b) & 0x40) != 0, "DQ6 toggles");
    }
    for (int i = 0; i < 100 && !mapped(); i++) {
        (void)rd(0x100);
    }
    CHECK(mapped() && rd(0x100) == 0x1234, "programmed %x", rd(0x100));
    program(0x100, 0xFF0F);
    CHECK(poll_dq6(0x100) > 0, "DQ6 poll toggled then stopped");
    CHECK(rd(0x100) == (0x1234 & 0xFF0F), "AND semantics %x", rd(0x100));
    program(0x102, 0x00FF);
    {
        int n = 0;

        while ((rd(0x102) & 0x80) != (0x00FF & 0x80) && n++ < 1000) {
        }
        CHECK(n < 1000 && rd(0x102) == 0x00FF, "DQ7 poll ends");
    }
    /* a broken unlock does nothing */
    wr(0xAAA, 0xAA); wr(0x558, 0x55); wr(0xAAA, 0xA0); wr(0x200, 0);
    CHECK(rd(0x200) == 0xFFFF && mapped(), "bad unlock ignored");

    /* sector erase, then chip erase */
    fresh();
    program(0x10000, 0); poll_dq6(0x10000);
    program(0x20000, 0); poll_dq6(0x20000);
    cmd(0x80);
    wr(0xAAA, 0xAA); wr(0x554, 0x55); wr(0x10000, 0x30);
    CHECK(!mapped(), "erase busy");
    CHECK(poll_dq6(0x10000) > 0, "erase DQ6 poll ends");
    CHECK(rd(0x10000) == 0xFFFF && rd(0x1FFFE) == 0xFFFF, "sector is 0xFF");
    CHECK(rd(0x20000) == 0, "next sector kept");
    CHECK(strstr(logtext(), "cdj_bf531: flash erase 0x20010000 +0x10000\n") != NULL,
          "erase logged: %s", logtext());
    /* a boot-sector erase takes 8 KiB and leaves its neighbours (the journal) */
    program(0x1FC000, 0x0300); poll_dq6(0x1FC000);
    program(0x1FA000, 0x1234); poll_dq6(0x1FA000);
    wr(0xAAA, 0xAA); wr(0x554, 0x55); wr(0xAAA, 0x80);
    wr(0xAAA, 0xAA); wr(0x554, 0x55); wr(0x1FA000, 0x30);
    CHECK(poll_dq6(0x1FA000) > 0, "boot-sector erase ends");
    CHECK(rd(0x1FA000) == 0xFFFF && rd(0x1FC000) == 0x0300, "8K boot sector erased, journal kept");
    CHECK(strstr(logtext(), "cdj_bf531: flash erase 0x201fa000 +0x2000\n") != NULL,
          "boot erase logged: %s", logtext());
    /* RY/BY# on PF0 (an input): low through an erase nobody polls, high after */
    {
        bfin_core *core = bf531_core(chip);

        wr(0xAAA, 0xAA); wr(0x554, 0x55); wr(0xAAA, 0x80);
        wr(0xAAA, 0xAA); wr(0x554, 0x55); wr(0x1F8000, 0x30);
        CHECK((bfin_load_nw(core, 0xFFC00700, 2) & 1) == 0, "PF0 low while erasing");
        bfin_skip_cycles(core, 500000);
        CHECK((bfin_load_nw(core, 0xFFC00700, 2) & 1) == 1, "PF0 high once the erase time passed");
        CHECK(rd(0x1F8000) == 0xFFFF && mapped(), "erase done, array mapped again");
    }
    cmd(0x80); cmd(0x10);
    CHECK(poll_dq6(0) > 0, "chip erase ends");
    CHECK(rd(0x20000) == 0xFFFF, "chip erased");
    bf531_flash_report(chip);
    CHECK(strstr(logtext(), "cdj_bf531: flash 4 words programmed, 42 sectors erased\n") != NULL,
          "totals: %s", logtext());

    /* snapshot mid-command */
    fresh();
    program(0x40, 0xBEEF);
    {
        size_t n = bf531_save(chip, NULL);
        uint8_t *snap = malloc(n);

        bf531_save(chip, snap);
        for (int i = 0; i < 100 && !mapped(); i++) {
            (void)rd(0x40);
        }
        CHECK(mapped(), "finished");
        bf531_load(chip, snap);
        CHECK(!mapped(), "snapshot restores the busy state and routing");
        poll_dq6(0x40);
        CHECK(rd(0x40) == 0xBEEF, "programmed value in the snapshot");
        free(snap);
    }

    /* flash boot, code-range count, FLASH_OUT */
    put_ldr(img);
    fresh();
    CHECK(bf531_flash_load(chip, img, SIZE - 1) == -1, "wrong size refused");
    CHECK(bf531_flash_load(chip, img, SIZE) == 0, "load");
    bf531_code_range(chip, 0xFFA08000u, 0xFFA09000u);
    CHECK(bf531_flash_boot(chip) == 0, "boot");
    CHECK(strstr(logtext(), "cdj_bf531: booting the flash's LDR at 0x10000\n") != NULL,
          "boot logged");
    CHECK(bfin_get_pc(bf531_core(chip)) == 0xFFA08000u, "pc at boot entry");
    bf531_run(chip, 2000);
    bf531_code_range_report(chip);
    CHECK(strstr(logtext(), "cdj_bf531: RANGE 0xffa08000:0xffa09000 lines_run=3\n") != NULL,
          "lines_run: %s", logtext());

    program(0x300, 0xA5A5);
    poll_dq6(0x300);
    fd = mkstemp(tmp);
    close(fd);
    CHECK(bf531_flash_save(chip, tmp) == 0, "save");
    {
        FILE *f = fopen(tmp, "rb");
        uint8_t *out = malloc(SIZE);
        size_t got = f ? fread(out, 1, SIZE, f) : 0;

        img[0x300] = 0xA5;
        img[0x301] = 0xA5;
        CHECK(got == SIZE && !memcmp(out, img, SIZE), "FLASH_OUT equals the expected image");
        if (f) {
            fclose(f);
        }
        free(out);
    }
    remove(tmp);
    scratch_loader();
    bf531_free(chip);
    chip = NULL;
    fclose(logstream);
    logstream = NULL;
    free(logbuf);
    printf(failures ? "flash_test: %d FAILED\n" : "flash_test: all passed\n", failures);
    return failures != 0;
}
