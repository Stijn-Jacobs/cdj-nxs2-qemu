/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Offline runner for the BF531 display processor: boots a GUI update section
 * without QEMU and writes the last PPI frame as a PPM.
 *
 *   bfinrun [-n cycles] [-t from:to] [-b pc] [-o frame.ppm] [-m sdram.bin] [-q]
 *           [-s hex]... C2KGUI.UPD
 *
 * -t traces every instruction whose cycle lies in [from, to) to stdout, in
 * the form bfin-elf-objdump can be lined up with; -b stops at a PC; -m saves
 * SDRAM at the end. -q drops the SoC's log of unmodelled registers. -s hands
 * the firmware a SPORT1 RX packet (hex bytes, no spaces), standing in for
 * MAIN's link: the first lands the moment it arms DMA3, each further one
 * after it has answered the one before; every answer is printed. On an
 * unimplemented instruction it stops and prints the PC and its 16-bit words.
 */
#include "bf531.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct runner {
    const char *ppm;
    unsigned w, h;
    uint16_t *last;
    bf531 *chip;
    /* SPORT1 packets in MAIN's order: the first is waiting at boot, each
     * later one goes out once the firmware has answered the one before. */
    uint8_t sport1[16][2048];
    size_t sport1_len[16];
    unsigned sport1_n, sport1_next;
} runner;

static void on_frame(void *opaque, const uint16_t *px, unsigned w, unsigned h)
{
    runner *r = opaque;

    r->last = realloc(r->last, (size_t)w * h * 2);
    memcpy(r->last, px, (size_t)w * h * 2);
    r->w = w;
    r->h = h;
}

static void on_answer(void *opaque, const uint8_t *data, size_t len)
{
    runner *r = opaque;

    fprintf(stderr, "answer %zu bytes:", len);
    for (size_t n = 0; n < len; n++) {
        fprintf(stderr, " %02x", data[n]);
    }
    fputc('\n', stderr);
    if (r->sport1_next < r->sport1_n) {
        bf531_sport1_rx(r->chip, r->sport1[r->sport1_next],
                        r->sport1_len[r->sport1_next]);
        r->sport1_next++;
    }
}

static void write_ppm(const runner *r)
{
    FILE *f = fopen(r->ppm, "wb");

    if (!f) {
        perror(r->ppm);
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", r->w, r->h);
    for (size_t n = 0; n < (size_t)r->w * r->h; n++) {
        uint16_t p = r->last[n];
        uint8_t rgb[3] = { (p >> 11) << 3, ((p >> 5) & 0x3F) << 2, (p & 0x1F) << 3 };

        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static size_t parse_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;

    while (s[0] && s[1] && n < cap) {
        unsigned byte;

        if (sscanf(s, "%2x", &byte) != 1) {
            break;
        }
        out[n++] = byte;
        s += 2;
    }
    return n;
}

static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;

    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc(*len);
    if (fread(buf, 1, *len, f) != *len) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    runner r = { .ppm = "frame.ppm" };
    bf531_host host = { .opaque = &r, .frame = on_frame,
                        .sport1_tx = on_answer };
    uint64_t cycles = 400000000, from = 0, to = 0;
    uint32_t brk = 0;
    const char *dump = NULL;
    FILE *log = stderr;
    bfin_stop stop;
    uint8_t *img;
    size_t len;
    bf531 *s;
    int opt;

    while ((opt = getopt(argc, argv, "n:t:o:b:m:qs:")) != -1) {
        switch (opt) {
        case 'n': cycles = strtoull(optarg, NULL, 0); break;
        case 't':
            from = strtoull(optarg, &optarg, 0);
            to = *optarg == ':' ? strtoull(optarg + 1, NULL, 0) : UINT64_MAX;
            break;
        case 'o': r.ppm = optarg; break;
        case 'b': brk = strtoul(optarg, NULL, 0); break;
        case 'm': dump = optarg; break;
        case 'q': log = NULL; break;
        case 's':
            if (r.sport1_n < 16) {
                r.sport1_len[r.sport1_n] = parse_hex(optarg, r.sport1[r.sport1_n],
                                                     sizeof(r.sport1[0]));
                r.sport1_n++;
            }
            break;
        default:
            fprintf(stderr, "usage: %s [-n cycles] [-t from:to] [-b pc] [-o frame.ppm] [-m sdram.bin] [-q] [-s hex]... update\n",
                    argv[0]);
            return 2;
        }
    }
    if (optind >= argc || !(img = read_file(argv[optind], &len))) {
        fprintf(stderr, "%s: no update section to boot\n", argv[0]);
        return 2;
    }
    s = r.chip = bf531_new(16u << 20, &host, log);
    if (r.sport1_n) {
        bf531_sport1_rx(s, r.sport1[0], r.sport1_len[0]);
        r.sport1_next = 1;
    }
    if (bf531_load_update(s, img, len)) {
        fprintf(stderr, "%s: not an LDR boot stream\n", argv[optind]);
        return 1;
    }
    if (to) {
        bfin_set_trace(bf531_core(s), stdout, from, to);
    }
    bfin_set_break(bf531_core(s), brk);
    stop = bf531_run(s, cycles);

    bfin_core *c = bf531_core(s);
    fprintf(stderr, "cycles %" PRIu64 ", pc 0x%08x, ipend 0x%04x, frames %" PRIu64 "\n",
            bfin_cycles(c), bfin_get_pc(c), bfin_get_ipend(c), bf531_frames(s));
    fprintf(stderr, "imask 0x%04x, ilat 0x%04x, tcntl 0x%x, tcount %u\n",
            bfin_get_mmr(c, 0xFFE02104), bfin_get_mmr(c, 0xFFE0210C),
            bfin_get_mmr(c, 0xFFE03000), bfin_get_mmr(c, 0xFFE0300C));
    if (stop == BFIN_STOP_UNDEF) {
        uint64_t insn = bfin_trap_insn(c);
        unsigned len = bfin_insn_len(insn >> (insn >> 32 ? 48 : 16));

        fprintf(stderr, "unimplemented at 0x%08x:", bfin_trap_pc(c));
        for (int n = len / 2 - 1; n >= 0; n--) {
            fprintf(stderr, " %04x", (unsigned)(insn >> (16 * n)) & 0xFFFF);
        }
        fputc('\n', stderr);
    } else if (stop == BFIN_STOP_BREAK) {
        fprintf(stderr, "break at 0x%08x\n", brk);
        fprintf(stderr, "r0 0x%08x r1 0x%08x r2 0x%08x r3 0x%08x\n",
                bfin_get_reg(c, 0, 0), bfin_get_reg(c, 0, 1),
                bfin_get_reg(c, 0, 2), bfin_get_reg(c, 0, 3));
        fprintf(stderr, "p0 0x%08x p1 0x%08x p2 0x%08x sp 0x%08x fp 0x%08x\n",
                bfin_get_reg(c, 1, 0), bfin_get_reg(c, 1, 1),
                bfin_get_reg(c, 1, 2), bfin_get_reg(c, 1, 6), bfin_get_reg(c, 1, 7));
    } else if (stop == BFIN_STOP_IDLE) {
        fprintf(stderr, "idle with no event pending\n");
    }
    if (r.last) {
        write_ppm(&r);
        fprintf(stderr, "last frame %ux%u -> %s\n", r.w, r.h, r.ppm);
    }
    if (dump) {
        uint32_t size;
        const uint8_t *mem = bf531_sdram(s, &size);
        FILE *f = fopen(dump, "wb");

        if (f) {
            fwrite(mem, 1, size, f);
            fclose(f);
        }
    }
    bf531_free(s);
    free(img);
    return stop == BFIN_STOP_UNDEF;
}
