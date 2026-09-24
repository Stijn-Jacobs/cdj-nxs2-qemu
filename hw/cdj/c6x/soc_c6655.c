/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * C6655 SoC: address decode, signal routing, virtual time, GPIO and the
 * RAM-like configuration blocks.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "soc_internal.h"

/* ---- signal routing, SPRS814 tables 7-32 (EDMA3), 7-33 (core), 7-34 (CIC0) -- */
typedef struct sig_route {
    int16_t core, cic0, edma;
} sig_route;

static sig_route routes[SIG_COUNT];

static void routes_init(void)
{
    static int done;
    if (done)
        return;
    for (int i = 0; i < SIG_COUNT; i++)
        routes[i] = (sig_route){ -1, -1, -1 };

    /* Timers. Timer0 is core 0's local timer; C6655 has no Timer1. */
    static const sig_route tl[8] = {
        { 64, -1, -1 }, { -1, -1, -1 }, { 66, -1, 2 },  { 68, -1, 34 },
        { -1, 145, 22 }, { -1, 151, 24 }, { -1, 153, 26 }, { -1, 162, 28 },
    };
    for (int n = 0; n < 8; n++) {
        routes[SIG_TINT_L0 + 2 * n] = tl[n];
        sig_route h = tl[n];
        if (h.core >= 0) h.core++;
        if (h.cic0 >= 0) h.cic0++;
        if (h.edma >= 0) h.edma++;
        routes[SIG_TINT_L0 + 2 * n + 1] = h;
    }
    routes[SIG_UPPINT]      = (sig_route){ -1, 156, -1 };
    routes[SIG_SPIINT0]     = (sig_route){ -1, 54, 16 };
    routes[SIG_SPIINT1]     = (sig_route){ -1, 55, 17 };
    routes[SIG_SPIXEVT]     = (sig_route){ -1, 56, 30 };
    routes[SIG_SPIREVT]     = (sig_route){ -1, 57, 31 };
    for (int p = 0; p < 2; p++) {
        routes[SIG_MCBSP_RINT0 + 4 * p] = (sig_route){ -1, 32 + 4 * p, -1 };
        routes[SIG_MCBSP_XINT0 + 4 * p] = (sig_route){ -1, 33 + 4 * p, -1 };
        routes[SIG_MCBSP_REVT0 + 4 * p] = (sig_route){ -1, 34 + 4 * p, 36 + 2 * p };
        routes[SIG_MCBSP_XEVT0 + 4 * p] = (sig_route){ -1, 35 + 4 * p, 37 + 2 * p };
    }
    routes[SIG_EDMA_ERRINT] = (sig_route){ -1, 16, -1 };
    routes[SIG_EDMA_GINT]   = (sig_route){ -1, 22, -1 };
    for (int r = 0; r < 8; r++)
        routes[SIG_EDMA_INT0 + r] = (sig_route){ -1, 24 + r, -1 };
    for (int p = 0; p < 32; p++) {
        sig_route g = { -1, -1, -1 };
        if (p <= 3)
            g.edma = 6 + p;
        if (p == 2 || p == 3)
            g.core = 72 + (p - 2);
        else if (p >= 4 && p <= 15)
            g.core = 78 + (p - 4);
        else if (p >= 16)
            g.cic0 = p - 16;
        routes[SIG_GPINT0 + p] = g;
    }
    done = 1;
}

void soc_signal(c6655_soc *s, soc_sig sig, int level)
{
    level = !!level;
    if (s->sig_level[sig] == level)
        return;
    s->sig_level[sig] = level;
    const sig_route *r = &routes[sig];
    if (r->core >= 0)
        intc_event(s, r->core, level);
    if (r->cic0 >= 0)
        cic_input(s, r->cic0, level);
    if (r->edma >= 0)
        edma_event(s, r->edma, level);
}

void soc_pulse(c6655_soc *s, soc_sig sig)
{
    soc_signal(s, sig, 1);
    soc_signal(s, sig, 0);
}

/* ---- diagnostics ----------------------------------------------------------- */
void soc_log(c6655_soc *s, const char *fmt, ...)
{
    if (!s->cfg.log)
        return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    s->cfg.log(s->cfg.opaque, buf);
}

void soc_log_unimp(c6655_soc *s, const char *block, uint32_t addr, int is_write, uint32_t val)
{
    uint32_t key = addr ^ (is_write ? 0x80000000u : 0);
    for (unsigned i = 0; i < s->nlogged; i++)
        if (s->logged[i] == key)
            return;
    if (s->nlogged < sizeof s->logged / sizeof s->logged[0])
        s->logged[s->nlogged++] = key;
    if (is_write)
        soc_log(s, "soc: unmodelled %s write 0x%08x <- 0x%08x", block, addr, val);
    else
        soc_log(s, "soc: unmodelled %s read 0x%08x", block, addr);
}

/* ---- RAM-like configuration blocks ----------------------------------------- */
static const struct { uint32_t base, size; const char *name; } ram_blocks[] = {
    { 0x01810000, 0x00040000, "CorePac cfg (PDC/MPPA/cache)" },
    { 0x02310000, 0x00000200, "PLL ctl" },
    { 0x02350000, 0x00001000, "PSC" },
    { 0x02620000, 0x00001000, "bootcfg" },
    { 0x02640000, 0x00000800, "semaphore" },
    { 0x08000000, 0x00010000, "XMC" },
    { 0x21000000, 0x00001000, "DDR3 ctl" },
};

static soc_ram_region *ram_find(c6655_soc *s, uint32_t addr)
{
    for (unsigned i = 0; i < s->nram; i++)
        if (addr - s->ram[i].base < s->ram[i].size)
            return &s->ram[i];
    return NULL;
}

static uint32_t ram_get32(const soc_ram_region *r, uint32_t off)
{
    const uint8_t *m = r->mem + off;
    return m[0] | m[1] << 8 | m[2] << 16 | (uint32_t)m[3] << 24;
}

static void ram_put32(soc_ram_region *r, uint32_t off, uint32_t v)
{
    uint8_t *m = r->mem + off;
    m[0] = v; m[1] = v >> 8; m[2] = v >> 16; m[3] = v >> 24;
}

static uint32_t ram_read(soc_ram_region *r, uint32_t addr, unsigned size)
{
    uint32_t off = (addr - r->base) & ~3u;
    uint32_t v = ram_get32(r, off);
    /* PLL: the GO operation completes at once (0x0231013C bit 0 reads 0). */
    if (r->base == 0x02310000 && off == 0x13C)
        v &= ~1u;
    /* PSC: no power transition is ever in progress; MDSTAT mirrors MDCTL. */
    if (r->base == 0x02350000) {
        if (off == 0x128)
            v = 0;
        else if (off >= 0x800 && off < 0x900)
            v = (v & ~0x1Fu) | (ram_get32(r, off + 0x200) & 0x1F);
    }
    return reg_get(v, addr - r->base, size);
}

static void ram_write(soc_ram_region *r, uint32_t addr, uint32_t val, unsigned size)
{
    uint32_t off = (addr - r->base) & ~3u;
    ram_put32(r, off, reg_put(ram_get32(r, off), addr - r->base, val, size));
}

static void ram_reset(c6655_soc *s)
{
    for (unsigned i = 0; i < s->nram; i++)
        memset(s->ram[i].mem, 0, s->ram[i].size);
    soc_ram_region *boot = ram_find(s, 0x02620000);
    /* DEVSTAT: little-endian, BOOTMODE = I2C (5). The slave-address and passive
     * bits are not set: nothing in stage 1 or the app reads them. Unverified. */
    ram_put32(boot, 0x20, 0x0000000B);
}

/* ---- GPIO @ 0x02320000 (KeyStone GPIO; stage 1 0x008017F4..0x00801918) ----- */
static uint32_t gpio_pins(const soc_gpio *g)
{
    return (g->out & ~g->dir) | (g->ext & g->dir);
}

static void gpio_update(c6655_soc *s, uint32_t old_driven, uint32_t old_dir)
{
    soc_gpio *g = &s->gpio;
    uint32_t driven = g->out & ~g->dir;
    uint32_t outputs = ~g->dir;
    if (s->cfg.gpio_out) {
        uint32_t changed = (driven ^ old_driven) | (outputs & old_dir);
        for (unsigned p = 0; p < 32; p++)
            if (((changed >> p) & 1) && ((outputs >> p) & 1))
                s->cfg.gpio_out(s->cfg.opaque, p, (driven >> p) & 1);
    }
    uint32_t pins = gpio_pins(g);
    uint32_t rise = pins & ~g->last_pins & g->ris;
    uint32_t fall = ~pins & g->last_pins & g->fal;
    g->last_pins = pins;
    if (g->binten & 1)
        for (unsigned p = 0; p < 32; p++)
            if (((rise | fall) >> p) & 1)
                soc_pulse(s, SIG_GPINT0 + p);
}

static uint32_t gpio_read(c6655_soc *s, uint32_t off, unsigned size)
{
    soc_gpio *g = &s->gpio;
    uint32_t v;
    switch (off & ~3u) {
    case 0x00: v = 0x44830105; break;    /* PID; value unverified */
    case 0x04: v = g->pcr; break;
    case 0x08: v = g->binten; break;
    case 0x10: v = g->dir; break;
    case 0x14: case 0x18: case 0x1C: v = g->out; break;
    case 0x20: v = gpio_pins(g); break;
    case 0x24: case 0x28: v = g->ris; break;
    case 0x2C: case 0x30: v = g->fal; break;
    default:
        soc_log_unimp(s, "GPIO", 0x02320000 + off, 0, 0);
        v = 0;
    }
    return reg_get(v, off, size);
}

static void gpio_write(c6655_soc *s, uint32_t off, uint32_t val, unsigned size)
{
    soc_gpio *g = &s->gpio;
    uint32_t old_driven = g->out & ~g->dir, old_dir = g->dir;
    uint32_t v = reg_put(0, off, val, size);
    switch (off & ~3u) {
    case 0x04: g->pcr = reg_put(g->pcr, off, val, size); break;
    case 0x08: g->binten = reg_put(g->binten, off, val, size); break;
    case 0x10: g->dir = reg_put(g->dir, off, val, size); break;
    case 0x14: g->out = reg_put(g->out, off, val, size); break;
    case 0x18:
        g->out |= v;
        if (s->cfg.gpio_setclr) s->cfg.gpio_setclr(s->cfg.opaque, v, 1);
        break;
    case 0x1C:
        g->out &= ~v;
        if (s->cfg.gpio_setclr) s->cfg.gpio_setclr(s->cfg.opaque, v, 0);
        break;
    case 0x24: g->ris |= v; break;
    case 0x28: g->ris &= ~v; break;
    case 0x2C: g->fal |= v; break;
    case 0x30: g->fal &= ~v; break;
    default:
        soc_log_unimp(s, "GPIO", 0x02320000 + off, 1, val);
        return;
    }
    gpio_update(s, old_driven, old_dir);
}

void c6655_soc_gpio_set_input(c6655_soc *s, unsigned pin, int level)
{
    uint32_t old_driven = s->gpio.out & ~s->gpio.dir, old_dir = s->gpio.dir;
    if (level)
        s->gpio.ext |= 1u << pin;
    else
        s->gpio.ext &= ~(1u << pin);
    gpio_update(s, old_driven, old_dir);
}

int c6655_soc_gpio_pin_level(const c6655_soc *s, unsigned pin)
{
    return (gpio_pins(&s->gpio) >> pin) & 1;
}

/* ---- address decode ------------------------------------------------------- */
enum blk { B_NONE, B_INTC, B_CIC0, B_TIMER, B_UPP, B_SPI, B_MCBSP0, B_MCBSP1,
           B_EDMA, B_GPIO, B_RAM };

static enum blk decode(uint32_t addr)
{
    if (addr - 0x01800000u < 0x10000) return B_INTC;
    if (addr - 0x02600000u < 0x2000)  return B_CIC0;
    if (addr - 0x02200000u < 0x80000 && ((addr & 0xFFFF) < 0x80)) return B_TIMER;
    if (addr - 0x02580000u < 0x1000)  return B_UPP;
    if (addr - 0x20BF0000u < 0x200)   return B_SPI;
    if (addr - 0x021B4000u < 0x2800)  return B_MCBSP0;
    if (addr - 0x021B8000u < 0x2800)  return B_MCBSP1;
    if (addr - 0x02740000u < EDMA_PARAM_END) return B_EDMA;
    if (addr - 0x02320000u < 0x100)   return B_GPIO;
    for (size_t i = 0; i < sizeof ram_blocks / sizeof ram_blocks[0]; i++)
        if (addr - ram_blocks[i].base < ram_blocks[i].size)
            return B_RAM;
    return B_NONE;
}

int c6655_soc_owns(uint32_t addr)
{
    return decode(addr) != B_NONE;
}

static void mcbsp_catch_up(c6655_soc *s, uint64_t t);

/* Deferred McBSP words are clocked out before the CPU looks at, or changes,
 * anything they touch. */
static void sync_for(c6655_soc *s, enum blk b, int is_write)
{
    if (s->in_catchup)
        return;
    if (b == B_MCBSP0 || b == B_MCBSP1 || b == B_EDMA || (is_write && b == B_CIC0))
        mcbsp_catch_up(s, s->now_ns);
}

uint32_t c6655_soc_read(c6655_soc *s, uint32_t addr, unsigned size)
{
    enum blk b = decode(addr);
    sync_for(s, b, 0);
    switch (b) {
    case B_INTC:   return intc_read(s, addr - 0x01800000, size);
    case B_CIC0:   return cic_read(s, addr - 0x02600000, size);
    case B_TIMER:  return timer_read(s, &s->timer[(addr >> 16) & 0xF], addr & 0xFFFF, size);
    case B_UPP:    return upp_read(s, addr - 0x02580000, size);
    case B_SPI:    return spi_read(s, addr - 0x20BF0000, size);
    case B_MCBSP0: return mcbsp_read(s, &s->mcbsp[0], addr - 0x021B4000, size);
    case B_MCBSP1: return mcbsp_read(s, &s->mcbsp[1], addr - 0x021B8000, size);
    case B_EDMA:   return edma_read(s, addr - 0x02740000, size);
    case B_GPIO:   return gpio_read(s, addr - 0x02320000, size);
    case B_RAM:    return ram_read(ram_find(s, addr), addr, size);
    default:
        soc_log_unimp(s, "SoC", addr, 0, 0);
        return 0;
    }
}

void c6655_soc_write(c6655_soc *s, uint32_t addr, uint32_t val, unsigned size)
{
    enum blk b = decode(addr);
    sync_for(s, b, 1);
    s->cache_valid = 0;
    switch (b) {
    case B_INTC:   intc_write(s, addr - 0x01800000, val, size); break;
    case B_CIC0:   cic_write(s, addr - 0x02600000, val, size); break;
    case B_TIMER:  timer_write(s, &s->timer[(addr >> 16) & 0xF], addr & 0xFFFF, val, size); break;
    case B_UPP:    upp_write(s, addr - 0x02580000, val, size); break;
    case B_SPI:    spi_write(s, addr - 0x20BF0000, val, size); break;
    case B_MCBSP0: mcbsp_write(s, &s->mcbsp[0], addr - 0x021B4000, val, size); break;
    case B_MCBSP1: mcbsp_write(s, &s->mcbsp[1], addr - 0x021B8000, val, size); break;
    case B_EDMA:   edma_write(s, addr - 0x02740000, val, size); break;
    case B_GPIO:   gpio_write(s, addr - 0x02320000, val, size); break;
    case B_RAM:    ram_write(ram_find(s, addr), addr, val, size); break;
    default:
        soc_log_unimp(s, "SoC", addr, 1, val);
    }
}

static uint32_t bus_read(void *opaque, uint32_t addr, unsigned size)
{
    return c6655_soc_read(opaque, addr, size);
}

static void bus_write(void *opaque, uint32_t addr, uint32_t val, unsigned size)
{
    c6655_soc_write(opaque, addr, val, size);
}

c66x_bus c6655_soc_bus(c6655_soc *s)
{
    return (c66x_bus){ s, bus_read, bus_write };
}

/* CorePac 0's global address of L2 (0x1080_0000) is what the program hands to
 * DMA masters (UPID0 0x108C8800, EDMA3 SRC/DST 0x1088xxxx). */
static uint32_t dma_fold(uint32_t addr)
{
    if (addr - 0x10800000u < 0x00100000)
        return addr - 0x10000000;
    return addr;
}

uint32_t soc_dma_read(c6655_soc *s, uint32_t addr, unsigned size)
{
    addr = dma_fold(addr);
    if (c6655_soc_owns(addr))
        return c6655_soc_read(s, addr, size);
    return s->cfg.mem.read ? s->cfg.mem.read(s->cfg.mem.opaque, addr, size) : 0;
}

void soc_dma_write(c6655_soc *s, uint32_t addr, uint32_t val, unsigned size)
{
    addr = dma_fold(addr);
    if (c6655_soc_owns(addr))
        c6655_soc_write(s, addr, val, size);
    else if (s->cfg.mem.write)
        s->cfg.mem.write(s->cfg.mem.opaque, addr, val, size);
}

/* ---- virtual time ---------------------------------------------------------- */
uint64_t c6655_soc_now_ns(const c6655_soc *s)
{
    return s->now_ns;
}

/*
 * McBSP words are not scheduled one by one. A word only changes what the CPU
 * can observe when it can interrupt (XINT/XEVT enabled into CIC0, or the EDMA3
 * transfer it triggers completes with an enabled TCC); the words before that
 * are clocked out in a tight loop when time passes them; per-word events at
 * 88.2 kHz stereo would cut every core step to ~137 cycles.
 */
static uint64_t mcbsp_next_visible_ns(const c6655_soc *s, unsigned p)
{
    const soc_mcbsp *m = &s->mcbsp[p];
    if (m->next_word_ns == UINT64_MAX)
        return UINT64_MAX;
    unsigned xint = 33 + 4 * p, xevt = 35 + 4 * p;
    uint64_t k;
    static int perword = -1;             /* C6655_MCBSP_PERWORD=1: the reference model */
    if (perword < 0)
        perword = getenv("C6655_MCBSP_PERWORD") != NULL;
    if (perword)
        return m->next_word_ns;
    if (((s->cic0.enable[xint / 32] >> (xint % 32)) & 1) ||
        ((s->cic0.enable[xevt / 32] >> (xevt % 32)) & 1))
        k = 1;
    else
        k = edma_events_until_irq(s, 37 + 2 * p, MCBSP_BATCH_WORDS);
    /* nothing can interrupt: still come back every MCBSP_BATCH_WORDS so the
     * sink hears the port at a bounded latency */
    return mcbsp_word_ns(s, m, k ? k : MCBSP_BATCH_WORDS);
}

/* Clock out every word due at or before t, in time order across both ports. */
static void mcbsp_catch_up(c6655_soc *s, uint64_t t)
{
    if (s->in_catchup)
        return;
    s->in_catchup = 1;
    s->cache_valid = 0;
    s->stat[5]++;
    for (;;) {
        soc_mcbsp *a = &s->mcbsp[0], *b = &s->mcbsp[1];
        soc_mcbsp *m = a->next_word_ns <= b->next_word_ns ? a : b;
        if (m->next_word_ns > t)
            break;
        uint64_t save = s->now_ns;
        if (m->next_word_ns > s->now_ns)
            s->now_ns = m->next_word_ns;
        mcbsp_fire(s, m);
        s->stat[4]++;
        if (s->now_ns < save)
            s->now_ns = save;
    }
    s->in_catchup = 0;
}

uint64_t c6655_soc_next_event_ns(const c6655_soc *s)
{
    if (s->cache_valid)
        return s->next_cache;
    ((c6655_soc *)s)->stat[3]++;
    uint64_t t = s->spi.done_ns;
    for (int i = 0; i < 8; i++) {
        uint64_t tt = timer_next_event(&s->timer[i]);
        if (tt < t)
            t = tt;
    }
    for (unsigned p = 0; p < 2; p++) {
        uint64_t tt = mcbsp_next_visible_ns(s, p);
        if (tt < t)
            t = tt;
    }
    ((c6655_soc *)s)->next_cache = t;
    ((c6655_soc *)s)->cache_valid = 1;
    return t;
}

/* Every event left in the schedule can raise an interrupt except the bounded
 * McBSP batch point, so the two coincide; kept as its own call for callers
 * that were written against it. */
uint64_t c6655_soc_next_irq_ns(const c6655_soc *s)
{
    return c6655_soc_next_event_ns(s);
}

void c6655_soc_advance(c6655_soc *s, uint64_t ns)
{
    uint64_t target = s->now_ns + ns;
    s->stat[0]++;
    /* The common case at a small core quantum: nothing is due. Words in
     * between stay deferred until a visible event or a register access. */
    if (s->cache_valid && target < s->next_cache) {
        s->stat[1]++;
        s->now_ns = target;
        return;
    }
    for (;;) {
        uint64_t t = c6655_soc_next_event_ns(s);
        if (t > target)
            break;
        s->stat[2]++;
        mcbsp_catch_up(s, t);
        if (t > s->now_ns)
            s->now_ns = t;
        for (int i = 0; i < 8; i++)
            if (timer_next_event(&s->timer[i]) <= s->now_ns)
                timer_fire(s, &s->timer[i]);
        if (s->spi.done_ns <= s->now_ns)
            spi_fire(s);
        s->cache_valid = 0;
    }
    s->now_ns = target;
}

void c6655_soc_debug_stats(const c6655_soc *s, uint64_t out[6])
{
    for (int i = 0; i < 6; i++)
        out[i] = s->stat[i];
}

/* ---- lifecycle ------------------------------------------------------------- */
c6655_soc *c6655_soc_new(const c6655_soc_config *cfg)
{
    routes_init();
    c6655_soc *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    s->cfg = *cfg;
    if (!s->cfg.mcbsp_clks_hz)
        s->cfg.mcbsp_clks_hz = 22579200;
    for (size_t i = 0; i < sizeof ram_blocks / sizeof ram_blocks[0]; i++) {
        soc_ram_region *r = &s->ram[s->nram++];
        r->base = ram_blocks[i].base;
        r->size = ram_blocks[i].size;
        r->name = ram_blocks[i].name;
        r->mem = calloc(1, r->size);
    }
    s->gpio.ext = 0xFFFFFFFF;             /* undriven inputs read high */
    c6655_soc_reset(s);
    return s;
}

void c6655_soc_free(c6655_soc *s)
{
    if (!s)
        return;
    upp_free(&s->upp);
    for (unsigned i = 0; i < s->nram; i++)
        free(s->ram[i].mem);
    free(s);
}

void c6655_soc_reset(c6655_soc *s)
{
    s->cache_valid = 0;
    memset(s->sig_level, 0, sizeof s->sig_level);
    intc_reset(&s->intc);
    cic_reset(&s->cic0);
    for (unsigned i = 0; i < 8; i++)
        timer_reset(s, &s->timer[i], i);
    upp_reset(s, &s->upp);
    spi_reset(s, &s->spi);
    mcbsp_reset(s, &s->mcbsp[0], 0);
    mcbsp_reset(s, &s->mcbsp[1], 1);
    edma_reset(&s->edma);
    /* External levels belong to the board, not to the DSP, so they survive. */
    uint32_t ext = s->gpio.ext;
    s->gpio = (soc_gpio){ .dir = 0xFFFFFFFF, .ext = ext, .last_pins = ext };
    ram_reset(s);
}
