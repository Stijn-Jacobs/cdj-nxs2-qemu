/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * M16C/63 peripherals as the CDJ-900 GUI firmware drives them. The register
 * layout is the one the firmware's init code writes: interrupt control
 * registers at 0x40 + vector, DMA channels at 0x180 + 0x10 * n, UARTs at
 * 0x258, 0x272 and 0x2A8 (MR, BRG, TB, C0, C1, RB), timers from 0x320 and the
 * CRC unit at 0x3B4. Only the modes the firmware selects are modelled;
 * anything else is reported through the unmodelled hook.
 */
#include "m16c63.h"
#include <stdlib.h>
#include <string.h>

#define SFR_SIZE   0x400
#define RAM_BASE   0x400
#define RAM_SIZE   0x6C00
#define F1_PER_FC32 19531   /* f1 = 20 MHz, fC32 = 32.768 kHz / 32 */
#define NEVER      UINT64_MAX

#define IC_BASE    0x40
#define IC_IR      0x08
#define NUM_VEC    64

enum { UART_LINK, UART_B, UART_DISPLAY, NUM_UART };
enum { NUM_DMA = 4, NUM_TIMER = 8, NUM_PORT = 11 };

static const uint16_t uart_base[NUM_UART] = { 0x258, 0x272, 0x2A8 };

/* DMA request sources, by the value of the channel's DMxSL register. The
 * firmware uses three: the link UART's receive for both of its channels (the
 * transmit buffer is refilled by the same event), and the transmit of the
 * display UART and of the UART at 0x272. */
#define DSEL_LINK_RX     0x0F
#define DSEL_DISPLAY_TX  0x15
#define DSEL_UART_B_TX   0x46

static const uint16_t dma_sl_addr[NUM_DMA] = { 0x398, 0x39A, 0x390, 0x392 };
static const uint8_t dma_vec[NUM_DMA] = { 11, 12, 41, 42 };

static const uint16_t timer_reg[NUM_TIMER] = {
    0x326, 0x328, 0x32A, 0x32C, 0x32E, 0x330, 0x332, 0x334,
};
static const uint16_t timer_mode_reg[NUM_TIMER] = {
    0x336, 0x337, 0x338, 0x339, 0x33A, 0x33B, 0x33C, 0x33D,
};
static const uint8_t timer_vec[NUM_TIMER] = { 21, 22, 23, 24, 25, 26, 27, 28 };

#define TABSR  0x320
#define ONSF   0x322

static const uint16_t port_data[NUM_PORT] = {
    0x3E0, 0x3E1, 0x3E4, 0x3E5, 0x3E8, 0x3E9, 0x3EC, 0x3ED, 0x3F0, 0x3F1, 0x3F4,
};

#define CRC_SAR   0x3B4
#define CRC_MR    0x3B6
#define CRC_D     0x3BC
#define CRC_IN    0x3BE

typedef struct uart {
    uint8_t mr, brg, c0, c1;
    uint16_t tb, rb;
    int tb_full, shifting, ri;
    uint8_t shift_byte;
    uint64_t shift_end;
} uart;

typedef struct timer {
    int enabled, running;
    uint64_t next_fire, period;
} timer;

struct m16c63 {
    m16c_core *cpu;
    m16c63_ops ops;
    uint8_t ram[RAM_SIZE];
    uint8_t sfr[SFR_SIZE];
    uint8_t touched[SFR_SIZE];
    uint8_t ic[NUM_VEC];
    uart uart[NUM_UART];
    timer timer[NUM_TIMER];
    uint16_t dma_tcr_reload[NUM_DMA];
    uint16_t crc;
};

static void update_irq(m16c63 *s);
static void dma_request(m16c63 *s, unsigned dsel);

static uint64_t now(const m16c63 *s)
{
    return m16c_cycles(s->cpu);
}

static void raise_irq(m16c63 *s, unsigned vec)
{
    s->ic[vec] |= IC_IR;
    update_irq(s);
}

static void update_irq(m16c63 *s)
{
    int best = -1;
    unsigned best_level = 0;

    for (unsigned vec = 0; vec < NUM_VEC; vec++) {
        unsigned level = s->ic[vec] & 7;

        if ((s->ic[vec] & IC_IR) && level > best_level) {
            best = vec;
            best_level = level;
        }
    }
    m16c_set_irq(s->cpu, best, best_level);
}

static void bus_ack(void *opaque, unsigned vec)
{
    m16c63 *s = opaque;

    s->ic[vec] &= ~IC_IR;
    update_irq(s);
}

static uint32_t dma_reg(const m16c63 *s, unsigned ch, unsigned off, unsigned size)
{
    const uint8_t *p = &s->sfr[0x180 + 0x10 * ch + off];

    return size == 3 ? p[0] | p[1] << 8 | (p[2] & 0xF) << 16
                     : p[0] | p[1] << 8;
}

static void dma_set(m16c63 *s, unsigned ch, unsigned off, uint32_t v, unsigned size)
{
    uint8_t *p = &s->sfr[0x180 + 0x10 * ch + off];

    p[0] = v;
    p[1] = v >> 8;
    if (size == 3) {
        p[2] = v >> 16;
    }
}

/* A channel with a count of n transfers n + 1 units: the one that finds the
 * count at zero is the last, clears DMAE and requests the channel's
 * interrupt. */
static void dma_transfer(m16c63 *s, unsigned ch)
{
    uint8_t *con = &s->sfr[0x180 + 0x10 * ch + 0xC];
    unsigned unit = (*con & 1) ? 1 : 2;
    uint32_t src = dma_reg(s, ch, 0, 3), dst = dma_reg(s, ch, 4, 3);
    uint16_t tcr = dma_reg(s, ch, 8, 2);

    m16c_write(s->cpu, dst, m16c_read(s->cpu, src, unit), unit);
    if (*con & 0x10) {
        dma_set(s, ch, 0, src + unit, 3);
    }
    if (*con & 0x20) {
        dma_set(s, ch, 4, dst + unit, 3);
    }
    *con &= ~0x04;
    if (tcr) {
        dma_set(s, ch, 8, tcr - 1, 2);
    } else {
        *con &= ~0x08;
        raise_irq(s, dma_vec[ch]);
    }
}

/* A request latches in DMAS; a channel with DMAE set transfers at once. */
static void dma_request(m16c63 *s, unsigned dsel)
{
    for (unsigned ch = 0; ch < NUM_DMA; ch++) {
        uint8_t *con = &s->sfr[0x180 + 0x10 * ch + 0xC];

        if (s->sfr[dma_sl_addr[ch]] != dsel) {
            continue;
        }
        *con |= 0x04;
        if (*con & 0x08) {
            dma_transfer(s, ch);
        }
    }
}

static void crc_feed(m16c63 *s, uint8_t byte)
{
    int msb_first = s->sfr[CRC_MR] & 0x80;
    int crc16 = s->sfr[CRC_MR] & 1;
    uint16_t poly = msb_first ? (crc16 ? 0x8005 : 0x1021)
                              : (crc16 ? 0xA001 : 0x8408);

    if (msb_first) {
        s->crc ^= byte << 8;
        for (int bit = 0; bit < 8; bit++) {
            s->crc = s->crc & 0x8000 ? s->crc << 1 ^ poly : s->crc << 1;
        }
    } else {
        s->crc ^= byte;
        for (int bit = 0; bit < 8; bit++) {
            s->crc = s->crc & 1 ? s->crc >> 1 ^ poly : s->crc >> 1;
        }
    }
}

/* The CRC unit watches reads of the SFR named in CRCSAR (bit 14 enables
 * it, bit 15 selects writes instead of reads). */
static void crc_snoop(m16c63 *s, uint32_t addr, int write, uint8_t byte)
{
    uint16_t sar = s->sfr[CRC_SAR] | s->sfr[CRC_SAR + 1] << 8;

    if ((sar & 0x4000) && !!(sar & 0x8000) == write && (sar & 0x3FFF) == addr) {
        crc_feed(s, byte);
    }
}

/* Time for the UART to shift one byte out with its own clock. */
static uint64_t uart_byte_cycles(const uart *u)
{
    static const unsigned div[4] = { 1, 8, 32, F1_PER_FC32 };

    return 8ull * 2 * (u->brg + 1) * div[u->c0 & 3];
}

static void uart_start_shift(m16c63 *s, unsigned unit, uint64_t start)
{
    uart *u = &s->uart[unit];

    u->shift_byte = u->tb;
    u->tb_full = 0;
    u->shifting = 1;
    u->shift_end = start + uart_byte_cycles(u);
    if (unit == UART_DISPLAY) {
        dma_request(s, DSEL_DISPLAY_TX);
    } else if (unit == UART_B) {
        dma_request(s, DSEL_UART_B_TX);
    }
}

static void uart_service(m16c63 *s, unsigned unit)
{
    uart *u = &s->uart[unit];

    while (u->shifting && u->shift_end <= now(s)) {
        u->shifting = 0;
        if (s->ops.uart_tx) {
            s->ops.uart_tx(s->ops.opaque, unit, u->shift_byte);
        }
        if (u->tb_full && (u->c1 & 1)) {
            uart_start_shift(s, unit, u->shift_end);
        }
    }
}

uint8_t m16c63_link_clock(m16c63 *s, uint8_t in)
{
    uart *u = &s->uart[UART_LINK];
    uint8_t out = u->tb_full ? u->tb : 0xFF;

    if (!(u->c1 & 4)) {
        return out;
    }
    u->tb_full = 0;
    u->rb = in;
    u->ri = 1;
    dma_request(s, DSEL_LINK_RX);
    return out;
}

static uint64_t timer_ticks(const m16c63 *s, unsigned n)
{
    static const unsigned div[4] = { 1, 8, 32, F1_PER_FC32 };

    return div[s->sfr[timer_mode_reg[n]] >> 6];
}

static uint16_t timer_reload(const m16c63 *s, unsigned n)
{
    return s->sfr[timer_reg[n]] | s->sfr[timer_reg[n] + 1] << 8;
}

/* The A timers' MOD field: 0 timer, 2 one-shot; the B timers have timer
 * mode only. */
static unsigned timer_mod(const m16c63 *s, unsigned n)
{
    return s->sfr[timer_mode_reg[n]] & 3;
}

static void timer_start(m16c63 *s, unsigned n)
{
    timer *t = &s->timer[n];

    if (timer_mod(s, n) == 0) {
        t->period = (timer_reload(s, n) + 1ull) * timer_ticks(s, n);
        t->next_fire = now(s) + t->period;
        t->running = 1;
    }
}

static void timer_service(m16c63 *s, unsigned n)
{
    timer *t = &s->timer[n];

    while (t->running && t->next_fire <= now(s)) {
        raise_irq(s, timer_vec[n]);
        if (timer_mod(s, n) == 2) {
            t->running = 0;
        } else {
            t->period = (timer_reload(s, n) + 1ull) * timer_ticks(s, n);
            t->next_fire += t->period;
        }
    }
}

static uint16_t timer_value(const m16c63 *s, unsigned n)
{
    const timer *t = &s->timer[n];
    uint64_t ticks, elapsed;

    if (!t->running || timer_mod(s, n) != 0) {
        return timer_reload(s, n);
    }
    ticks = timer_ticks(s, n);
    elapsed = (now(s) - (t->next_fire - t->period)) / ticks;
    return timer_reload(s, n) - elapsed % (timer_reload(s, n) + 1ull);
}

static void timer_write_tabsr(m16c63 *s, uint8_t val)
{
    for (unsigned n = 0; n < NUM_TIMER; n++) {
        int on = val >> n & 1;
        timer *t = &s->timer[n];

        if (on && !t->enabled) {
            timer_start(s, n);
        } else if (!on) {
            t->running = 0;
        }
        t->enabled = on;
    }
}

/* ONSF: writing a TAiOS bit starts a one-shot timer that is enabled. */
static void timer_write_onsf(m16c63 *s, uint8_t val)
{
    for (unsigned n = 0; n < 5; n++) {
        timer *t = &s->timer[n];

        if ((val >> n & 1) && t->enabled && timer_mod(s, n) == 2) {
            t->period = timer_reload(s, n) * timer_ticks(s, n);
            t->next_fire = now(s) + t->period;
            t->running = 1;
        }
    }
}

static int modelled(uint32_t addr)
{
    if (addr < 0x10 || (addr >= IC_BASE && addr < IC_BASE + NUM_VEC) ||
        (addr >= 0x180 && addr < 0x1C0) || (addr >= 0x320 && addr < 0x340) ||
        (addr >= CRC_SAR && addr < 0x3C0) || (addr >= 0x3E0 && addr < 0x3F8)) {
        return 1;
    }
    for (unsigned u = 0; u < NUM_UART; u++) {
        if (addr >= uart_base[u] && addr < uart_base[u] + 8u) {
            return 1;
        }
    }
    for (unsigned ch = 0; ch < NUM_DMA; ch++) {
        if (addr == dma_sl_addr[ch]) {
            return 1;
        }
    }
    return 0;
}

static void note_access(m16c63 *s, uint32_t addr, int write, uint32_t val)
{
    if (addr < SFR_SIZE) {
        if (s->touched[addr] || modelled(addr)) {
            return;
        }
        s->touched[addr] = 1;
    }
    if (s->ops.unmodelled) {
        s->ops.unmodelled(s->ops.opaque, addr, write, val,
                          m16c_get_reg(s->cpu, M16C_PC));
    }
}

/* The port whose data register is at addr, or whose direction register is
 * (always the data register + 2); NUM_PORT for neither. */
static unsigned port_of(uint32_t addr, int *is_dir)
{
    for (unsigned p = 0; p < NUM_PORT; p++) {
        if (addr == port_data[p] || addr == port_data[p] + 2u) {
            *is_dir = addr != port_data[p];
            return p;
        }
    }
    return NUM_PORT;
}

static uint8_t sfr_read8(m16c63 *s, uint32_t addr)
{
    int is_dir = 0;
    unsigned p = port_of(addr, &is_dir);

    if (addr >= IC_BASE && addr < IC_BASE + NUM_VEC) {
        return s->ic[addr - IC_BASE];
    }
    for (unsigned n = 0; n < NUM_UART; n++) {
        uart *u = &s->uart[n];
        unsigned off = addr - uart_base[n];

        if (addr < uart_base[n] || off >= 8) {
            continue;
        }
        switch (off) {
        case 0: return u->mr;
        case 1: return u->brg;
        case 2: return u->tb;
        case 3: return u->tb >> 8;
        case 4: return (u->c0 & ~0x08) | (u->shifting ? 0 : 0x08);
        case 5: return (u->c1 & 5) | (u->tb_full ? 0 : 2) | (u->ri ? 8 : 0);
        case 6:
            u->ri = 0;
            crc_snoop(s, addr, 0, u->rb);
            return u->rb;
        default: return u->rb >> 8;
        }
    }
    for (unsigned n = 0; n < NUM_TIMER; n++) {
        if (addr == timer_reg[n]) {
            return timer_value(s, n);
        }
        if (addr == timer_reg[n] + 1u) {
            return timer_value(s, n) >> 8;
        }
    }
    if (addr == CRC_D || addr == CRC_D + 1) {
        return s->crc >> (addr == CRC_D ? 0 : 8);
    }
    if (p < NUM_PORT && !is_dir) {
        uint8_t dir = s->sfr[addr + 2];
        uint8_t in = s->ops.port_in ? s->ops.port_in(s->ops.opaque, p) : 0;

        return (s->sfr[addr] & dir) | (in & ~dir);
    }
    return s->sfr[addr];
}

/* Setting DMAE loads the transfer counter from the value last written to
 * it, so a channel armed again without rewriting its count (the display
 * channels) repeats the whole burst. */
static void dma_write_con(m16c63 *s, unsigned ch, uint8_t val)
{
    uint8_t *con = &s->sfr[0x180 + 0x10 * ch + 0xC];
    int enabling = (val & 0x08) && !(*con & 0x08);

    /* DMAS can only be cleared by software. */
    *con = (val & ~0x04) | (*con & val & 0x04);
    if (enabling) {
        dma_set(s, ch, 8, s->dma_tcr_reload[ch], 2);
    }
    if ((*con & 0x08) && (*con & 0x04)) {
        dma_transfer(s, ch);
    }
}

static void uart_write(m16c63 *s, unsigned n, unsigned off, uint8_t val)
{
    uart *u = &s->uart[n];

    switch (off) {
    case 0: u->mr = val; break;
    case 1: u->brg = val; break;
    case 2:
        u->tb = val;
        u->tb_full = 1;
        if ((u->c1 & 1) && !u->shifting && !(u->mr & 8)) {
            uart_start_shift(s, n, now(s));
        }
        break;
    case 3: u->tb = (u->tb & 0xFF) | val << 8; break;
    case 4: u->c0 = val; break;
    case 5: {
        int te_rising = (val & 1) && !(u->c1 & 1);

        u->c1 = val & 5;
        if (!(val & 1)) {
            u->shifting = 0;
            u->tb_full = 0;
        } else if (te_rising && u->tb_full && !u->shifting && !(u->mr & 8)) {
            uart_start_shift(s, n, now(s));
        }
        break;
    }
    }
}

static void sfr_write8(m16c63 *s, uint32_t addr, uint8_t val)
{
    int is_dir;
    unsigned port = port_of(addr, &is_dir);

    if (addr >= IC_BASE && addr < IC_BASE + NUM_VEC) {
        uint8_t *ic = &s->ic[addr - IC_BASE];

        *ic = (val & 0x37) | (*ic & val & IC_IR);
        update_irq(s);
        return;
    }
    for (unsigned n = 0; n < NUM_UART; n++) {
        if (addr >= uart_base[n] && addr < uart_base[n] + 8u) {
            uart_write(s, n, addr - uart_base[n], val);
            return;
        }
    }
    for (unsigned ch = 0; ch < NUM_DMA; ch++) {
        if (addr == 0x180 + 0x10 * ch + 0xC) {
            dma_write_con(s, ch, val);
            return;
        }
    }
    s->sfr[addr] = val;
    for (unsigned ch = 0; ch < NUM_DMA; ch++) {
        uint32_t tcr = 0x180 + 0x10 * ch + 8;

        if (addr == tcr || addr == tcr + 1) {
            s->dma_tcr_reload[ch] = s->sfr[tcr] | s->sfr[tcr + 1] << 8;
        }
    }
    if (addr == TABSR) {
        timer_write_tabsr(s, val);
    } else if (addr == ONSF) {
        timer_write_onsf(s, val);
        /* The start flags read back as 0, so a read-modify-write of another
         * timer's flag does not start this one again. */
        s->sfr[ONSF] &= ~0x1F;
    } else if (addr == CRC_D || addr == CRC_D + 1) {
        s->crc = addr == CRC_D ? (s->crc & 0xFF00) | val
                               : (s->crc & 0x00FF) | val << 8;
    } else if (addr == CRC_IN) {
        crc_feed(s, val);
    } else if (port < NUM_PORT && s->ops.port_out) {
        s->ops.port_out(s->ops.opaque, port, s->sfr[port_data[port]],
                        s->sfr[port_data[port] + 2]);
    }
}

static uint32_t bus_read(void *opaque, uint32_t addr, unsigned size)
{
    m16c63 *s = opaque;
    uint32_t val = 0;

    if (addr < SFR_SIZE) {
        val = sfr_read8(s, addr);
        if (size == 2) {
            val |= sfr_read8(s, addr + 1) << 8;
        }
    }
    note_access(s, addr, 0, val);
    return val;
}

static void bus_write(void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
    m16c63 *s = opaque;

    note_access(s, addr, 1, val);
    if (addr < SFR_SIZE) {
        sfr_write8(s, addr, val);
        if (size == 2) {
            sfr_write8(s, addr + 1, val >> 8);
        }
    }
}

m16c63 *m16c63_new(const uint8_t *flash, uint32_t base, uint32_t size,
                   const m16c63_ops *ops)
{
    m16c63 *s = calloc(1, sizeof(*s));
    m16c_bus bus = { s, bus_read, bus_write, bus_ack };

    s->ops = *ops;
    s->cpu = m16c_new(&bus);
    m16c_map_rom(s->cpu, base, (size + 1023) & ~1023u, flash);
    m16c_map_ram(s->cpu, RAM_BASE, RAM_SIZE, s->ram);
    m16c63_reset(s);
    return s;
}

void m16c63_free(m16c63 *s)
{
    m16c_free(s->cpu);
    free(s);
}

m16c_core *m16c63_cpu(m16c63 *s)
{
    return s->cpu;
}

void m16c63_reset(m16c63 *s)
{
    memset(s->sfr, 0, sizeof(s->sfr));
    memset(s->touched, 0, sizeof(s->touched));
    memset(s->ic, 0, sizeof(s->ic));
    memset(s->uart, 0, sizeof(s->uart));
    memset(s->timer, 0, sizeof(s->timer));
    memset(s->dma_tcr_reload, 0, sizeof(s->dma_tcr_reload));
    s->crc = 0;
    m16c_reset(s->cpu);
}

static uint64_t next_event(const m16c63 *s)
{
    uint64_t due = NEVER;

    for (unsigned n = 0; n < NUM_UART; n++) {
        if (s->uart[n].shifting && s->uart[n].shift_end < due) {
            due = s->uart[n].shift_end;
        }
    }
    for (unsigned n = 0; n < NUM_TIMER; n++) {
        if (s->timer[n].running && s->timer[n].next_fire < due) {
            due = s->timer[n].next_fire;
        }
    }
    return due;
}

m16c_stop m16c63_run(m16c63 *s, uint64_t cycles)
{
    uint64_t end = now(s) + cycles;

    while (now(s) < end) {
        uint64_t due = next_event(s);
        m16c_stop why;

        if (due <= now(s)) {
            for (unsigned n = 0; n < NUM_UART; n++) {
                uart_service(s, n);
            }
            for (unsigned n = 0; n < NUM_TIMER; n++) {
                timer_service(s, n);
            }
            continue;
        }
        if (due > end) {
            due = end;
        }
        m16c_set_deadline(s->cpu, due);
        why = m16c_step(s->cpu, UINT64_MAX, NULL);
        if (why == M16C_STOP_WAIT) {
            m16c_skip_cycles(s->cpu, due - now(s));
        } else if (why != M16C_STOP_BUDGET) {
            return why;
        }
    }
    return M16C_STOP_BUDGET;
}
