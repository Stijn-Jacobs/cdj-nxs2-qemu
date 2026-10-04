/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * ADSP-BF531 SoC model (see bf531.h). System MMRs sit at 0xFFC00000; the
 * register offsets below are relative to that base and follow the BF533
 * Hardware Reference. What the display firmware does not use is a plain
 * register file that logs its first access, so the next block to model shows
 * up in the log.
 */
#include "bf531.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define SYS_BASE    0xFFC00000u
#define SYS_SIZE    0x2000u

#define ASYNC_BASE  0x20000000u
#define FLASH_SIZE  (2u << 20)
/* The update section (after its title) sits at flash +0x10000: the firmware
 * reads its resources at 0x200EBBA0, which is file offset 0xDBBC0 there.
 * The first 64 KB hold the loader the boot ROM runs, which no update
 * carries; it stays erased, since the model boots the stream directly. */
#define FLASH_APP   0x10000u

#define L1_DATA_A   0xFF800000u
#define L1_DATA_B   0xFF900000u
#define L1_CODE     0xFFA00000u
#define L1_SCRATCH  0xFFB00000u

/* The BF531/532 boot ROM enters the loaded program at the start of their
 * only L1 instruction bank. */
#define BOOT_ENTRY  0xFFA08000u

enum {
    PLL_CTL = 0x000, PLL_DIV = 0x004, PLL_STAT = 0x00C, CHIPID = 0x014,
    SIC_IMASK = 0x10C, SIC_IAR0 = 0x110, SIC_ISR = 0x120, SIC_IWR = 0x124,
    SPI_CTL = 0x500, SPI_STAT = 0x508, SPI_TDBR = 0x50C, SPI_RDBR = 0x510,
    SPORT1_TCR1 = 0x900,
    TIMER0 = 0x600, TIMER_ENABLE = 0x640, TIMER_DISABLE = 0x644,
    TIMER_STATUS = 0x648,
    FIO_FLAG_D = 0x700, FIO_FLAG_C = 0x704, FIO_FLAG_S = 0x708,
    FIO_FLAG_T = 0x70C, FIO_DIR = 0x730,
    EBIU_SDSTAT = 0xA1C,
    DMA0 = 0xC00, DMA_END = 0xE00,
    PPI_CONTROL = 0x1000,
};

/* DMA channel registers, offsets within a channel's 0x40 bytes. */
enum {
    D_NEXT = 0x00, D_START = 0x04, D_CONFIG = 0x08, D_XCOUNT = 0x10,
    D_XMOD = 0x14, D_YCOUNT = 0x18, D_YMOD = 0x1C, D_CURR_DESC = 0x20,
    D_CURR_ADDR = 0x24, D_IRQ_STATUS = 0x28, D_PMAP = 0x2C,
    D_CURR_X = 0x30, D_CURR_Y = 0x38,
};

#define DMAEN       0x0001
#define TSPEN       0x0001
#define DMA2D       0x0010
#define DI_EN       0x0080
#define DMA_DONE    0x0001
#define DMA_RUN     0x0008

/* SIC peripheral interrupt numbers. */
enum { IRQ_PPI_DMA = 8, IRQ_TIMER0 = 16 };

#define TIMER_PWM_OUT   0x0001
#define TIMER_IRQ_ENA   0x0010

#define NEVER UINT64_MAX

typedef struct bf531_timer {
    uint32_t config, counter, period, width;
    uint64_t due;
} bf531_timer;

typedef struct bf531_dma {
    uint32_t reg[0x40 / 4];
    uint64_t due;
} bf531_dma;

struct bf531 {
    bfin_core *core;
    bf531_host host;
    FILE *log;

    uint8_t *sdram;
    uint32_t sdram_size;
    uint8_t  l1_data_a[0x8000], l1_data_b[0x8000], l1_code[0x14000];
    uint8_t  l1_scratch[0x1000];
    uint8_t  flash[FLASH_SIZE];

    uint32_t mmr[SYS_SIZE / 4];
    uint8_t  logged[SYS_SIZE / 4];
    uint32_t sic_isr;

    bf531_timer timer[3];
    uint32_t timer_status;
    bf531_dma dma[8];
    uint16_t *fb;
    uint64_t frames;

    /* One standing SPORT1 RX packet from the host, consumed the next time
     * the firmware arms DMA3 (see bf531_sport1_rx). MAIN's packets are at
     * most its 2 KB transmit buffer. */
    uint8_t  sport1_rx[2048];
    size_t   sport1_rx_len;
    bool     sport1_rx_armed;
};

static void __attribute__((format(printf, 2, 3))) slog(bf531 *s, const char *fmt, ...)
{
    va_list ap;

    if (s->log) {
        va_start(ap, fmt);
        fprintf(s->log, "bf531: ");
        vfprintf(s->log, fmt, ap);
        fputc('\n', s->log);
        va_end(ap);
    }
}

/* SCLK cycles to core cycles: CCLK = VCO >> CSEL, SCLK = VCO / SSEL. */
static uint64_t sclk_to_cclk(bf531 *s, uint64_t n)
{
    uint32_t div = s->mmr[PLL_DIV / 4];
    unsigned ssel = div & 0xF ? div & 0xF : 5, csel = (div >> 4) & 3;

    return (n * ssel) >> csel;
}

/* ---- SIC ---------------------------------------------------------------- */

static void sic_update(bf531 *s)
{
    uint32_t pend = s->sic_isr & s->mmr[SIC_IMASK / 4];
    uint32_t level = 0;

    for (int n = 0; n < 24; n++) {
        if (pend & (1u << n)) {
            level |= 1u << (((s->mmr[SIC_IAR0 / 4 + n / 8] >> (4 * (n % 8))) & 0xF) + 7);
        }
    }
    for (int ivg = 7; ivg < 16; ivg++) {
        bfin_set_ivg(s->core, ivg, (level >> ivg) & 1);
    }
}

static void sic_set(bf531 *s, int irq, int on)
{
    if (on) {
        s->sic_isr |= 1u << irq;
    } else {
        s->sic_isr &= ~(1u << irq);
    }
    sic_update(s);
}

/* ---- GP timers ---------------------------------------------------------- */

static void timer_arm(bf531 *s, int n)
{
    bf531_timer *t = &s->timer[n];

    t->due = (t->config & 3) == TIMER_PWM_OUT && t->period ?
        bfin_cycles(s->core) + sclk_to_cclk(s, t->period) : NEVER;
    bfin_yield(s->core);
}

static void timer_fire(bf531 *s, int n)
{
    bf531_timer *t = &s->timer[n];

    if (t->config & TIMER_IRQ_ENA) {
        s->timer_status |= 1u << n;
        sic_set(s, IRQ_TIMER0 + n, 1);
    }
    t->due = t->period ? t->due + sclk_to_cclk(s, t->period) : NEVER;
}

static uint32_t timer_read(bf531 *s, uint32_t off)
{
    if (off >= TIMER_ENABLE) {
        switch (off) {
        case TIMER_ENABLE:
        case TIMER_DISABLE: {
            uint32_t on = 0;

            for (int n = 0; n < 3; n++) {
                on |= (s->timer[n].due != NEVER) << n;
            }
            return on;
        }
        case TIMER_STATUS: {
            uint32_t st = s->timer_status;

            for (int n = 0; n < 3; n++) {
                st |= (uint32_t)(s->timer[n].due != NEVER) << (12 + n);
            }
            return st;
        }
        }
        return 0;
    }
    bf531_timer *t = &s->timer[(off - TIMER0) / 0x10];

    switch (off & 0xF) {
    case 0x0: return t->config;
    case 0x4: return t->counter;
    case 0x8: return t->period;
    default:  return t->width;
    }
}

static void timer_write(bf531 *s, uint32_t off, uint32_t v)
{
    if (off >= TIMER_ENABLE) {
        for (int n = 0; n < 3; n++) {
            if (!(v & (1u << n))) {
                continue;
            }
            if (off == TIMER_ENABLE) {
                timer_arm(s, n);
            } else if (off == TIMER_DISABLE) {
                s->timer[n].due = NEVER;
            }
        }
        if (off == TIMER_STATUS) {
            s->timer_status &= ~(v & 7);
            for (int n = 0; n < 3; n++) {
                sic_set(s, IRQ_TIMER0 + n, (s->timer_status >> n) & 1);
            }
        }
        return;
    }
    bf531_timer *t = &s->timer[(off - TIMER0) / 0x10];

    switch (off & 0xF) {
    case 0x0: t->config = v; break;
    case 0x4: t->counter = v; break;
    case 0x8: t->period = v; break;
    default:  t->width = v; break;
    }
}

/* ---- DMA and the PPI ---------------------------------------------------- */

static uint8_t *load_target(bf531 *s, uint32_t addr, uint32_t len)
{
    if (addr < s->sdram_size && len <= s->sdram_size - addr) {
        return s->sdram + addr;
    }
    if (addr - L1_CODE < sizeof(s->l1_code) && len <= sizeof(s->l1_code) - (addr - L1_CODE)) {
        return s->l1_code + (addr - L1_CODE);
    }
    if (addr - L1_DATA_A < sizeof(s->l1_data_a) && len <= sizeof(s->l1_data_a) - (addr - L1_DATA_A)) {
        return s->l1_data_a + (addr - L1_DATA_A);
    }
    if (addr - L1_DATA_B < sizeof(s->l1_data_b) && len <= sizeof(s->l1_data_b) - (addr - L1_DATA_B)) {
        return s->l1_data_b + (addr - L1_DATA_B);
    }
    return NULL;
}

static uint32_t mem_read(bf531 *s, uint32_t addr, unsigned size);

static uint16_t desc_word(bf531 *s, uint32_t *p)
{
    uint16_t v = mem_read(s, *p, 2);

    *p += 2;
    return v;
}

/* Loads the next descriptor for the large-list, small-list and array flows:
 * NDSIZE 16-bit words in the order NEXT_DESC (large list only), START,
 * CONFIG, X_COUNT, X_MODIFY, Y_COUNT, Y_MODIFY. */
static void dma_fetch(bf531 *s, bf531_dma *d)
{
    uint32_t *r = d->reg;
    unsigned flow = (r[D_CONFIG / 4] >> 12) & 7;
    unsigned nd = (r[D_CONFIG / 4] >> 8) & 0xF;
    uint32_t p = flow == 4 ? r[D_CURR_DESC / 4] : r[D_NEXT / 4];
    static const uint8_t order[] = { D_START, D_CONFIG, D_XCOUNT, D_XMOD,
                                     D_YCOUNT, D_YMOD };
    unsigned k = 0;

    if (flow == 6) {
        r[D_NEXT / 4] = (r[D_NEXT / 4] & 0xFFFF0000) | desc_word(s, &p);
        k = 1;
    } else if (flow == 7) {
        r[D_NEXT / 4] = desc_word(s, &p);
        r[D_NEXT / 4] |= (uint32_t)desc_word(s, &p) << 16;
        k = 2;
    }
    for (unsigned i = 0; k < nd && i < sizeof(order); i++) {
        uint32_t v;

        if (order[i] == D_START) {
            v = desc_word(s, &p);
            v |= k + 1 < nd ? (uint32_t)desc_word(s, &p) << 16 : 0;
            k += 2;
        } else {
            v = desc_word(s, &p);
            k++;
        }
        r[order[i] / 4] = v;
    }
    r[D_CURR_DESC / 4] = flow == 4 ? p : r[D_NEXT / 4];
}

static unsigned dma_rows(const bf531_dma *d)
{
    return d->reg[D_CONFIG / 4] & DMA2D ? (d->reg[D_YCOUNT / 4] & 0xFFFF) : 1;
}

/* The PPI clocks one 16-bit word per pixel. Its clock is not modelled: a
 * work unit takes the time a 60 Hz panel spends on its share of a 525-line
 * frame, which is all the firmware can observe of it. */
static void dma_start_unit(bf531 *s, bf531_dma *d)
{
    uint64_t lines = dma_rows(d);

    d->reg[D_CURR_ADDR / 4] = d->reg[D_START / 4];
    d->reg[D_IRQ_STATUS / 4] |= DMA_RUN;
    d->due = bfin_cycles(s->core) + lines * (BF531_CCLK_HZ / 60 / 262);
    bfin_yield(s->core);
}

static void ppi_frame(bf531 *s, bf531_dma *d)
{
    uint32_t *r = d->reg;
    unsigned w = r[D_XCOUNT / 4] & 0xFFFF, h = dma_rows(d);
    int32_t xmod = (int16_t)r[D_XMOD / 4], ymod = (int16_t)r[D_YMOD / 4];
    uint32_t a = r[D_START / 4];

    if (w < 64 || h < 64 || w > 1024 || h > 1024) {
        return;
    }
    s->fb = realloc(s->fb, (size_t)w * h * 2);
    for (unsigned y = 0; y < h; y++) {
        uint8_t *row = xmod == 2 ? load_target(s, a, w * 2) : NULL;

        if (row) {
            memcpy(&s->fb[y * w], row, w * 2);
            a += (w - 1) * 2 + ymod;
            continue;
        }
        for (unsigned x = 0; x < w; x++) {
            s->fb[y * w + x] = mem_read(s, a, 2);
            a += x + 1 < w ? xmod : ymod;
        }
    }
    s->frames++;
    if (s->host.frame) {
        s->host.frame(s->host.opaque, s->fb, w, h);
    }
}

static void dma_unit_done(bf531 *s, int ch)
{
    bf531_dma *d = &s->dma[ch];
    uint32_t cfg = d->reg[D_CONFIG / 4];
    unsigned flow = (cfg >> 12) & 7;

    if (ch == 0) {
        ppi_frame(s, d);
    }
    if (cfg & DI_EN) {
        d->reg[D_IRQ_STATUS / 4] |= DMA_DONE;
        sic_set(s, IRQ_PPI_DMA + ch, 1);
    }
    switch (flow) {
    case 0:
        d->reg[D_IRQ_STATUS / 4] &= ~DMA_RUN;
        d->due = NEVER;
        return;
    case 1:
        break;
    default:
        dma_fetch(s, d);
        break;
    }
    dma_start_unit(s, d);
}

/* Copies one host packet into an armed DMA3 (SPORT1 RX) target and
 * completes the unit, as MAIN's own clock would over a real link. Nothing
 * in the firmware unmasks the SIC bit for this channel before it gets here
 * on a plain boot (checked to 2 G cycles with no packet waiting), so it is
 * set here too. */
static void sport1_rx_complete(bf531 *s, int ch)
{
    bf531_dma *d = &s->dma[ch];
    size_t want = (d->reg[D_XCOUNT / 4] & 0xFFFF) * 2;
    size_t len = s->sport1_rx_len < want ? s->sport1_rx_len : want;
    uint8_t *to = load_target(s, d->reg[D_START / 4], len);

    if (to) {
        memcpy(to, s->sport1_rx, len);
    }
    s->sport1_rx_len = 0;
    s->sport1_rx_armed = false;
    s->mmr[SIC_IMASK / 4] |= 1u << (IRQ_PPI_DMA + ch);
    dma_unit_done(s, ch);
}

/* Hands DMA4's buffer (SPORT1 TX, the answer to MAIN) to the host and
 * completes the unit, once both the channel and the transmitter are on: the
 * firmware enables DMA4, fills the buffer, then sets TSPEN. */
static void sport1_tx_start(bf531 *s)
{
    bf531_dma *d = &s->dma[4];
    uint8_t pkt[128];
    size_t len = (d->reg[D_XCOUNT / 4] & 0xFFFF) * 2;

    if (len > sizeof(pkt)) {
        len = sizeof(pkt);
    }
    for (size_t i = 0; i < len; i++) {
        pkt[i] = mem_read(s, d->reg[D_START / 4] + i, 1);
    }
    if (s->host.sport1_tx) {
        s->host.sport1_tx(s->host.opaque, pkt, len);
    }
    dma_unit_done(s, 4);
}

static void dma_write(bf531 *s, uint32_t off, uint32_t v)
{
    int ch = (off - DMA0) / 0x40;
    bf531_dma *d = &s->dma[ch];
    uint32_t reg = off & 0x3F;

    if (reg == D_IRQ_STATUS) {
        d->reg[reg / 4] &= ~(v & 3);
        sic_set(s, IRQ_PPI_DMA + ch, d->reg[reg / 4] & DMA_DONE);
        return;
    }
    d->reg[reg / 4] = v;
    if (reg != D_CONFIG) {
        return;
    }
    if (!(v & DMAEN)) {
        d->due = NEVER;
        s->sport1_rx_armed &= ch != 3;
        d->reg[D_IRQ_STATUS / 4] &= ~DMA_RUN;
        return;
    }
    /* The PPI channel moves frames; DMA3 and DMA4 are SPORT1's receive and
     * transmit, MAIN's link. */
    if (ch == 0) {
        if (((v >> 12) & 7) >= 4) {
            if (((v >> 12) & 7) == 4) {
                d->reg[D_CURR_DESC / 4] = d->reg[D_NEXT / 4];
            }
            dma_fetch(s, d);
        }
        dma_start_unit(s, d);
    } else {
        slog(s, "DMA%u enabled, config 0x%04x, start 0x%08x, %u words", ch, v,
             d->reg[D_START / 4], d->reg[D_XCOUNT / 4] & 0xFFFF);
        if (ch == 3) {
            s->sport1_rx_armed = true;
            if (s->sport1_rx_len) {
                sport1_rx_complete(s, ch);
            }
        } else if (ch == 4) {
            d->reg[D_IRQ_STATUS / 4] |= DMA_RUN;
            if (s->mmr[SPORT1_TCR1 / 4] & TSPEN) {
                sport1_tx_start(s);
            }
        }
    }
}

static uint32_t dma_read(bf531 *s, uint32_t off)
{
    return s->dma[(off - DMA0) / 0x40].reg[(off & 0x3F) / 4];
}

/* ---- system MMRs -------------------------------------------------------- */

static int mmr_modelled(uint32_t off)
{
    return off < 0x020 || (off >= 0x100 && off < 0x128) ||
           (off >= 0x500 && off < 0x520) || (off >= 0x600 && off < 0x650) ||
           (off >= 0x700 && off < 0x750) || (off >= 0xA00 && off < 0xA20) ||
           (off >= DMA0 && off < DMA_END) || (off >= PPI_CONTROL && off < 0x1014);
}

static uint32_t sys_read(bf531 *s, uint32_t off)
{
    if (!mmr_modelled(off) && !s->logged[off / 4]) {
        s->logged[off / 4] = 1;
        slog(s, "read 0x%08x (unmodelled)", SYS_BASE + off);
    }
    if (off >= DMA0 && off < DMA_END) {
        return dma_read(s, off);
    }
    if (off >= TIMER0 && off < TIMER_STATUS + 4) {
        return timer_read(s, off);
    }
    switch (off) {
    case PLL_STAT:  return 0x20;              /* PLL_LOCKED */
    case CHIPID:    return 0x027A50CB;
    case SIC_ISR:   return s->sic_isr;
    case SPI_STAT:  return 0x21;              /* SPIF, RXS: a transfer is done */
    case SPI_RDBR:  return 0;
    case FIO_FLAG_C: case FIO_FLAG_S: case FIO_FLAG_T:
        return s->mmr[FIO_FLAG_D / 4];
    case EBIU_SDSTAT: return 0x8;             /* SDRS: SDRAM powered up */
    }
    return s->mmr[off / 4];
}

static void sys_write(bf531 *s, uint32_t off, uint32_t v)
{
    if (!mmr_modelled(off) && !s->logged[off / 4]) {
        s->logged[off / 4] = 1;
        slog(s, "write 0x%08x = 0x%x (unmodelled)", SYS_BASE + off, v);
    }
    if (off >= DMA0 && off < DMA_END) {
        dma_write(s, off, v);
        return;
    }
    if (off >= TIMER0 && off < TIMER_STATUS + 4) {
        timer_write(s, off, v);
        return;
    }
    switch (off) {
    case PLL_CTL:
        /* The relock ends the IDLE the firmware issues next. */
        s->mmr[off / 4] = v;
        bfin_wake(s->core);
        return;
    case PLL_STAT: case CHIPID: case SIC_ISR:
        return;
    case SPORT1_TCR1:
        if (v & ~s->mmr[off / 4] & TSPEN &&
            s->dma[4].reg[D_IRQ_STATUS / 4] & DMA_RUN) {
            s->mmr[off / 4] = v;
            sport1_tx_start(s);
            return;
        }
        break;
    case FIO_FLAG_C: s->mmr[FIO_FLAG_D / 4] &= ~v; return;
    case FIO_FLAG_S: s->mmr[FIO_FLAG_D / 4] |= v; return;
    case FIO_FLAG_T: s->mmr[FIO_FLAG_D / 4] ^= v; return;
    }
    s->mmr[off / 4] = v;
    if (off == SIC_IMASK || (off >= SIC_IAR0 && off < SIC_IAR0 + 12)) {
        sic_update(s);
    }
}

/* ---- bus ---------------------------------------------------------------- */

static uint32_t mem_read(bf531 *s, uint32_t addr, unsigned size)
{
    uint8_t *h = load_target(s, addr, size);
    uint32_t v = 0;

    if (!h && addr - ASYNC_BASE < FLASH_SIZE) {
        h = s->flash + (addr - ASYNC_BASE);
    }
    if (h) {
        memcpy(&v, h, size);
        return v;
    }
    if (addr - SYS_BASE < SYS_SIZE) {
        return sys_read(s, addr - SYS_BASE);
    }
    slog(s, "read 0x%08x size %u (unmapped)", addr, size);
    return 0;
}

static uint32_t bus_read(void *opaque, uint32_t addr, unsigned size)
{
    return mem_read(opaque, addr, size);
}

static void bus_write(void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
    bf531 *s = opaque;

    if (addr - SYS_BASE < SYS_SIZE) {
        sys_write(s, addr - SYS_BASE, val);
    } else if (addr - ASYNC_BASE < FLASH_SIZE) {
        slog(s, "flash write 0x%08x = 0x%x", addr, val);
    } else {
        slog(s, "write 0x%08x = 0x%x (unmapped)", addr, val);
    }
}

/* ---- the chip ----------------------------------------------------------- */

bf531 *bf531_new(uint32_t sdram_size, const bf531_host *host, FILE *log)
{
    bf531 *s = calloc(1, sizeof(*s));
    bfin_bus bus = { s, bus_read, bus_write };
    static const uint32_t iar[3] = { 0x10000000, 0x33322221, 0x66655444 };

    s->host = *host;
    s->log = log;
    s->sdram_size = sdram_size;
    s->sdram = calloc(1, sdram_size);
    memset(s->flash, 0xFF, sizeof(s->flash));
    s->core = bfin_new(&bus);
    bfin_map_ram(s->core, 0, sdram_size, s->sdram);
    bfin_map_ram(s->core, L1_CODE, sizeof(s->l1_code), s->l1_code);
    bfin_map_ram(s->core, L1_DATA_A, sizeof(s->l1_data_a), s->l1_data_a);
    bfin_map_ram(s->core, L1_DATA_B, sizeof(s->l1_data_b), s->l1_data_b);
    bfin_map_ram(s->core, L1_SCRATCH, sizeof(s->l1_scratch), s->l1_scratch);
    bfin_map_rom(s->core, ASYNC_BASE, FLASH_SIZE, s->flash);

    /* Reset values: PLL_DIV SSEL 5, the default SIC assignment (PPI DMA
     * IVG8, SPORT DMA IVG9, the timers IVG11). */
    s->mmr[PLL_DIV / 4] = 0x0005;
    for (int n = 0; n < 3; n++) {
        s->mmr[SIC_IAR0 / 4 + n] = iar[n];
        s->timer[n].due = NEVER;
    }
    for (int n = 0; n < 8; n++) {
        s->dma[n].due = NEVER;
    }
    return s;
}

void bf531_free(bf531 *s)
{
    bfin_free(s->core);
    free(s->sdram);
    free(s->fb);
    free(s);
}

void bf531_sport1_rx(bf531 *s, const uint8_t *data, size_t len)
{
    if (len > sizeof(s->sport1_rx)) {
        len = sizeof(s->sport1_rx);
    }
    if (data) {
        memcpy(s->sport1_rx, data, len);
    }
    s->sport1_rx_len = data ? len : 0;
    /* Lands as the chip's next event, not from the host's call, so the
     * interrupt it raises reaches the core inside bf531_run. */
    if (data && s->sport1_rx_armed) {
        s->dma[3].due = bfin_cycles(s->core);
    }
}

uint16_t bf531_flags(const bf531 *s)
{
    return s->mmr[FIO_FLAG_D / 4] & s->mmr[FIO_DIR / 4];
}

/* LDR block: u32 destination, u32 byte count, u16 flags; bit 0 zero-fill
 * (no payload), bit 4 ignore (payload skipped), bit 15 the last block. */
/* A whole .UPD starts with the section lengths as decimal lines; the display
 * program is the first section. A bare section starts with its title. */
static void first_section(const uint8_t **img, size_t *len)
{
    const uint8_t *p = *img, *end = *img + *len;
    size_t first = 0;
    bool seen = false;

    while (p < end && *p >= '0' && *p <= '9') {
        size_t n = 0;

        while (p < end && *p >= '0' && *p <= '9') {
            n = n * 10 + *p++ - '0';
        }
        if (end - p < 2 || p[0] != '\r' || p[1] != '\n') {
            return;
        }
        p += 2;
        if (!seen) {
            first = n;
            seen = true;
        }
    }
    if (seen && first <= (size_t)(end - p)) {
        *img = p;
        *len = first;
    }
}

int bf531_load_update(bf531 *s, const uint8_t *img, size_t len)
{
    size_t off = 0x20;

    first_section(&img, &len);
    if (len - off > FLASH_SIZE - FLASH_APP) {
        return -1;
    }
    memcpy(s->flash + FLASH_APP, img + off, len - off);
    while (off + 10 <= len) {
        uint32_t dest, count;
        uint16_t flags;
        uint8_t *to;

        memcpy(&dest, img + off, 4);
        memcpy(&count, img + off + 4, 4);
        memcpy(&flags, img + off + 8, 2);
        off += 10;
        if (!(flags & 1) && count > len - off) {
            return -1;
        }
        if (!(flags & 0x10)) {
            to = load_target(s, dest, count);
            if (!to) {
                slog(s, "LDR block to 0x%08x (0x%x bytes) has no target", dest, count);
                return -1;
            }
            if (flags & 1) {
                memset(to, 0, count);
            } else {
                memcpy(to, img + off, count);
            }
        }
        if (!(flags & 1)) {
            off += count;
        }
        if (flags & 0x8000) {
            bfin_reset(s->core, BOOT_ENTRY);
            return 0;
        }
    }
    return -1;
}

static uint64_t next_event(bf531 *s)
{
    uint64_t due = NEVER, core = bfin_timer_due(s->core);

    for (int n = 0; n < 3; n++) {
        due = s->timer[n].due < due ? s->timer[n].due : due;
    }
    for (int n = 0; n < 8; n++) {
        due = s->dma[n].due < due ? s->dma[n].due : due;
    }
    if (core != NEVER && bfin_cycles(s->core) + core < due) {
        due = bfin_cycles(s->core) + core;
    }
    return due;
}

static void advance(bf531 *s)
{
    uint64_t now = bfin_cycles(s->core);

    for (int n = 0; n < 3; n++) {
        while (s->timer[n].due <= now) {
            timer_fire(s, n);
        }
    }
    for (int n = 0; n < 8; n++) {
        if (s->dma[n].due <= now && n == 3) {
            sport1_rx_complete(s, n);
        } else if (s->dma[n].due <= now) {
            dma_unit_done(s, n);
        }
    }
}

bfin_stop bf531_run(bf531 *s, uint64_t n)
{
    uint64_t end = bfin_cycles(s->core) + n;

    while (bfin_cycles(s->core) < end) {
        uint64_t now = bfin_cycles(s->core), due = next_event(s);
        uint64_t stop_at = due < end ? due : end;
        bfin_stop stop;

        /* At least one cycle: an expired core timer is serviced by the step. */
        stop = bfin_step(s->core, stop_at > now ? stop_at - now : 1, NULL);
        if (stop == BFIN_STOP_UNDEF || stop == BFIN_STOP_BREAK) {
            return stop;
        }
        if (stop == BFIN_STOP_IDLE) {
            if (due == NEVER) {
                return stop;
            }
            if (stop_at > bfin_cycles(s->core)) {
                bfin_skip_cycles(s->core, stop_at - bfin_cycles(s->core));
            }
        }
        advance(s);
    }
    return BFIN_STOP_BUDGET;
}

bfin_core *bf531_core(bf531 *s)
{
    return s->core;
}

uint64_t bf531_frames(const bf531 *s)
{
    return s->frames;
}

const uint8_t *bf531_sdram(const bf531 *s, uint32_t *size)
{
    *size = s->sdram_size;
    return s->sdram;
}
