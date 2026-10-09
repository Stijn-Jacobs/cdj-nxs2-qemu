/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Offline runner for the CDJ-900 GUI processor: the M16C/63 model with a
 * flash image, run from reset with no MAIN behind it.
 *
 *   m16crun [-c cycles] [-t from:to] [-b pc] [-p port=hex]... [-l frames]
 *           [-w cycles] [-i cycles] [-d addr:len]... [-s] image.flat base
 *
 * -c is the run length in f1 cycles (20 per microsecond); -t traces every
 * instruction whose cycle lies in [from, to); -b stops at a PC; -p sets the
 * pin levels read on a port (0-10); -d dumps memory (hex addr:len) at the end.
 * -l gives MAIN's link frames, one line of hex bytes each, clocked in one byte
 * every -i cycles from cycle -w on, the next frame once the reply to the last
 * has ended; port 6 bit 4 (the chip select) reads low while a frame is
 * being clocked. Everything the UARTs at 0x272 and 0x2A8 shift out and the reply
 * on the link are printed, one line per burst. -s adds the first access to
 * every SFR nothing models and the addresses the program counter sampled
 * most.
 */
#include "m16c63.h"
#include <stdlib.h>
#include <string.h>

#define SLICE_CYCLES 64
#define TX_BURST     128
#define MAX_FRAMES   64
#define FRAME_BYTES  4096

typedef struct runner {
    m16c63 *chip;
    uint8_t port_pins[11];
    uint32_t dump_addr[8], dump_len[8];
    unsigned ndump;
    int show_sfr;
    uint32_t hits[0x100000];
    uint64_t last_tx[3], tx_start[3];
    unsigned tx_len[3];
    uint8_t tx[3][TX_BURST];
    uint8_t frames[MAX_FRAMES][FRAME_BYTES];
    unsigned frame_len[MAX_FRAMES], nframes, next_frame, sent;
    unsigned reply_len;
    int link_selected;
    uint8_t reply[FRAME_BYTES];
} runner;

static uint64_t cycles_now(runner *r)
{
    return m16c_cycles(m16c63_cpu(r->chip));
}

static void flush_burst(runner *r, unsigned unit)
{
    if (r->tx_len[unit]) {
        printf("uart%u %llu:", unit, (unsigned long long)r->tx_start[unit]);
        for (unsigned i = 0; i < r->tx_len[unit]; i++) {
            printf(" %02x", r->tx[unit][i]);
        }
        printf("\n");
        r->tx_len[unit] = 0;
    }
}

/* One line per burst: a new one once the UART has been quiet a while. */
static void uart_tx(void *opaque, unsigned unit, uint8_t byte)
{
    runner *r = opaque;
    uint64_t t = cycles_now(r);

    if (r->tx_len[unit] && (t - r->last_tx[unit] > 2000 || r->tx_len[unit] == TX_BURST)) {
        flush_burst(r, unit);
    }
    if (!r->tx_len[unit]) {
        r->tx_start[unit] = t;
    }
    r->tx[unit][r->tx_len[unit]++] = byte;
    r->last_tx[unit] = t;
}

static uint8_t port_in(void *opaque, unsigned port)
{
    runner *r = opaque;

    if (port == 6 && r->link_selected) {
        return r->port_pins[port] & ~0x10;
    }
    return r->port_pins[port];
}

static void unmodelled(void *opaque, uint32_t addr, int write, uint32_t val,
                       uint32_t pc)
{
    runner *r = opaque;

    if (r->show_sfr) {
        for (unsigned unit = 0; unit < 3; unit++) {
            flush_burst(r, unit);
        }
        printf("%s %05x %s %04x at pc %05x cycle %llu\n", addr < 0x400 ? "sfr" : "mem", addr,
               write ? "w" : "r", val, pc, (unsigned long long)cycles_now(r));
    }
}

static void top_pcs(runner *r)
{
    for (int rank = 0; rank < 12; rank++) {
        uint32_t best = 0;

        for (uint32_t pc = 1; pc < 0x100000; pc++) {
            if (r->hits[pc] > r->hits[best]) {
                best = pc;
            }
        }
        if (!r->hits[best]) {
            break;
        }
        printf("hot pc %05x %u\n", best, r->hits[best]);
        r->hits[best] = 0;
    }
}

static void load_frames(runner *r, const char *path)
{
    char line[2 * FRAME_BYTES + 8];
    FILE *f = fopen(path, "r");

    while (f && r->nframes < MAX_FRAMES && fgets(line, sizeof(line), f)) {
        unsigned n = 0, v;
        char *p = line;

        while (sscanf(p, " %2x", &v) == 1 && n < FRAME_BYTES) {
            r->frames[r->nframes][n++] = v;
            p += strspn(p, " ") + 2;
        }
        if (n) {
            r->frame_len[r->nframes++] = n;
        }
    }
    if (f) {
        fclose(f);
    }
}

/* One link byte per call, when the previous frame's reply is over. */
static void feed_link(runner *r, uint64_t byte_gap, uint64_t *next_byte)
{
    uint8_t out;

    r->link_selected = r->next_frame < r->nframes && cycles_now(r) + 1000 >= *next_byte;
    if (r->next_frame >= r->nframes || cycles_now(r) < *next_byte) {
        return;
    }
    out = m16c63_link_clock(r->chip, r->frames[r->next_frame][r->sent]);
    r->reply[r->reply_len++] = out;
    *next_byte = cycles_now(r) + byte_gap;
    if (++r->sent == r->frame_len[r->next_frame]) {
        printf("reply %llu:", (unsigned long long)cycles_now(r));
        for (unsigned i = 0; i < r->reply_len; i++) {
            printf(" %02x", r->reply[i]);
        }
        printf("\n");
        r->next_frame++;
        r->sent = 0;
        r->reply_len = 0;
        *next_byte = cycles_now(r) + 20 * byte_gap;
    }
}

int main(int argc, char **argv)
{
    static uint8_t flash[0x100000];
    static runner r;
    m16c63_ops ops = { &r, uart_tx, port_in, NULL, unmodelled };
    uint64_t budget = 20000000, from = 1, to = 0, next_byte = 2000000, gap = 640;
    uint32_t brk = 0, base;
    FILE *f, *trace = NULL;
    size_t len;
    int opt = 1;
    m16c_stop why = M16C_STOP_BUDGET;

    for (; opt < argc && argv[opt][0] == '-'; opt++) {
        if (!strcmp(argv[opt], "-c") && opt + 1 < argc) {
            budget = strtoull(argv[++opt], NULL, 0);
        } else if (!strcmp(argv[opt], "-t") && opt + 1 < argc) {
            from = strtoull(argv[++opt], NULL, 0);
            to = strtoull(strchr(argv[opt], ':') + 1, NULL, 0);
            trace = stdout;
        } else if (!strcmp(argv[opt], "-b") && opt + 1 < argc) {
            brk = strtoul(argv[++opt], NULL, 16);
        } else if (!strcmp(argv[opt], "-p") && opt + 1 < argc) {
            unsigned port = strtoul(argv[++opt], NULL, 0);
            const char *eq = strchr(argv[opt], '=');

            if (eq && port < 11) {
                r.port_pins[port] = strtoul(eq + 1, NULL, 16);
            }
        } else if (!strcmp(argv[opt], "-l") && opt + 1 < argc) {
            load_frames(&r, argv[++opt]);
        } else if (!strcmp(argv[opt], "-w") && opt + 1 < argc) {
            next_byte = strtoull(argv[++opt], NULL, 0);
        } else if (!strcmp(argv[opt], "-i") && opt + 1 < argc) {
            gap = strtoull(argv[++opt], NULL, 0);
        } else if (!strcmp(argv[opt], "-d") && opt + 1 < argc && r.ndump < 8) {
            r.dump_addr[r.ndump] = strtoul(argv[++opt], NULL, 16);
            r.dump_len[r.ndump++] = strtoul(strchr(argv[opt], ':') + 1, NULL, 16);
        } else if (!strcmp(argv[opt], "-s")) {
            r.show_sfr = 1;
        }
    }
    if (argc - opt != 2 || !(f = fopen(argv[opt], "rb"))) {
        fprintf(stderr, "usage: %s [-c cycles] [-t from:to] [-b pc] [-p port=hex] "
                "[-l frames] [-i cycles] [-s] image.flat base\n", argv[0]);
        return 2;
    }
    base = strtoul(argv[opt + 1], NULL, 0);
    len = fread(flash + base, 1, sizeof(flash) - base, f);
    fclose(f);

    r.chip = m16c63_new(flash + base, base, len, &ops);
    m16c_set_trace(m16c63_cpu(r.chip), trace, from, to);
    m16c_set_break(m16c63_cpu(r.chip), brk);
    while (cycles_now(&r) < budget) {
        why = m16c63_run(r.chip, SLICE_CYCLES);
        r.hits[m16c_get_reg(m16c63_cpu(r.chip), M16C_PC)]++;
        feed_link(&r, gap, &next_byte);
        if (why != M16C_STOP_BUDGET) {
            break;
        }
    }
    for (unsigned unit = 0; unit < 3; unit++) {
        flush_burst(&r, unit);
    }
    printf("%llu cycles, stopped: %s\n", (unsigned long long)cycles_now(&r),
           why == M16C_STOP_BUDGET ? "budget" : why == M16C_STOP_WAIT ? "wait" :
           why == M16C_STOP_UNDEF ? "undefined instruction" : "break");
    if (why == M16C_STOP_UNDEF) {
        printf("undefined at %05x\n", m16c_trap_pc(m16c63_cpu(r.chip)));
    }
    printf("pc %05x flg %04x sp %04x r0 %04x r1 %04x r2 %04x r3 %04x a0 %04x "
           "a1 %04x fb %04x sb %04x\n",
           m16c_get_reg(m16c63_cpu(r.chip), M16C_PC),
           m16c_get_reg(m16c63_cpu(r.chip), M16C_FLG),
           m16c_get_reg(m16c63_cpu(r.chip), M16C_SP),
           m16c_get_reg(m16c63_cpu(r.chip), M16C_R0),
           m16c_get_reg(m16c63_cpu(r.chip), M16C_R1),
           m16c_get_reg(m16c63_cpu(r.chip), M16C_R2),
           m16c_get_reg(m16c63_cpu(r.chip), M16C_R3),
           m16c_get_reg(m16c63_cpu(r.chip), M16C_A0),
           m16c_get_reg(m16c63_cpu(r.chip), M16C_A1),
           m16c_get_reg(m16c63_cpu(r.chip), M16C_FB),
           m16c_get_reg(m16c63_cpu(r.chip), M16C_SB));
    for (unsigned d = 0; d < r.ndump; d++) {
        for (uint32_t i = 0; i < r.dump_len[d]; i++) {
            if (i % 16 == 0) {
                printf("%s%05x:", i ? "\n" : "", r.dump_addr[d] + i);
            }
            printf(" %02x", m16c_read(m16c63_cpu(r.chip), r.dump_addr[d] + i, 1));
        }
        printf("\n");
    }
    if (r.show_sfr) {
        top_pcs(&r);
    }
    return 0;
}
