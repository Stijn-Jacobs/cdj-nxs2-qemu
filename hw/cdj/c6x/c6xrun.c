/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Offline runner for the C66x core: boots the DSP the way the board does,
 * with no QEMU.
 *
 * Loads the I2C first stage (stage1_dir/*.bin), starts at 0x00800200 in
 * supervisor mode with GIE off, and plays MAIN's side of the upload: a
 * READY/ACK GPIO handshake per 16 KB window and the 580,948-byte app boot
 * table (plus padding and the checksum window) delivered into the uPP
 * channel-A buffer the stage arms. Without -soc, everything else on the bus is
 * register-like memory with a log of the first access to each address.
 *
 * usage: c6xrun [-f main_unpacked.bin] [-s stage1_dir] [-c cycles]
 *               [-t lo:hi] (trace packets whose PC is in range)
 *               [-soc] link the SoC models and the MAIN command-link peer
 *               [-m4] run the request write/read checks after boot
 *               [-corpus file [-upto seq]] replay MAIN->DSP frames of run 0
 *               [-frames n] print the first n DSP frames raw
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <glib.h>
#include <unistd.h>

#include "c66x.h"
#include "c66x_decode.h"
#include "soc_c6655.h"
#include "peer_main.h"
#include "peer_script.h"

#define L2_BASE    0x00800000u
#define L2_ALIAS   0x10800000u
#define L2_SIZE    0x00100000u
#define DDR_BASE   0x80000000u
#define DDR_SIZE   0x10000000u

#define GPIO_BASE  0x02320000u
#define UPP_BASE   0x02580000u
#define PLL_GO     0x0231013Cu

#define APP_OFF    0x34E0u
#define APP_LEN    0x8DD54u
#define WINDOW     0x4000u

#define PIN_ACK    21
#define PIN_READY  24
#define PIN_BOOTED 25

#define APP_MAIN   0x80076EF8u
#define REQ_TABLE  0x008A7278u

typedef struct mmio_log { uint32_t addr; uint32_t pc; uint32_t val; char rw; } mmio_log;

typedef struct board {
    c66x_core *core;
    /* sparse register memory */
    uint32_t  *keys, *vals;
    unsigned   cap, used;
    /* first-touch log */
    mmio_log   log[4096];
    unsigned   nlog;
    /* GPIO */
    uint32_t   gpio_dir, gpio_out;
    int        main_ack;
    /* uPP channel A */
    uint32_t   upid0, upid1, upier;
    int        armed;
    /* MAIN's upload */
    uint8_t   *stream;
    size_t     stream_len, stream_pos;
    unsigned   windows;
    int        booted_seen;
    uint64_t   booted_cycle;
    /* the DSP's request-dispatch table */
    int        req_writes;
    /* -soc: the peripheral models own their windows */
    c6655_soc *soc;
    int        pending_ack;       /* -1 none, else the level MAIN drives on pin 21 */
    unsigned   pending_ships;
    /* MAIN's end of the SPI command link */
    peer_script *script;
} board;

static board B;
static uint8_t *l2_host, *ddr_host;

static uint32_t *reg_slot(board *b, uint32_t addr, int create)
{
    if (b->used * 2 >= b->cap) {
        unsigned ncap = b->cap ? b->cap * 2 : 4096;
        uint32_t *nk = calloc(ncap, sizeof *nk), *nv = calloc(ncap, sizeof *nv);
        for (unsigned i = 0; i < b->cap; i++)
            if (b->keys[i]) {
                unsigned h = (b->keys[i] * 2654435761u) & (ncap - 1);
                while (nk[h]) h = (h + 1) & (ncap - 1);
                nk[h] = b->keys[i];
                nv[h] = b->vals[i];
            }
        free(b->keys); free(b->vals);
        b->keys = nk; b->vals = nv; b->cap = ncap;
    }
    uint32_t key = addr | 1;       /* addresses are word-aligned in practice; 0 marks empty */
    unsigned h = (key * 2654435761u) & (b->cap - 1);
    while (b->keys[h] && b->keys[h] != key)
        h = (h + 1) & (b->cap - 1);
    if (!b->keys[h]) {
        if (!create)
            return NULL;
        b->keys[h] = key;
        b->vals[h] = 0;
        b->used++;
    }
    return &b->vals[h];
}

static void note(board *b, char rw, uint32_t addr, uint32_t val)
{
    /* polling loops hit the same register millions of times: hash the seen set */
    enum { NSEEN = 1 << 14 };
    static uint32_t seen[NSEEN];
    uint32_t key = (addr | 1) ^ (rw == 'w' ? 0x80000000u : 0);
    unsigned h = (key * 2654435761u) >> 18;
    while (seen[h] && seen[h] != key)
        h = (h + 1) & (NSEEN - 1);
    if (seen[h])
        return;
    seen[h] = key;
    if (b->nlog < 4096)
        b->log[b->nlog++] = (mmio_log){ addr, c66x_get_exec_pc(b->core), val, rw };
}

/* MAIN answers READY at once, and ships a window once the stage has armed uPP
 * and dropped READY: the order MAIN's 0x083265AE enforces. */
static void upp_fill(board *b)
{
    if (!b->armed || (b->gpio_out >> PIN_READY) & 1)
        return;
    uint32_t bytes = (b->upid1 & 0xffff) * ((b->upid1 >> 16) ? (b->upid1 >> 16) : 1);
    uint8_t *buf = calloc(1, bytes);
    size_t n = b->stream_len - b->stream_pos;
    if (n > bytes) n = bytes;
    memcpy(buf, b->stream + b->stream_pos, n);
    b->stream_pos += n;
    uint32_t dst = b->upid0;
    /* The uPP DMA writes straight into RAM, usually through the L2 alias. */
    if (dst - L2_ALIAS < L2_SIZE && dst - L2_ALIAS + bytes <= L2_SIZE)
        memcpy(l2_host + (dst - L2_ALIAS), buf, bytes);
    else if (dst - L2_BASE < L2_SIZE && dst - L2_BASE + bytes <= L2_SIZE)
        memcpy(l2_host + (dst - L2_BASE), buf, bytes);
    else if (dst - DDR_BASE < DDR_SIZE)
        memcpy(ddr_host + (dst - DDR_BASE), buf, bytes);
    else
        fprintf(stderr, "upp: window into unmapped 0x%08x\n", dst);
    free(buf);
    b->armed = 0;
    b->upier |= 8;
    b->windows++;
}

static uint32_t bus_read(void *opaque, uint32_t addr, unsigned size)
{
    board *b = opaque;
    uint32_t v;
    if (b->soc && addr != PLL_GO && c6655_soc_owns(addr)) {
        v = c6655_soc_read(b->soc, addr, size);
        static int upprd = -1;
        if (upprd < 0)
            upprd = getenv("C5_UPPRD") ? 1 : 0;
        if (upprd && (addr >> 12) == 0x02580 && b->booted_seen)
            printf("upp-r *0x%08x = 0x%08x (%u) pc 0x%08x\n", addr, v, size, c66x_get_exec_pc(b->core));
        note(b, 'r', addr, v);
        return v;
    }
    if (addr == GPIO_BASE + 0x10) v = b->gpio_dir;
    else if (addr == GPIO_BASE + 0x14) v = b->gpio_out;
    else if (addr == GPIO_BASE + 0x20) {
        b->main_ack = (b->gpio_out >> PIN_READY) & 1;
        v = b->gpio_out | ((uint32_t)b->main_ack << PIN_ACK);
    } else if (addr == UPP_BASE + 0x24 || addr == UPP_BASE + 0x20) {
        upp_fill(b);
        v = b->upier;
    } else if (addr == PLL_GO) {
        uint32_t *s = reg_slot(b, addr, 0);
        v = s ? *s & ~1u : 0;
    } else {
        uint32_t *s = reg_slot(b, addr, 0);
        v = s ? *s : 0;
    }
    note(b, 'r', addr, v);
    return size == 1 ? v & 0xff : size == 2 ? v & 0xffff : v;
}

static void bus_write(void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
    board *b = opaque;
    note(b, 'w', addr, val);
    if (b->soc && c6655_soc_owns(addr)) {
        static int irqdbg = -1;
        static unsigned nirq;
        if (irqdbg < 0)
            irqdbg = getenv("C5_IRQ_DEBUG") ? atoi(getenv("C5_IRQ_DEBUG")) : 0;
        if (nirq < (unsigned)irqdbg &&
            ((addr >> 12) == 0x02742 || (addr >> 12) == 0x02741 || (addr >> 16) == 0x0180 ||
             (addr >> 16) == 0x0260 || (addr >> 8) == 0x20BF00)) {
            nirq++;
            printf("irq-w *0x%08x = 0x%08x pc 0x%08x @%.6f\n", addr, val,
                   c66x_get_exec_pc(b->core), c6655_soc_now_ns(b->soc) / 1e9);
        }
        if ((addr >> 12) == 0x01800 && addr < 0x01800100 && b->booted_seen && getenv("C5_INTC_WATCH"))
            printf("intc-w *0x%08x = 0x%08x pc 0x%08x\n", addr, val, c66x_get_exec_pc(b->core));
        {
            /* C5_MMIOW=lo:hi[:max]  log register writes in a range, after boot */
            static uint32_t mw_lo = 1, mw_hi = 0, mw_max = 200, mw_n;
            static int mw_init;
            if (!mw_init) {
                mw_init = 1;
                if (getenv("C5_MMIOW"))
                    sscanf(getenv("C5_MMIOW"), "%x:%x:%u", &mw_lo, &mw_hi, &mw_max);
            }
            if (addr >= mw_lo && addr <= mw_hi && b->booted_seen && mw_n++ < mw_max)
                printf("mmio-w *0x%08x = 0x%08x (%u) pc 0x%08x t %.4f\n", addr, val, size,
                       c66x_get_exec_pc(b->core), c6655_soc_now_ns(b->soc) / 1e9);
        }
        static int uppdbg = -1;
        if (uppdbg < 0)
            uppdbg = getenv("C5_UPP_DEBUG") ? 1 : 0;
        if (uppdbg && addr >= 0x02580040 && addr <= 0x02580048 && b->booted_seen)
            printf("upp-w *0x%08x = 0x%08x pc 0x%08x @%.6f queued %zu\n", addr, val,
                   c66x_get_exec_pc(b->core), c6655_soc_now_ns(b->soc) / 1e9,
                   c6655_soc_upp_queued(b->soc));
        static unsigned ntw;
        if ((addr >> 16) == 0x0223 || (addr >> 16) == 0x0224 || (addr >> 16) == 0x0220)
            if (ntw++ < 24)
                printf("timer write *0x%08x = 0x%08x (%u) pc 0x%08x\n", addr, val, size,
                       c66x_get_exec_pc(b->core));
        c6655_soc_write(b->soc, addr, val, size);
        return;
    }
    if (addr == GPIO_BASE + 0x10) { b->gpio_dir = val; return; }
    if (addr == GPIO_BASE + 0x14) { b->gpio_out = val; return; }
    if (addr == GPIO_BASE + 0x18) { b->gpio_out |= val; return; }
    if (addr == GPIO_BASE + 0x1C) {
        b->gpio_out &= ~val;
        if ((val >> PIN_BOOTED) & 1 && !b->booted_seen) {
            b->booted_seen = 1;
            c66x_stats st;
            c66x_get_stats(b->core, &st);
            b->booted_cycle = st.cycles;
            printf("B2: GPIO25 cleared (booted) at pc 0x%08x after %u windows\n",
                   c66x_get_exec_pc(b->core), b->windows);
        }
        return;
    }
    if (addr == UPP_BASE + 0x40) { b->upid0 = val; return; }
    if (addr == UPP_BASE + 0x44) { b->upid1 = val; b->armed = 1; b->upier &= ~8u; return; }
    if (addr == UPP_BASE + 0x24) { b->upier &= ~val; return; }
    uint32_t *s = reg_slot(b, addr, 1);
    if (size == 4) *s = val;
    else if (size == 2) *s = (*s & ~0xffffu) | (val & 0xffff);
    else *s = (*s & ~0xffu) | (val & 0xff);
}

/* The SoC's DMA masters (uPP, EDMA3) see DSP memory through this. */
static uint32_t dsp_mem_read(void *opaque, uint32_t addr, unsigned size)
{
    uint8_t *p = NULL;
    if (addr - L2_BASE < L2_SIZE) p = l2_host + (addr - L2_BASE);
    else if (addr - DDR_BASE < DDR_SIZE) p = ddr_host + (addr - DDR_BASE);
    if (!p) return bus_read(opaque, addr, size);
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static void dsp_mem_write(void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
    uint8_t *p = NULL;
    if (addr - L2_BASE < L2_SIZE) p = l2_host + (addr - L2_BASE);
    else if (addr - DDR_BASE < DDR_SIZE) p = ddr_host + (addr - DDR_BASE);
    if (!p) { bus_write(opaque, addr, val, size); return; }
    for (unsigned i = 0; i < size; i++) p[i] = val >> (8 * i);
    c66x_invalidate(((board *)opaque)->core, addr, size);
}

/* MAIN acknowledges READY by mirroring it on its ACK line, and ships the next
 * window once READY drops. Applied between CPU quanta: the stage waits on both
 * in polling loops, and the callback runs inside a GPIO register write. */
static void soc_gpio_out(void *opaque, unsigned pin, int level)
{
    board *b = opaque;
    if (pin != PIN_READY)
        return;
    b->pending_ack = level;
    if (!level)
        b->pending_ships++;
}

static void soc_gpio_setclr(void *opaque, uint32_t mask, int set)
{
    board *b = opaque;
    if (!set && (mask >> PIN_BOOTED) & 1 && !b->booted_seen) {
        b->booted_seen = 1;
        printf("B2: GPIO25 cleared (booted) at pc 0x%08x after %u windows\n",
               c66x_get_exec_pc(b->core), b->windows);
    }
}

/* CS0 carries MAIN's link; the template's CSNR field reads 2 for it and 1 for
 * the second slave. */
static uint64_t spi_words_by_csnr[256];

static uint16_t soc_spi_xfer(void *opaque, uint16_t tx, unsigned bits, unsigned csnr, int cshold)
{
    board *b = opaque;
    static int dbg = -1;
    static unsigned nshown;
    if (dbg < 0)
        dbg = getenv("C5_SPI_DEBUG") ? atoi(getenv("C5_SPI_DEBUG")) : 0;
    if (nshown < (unsigned)dbg) {
        nshown++;
        printf("spi w%-4u @%.6f tx %04x bits %u csnr %u hold %d pc %08x\n", nshown,
               c6655_soc_now_ns(b->soc) / 1e9, tx, bits, csnr, cshold, c66x_get_exec_pc(b->core));
        if (tx == 0x5533 && csnr == 2) {
            printf("  frame buf 0x008A6CB8:");
            for (unsigned i = 0; i < 64; i++)
                printf("%s%04x", i % 16 ? " " : "\n    ", l2_host[0xA6CB8 + 2 * i] | l2_host[0xA6CB9 + 2 * i] << 8);
            printf("\n  spi words 0x008A6FD0:");
            for (unsigned i = 0; i < 66; i++)
                printf("%s%08x", i % 8 ? " " : "\n    ", (uint32_t)l2_host[0xA6FD0 + 4 * i] |
                       l2_host[0xA6FD1 + 4 * i] << 8 | l2_host[0xA6FD2 + 4 * i] << 16 |
                       (uint32_t)l2_host[0xA6FD3 + 4 * i] << 24);
            printf("\n");
        }
    }
    spi_words_by_csnr[csnr & 0xFF]++;
    if (csnr == 2 && b->script)
        return peer_script_spi(b->script, tx);
    return 0;
}

static uint64_t script_now(void *opaque)
{
    board *b = opaque;
    return c6655_soc_now_ns(b->soc);
}

static void script_ship(void *opaque, const uint8_t *buf, size_t len)
{
    board *b = opaque;
    if (getenv("C5_UPP_DEBUG"))
        printf("ship %zu B @%.6f (first word %02x%02x%02x%02x)\n", len,
               c6655_soc_now_ns(b->soc) / 1e9, buf[0], buf[1], buf[2], buf[3]);
    c6655_soc_upp_rx(b->soc, buf, len);
}

static void soc_log(void *opaque, const char *msg)
{
    static unsigned n;
    if (n++ < 40)
        printf("soc: %s\n", msg);
}

static void main_side(board *b)
{
    if (b->pending_ack >= 0) {
        c6655_soc_gpio_set_input(b->soc, PIN_ACK, b->pending_ack);
        b->pending_ack = -1;
    }
    while (b->pending_ships) {
        b->pending_ships--;
        if (b->stream_pos >= b->stream_len)
            break;
        c6655_soc_upp_rx(b->soc, b->stream + b->stream_pos, WINDOW);
        b->stream_pos += WINDOW;
        b->windows++;
    }
}

/* first-execution times of the boot milestones, and a coarse PC profile */
static const uint32_t marks[] = { 0x00801450, 0x008015E0, 0x008017F4, 0x00801834,
                                  0x8007E140, APP_MAIN, 0x00803DA0, 0x00804588, 0x00803F68 };
static uint64_t mark_cycle[sizeof marks / sizeof marks[0]];
static int mark_hit[sizeof marks / sizeof marks[0]];

static void mark_tracer(c66x_core *c, void *opaque, uint32_t pc)
{
    for (unsigned i = 0; i < sizeof marks / sizeof marks[0]; i++)
        if (pc == marks[i] && !mark_hit[i]) {
            c66x_stats st;
            c66x_get_stats(c, &st);
            mark_hit[i] = 1;
            mark_cycle[i] = st.packets;
        }
}

static void req_watch(c66x_core *c, void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
    board *b = opaque;
    /* C5_WATCH skips stage 1's memory test; C5_WATCHN raises the print cap */
    if (getenv("C5_WATCH") && !b->booted_seen)
        return;
    if (b->req_writes++ < (getenv("C5_WATCH") ? (getenv("C5_WATCHN") ? atoi(getenv("C5_WATCHN")) : 40) : 8))
        printf("req-table write: *0x%08x = 0x%08x (%u B) from pc 0x%08x t %.4f\n",
               addr, val, size, c66x_get_exec_pc(c), b->soc ? c6655_soc_now_ns(b->soc) / 1e9 : 0.0);
}

/* C5_NANWATCH=lo:hi  report stores in [lo, hi] of a 4-byte value whose single-precision
 * exponent is all ones (NaN or Inf), with the storing packet; the first few only,
 * plus a count of every store so an empty buffer is distinguishable. */
/* packets executed so far, counted by probes(): a per-packet clock for store/trace logs */
static uint64_t pkt_clock;

static void nan_watch(c66x_core *c, void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
    board *b = opaque;
    static unsigned shown, stores, bad;
    stores++;
    /* C5_STORELOG: log every store in the range with the core's cycle count when it lands */
    if (getenv("C5_STORELOG")) {
        if (!getenv("C5_TFTIME") || (b->soc && c6655_soc_now_ns(b->soc) >= (uint64_t)(atof(getenv("C5_TFTIME")) * 1e6)))
            printf("st c %llu pc %08x addr %08x size %u val %08x\n", (unsigned long long)pkt_clock,
                   c66x_get_exec_pc(c), addr, size, val);
        return;
    }
    /* C5_VALS=v,v,...: instead report stores whose value is one of these (any size) */
    if (getenv("C5_VALS")) {
        static uint32_t vals[16];
        static int nv = -1;
        if (nv < 0) {
            nv = 0;
            char *e = getenv("C5_VALS");
            while (*e && nv < 16) {
                vals[nv++] = strtoul(e, &e, 0);
                if (*e != ',') break;
                e++;
            }
        }
        for (int i = 0; i < nv; i++)
            if (val == vals[i] && shown++ < (getenv("C5_NANN") ? (unsigned)atoi(getenv("C5_NANN")) : 10)) {
                printf("valwatch t %.6f: *0x%08x = %u (%u B) from pc 0x%08x (store %u)\n",
                       b->soc ? c6655_soc_now_ns(b->soc) / 1e9 : 0.0, addr, val, size, c66x_get_exec_pc(c), stores);
                break;
            }
        return;
    }
    /* C5_NANBIG: instead report finite floats with |x| >= 2^65 (exponent >= 0xC0) --
     * where decoder garbage starts, before it overflows to Inf/NaN */
    if (getenv("C5_NANBIG")) {
        unsigned ex = (val >> 23) & 0xFF;
        if (size != 4 || ex < 0xC0 || ex == 0xFF || val >= 0xFFF00000u)
            return;
        bad++;
        if (shown++ < (getenv("C5_NANN") ? (unsigned)atoi(getenv("C5_NANN")) : 10))
            printf("bigwatch t %.6f: *0x%08x = %08x (%g) from pc 0x%08x (store %u)\n",
                   b->soc ? c6655_soc_now_ns(b->soc) / 1e9 : 0.0, addr, val,
                   (double)((union { uint32_t u; float f; }){ .u = val }).f, c66x_get_exec_pc(c), stores);
        return;
    }
    if (size != 4 || (val & 0x7F800000) != 0x7F800000 || val >= 0xFFF00000u)
        return;
    /* C5_NANEXACT: only the default quiet NaN and the infinities, so integers that
     * happen to have the exponent bits set are not reported */
    if (getenv("C5_NANEXACT") && (val & 0x7FFFFFFF) != 0x7FC00000 && (val & 0x7FFFFFFF) != 0x7F800000)
        return;
    bad++;
    if (shown++ < (getenv("C5_NANN") ? (unsigned)atoi(getenv("C5_NANN")) : 10))
        printf("nanwatch t %.6f: *0x%08x = %08x from pc 0x%08x (store %u, bad %u)\n",
               b->soc ? c6655_soc_now_ns(b->soc) / 1e9 : 0.0, addr, val, c66x_get_exec_pc(c),
               stores, bad);
}

/* The last execute packets before a fault, with the registers a bad return
 * usually comes through. */
enum { RING = 256 };
static struct { uint32_t pc, b3, b15, a3, a4; } ring[RING];
static unsigned ring_pos;
static int ring_on;

static void ring_tracer(c66x_core *c, void *opaque, uint32_t pc)
{
    mark_tracer(c, opaque, pc);
    ring[ring_pos % RING].pc = pc;
    ring[ring_pos % RING].b3 = c66x_get_reg(c, C66X_B(3));
    ring[ring_pos % RING].b15 = c66x_get_reg(c, C66X_B(15));
    ring[ring_pos % RING].a3 = c66x_get_reg(c, C66X_A(3));
    ring[ring_pos % RING].a4 = c66x_get_reg(c, C66X_A(4));
    ring_pos++;
}

static void ring_dump(void)
{
    printf("last %d packets:\n", RING);
    for (unsigned i = 0; i < RING; i++) {
        unsigned k = (ring_pos + i) % RING;
        printf("  %08x b3 %08x b15 %08x a3 %08x a4 %08x\n", ring[k].pc, ring[k].b3,
               ring[k].b15, ring[k].a3, ring[k].a4);
    }
}

static uint32_t dsp_mem_read(void *opaque, uint32_t addr, unsigned size);

/* C5_HEAPWALK=base:end  walk the RTS heap's packet headers (size|used, next) from
 * base and print each one until the chain leaves [base, end] -- names the block
 * whose header was overwritten. */
static void heap_walk(void)
{
    uint32_t base = 0, end = 0;
    if (!getenv("C5_HEAPWALK") || sscanf(getenv("C5_HEAPWALK"), "%x:%x", &base, &end) != 2)
        return;
    uint32_t p = base;
    for (unsigned i = 0; i < 4096 && p >= base && p < end; i++) {
        uint32_t sz = dsp_mem_read(NULL, p, 4), nx = dsp_mem_read(NULL, p + 4, 4);
        printf("heap %4u 0x%08x size 0x%08x %s next 0x%08x\n", i, p, sz & ~1u,
               sz & 1 ? "used" : "free", nx);
        p += (sz & ~1u) + 8;
    }
    printf("heap walk stopped at 0x%08x\n", p);
}

/* A sleeping GThread instead of SIGALRM, which Windows does not have. */
static gpointer watchdog_fire(gpointer secs)
{
    g_usleep((gulong)GPOINTER_TO_UINT(secs) * G_USEC_PER_SEC);
    printf("\nwatchdog: no exit in time; last packets follow\n");
    heap_walk();
    ring_dump();
    fflush(stdout);
    _exit(3);
    return NULL;
}

static struct { uint32_t addr; unsigned n; } dumps[16];
static unsigned ndumps;

/* C5_PEEK=addr,addr,...: log a DSP memory word whenever it changes, with virtual time. */
static uint32_t peek_addr[16], peek_val[16];
static unsigned npeek = ~0u;

static uint32_t dsp_mem_read(void *opaque, uint32_t addr, unsigned size);
static uint8_t *slurp(const char *path, size_t *len);

static void peek_sample(c6655_soc *soc)
{
    if (npeek == ~0u) {
        npeek = 0;
        const char *e = getenv("C5_PEEK");
        while (e && *e && npeek < 16) {
            char *end;
            peek_addr[npeek] = strtoul(e, &end, 16);
            peek_val[npeek] = 0xDEADBEEF;
            npeek++;
            e = *end == ',' ? end + 1 : end;
            if (end == e && *e != ',')
                break;
        }
    }
    if (!B.booted_seen)            /* the upload overwrites L2 wholesale before this */
        return;
    for (unsigned i = 0; i < npeek; i++) {
        uint32_t v = dsp_mem_read(&B, peek_addr[i], 4);
        if (v != peek_val[i]) {
            printf("peek %.4f s *0x%08x = 0x%08x\n", soc ? c6655_soc_now_ns(soc) / 1e9 : 0.0,
                   peek_addr[i], v);
            peek_val[i] = v;
        }
    }
}

/* -w idx=val,...  a write list;  -poll n  n reads of 0x0AF0 + status;  -waitms n */
static struct { char kind; const char *arg; const char *flag; } cli_steps[512];
static unsigned nsteps_cli;

static const char *ship_file = "/tmp/c5_zedd.mp3";

/* "a,b,c" -> up to max numbers */
static unsigned parse_list(const char *a, uint32_t *out, unsigned max)
{
    unsigned n = 0;
    char *p = (char *)a;
    while (*p && n < max) {
        out[n++] = strtoul(p, &p, 0);
        if (*p != ',')
            break;
        p++;
    }
    return n;
}

static void add_cli_steps(peer_script *s)
{
    static uint8_t *file;
    static size_t file_len;
    static const uint32_t poll[] = { 0x0AF0, 0x0AF4, 0x0B18, 0x0B1C, 0x0B74, 0x0B14, 0x0150, 0x0158 };
    for (unsigned k = 0; k < nsteps_cli; k++) {
        const char *a = cli_steps[k].arg;
        if (!strcmp(cli_steps[k].flag, "-w")) {
            uint32_t args[PEER_MAX_ARGS];
            unsigned n = 0;
            char *p = (char *)a;
            while (*p && n + 2 <= PEER_MAX_ARGS) {
                args[n++] = strtoul(p, &p, 0);
                if (*p != '=')
                    break;
                args[n++] = strtoul(p + 1, &p, 0);
                if (*p == ',')
                    p++;
            }
            char label[64];
            snprintf(label, sizeof label, "write %s", a);
            peer_script_mark(s, label);
            peer_script_request(s, 2, args, n / 2 * 2);
        } else if (!strcmp(cli_steps[k].flag, "-arm")) {
            uint32_t v[3];
            if (parse_list(a, v, 3) == 3)
                peer_script_request(s, 6, v, 3);
        } else if (!strcmp(cli_steps[k].flag, "-r")) {
            uint32_t v[PEER_MAX_ARGS];
            unsigned n = parse_list(a, v, PEER_MAX_ARGS);
            peer_script_request(s, 4, v, n);
        } else if (!strcmp(cli_steps[k].flag, "-ship")) {
            /* file offset, length, ship unit: MAIN's DMA ships whole units */
            uint32_t v[3] = { 0, 0, 0x4000 };
            parse_list(a, v, 3);
            if (!file)
                file = slurp(ship_file, &file_len);
            if (!file || v[0] + v[1] > file_len)
                continue;
            for (uint32_t off = 0; off < v[1]; off += v[2])
                peer_script_ship(s, file + v[0] + off, v[1] - off < v[2] ? v[1] - off : v[2]);
        } else if (cli_steps[k].kind == 'p') {
            unsigned n = strtoul(a, NULL, 0);
            for (unsigned i = 0; i < n; i++) {
                peer_script_wait_ns(s, 13000000);
                peer_script_request(s, 4, poll, 8);
            }
        } else {
            peer_script_wait_ns(s, strtoull(a, NULL, 0) * 1000000ull);
        }
    }
}

static uint32_t tr_lo = 1, tr_hi = 0;
static int tr_cover;          /* -cover: count distinct PCs in range instead of printing */
enum { NCOV = 1 << 16 };
static uint32_t cov_pc[NCOV], cov_n[NCOV];

/* C5_SNAP=pc:addr:n[,...]  dump n bytes at addr on the first execution of pc (after boot).
 * C5_HITS=pc[,...]  count executions of pc and print the first C5_HITN with registers. */
static struct { uint32_t pc, addr, n; int done; } snaps[8];
static unsigned nsnaps = ~0u;
static struct { uint32_t pc; uint64_t n; } hits[16];
static unsigned nhits = ~0u, hit_print = 12;

static void probes_init(void)
{
    nsnaps = nhits = 0;
    const char *e = getenv("C5_SNAP");
    while (e && *e && nsnaps < 8) {
        char *p;
        snaps[nsnaps].pc = strtoul(e, &p, 16);
        if (*p != ':') break;
        snaps[nsnaps].addr = strtoul(p + 1, &p, 16);
        if (*p != ':') break;
        snaps[nsnaps].n = strtoul(p + 1, &p, 0);
        nsnaps++;
        e = *p == ',' ? p + 1 : p;
    }
    e = getenv("C5_HITS");
    while (e && *e && nhits < 16) {
        char *p;
        hits[nhits++].pc = strtoul(e, &p, 16);
        e = *p == ',' ? p + 1 : p;
        if (p == e && !*p) break;
    }
    if (getenv("C5_HITN"))
        hit_print = strtoul(getenv("C5_HITN"), NULL, 0);
}

static void probes(c66x_core *c, uint32_t pc)
{
    pkt_clock++;
    if (nsnaps == ~0u)
        probes_init();
    if (!B.booted_seen)
        return;
    /* C5_FIXUP=pc:dst=src[;pc:dst=src]  copy register src into dst (0-31 A, 32-63 B)
     * just before the packet at pc runs: an experiment knob to test what a
     * suspected mis-executed instruction should have read. */
    static struct { uint32_t pc; unsigned dst, src; } fx[8];
    static int nfx = -1;
    if (nfx < 0) {
        nfx = 0;
        const char *e = getenv("C5_FIXUP");
        while (e && *e && nfx < 8) {
            char *p;
            fx[nfx].pc = strtoul(e, &p, 16);
            if (*p != ':') break;
            fx[nfx].dst = strtoul(p + 1, &p, 0);
            if (*p != '=') break;
            fx[nfx].src = strtoul(p + 1, &p, 0);
            nfx++;
            e = *p == ';' ? p + 1 : p;
        }
    }
    for (int i = 0; i < nfx; i++)
        if (fx[i].pc == pc)
            c66x_set_reg(c, fx[i].dst, c66x_get_reg(c, fx[i].src));
    /* C5_NANHUNT=t_ms[:count]  after virtual time t, report packets at which a register
     * newly holds a quiet-NaN single (0x7FC00000 / 0xFFC00000) or a NaN double high word,
     * with the preceding packet PCs -- the producer is among them (results land up to a
     * few cycles late). */
    {
        static int nh_init;
        static uint64_t nh_ns;
        static unsigned nh_left;
        static uint32_t prev[64], hist[12];
        static unsigned hpos;
        if (!nh_init) {
            nh_init = 1;
            const char *e = getenv("C5_NANHUNT");
            if (e) {
                double ms = 0;
                nh_left = 20;
                sscanf(e, "%lf:%u", &ms, &nh_left);
                nh_ns = (uint64_t)(ms * 1e6);
            }
        }
        if (nh_left && B.soc && c6655_soc_now_ns(B.soc) >= nh_ns) {
            for (unsigned r = 0; r < 64 && nh_left; r++) {
                uint32_t v = c66x_get_reg(c, r);
                int nan = (v & 0x7F800000) == 0x7F800000 && v < 0xFFF00000u;
                int was = (prev[r] & 0x7F800000) == 0x7F800000 && prev[r] < 0xFFF00000u;
                if (nan && !was) {
                    nh_left--;
                    printf("nan %.6f s: %c%u = %08x (was %08x) before packet %08x; previous:",
                           c6655_soc_now_ns(B.soc) / 1e9, r < 32 ? 'a' : 'b', r % 32, v, prev[r], pc);
                    for (unsigned k = 0; k < 12; k++)
                        printf(" %08x", hist[(hpos + k) % 12]);
                    printf("\n");
                    fflush(stdout);
                }
                prev[r] = v;
            }
            hist[hpos++ % 12] = pc;
        }
    }
    /* C5_TRACEFULL=lo:hi:count  every packet in [lo, hi] with A0-A15 and B0-B7 */
    static uint32_t tf_lo, tf_hi, tf_left = ~0u;
    if (tf_left == ~0u) {
        tf_left = 0;
        const char *e = getenv("C5_TRACEFULL");
        if (e)
            sscanf(e, "%x:%x:%u", &tf_lo, &tf_hi, &tf_left);
    }
    /* C5_TFTIME=ms: the full trace starts only after that virtual time */
    static uint64_t tf_ns = ~0ull;
    if (tf_ns == ~0ull)
        tf_ns = getenv("C5_TFTIME") ? (uint64_t)(atof(getenv("C5_TFTIME")) * 1e6) : 0;
    if (tf_left && pc >= tf_lo && pc <= tf_hi && (!tf_ns || (B.soc && c6655_soc_now_ns(B.soc) >= tf_ns))) {
        tf_left--;
        printf("tf %08x c %llu a:", pc, (unsigned long long)pkt_clock);
        for (int r = 0; r < (getenv("C5_TF32") ? 32 : 16); r++) printf(" %x", c66x_get_reg(c, C66X_A(r)));
        printf(" b:");
        for (int r = 0; r < (getenv("C5_TF32") ? 32 : 8); r++) printf(" %x", c66x_get_reg(c, C66X_B(r)));
        printf("\n");
    }
    for (unsigned i = 0; i < nsnaps; i++)
        if (!snaps[i].done && snaps[i].pc == pc) {
            snaps[i].done = 1;
            printf("snap @pc %08x t %.4f: *0x%08x:", pc, B.soc ? c6655_soc_now_ns(B.soc) / 1e9 : 0.0,
                   snaps[i].addr);
            for (unsigned k = 0; k < snaps[i].n; k++)
                printf("%s%02x", k % 32 ? " " : "\n  ", dsp_mem_read(&B, snaps[i].addr + k, 1) & 0xFF);
            printf("\n");
        }
    for (unsigned i = 0; i < nhits; i++)
        if (hits[i].pc == pc && hits[i].n++ < hit_print) {
            printf("hit %08x #%llu a0-a8", pc, (unsigned long long)hits[i].n);
            for (int r = 0; r <= 8; r++) printf(" %08x", c66x_get_reg(c, C66X_A(r)));
            printf(" b0-b8");
            for (int r = 0; r <= 8; r++) printf(" %08x", c66x_get_reg(c, C66X_B(r)));
            printf("\n");
        }
}

static void tracer(c66x_core *c, void *opaque, uint32_t pc)
{
    probes(c, pc);
    if (pc == 0x80073D84u) {
        uint32_t obj = c66x_get_reg(c, C66X_A(4));
        printf("0x80073D84: obj 0x%08x obj[4] 0x%08x dst 0x%08x\n", obj,
               dsp_mem_read(&B, obj + 4, 4), c66x_get_reg(c, C66X_B(4)));
    }
    if (tr_cover) {
        if (pc < tr_lo || pc > tr_hi)
            return;
        unsigned h = (pc * 2654435761u) >> 16;
        while (cov_n[h] && cov_pc[h] != pc) h = (h + 1) & (NCOV - 1);
        cov_pc[h] = pc;
        cov_n[h]++;
        return;
    }
    if (pc >= tr_lo && pc <= tr_hi)
        printf("  pc %08x  a4=%08x b4=%08x a6=%08x b3=%08x b15=%08x b31=%08x a3=%08x\n", pc,
               c66x_get_reg(c, C66X_A(4)), c66x_get_reg(c, C66X_B(4)),
               c66x_get_reg(c, C66X_A(6)), c66x_get_reg(c, C66X_B(3)),
               c66x_get_reg(c, C66X_B(15)), c66x_get_reg(c, C66X_B(31)),
               c66x_get_reg(c, C66X_A(3)));
}

static uint8_t *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    if (fread(p, 1, *len, f) != *len) { free(p); p = NULL; }
    fclose(f);
    return p;
}

/* A write list followed by a read list of the same indices. The
 * indices are MAIN's own bring-up writes (from the corpus) plus one unused slot
 * in the track descriptor carrying a pattern no bring-up value could fake. */
static const uint32_t m4_idx[] = { 0x0134, 0x0138, 0x013C, 0x0AD0 };
static const uint32_t m4_val[] = { 0x00000001, 0x00000002, 0x00000002, 0x5A5AC3C3 };
static const uint32_t m4_status_idx[] = { 0x0150, 0x0154, 0x0158, 0x015C, 0x0160, 0x0164,
                                          0x0B14, 0x0B1C, 0x0B30, 0x0B34, 0x0B38, 0x0B3C,
                                          0x0B40, 0x0B44, 0x0B48, 0x0B4C, 0x0B50, 0x0B54,
                                          0x0B58 };

static void add_m4_checks(peer_script *s)
{
    uint32_t args[PEER_MAX_ARGS];
    unsigned n = 0;
    peer_script_mark(s, "M4 (a) version");
    peer_script_request(s, 1, NULL, 0);
    peer_script_mark(s, "M4 (b) write list, read back");
    for (unsigned i = 0; i < 4; i++) {
        args[n++] = m4_idx[i];
        args[n++] = m4_val[i];
    }
    peer_script_request(s, 2, args, n);
    peer_script_request(s, 4, m4_idx, 4);
    peer_script_mark(s, "M4 (d) status block");
    peer_script_request(s, 4, m4_status_idx, 10);
    peer_script_request(s, 4, m4_status_idx + 10, 9);
}

static void report_m4(const peer_script *s)
{
    unsigned n;
    const uint32_t *w = peer_script_last_reply(s, 1, 0, &n);
    printf("\nM4 (a) version: %s", w ? "" : "no answer\n");
    if (w)
        printf("0x%08x -> %s\n", n ? w[0] : 0, n == 1 && w[0] == 0x0006000F ? "PASS" : "FAIL");
    w = peer_script_last_reply(s, 4, m4_idx[0], &n);
    int pass = w && n == 4;
    for (unsigned i = 0; pass && i < 4; i++)
        pass = w[i] == m4_val[i];
    printf("M4 (b) write/read back: %s", w ? "" : "no answer ");
    for (unsigned i = 0; w && i < n; i++)
        printf("[0x%04x]=0x%08x ", m4_idx[i], w[i]);
    printf("-> %s\n", pass ? "PASS" : "FAIL");
    printf("M4 (d) status block:\n");
    for (unsigned part = 0; part < 2; part++) {
        unsigned off = part ? 10 : 0, cnt = part ? 9 : 10;
        w = peer_script_last_reply(s, 4, m4_status_idx[off], &n);
        for (unsigned i = 0; i < cnt; i++)
            printf("  [0x%04x] = %s%08x\n", m4_status_idx[off + i], w && i < n ? "0x" : "--",
                   w && i < n ? w[i] : 0);
    }
}

/*
 * The audio data plane as MAIN drives it for one loaded track: the bring-up
 * prefix of the corpus, then per block a lane-A command, the status poll, two
 * uPP arms, the lane-B descriptor, the ship, and the doorbell close. Each ship
 * is 0xFC00 file bytes on a 0x400 boundary, bases stepping 0x9800, then a
 * 0x40-byte record whose first words
 * are {frame index, 1, 0, 0x28}. AFC is the base relative to the audio data
 * start the track descriptor implies (file size - 0x0AC8).
 */
enum { SHIP_DATA = 0xFC00, SHIP_TAIL = 0x40, SHIP_STEP = 0x9800 };

static const uint32_t status_poll[] = { 0x0AF0, 0x0150, 0x0154, 0x0158, 0x0B30, 0x0B34, 0x0B38,
                                        0x0B3C, 0x0B40, 0x0B44, 0x0B48, 0x0B50, 0x0B54, 0x0B58,
                                        0x0B5C, 0x0B60, 0x0B64, 0x0B68 };

static int mp3_frame_len(const uint8_t *d, size_t len, size_t o)
{
    static const int br[] = { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 };
    static const int sr[] = { 44100, 48000, 32000 };
    if (o + 4 > len || d[o] != 0xFF || (d[o + 1] & 0xE0) != 0xE0)
        return 0;
    unsigned b = d[o + 2] >> 4, s = (d[o + 2] >> 2) & 3;
    if (b == 0 || b == 15 || s == 3)
        return 0;
    return 144000 * br[b] / sr[s] + ((d[o + 2] >> 1) & 1);
}

/* -cmdat <block>:<idx>=<val>,...  a write list injected after that block, for
 * trying lane-A commands MAIN never sent in the captured corpus. */
static struct { unsigned block; uint32_t args[PEER_MAX_ARGS]; unsigned n; } cmdat[8];
static unsigned ncmdat;

static void parse_cmdat(const char *spec)
{
    if (ncmdat >= 8)
        return;
    char *p;
    cmdat[ncmdat].block = strtoul(spec, &p, 0);
    while (*p == ':' || *p == ',') {
        uint32_t idx = strtoul(p + 1, &p, 0);
        if (*p != '=' || cmdat[ncmdat].n + 2 > PEER_MAX_ARGS)
            break;
        cmdat[ncmdat].args[cmdat[ncmdat].n++] = idx;
        cmdat[ncmdat].args[cmdat[ncmdat].n++] = strtoul(p + 1, &p, 0);
    }
    ncmdat++;
}

static void add_cmdat(peer_script *s, unsigned block)
{
    for (unsigned i = 0; i < ncmdat; i++)
        if (cmdat[i].block == block) {
            char label[48];
            snprintf(label, sizeof label, "injected command after block %u", block);
            peer_script_mark(s, label);
            peer_script_request(s, 2, cmdat[i].args, cmdat[i].n / 2 * 2);
            peer_script_wait_ns(s, 20000000);
        }
}

/* Lane id MAIN puts in 0x0B18 with every lane-A command; the corpus has 0x150,
 * which every lane-A handler rejects against the DSP's lane byte b14+1656. */
static uint32_t lane_b18 = 0x150;

/* Lane-B descriptors MAIN sent in corpus run 0, in order: (AFC, B08, B0C). */
static struct { uint32_t afc, b08, b0c; } laneb[256];
static unsigned nlaneb;

static void load_laneb(const char *corpus)
{
    FILE *f = fopen(corpus, "r");
    char line[4096];
    while (f && fgets(line, sizeof line, f) && nlaneb < 256) {
        if (!strncmp(line, "# type=1 ", 9) && nlaneb)
            break;                       /* the next run starts */
        char *p = strstr(line, "[0x0afc]=");
        if (!p || line[0] != '#')
            continue;
        uint32_t afc = strtoul(p + 9, NULL, 0);
        char *q8 = strstr(line, "[0x0b08]="), *qc = strstr(line, "[0x0b0c]=");
        if (!q8 || !qc)
            continue;
        laneb[nlaneb].afc = afc;
        laneb[nlaneb].b08 = strtoul(q8 + 9, NULL, 0);
        laneb[nlaneb].b0c = strtoul(qc + 9, NULL, 0);
        nlaneb++;
    }
    if (f)
        fclose(f);
}

static void add_audio(peer_script *s, const uint8_t *mp3, size_t len, uint32_t data_len,
                      uint32_t afc0, unsigned blocks)
{
    uint32_t data_start = (uint32_t)len - data_len;
    /* frame offsets, from the first header at or after the data start */
    size_t cap = len / 100 + 16, n = 0;
    uint32_t *frames = malloc(cap * sizeof *frames);
    for (size_t o = data_start; o < len && n < cap;) {
        int fl = mp3_frame_len(mp3, len, o);
        if (!fl) { o++; continue; }
        frames[n++] = (uint32_t)o;
        o += fl;
    }
    printf("audio: %zu B, data from 0x%x, %zu frames\n", len, data_start, n);
    size_t fi = 0;
    for (unsigned k = 0; k < blocks; k++) {
        uint32_t afc = afc0 + k * SHIP_STEP, base = data_start + afc;
        if (base + SHIP_DATA > len)
            break;
        while (fi < n && frames[fi] < base) fi++;
        uint32_t b08 = fi < n ? frames[fi] - base : 0, b0c = (uint32_t)fi;
        if (k < nlaneb && laneb[k].afc == afc) {
            b08 = laneb[k].b08;           /* MAIN's own values from the corpus */
            b0c = laneb[k].b0c;
        }
        char label[48];
        snprintf(label, sizeof label, "block %u AFC 0x%x", k, afc);
        peer_script_mark(s, label);
        uint32_t a[16];
        a[0] = 0x0AF4; a[1] = 6; a[2] = 0x0B18; a[3] = lane_b18; a[4] = 0x0AF0; a[5] = 0x01010100;
        peer_script_request(s, 2, a, 6);
        peer_script_request(s, 4, status_poll, 18);
        uint8_t *tail = calloc(1, SHIP_TAIL);
        uint32_t rec[] = { b0c, 1, 0, 0x28 };
        for (unsigned i = 0; i < 4; i++)
            for (unsigned j = 0; j < 4; j++)
                tail[4 * i + j] = rec[i] >> (8 * j);
        /* The DSP answers a second arm with 1 (busy) and ignores it while the
         * first window is open, so each arm is followed by its own ship. */
        a[0] = 0x0B90; a[1] = SHIP_DATA; a[2] = SHIP_DATA;
        peer_script_request(s, 6, a, 3);
        /* MAIN ships whole 16 KB DMA units; the uPP model drops the overshoot */
        for (unsigned u = 0; u < 4 && base + (u + 1) * 0x4000u <= len; u++)
            peer_script_ship(s, mp3 + base + u * 0x4000u, 0x4000);
        peer_script_wait_ns(s, 5000000);
        a[0] = 0x10790; a[1] = SHIP_TAIL; a[2] = 0x18;
        peer_script_request(s, 6, a, 3);
        peer_script_ship(s, tail, SHIP_TAIL);
        peer_script_wait_ns(s, 5000000);
        uint32_t lb[] = { 0x0AFC, afc, 0x0B04, 1, 0x0B08, b08, 0x0B0C, b0c, 0x0B10, 0,
                          0x0B78, 0, 0x0B7C, 0, 0x0B74, 1 };
        peer_script_request(s, 2, lb, 16);
        a[0] = 0x0B14;
        peer_script_request(s, 4, a, 1);
        a[0] = 0x0B1C;
        peer_script_request(s, 4, a, 1);
        a[0] = 0x0B74; a[1] = 0x0B84;
        peer_script_request(s, 4, a, 2);
        a[0] = 0x0B74; a[1] = 0;
        peer_script_request(s, 2, a, 2);
        add_cmdat(s, k);
    }
    /* keep polling status so a running decoder shows up in the report */
    for (unsigned i = 0; i < 40; i++) {
        peer_script_wait_ns(s, 25000000);
        peer_script_request(s, 4, status_poll, 18);
    }
    free(frames);
}

/* McBSP0 PCM to a WAV file: 2 channels at 44.1 kHz, the word width as sent. */
static FILE *wav;
static uint64_t wav_words;
static unsigned wav_bits;

static void soc_mcbsp_tx(void *opaque, unsigned port, uint32_t word, unsigned bits)
{
    if (!wav || port != 0)
        return;
    wav_bits = bits;
    fwrite(&word, 4, 1, wav);
    wav_words++;
}

static void wav_close(void)
{
    if (!wav)
        return;
    /* 32-bit container regardless of the McBSP word width; the header is
     * written last because the length is only known now. */
    uint32_t data = (uint32_t)(wav_words * 4), riff = data + 36;
    uint8_t h[44] = "RIFF....WAVEfmt ";
    memcpy(h + 4, &riff, 4);
    uint32_t fmt_len = 16, rate = 44100, byte_rate = rate * 2 * 4;
    uint16_t pcm = 1, ch = 2, align = 8, bps = 32;
    memcpy(h + 16, &fmt_len, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &ch, 2);
    memcpy(h + 24, &rate, 4); memcpy(h + 28, &byte_rate, 4); memcpy(h + 32, &align, 2);
    memcpy(h + 34, &bps, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &data, 4);
    fseek(wav, 0, SEEK_SET);
    fwrite(h, 1, 44, wav);
    fclose(wav);
    printf("wav: %" PRIu64 " McBSP0 words (%u bits each)\n", wav_words, wav_bits);
}

/* MAIN's byte stream: 35 full windows, the remainder padded to a window, then
 * a window whose first word is the inverted end-around-carry sum16. */
static void build_stream(board *b, const uint8_t *fw)
{
    size_t padded = (APP_LEN + WINDOW - 1) / WINDOW * WINDOW;
    b->stream_len = padded + WINDOW;
    b->stream = calloc(1, b->stream_len);
    memcpy(b->stream, fw + APP_OFF, APP_LEN);
    uint32_t sum = 0;
    for (size_t i = 0; i < APP_LEN; i += 2) {
        sum += b->stream[i] | (b->stream[i + 1] << 8);
        sum = (sum & 0xffff) + (sum >> 16);
    }
    uint16_t ck = ~sum & 0xffff;
    b->stream[padded] = ck & 0xff;
    b->stream[padded + 1] = ck >> 8;
    printf("upload: %zu B table, %zu windows, checksum 0x%04x\n",
           (size_t)APP_LEN, b->stream_len / WINDOW, ck);
}

int main(int argc, char **argv)
{
    const char *fw_path = "../../extract/main_unpacked.bin";
    const char *s1_dir = "../c3_out/stage1";
    uint64_t max_cycles = 400000000ULL;
    int use_soc = 0, m4 = 0;
    uint64_t quantum = 10000;       /* cycles per step when no SoC event is nearer */
    const char *corpus = NULL, *audio = NULL, *wav_path = NULL, *mainlog = NULL, *loadrepro = NULL;
    unsigned blocks = 4;
    uint32_t data_len = 0x23BD63, afc0 = 0x11ED4;   /* the corpus track's descriptor */
    unsigned upto = ~0u, frames = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-soc")) use_soc = 1;
        if (!strcmp(argv[i], "-m4")) m4 = use_soc = 1;
        if (!strcmp(argv[i], "-corpus") && i + 1 < argc) { corpus = argv[++i]; use_soc = 1; }
        if (!strcmp(argv[i], "-upto") && i + 1 < argc) upto = strtoul(argv[++i], NULL, 0);
        if (!strcmp(argv[i], "-frames") && i + 1 < argc) frames = strtoul(argv[++i], NULL, 0);
        if (!strcmp(argv[i], "-ring")) ring_on = 1;
        if (!strcmp(argv[i], "-cover")) tr_cover = 1;
        if (!strcmp(argv[i], "-shipfile") && i + 1 < argc) ship_file = argv[++i];
        if ((!strcmp(argv[i], "-w") || !strcmp(argv[i], "-poll") || !strcmp(argv[i], "-waitms") ||
             !strcmp(argv[i], "-arm") || !strcmp(argv[i], "-ship") || !strcmp(argv[i], "-r"))
            && i + 1 < argc && nsteps_cli < 512) {
            cli_steps[nsteps_cli].kind = argv[i][1];
            cli_steps[nsteps_cli].flag = argv[i];
            cli_steps[nsteps_cli].arg = argv[++i];
            nsteps_cli++;
            use_soc = 1;
        }
        if (!strcmp(argv[i], "-q") && i + 1 < argc) quantum = strtoull(argv[++i], NULL, 0);
        if (!strcmp(argv[i], "-mainlog") && i + 1 < argc) { mainlog = argv[++i]; use_soc = 1; }
        if (!strcmp(argv[i], "-loadrepro") && i + 1 < argc) loadrepro = argv[++i];
        if (!strcmp(argv[i], "-lane") && i + 1 < argc) lane_b18 = strtoul(argv[++i], NULL, 0);
        if (!strcmp(argv[i], "-cmdat") && i + 1 < argc) parse_cmdat(argv[++i]);
        if (!strcmp(argv[i], "-audio") && i + 1 < argc) { audio = argv[++i]; use_soc = 1; }
        if (!strcmp(argv[i], "-blocks") && i + 1 < argc) blocks = strtoul(argv[++i], NULL, 0);
        if (!strcmp(argv[i], "-wav") && i + 1 < argc) wav_path = argv[++i];
        if (!strcmp(argv[i], "-datalen") && i + 1 < argc) data_len = strtoul(argv[++i], NULL, 0);
        if (!strcmp(argv[i], "-afc") && i + 1 < argc) afc0 = strtoul(argv[++i], NULL, 0);
        if (!strcmp(argv[i], "-dump") && i + 1 < argc && ndumps < 16) {
            sscanf(argv[++i], "%x:%u", &dumps[ndumps].addr, &dumps[ndumps].n);
            ndumps++;
        }
        if (!strcmp(argv[i], "-f") && i + 1 < argc) fw_path = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc) s1_dir = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) max_cycles = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc)
            sscanf(argv[++i], "%x:%x", &tr_lo, &tr_hi);
    }

    size_t fwlen;
    uint8_t *fw = slurp(fw_path, &fwlen);
    if (!fw) { fprintf(stderr, "cannot read %s\n", fw_path); return 1; }

    l2_host = calloc(1, L2_SIZE);
    ddr_host = calloc(1, DDR_SIZE);

    static const struct { const char *file; uint32_t addr; } s1[] = {
        { "s00_00800000.bin", 0x00800000 },
        { "s01_00800200.bin", 0x00800200 },
        { "s02_00812ae8.bin", 0x00812ae8 },
    };
    for (unsigned i = 0; i < 3; i++) {
        char path[512];
        size_t n;
        snprintf(path, sizeof path, "%s/%s", s1_dir, s1[i].file);
        uint8_t *d = slurp(path, &n);
        if (!d) { fprintf(stderr, "cannot read %s\n", path); return 1; }
        memcpy(l2_host + (s1[i].addr - L2_BASE), d, n);
        free(d);
    }

    build_stream(&B, fw);

    c66x_bus bus = { &B, bus_read, bus_write };
    c66x_core *c = c66x_new(&bus);
    B.core = c;
    c66x_map_ram(c, L2_BASE, L2_SIZE, l2_host);
    c66x_map_ram(c, L2_ALIAS, L2_SIZE, l2_host);
    c66x_map_ram(c, DDR_BASE, DDR_SIZE, ddr_host);
    c66x_reset(c, 0x00800200);
    {
        /* C5_WATCH=lo:hi replaces the request-table watch with any range */
        uint32_t lo = REQ_TABLE, hi = REQ_TABLE + 0x40;
        if (getenv("C5_WATCH"))
            sscanf(getenv("C5_WATCH"), "%x:%x", &lo, &hi);
        if (getenv("C5_NANWATCH") && sscanf(getenv("C5_NANWATCH"), "%x:%x", &lo, &hi) == 2)
            c66x_watch_writes(c, lo, hi, nan_watch, &B);
        else
            c66x_watch_writes(c, lo, hi, req_watch, &B);
    }
    /* C5_WATCHDOG=secs: after that much host time, dump the last packets and exit --
     * for a run that stops making progress inside one core step. Needs -ring. */
    if (getenv("C5_WATCHDOG")) {
        ring_on = 1;
        g_thread_unref(g_thread_new("c5-watchdog", watchdog_fire,
                       GUINT_TO_POINTER((guint)atoi(getenv("C5_WATCHDOG")))));
    }
    c66x_set_trace(c, tr_lo <= tr_hi ? tracer : ring_on ? ring_tracer : mark_tracer, NULL);

    B.pending_ack = -1;
    if (use_soc) {
        c6655_soc_config cfg = {
            .core = c,
            .mem = { &B, dsp_mem_read, dsp_mem_write },
            .opaque = &B,
            .gpio_out = soc_gpio_out,
            .gpio_setclr = soc_gpio_setclr,
            .spi_xfer = soc_spi_xfer,
            .mcbsp_tx = soc_mcbsp_tx,
            .log = soc_log,
        };
        if (wav_path) {
            wav = fopen(wav_path, "wb");
            if (wav)
                fwrite((uint8_t[44]){ 0 }, 1, 44, wav);
        }
        B.soc = c6655_soc_new(&cfg);
        c6655_soc_reset(B.soc);
        printf("SoC models linked (c2)\n");
        peer_script_io io = { &B, script_now, script_ship };
        B.script = peer_script_new(&io);
        peer_script_trace_frames(B.script, frames);
        if (m4)
            add_m4_checks(B.script);
        if (corpus)
            printf("corpus: %d frames queued from %s\n",
                   peer_script_corpus(B.script, corpus, 0, upto), corpus);
        add_cli_steps(B.script);
        if (mainlog)
            printf("mainlog: %d frames queued from %s\n",
                   peer_script_mainlog(B.script, mainlog, upto), mainlog);
        if (loadrepro) {
            /* The load as in the machine: after MAIN's frames, one 0xFC00 arm and
             * the track's first 64 KB shipped as MAIN does, in 16 KB DMA units */
            size_t alen;
            uint8_t *mp3 = slurp(loadrepro, &alen);
            if (!mp3 || alen < 0x10000) { fprintf(stderr, "cannot read %s\n", loadrepro); return 1; }
            uint32_t a[3] = { 0x0B90, 0xFC00, 0xFC00 };
            peer_script_mark(B.script, "load repro: arm 0x0B90 and ship 4 x 16 KB");
            peer_script_request(B.script, 6, a, 3);
            for (unsigned u = 0; u < 4; u++)
                peer_script_ship(B.script, mp3 + u * 0x4000u, 0x4000);
            for (unsigned k = 0; k < 60; k++) {
                peer_script_wait_ns(B.script, 25000000);
                peer_script_request(B.script, 4, status_poll, 18);
            }
        }
        if (audio) {
            size_t alen;
            uint8_t *mp3 = slurp(audio, &alen);
            if (!mp3) { fprintf(stderr, "cannot read %s\n", audio); return 1; }
            if (!corpus)
                fprintf(stderr, "-audio expects -corpus <file> -upto 28 for the bring-up and descriptor\n");
            if (corpus)
                load_laneb(corpus);
            add_audio(B.script, mp3, alen, data_len, afc0, blocks);
        }
        peer_script_start(B.script);
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t done = 0, n;
    c66x_stop stop = C66X_STOP_BUDGET;
    uint32_t last_pc = 0;
    enum { NPROF = 4096 };
    static uint32_t prof_pc[NPROF], prof_n[NPROF];
    uint64_t script_done_ns = 0, steps = 0;
    double adv_secs = 0;
    /* C5_MHZ=n: the core runs n MHz of cycles per virtual second instead of 1 GHz,
     * as the machine's CDJ_C6X_MHZ does -- to reproduce a starved DSP offline. */
    uint64_t nspc = 1;
    if (getenv("C5_MHZ") && atoi(getenv("C5_MHZ")) > 0 && atoi(getenv("C5_MHZ")) < 1000)
        nspc = 1000 / atoi(getenv("C5_MHZ"));
    while (done < max_cycles) {
        if ((m4 || corpus) && peer_script_done(B.script)) {
            /* keep watching the status frames for a little after the last step */
            if (!script_done_ns)
                script_done_ns = c6655_soc_now_ns(B.soc);
            else if (c6655_soc_now_ns(B.soc) - script_done_ns > 300000000ULL)
                break;
        }
        uint64_t q = quantum;
        if (B.soc) {
            /* 1 GHz core: one pipeline cycle is one virtual nanosecond. */
            uint64_t now = c6655_soc_now_ns(B.soc), next = c6655_soc_next_event_ns(B.soc);
            if (next > now && (next - now) / nspc < q)
                q = (next - now) / nspc;
            if (q == 0)
                q = 1;
        }
        stop = c66x_step(c, q, &n);
        done += n;
        steps++;
        /* C5_MPOKE=t_ms:addr:val[;t_ms:addr:val]  one-shot DSP memory pokes at virtual
         * times -- a diagnostic only; results that depend on it must say so */
        {
            static struct { uint64_t ns; uint32_t addr, val; int done; } mp[8];
            static int nmp = -1;
            if (nmp < 0) {
                nmp = 0;
                const char *e = getenv("C5_MPOKE");
                while (e && *e && nmp < 8) {
                    double ms;
                    unsigned a, v;
                    if (sscanf(e, "%lf:%x:%x", &ms, &a, &v) != 3) break;
                    mp[nmp].ns = (uint64_t)(ms * 1e6); mp[nmp].addr = a; mp[nmp].val = v; nmp++;
                    e = strchr(e, ';');
                    if (!e) break;
                    e++;
                }
            }
            for (int i = 0; i < nmp && B.soc; i++)
                if (!mp[i].done && c6655_soc_now_ns(B.soc) >= mp[i].ns) {
                    mp[i].done = 1;
                    dsp_mem_write(&B, mp[i].addr, mp[i].val, 4);
                    c66x_invalidate(c, mp[i].addr, 4);
                    printf("mpoke %.4f s: *0x%08x = 0x%08x (diagnostic poke)\n",
                           c6655_soc_now_ns(B.soc) / 1e9, mp[i].addr, mp[i].val);
                }
        }
        /* C5_PROGRESS=n: every n steps, where the core is and how far virtual time got */
        static long progress = -1;
        if (progress < 0)
            progress = getenv("C5_PROGRESS") ? atol(getenv("C5_PROGRESS")) : 0;
        if (progress && steps % progress == 0) {
            printf("progress step %" PRIu64 " cycles %" PRIu64 " pc 0x%08x q %" PRIu64 " n %" PRIu64 " t %.4f\n",
                   steps, done, c66x_get_exec_pc(c), q, n, B.soc ? c6655_soc_now_ns(B.soc) / 1e9 : 0.0);
            fflush(stdout);
        }
        peek_sample(B.soc);
        if (B.soc) {
            struct timespec a0, a1;
            clock_gettime(CLOCK_MONOTONIC, &a0);
            c6655_soc_advance(B.soc, n * nspc);
            clock_gettime(CLOCK_MONOTONIC, &a1);
            adv_secs += (a1.tv_sec - a0.tv_sec) + (a1.tv_nsec - a0.tv_nsec) / 1e9;
            main_side(&B);
        }
        uint32_t pc = c66x_get_exec_pc(c);
        unsigned h = (pc >> 1) % NPROF;
        while (prof_n[h] && prof_pc[h] != pc) h = (h + 1) % NPROF;
        prof_pc[h] = pc;
        prof_n[h] += n;
        if (stop == C66X_STOP_IDLE && B.soc) {
            /* idle: let virtual time run to the next peripheral event */
            uint64_t next = c6655_soc_next_event_ns(B.soc);
            if (next == UINT64_MAX)
                break;
            c6655_soc_advance(B.soc, next - c6655_soc_now_ns(B.soc));
            main_side(&B);
            continue;
        }
        if (stop != C66X_STOP_BUDGET)
            break;
        last_pc = pc;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

    static const char *why[] = { "budget", "idle", "undefined instruction", "hook", "fault" };
    c66x_stats st;
    c66x_get_stats(c, &st);
    printf("\nstop: %s at pc 0x%08x (trap 0x%08x), last exec pc 0x%08x\n",
           why[stop], c66x_get_pc(c), c66x_trap_pc(c), last_pc);
    if (ring_on && stop != C66X_STOP_BUDGET)
        ring_dump();
    for (unsigned i = 0; i < (nhits == ~0u ? 0 : nhits); i++)
        printf("hits %08x total %llu\n", hits[i].pc, (unsigned long long)hits[i].n);
    if (tr_cover) {
        unsigned hits = 0;
        for (unsigned i = 0; i < NCOV; i++)
            if (cov_n[i]) {
                hits++;
                printf("cover %08x %u\n", cov_pc[i], cov_n[i]);
            }
        printf("cover: %u distinct packets in %08x..%08x\n", hits, tr_lo, tr_hi);
    }
    for (unsigned d = 0; d < ndumps; d++) {
        printf("dump 0x%08x:", dumps[d].addr);
        for (unsigned i = 0; i < dumps[d].n; i++)
            printf("%s%08x", i % 8 ? " " : "\n  ", dsp_mem_read(&B, dumps[d].addr + 4 * i, 4));
        printf("\n");
    }
    printf("cycles %" PRIu64 "  packets %" PRIu64 "  insns %" PRIu64 "  stalls %" PRIu64
           "  sploops %" PRIu64 "  irqs %" PRIu64 "  decodes %" PRIu64 "\n",
           st.cycles, st.packets, st.insns, st.stalls, st.sploops, st.interrupts, st.decodes);
    printf("interrupt waits: %" PRIu64 " cycles on branch slots, %" PRIu64 " on SPLOOP;"
           " IFR 0x%08x\n", st.irq_wait_branch, st.irq_wait_sploop, c66x_get_creg(c, 2));
    printf("interrupt edges by line:");
    for (int i = 0; i < 16; i++)
        if (st.irq_edges[i]) printf(" INT%d x%u", i, st.irq_edges[i]);
    printf("  (virtual time %.3f s)\n", B.soc ? c6655_soc_now_ns(B.soc) / 1e9 : st.cycles / 1e9);
    if (B.soc) {
        printf("soc next event in %" PRId64 " ns\n",
               (int64_t)(c6655_soc_next_event_ns(B.soc) - c6655_soc_now_ns(B.soc)));
        for (unsigned t = 0; t < 8; t++) {
            uint32_t base = 0x02200000 + (t << 16);
            printf("  timer%u CNT %08x%08x PRD %08x%08x TCR %08x TGCR %08x INTCTL %08x\n", t,
                   c6655_soc_read(B.soc, base + 0x14, 4), c6655_soc_read(B.soc, base + 0x10, 4),
                   c6655_soc_read(B.soc, base + 0x1c, 4), c6655_soc_read(B.soc, base + 0x18, 4),
                   c6655_soc_read(B.soc, base + 0x20, 4), c6655_soc_read(B.soc, base + 0x24, 4),
                   c6655_soc_read(B.soc, base + 0x44, 4));
        }
        printf("  EDMA ER %08x EER %08x IER %08x IPR %08x EMR %08x  R2 IPR %08x IER %08x\n",
               c6655_soc_read(B.soc, 0x02741004, 4), c6655_soc_read(B.soc, 0x02741024, 4),
               c6655_soc_read(B.soc, 0x02741054, 4), c6655_soc_read(B.soc, 0x0274106c, 4),
               c6655_soc_read(B.soc, 0x02740304, 4), c6655_soc_read(B.soc, 0x02742468, 4),
               c6655_soc_read(B.soc, 0x02742450, 4));
        for (unsigned set = 0; set < 128; set++) {
            uint32_t base = 0x02744000 + set * 32;
            uint32_t opt = c6655_soc_read(B.soc, base, 4);
            if (!opt) continue;
            printf("  PaRAM %3u OPT %08x SRC %08x AB %08x DST %08x BIDX %08x LNK %08x CIDX %08x CCNT %08x\n",
                   set, opt, c6655_soc_read(B.soc, base + 4, 4), c6655_soc_read(B.soc, base + 8, 4),
                   c6655_soc_read(B.soc, base + 12, 4), c6655_soc_read(B.soc, base + 16, 4),
                   c6655_soc_read(B.soc, base + 20, 4), c6655_soc_read(B.soc, base + 24, 4),
                   c6655_soc_read(B.soc, base + 28, 4));
        }
        printf("  SPI GCR1 %08x INT0 %08x FLG %08x BUF %08x  CIC0 raw/stat %08x %08x %08x %08x\n",
               c6655_soc_read(B.soc, 0x20BF0004, 4), c6655_soc_read(B.soc, 0x20BF0008, 4),
               c6655_soc_read(B.soc, 0x20BF0010, 4), c6655_soc_read(B.soc, 0x20BF0044, 4),
               c6655_soc_read(B.soc, 0x02600200, 4), c6655_soc_read(B.soc, 0x02600280, 4),
               c6655_soc_read(B.soc, 0x02600204, 4), c6655_soc_read(B.soc, 0x02600284, 4));
        printf("  INTC EVTFLAG %08x %08x %08x %08x  EVTMASK %08x %08x %08x %08x  INTMUX %08x %08x %08x\n",
               c6655_soc_read(B.soc, 0x01800000, 4), c6655_soc_read(B.soc, 0x01800004, 4),
               c6655_soc_read(B.soc, 0x01800008, 4), c6655_soc_read(B.soc, 0x0180000c, 4),
               c6655_soc_read(B.soc, 0x01800080, 4), c6655_soc_read(B.soc, 0x01800084, 4),
               c6655_soc_read(B.soc, 0x01800088, 4), c6655_soc_read(B.soc, 0x0180008c, 4),
               c6655_soc_read(B.soc, 0x01800104, 4), c6655_soc_read(B.soc, 0x01800108, 4),
               c6655_soc_read(B.soc, 0x0180010c, 4));
    }
    if (B.soc)
        printf("edma end: IPR %08x:%08x IER %08x:%08x  region1 IPR %08x:%08x IER %08x:%08x  "
               "region2 IPR %08x:%08x  CIC0 raw3 %08x en3 %08x\n",
               c6655_soc_read(B.soc, 0x0274106c, 4), c6655_soc_read(B.soc, 0x02741068, 4),
               c6655_soc_read(B.soc, 0x02741054, 4), c6655_soc_read(B.soc, 0x02741050, 4),
               c6655_soc_read(B.soc, 0x0274226c, 4), c6655_soc_read(B.soc, 0x02742268, 4),
               c6655_soc_read(B.soc, 0x02742254, 4), c6655_soc_read(B.soc, 0x02742250, 4),
               c6655_soc_read(B.soc, 0x0274246c, 4), c6655_soc_read(B.soc, 0x02742468, 4),
               c6655_soc_read(B.soc, 0x0260020c, 4), c6655_soc_read(B.soc, 0x0260030c, 4));
    printf("steps %" PRIu64 " (%.0f cycles each), SoC advance %.2f s of %.2f s host (%.1f%%)\n",
           steps, steps ? (double)st.cycles / steps : 0.0, adv_secs, secs,
           secs > 0 ? 100 * adv_secs / secs : 0.0);
    if (B.soc) {
        uint64_t ds[6];
        c6655_soc_debug_stats(B.soc, ds);
        printf("soc stats: advance %" PRIu64 " fast %" PRIu64 " loops %" PRIu64 " recompute %" PRIu64
               " words %" PRIu64 " catchups %" PRIu64 "\n", ds[0], ds[1], ds[2], ds[3], ds[4], ds[5]);
    }
    printf("time %.2f s: %.1f M insns/s, %.1f M cycles/s\n", secs,
           st.insns / secs / 1e6, st.cycles / secs / 1e6);
    printf("CSR 0x%08x TSR 0x%08x IER 0x%08x ISTP 0x%08x  B14 0x%08x B15 0x%08x\n",
           c66x_get_creg(c, 1), c66x_get_creg(c, 0x1a), c66x_get_creg(c, 4),
           c66x_get_creg(c, 5), c66x_get_reg(c, C66X_B(14)), c66x_get_reg(c, C66X_B(15)));
    printf("uPP windows delivered %u of %zu; stream consumed %zu B\n",
           B.windows, B.stream_len / WINDOW, B.stream_pos);

    /* Cross-check: loaded memory equals the app table's sections. */
    const uint8_t *p = fw + APP_OFF + 4;
    unsigned secs_ok = 0, secs_n = 0;
    for (;;) {
        uint32_t size = p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
        if (!size) break;
        uint32_t addr = p[4] | (p[5] << 8) | (p[6] << 16) | ((uint32_t)p[7] << 24);
        uint8_t *host = addr >= DDR_BASE ? ddr_host + (addr - DDR_BASE) : l2_host + (addr - L2_BASE);
        secs_n++;
        if (!memcmp(host, p + 8, size)) secs_ok++;
        p += 8 + ((size + 3) & ~3u);
    }
    printf("app sections in memory equal to the table: %u of %u\n", secs_ok, secs_n);

    if (B.script) {
        printf("SPI words by CSNR:");
        for (unsigned i = 0; i < 256; i++)
            if (spi_words_by_csnr[i])
                printf(" %u:x%" PRIu64, i, spi_words_by_csnr[i]);
        printf("\n");
        peer_script_report(B.script, stdout);
        if (m4)
            report_m4(B.script);
        wav_close();
    }

    printf("\nmilestones (packet count at first execution):\n");
    for (unsigned i = 0; i < sizeof marks / sizeof marks[0]; i++)
        printf("  0x%08x  %s%" PRIu64 "\n", marks[i], mark_hit[i] ? "" : "never ",
               mark_hit[i] ? mark_cycle[i] : 0);
    printf("\nwhere the cycles went (sampled at quantum ends):\n");
    for (int k = 0; k < 12; k++) {
        unsigned best = 0;
        for (unsigned i = 1; i < NPROF; i++)
            if (prof_n[i] > prof_n[best]) best = i;
        if (!prof_n[best]) break;
        printf("  0x%08x  %u\n", prof_pc[best], prof_n[best]);
        prof_n[best] = 0;
    }

    printf("\nfirst MMIO accesses (%u distinct):\n", B.nlog);
    for (unsigned i = 0; i < B.nlog && i < 60; i++)
        printf("  %c 0x%08x = 0x%08x  pc 0x%08x\n", B.log[i].rw, B.log[i].addr,
               B.log[i].val, B.log[i].pc);
    c66x_free(c);
    return 0;
}
