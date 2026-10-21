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
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
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
    WDOG_CTL = 0x200, WDOG_CNT = 0x204, WDOG_STAT = 0x208,
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
#define DI_SEL      0x0100
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
    /* Host-side flash config and bookkeeping: not in the snapshot. */
    bool     fl_mapped;             /* the core reads the array directly */
    bool     code_range_on;
    uint32_t code_lo, code_hi;
    /* Host-only diagnostics, excluded from every snapshot run. */
    bf531_ppi_injection ppi_injection;
    bool ppi_injection_configured, ppi_started;
    bf531_watchdog_timing ownership_watchdog[2];
    uint64_t ownership_overflow, ownership_arm;
    struct {
        bool active, expired, sampled;
        unsigned phase;
        uint64_t id, scheduled, observed;
    } ownership_pending;

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
    uint64_t wdog_stamp;
    uint32_t wdog_left, wdog_fraction;
    bool wdog_unsupported;
    bf531_dma dma[8];
    uint16_t *fb;
    uint64_t frames;

    /* One standing SPORT1 RX packet from the host, consumed the next time
     * the firmware arms DMA3 (see bf531_sport1_rx). MAIN's packets are at
     * most its 2 KB transmit buffer. */
    uint8_t  sport1_rx[2048];
    size_t   sport1_rx_len;
    bool     sport1_rx_armed;
    /* Packets that land while one is standing wait here in order
     * (a free-running link sends without waiting for an answer). Lives after
     * sport1_rx, so the snapshot's tail run carries it. */
    uint8_t  sport1_q[16][2048];
    uint16_t sport1_q_len[16];
    unsigned sport1_q_head, sport1_q_count;
    uint64_t sport1_q_dropped;
    uint64_t sport1_rx_waits;   /* arms too small for the standing record */
    /* The flash's command state (AMD command set). After sport1_rx, so the
     * snapshot's tail run carries it. */
    uint8_t  fl_mode, fl_cyc, fl_op, fl_toggle;
    uint16_t fl_data;
    uint16_t fl_pad;
    uint32_t fl_reads;
    uint64_t fl_until;
    uint32_t fl_words, fl_sectors;
};

static void bus_write(void *opaque, uint32_t addr, uint32_t val, unsigned size);

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

    bfin_set_wake(s->core, !!(s->sic_isr & s->mmr[SIC_IWR / 4]));

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

/* ---- opt-in host ownership observations -------------------------------- */
static void ownership_add(bf531 *s, uint64_t *field, uint64_t n)
{
    if (UINT64_MAX - *field < n) {
        *field = UINT64_MAX;
        if (s->ownership_overflow != UINT64_MAX) { s->ownership_overflow++; }
    } else { *field += n; }
}
static uint64_t wdog_due(bf531 *s);
static void ownership_carry_at(bf531 *s, uint64_t deadline)
{
    if (s->ownership_pending.active && !s->ownership_pending.expired &&
        s->ownership_pending.phase == 0 && bfin_timing_phase(s->core) == 1) {
        bfin_ownership_timing t;
        bfin_timing_read(s->core, &t);
        /* Attribute a late observation by its actual deadline, not the read. */
        if (deadline < t.start_cycle) { return; }
        s->ownership_pending.phase = 1;
        ownership_add(s, &s->ownership_watchdog[1].carried_arms, 1);
    }
}
static void ownership_carry(bf531 *s)
{
    ownership_carry_at(s, wdog_due(s));
}
static void ownership_finish(bf531 *s)
{
    if (!s->ownership_pending.active) { return; }
    ownership_carry(s);
    bf531_watchdog_timing *w = &s->ownership_watchdog[s->ownership_pending.phase];
    if (!s->ownership_pending.expired) { ownership_add(s, &w->cancelled_before_expiry, 1); }
    else if (!s->ownership_pending.sampled) { ownership_add(s, &w->expired_without_sample, 1); }
    s->ownership_pending.active = false;
}
static void ownership_arm(bf531 *s)
{
    int phase = bfin_timing_phase(s->core);
    if (phase < 0) { return; }
    ownership_finish(s);
    if (s->ownership_arm == UINT64_MAX) {
        if (s->ownership_overflow != UINT64_MAX) { s->ownership_overflow++; }
        return;
    }
    s->ownership_pending = (typeof(s->ownership_pending)){
        .active = true, .phase = phase, .id = ++s->ownership_arm,
    };
    ownership_add(s, &s->ownership_watchdog[phase].arms, 1);
}
static void ownership_expire(bf531 *s, uint64_t scheduled)
{
    if (!s->ownership_pending.active || s->ownership_pending.expired) { return; }
    ownership_carry_at(s, scheduled);
    bf531_watchdog_timing *w = &s->ownership_watchdog[s->ownership_pending.phase];
    uint64_t now = bfin_cycles(s->core);
    s->ownership_pending.expired = true;
    s->ownership_pending.scheduled = scheduled;
    s->ownership_pending.observed = now;
    ownership_add(s, &w->expiries, 1);
    uint64_t delay = now - scheduled;
    if (delay > w->max_observation_delay_cycles) { w->max_observation_delay_cycles = delay; }
}
static void ownership_stat(bf531 *s)
{
    if (!s->ownership_pending.active || !s->ownership_pending.expired || s->ownership_pending.sampled) { return; }
    bf531_watchdog_timing *w = &s->ownership_watchdog[s->ownership_pending.phase];
    bf531_timing_sample sample = { s->ownership_pending.id,
        s->ownership_pending.scheduled, s->ownership_pending.observed, bfin_cycles(s->core) };
    uint64_t delay = sample.stat_cycle - sample.scheduled_cycle;
    s->ownership_pending.sampled = true;
    ownership_add(s, &w->stat_samples, 1);
    ownership_add(s, &w->total_expiry_to_stat_cycles, delay);
    w->last_sample = sample;
    if (!w->worst_sample.arm || delay > w->max_expiry_to_stat_cycles) {
        w->worst_sample = sample; w->max_expiry_to_stat_cycles = delay;
    }
}
int bf531_timing_enable(bf531 *s, int configured, uint32_t pc)
{
    return bfin_timing_enable(s->core, configured, pc);
}
void bf531_timing_read(bf531 *s, bf531_ownership_timing *out)
{
    ownership_carry(s);
    bfin_timing_read(s->core, &out->core);
    memcpy(out->watchdog, s->ownership_watchdog, sizeof(out->watchdog));
    out->overflow = s->ownership_overflow;
    if (UINT64_MAX - out->overflow < out->core.overflow) { out->overflow = UINT64_MAX; }
    else { out->overflow += out->core.overflow; }
    if (s->ownership_pending.active && s->ownership_pending.expired && !s->ownership_pending.sampled) {
        bf531_watchdog_timing *w = &out->watchdog[s->ownership_pending.phase];
        w->pending_expiry = 1;
        if (w->expired_without_sample == UINT64_MAX) { out->overflow = UINT64_MAX; }
        else { w->expired_without_sample++; }
    }
}
static void timing_mask_json(FILE *f, const bfin_mask_timing *m)
{
    fprintf(f, "{\"intervals\":%" PRIu64 ",\"total_cycles\":%" PRIu64
        ",\"total_awake_cycles\":%" PRIu64 ",\"total_idle_cycles\":%" PRIu64
        ",\"max_cycles\":%" PRIu64 ",\"max_awake_cycles\":%" PRIu64 ",\"max_idle_cycles\":%" PRIu64
        ",\"open\":%s,\"censored_intervals\":%" PRIu64 ",\"censored_cycles\":%" PRIu64
        ",\"censored_idle_cycles\":%" PRIu64 "}", m->intervals, m->total_cycles,
        m->total_awake_cycles, m->total_idle_cycles, m->max_cycles, m->max_awake_cycles,
        m->max_idle_cycles, m->open ? "true" : "false", m->censored_intervals,
        m->censored_cycles, m->censored_idle_cycles);
}
static void timing_sample_json(FILE *f, const bf531_timing_sample *s)
{
    if (!s->arm) { fputs("null", f); return; }
    fprintf(f, "{\"arm\":%" PRIu64 ",\"scheduled_cycle\":%" PRIu64
        ",\"observed_cycle\":%" PRIu64 ",\"stat_cycle\":%" PRIu64 "}",
        s->arm, s->scheduled_cycle, s->observed_cycle, s->stat_cycle);
}
static void timing_watchdog_json(FILE *f, const bf531_watchdog_timing *w)
{
    fprintf(f, "{\"arms\":%" PRIu64 ",\"carried_arms\":%" PRIu64 ",\"expiries\":%" PRIu64
        ",\"stat_samples\":%" PRIu64 ",\"cancelled_before_expiry\":%" PRIu64
        ",\"expired_without_sample\":%" PRIu64 ",\"max_observation_delay_cycles\":%" PRIu64
        ",\"max_expiry_to_stat_cycles\":%" PRIu64 ",\"total_expiry_to_stat_cycles\":%" PRIu64
        ",\"pending_expiry\":%s,\"last_sample\":", w->arms, w->carried_arms, w->expiries,
        w->stat_samples, w->cancelled_before_expiry, w->expired_without_sample,
        w->max_observation_delay_cycles, w->max_expiry_to_stat_cycles,
        w->total_expiry_to_stat_cycles, w->pending_expiry ? "true" : "false");
    timing_sample_json(f, &w->last_sample); fputs(",\"worst_sample\":", f);
    timing_sample_json(f, &w->worst_sample); fputc('}', f);
}
int bf531_timing_json(bf531 *s, FILE *f)
{
    bf531_ownership_timing t;
    bf531_timing_read(s, &t);
    fprintf(f, "{\"schema\":1,\"units\":\"elapsed_model_cycles\",\"cycles_per_second\":%" PRIu64
        ",\"start_pc\":", (uint64_t)BF531_CCLK_HZ);
    if (t.core.start_configured) { fprintf(f, "\"0x%08x\"", t.core.start_pc); } else { fputs("null", f); }
    fprintf(f, ",\"start_seen\":%s,\"start_cycle\":", t.core.start_seen ? "true" : "false");
    if (t.core.start_seen) { fprintf(f, "%" PRIu64, t.core.start_cycle); } else { fputs("null", f); }
    fprintf(f, ",\"end_cycle\":%" PRIu64 ",\"snapshot_loaded\":%s,\"overflow\":%" PRIu64,
        t.core.end_cycle, t.core.snapshot_loaded ? "true" : "false", t.overflow);
    for (unsigned phase = 0; phase < 2; phase++) {
        fprintf(f, ",\"%s\":{", phase ? "runtime" : "boot");
        if (phase) { fprintf(f, "\"status\":\"%s\",", t.core.start_seen && !t.core.snapshot_loaded ? "measured" : "unavailable"); }
        fputs("\"imask\":", f); timing_mask_json(f, &t.core.phase[phase].imask);
        fputs(",\"ipend_global\":", f); timing_mask_json(f, &t.core.phase[phase].ipend_global);
        fputs(",\"watchdog\":", f); timing_watchdog_json(f, &t.watchdog[phase]); fputc('}', f);
    }
    fputs("}\n", f);
    return ferror(f) ? -1 : 0;
}

int bf531_ppi_delay_configure(bf531 *s, uint32_t delay_us, uint32_t count)
{
    if (delay_us > 50000 || count > 1024 || s->ppi_started ||
        s->ppi_injection_configured || (s->dma[0].reg[D_CONFIG / 4] & DMAEN) ||
        (delay_us && !count)) { return -1; }
    s->ppi_injection.delay_us = delay_us;
    s->ppi_injection.requested_count = count;
    s->ppi_injection_configured = true;
    return 0;
}
void bf531_ppi_delay_read(const bf531 *s, bf531_ppi_injection *out)
{
    *out = s->ppi_injection;
}
int bf531_ppi_delay_json(const bf531 *s, FILE *f)
{
    bfin_ownership_timing t;
    const bf531_ppi_injection *i = &s->ppi_injection;
    bfin_timing_read(s->core, &t);
    fprintf(f, "{\"schema\":1,\"units\":\"elapsed_model_cycles\",\"cycles_per_second\":%" PRIu64
        ",\"delay_us\":%u,\"requested_count\":%u,\"applied_count\":%u,\"runtime_seen\":%s"
        ",\"snapshot_loaded\":%s,\"start_cycle\":", (uint64_t)BF531_CCLK_HZ,
        i->delay_us, i->requested_count, i->applied_count, t.start_seen ? "true" : "false",
        t.snapshot_loaded ? "true" : "false");
    if (t.start_seen) { fprintf(f, "%" PRIu64, t.start_cycle); } else { fputs("null", f); }
    fprintf(f, ",\"end_cycle\":%" PRIu64 ",\"status\":\"%s\",\"last\":", t.end_cycle,
        i->applied_count && !t.snapshot_loaded ? "applied" : "unavailable");
    if (i->applied_count) {
        fprintf(f, "{\"scheduled_cycle\":%" PRIu64 ",\"original_due_cycle\":%" PRIu64
            ",\"delayed_due_cycle\":%" PRIu64 "}", i->scheduled_cycle,
            i->original_due_cycle, i->delayed_due_cycle);
    } else { fputs("null", f); }
    fputs("}\n", f);
    return ferror(f) ? -1 : 0;
}

/* ---- SCLK watchdog (BF53x HRM15-50..54) ------------------------------- */

/* As the ADSP-BF533 Hardware Reference (Rev. 3.6, chapter 15) gives it: the
 * WDEN disable key, CNT preloaded while disabled and reloaded into STAT while
 * enabled, a countdown that stops at zero, and WDRO sticky until cleared after
 * a disable. It counts elapsed time at the current PLL_DIV ratio. Of the
 * expiry events the general-purpose interrupt and none are modelled; reset
 * and NMI stop the model (BFIN_STOP_UNDEF) with a diagnostic. */
#define WDRO 0x8000u
#define WD_DISABLE 0x0ad0u
static int wdog_enabled(bf531 *s) { return (s->mmr[WDOG_CTL / 4] & 0xff0) != WD_DISABLE; }
static void wdog_irq(bf531 *s)
{
    uint32_t control = s->mmr[WDOG_CTL / 4];
    sic_set(s, 23, (control & (WDRO | 6)) == (WDRO | 4));
}
static void wdog_expire(bf531 *s, uint64_t scheduled)
{
    s->wdog_left = 0;
    ownership_expire(s, scheduled); /* per arm, including reload with sticky WDRO */
    if (!(s->mmr[WDOG_CTL / 4] & WDRO)) {
        unsigned event = s->mmr[WDOG_CTL / 4] & 6;
        s->mmr[WDOG_CTL / 4] |= WDRO;
        if (event == 0 || event == 2) {
            slog(s, "watchdog reset/NMI expiry is unsupported (WDEV=%u)", event >> 1);
            s->wdog_unsupported = true;
        }
    }
    wdog_irq(s);
}
static void wdog_sync(bf531 *s)
{
    uint64_t now = bfin_cycles(s->core), stamp = s->wdog_stamp, delta = now - stamp;
    s->wdog_stamp = now;
    if (!wdog_enabled(s)) { return; }
    if (!s->wdog_left) { wdog_expire(s, now); return; }
    uint32_t divider = s->mmr[PLL_DIV / 4];
    unsigned ssel = divider & 15 ? divider & 15 : 5;
    unsigned scale = 1u << ((divider >> 4) & 3);
    uint64_t left = (uint64_t)s->wdog_left * ssel - s->wdog_fraction;
    if (delta >= (left + scale - 1) / scale) {
        wdog_expire(s, stamp + (left + scale - 1) / scale); return;
    }
    uint64_t ticks = delta * scale + s->wdog_fraction;
    s->wdog_left -= ticks / ssel;
    s->wdog_fraction = ticks % ssel;
}
static uint64_t wdog_due(bf531 *s)
{
    if (!wdog_enabled(s) || !s->wdog_left) { return NEVER; }
    uint32_t divider = s->mmr[PLL_DIV / 4];
    unsigned ssel = divider & 15 ? divider & 15 : 5;
    unsigned scale = 1u << ((divider >> 4) & 3);
    uint64_t left = (uint64_t)s->wdog_left * ssel - s->wdog_fraction;
    return s->wdog_stamp + (left + scale - 1) / scale;
}
static int wdog_access(uint32_t off, unsigned size)
{
    return (off == WDOG_CTL && size == 2) ||
           ((off == WDOG_CNT || off == WDOG_STAT) && size == 4);
}
static void wdog_write(bf531 *s, uint32_t off, uint32_t value, unsigned size)
{
    if (!wdog_access(off, size)) { slog(s, "invalid watchdog write width/address"); return; }
    wdog_sync(s);
    if (off == WDOG_CTL) {
        uint32_t sticky = s->mmr[WDOG_CTL / 4] & WDRO;
        if (!wdog_enabled(s) && (value & WDRO)) { sticky = 0; }
        int enabled = wdog_enabled(s);
        s->mmr[WDOG_CTL / 4] = (value & 0xff6) | sticky;
        if (!enabled && wdog_enabled(s)) { ownership_arm(s); }
        else if (enabled && !wdog_enabled(s)) { ownership_finish(s); }
        wdog_sync(s); /* enabling a zero count expires immediately */
        wdog_irq(s);
    } else if (off == WDOG_CNT) {
        if (!wdog_enabled(s)) {
            s->mmr[WDOG_CNT / 4] = s->wdog_left = value;
            s->wdog_fraction = 0;
        }
    } else if (wdog_enabled(s)) {
        ownership_arm(s);
        s->wdog_left = s->mmr[WDOG_CNT / 4];
        s->wdog_fraction = 0;
        if (!s->wdog_left) { wdog_expire(s, bfin_cycles(s->core)); }
    }
    bfin_yield(s->core); /* expose a changed deadline to the SoC runner */
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

static uint8_t *load_target(bf531 *s, uint32_t addr, size_t len)
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
    if (addr - L1_SCRATCH < sizeof(s->l1_scratch) && len <= sizeof(s->l1_scratch) - (addr - L1_SCRATCH)) {
        return s->l1_scratch + (addr - L1_SCRATCH);
    }
    return NULL;
}

static uint32_t mem_read(bf531 *s, uint32_t addr, unsigned size);

/* ---- the flash ---------------------------------------------------------- */

/*
 * 2 MiB, x16, AMD/JEDEC command set (CFI cmdset 0x0002), top boot: 31
 * sectors of 64 KiB, then 8 of 8 KiB (see FL_BOOT). Written from the
 * command-set description, not from any other simulator. The ID pair is
 * FL_ID_MAN/FL_ID_DEV below; drivers that care read the CFI table instead.
 *
 * Reads: in read-array mode the core reads the flash array directly (a ROM
 * bank); in every other mode (autoselect, CFI query, a program or erase in
 * flight) the bank is unmapped and every read, data or instruction, goes
 * through the bus to fl_read16.
 */
enum { FL_ARRAY, FL_AUTO, FL_CFI, FL_BUSY };
enum { FLOP_PROGRAM, FLOP_SECTOR, FLOP_CHIP };
#define FL_SECTOR     0x10000u
#define FL_ID_MAN   0x0089   /* as the CFI model the stock GUI is known to update against reports */
#define FL_ID_DEV   0x00AD

static void __attribute__((format(printf, 2, 3))) flog(bf531 *s, const char *fmt, ...)
{
    va_list ap;

    if (s->log) {
        va_start(ap, fmt);
        fprintf(s->log, "cdj_bf531: ");
        vfprintf(s->log, fmt, ap);
        fputc('\n', s->log);
        va_end(ap);
    }
}

static void fl_sync(bf531 *s)
{
    bool want = s->fl_mode == FL_ARRAY;

    if (want && !s->fl_mapped) {
        bfin_map_rom(s->core, ASYNC_BASE, FLASH_SIZE, s->flash);
    } else if (!want && s->fl_mapped) {
        bfin_unmap_rom(s->core, ASYNC_BASE);
    }
    s->fl_mapped = want;
}

static uint16_t fl_cfi(unsigned w)
{
    static const uint8_t t[0x50] = {
        [0x10] = 'Q', 'R', 'Y', 0x02, 0x00, 0x40, 0x00, 0x00, 0x00,
        [0x1B] = 0x27, 0x36, 0x00, 0x00, 0x04, 0x00, 0x0A, 0x0F, 0x05, 0x00,
                 0x04, 0x04,
        [0x27] = 0x15, 0x01, 0x00, 0x00, 0x00, 0x02,   /* 2^21, x16, 2 regions */
        [0x2D] = 0x1E, 0x00, 0x00, 0x01,               /* 31 x 64 KiB */
                 0x07, 0x00, 0x20, 0x00,               /* then 8 x 8 KiB (top boot) */
        [0x40] = 'P', 'R', 'I', '1', '3',
        [0x4F] = 0x03,                                 /* top boot */
    };

    return w < sizeof(t) ? t[w] : 0;
}

static void fl_finish(bf531 *s)
{
    s->fl_mode = FL_ARRAY;
    fl_sync(s);
}

/* RY/BY#: busy ends by the modelled time when nothing polls the status bits. */
static uint32_t fl_ready(bf531 *s)
{
    if (s->fl_mode == FL_BUSY && bfin_cycles(s->core) >= s->fl_until) {
        fl_finish(s);
    }
    return s->fl_mode != FL_BUSY;
}

/* CDJ_BF531_FLASH_LOG=1: every command write and how each operation ended. */
static int fl_log = -1;

static bool fl_logging(void)
{
    if (fl_log < 0) {
        fl_log = getenv("CDJ_BF531_FLASH_LOG") != NULL;
    }
    return fl_log;
}

static uint16_t fl_read16(bf531 *s, uint32_t addr)
{
    uint32_t off = (addr - ASYNC_BASE) & ~1u;
    unsigned w = (off >> 1) & 0xFF;
    uint16_t v;

    if (s->fl_mode == FL_BUSY) {
        s->fl_reads++;
        /* Busy ends by status reads or by model time, whichever comes
         * first, so polling that never advances the core clock still ends. */
        if (s->fl_reads > s->fl_op * 32u + 4 ||
            bfin_cycles(s->core) >= s->fl_until) {
            if (fl_logging()) {
                flog(s, "flash op %d done after %u status reads, pc 0x%08x",
                     s->fl_op, s->fl_reads, bfin_get_pc(s->core));
            }
            fl_finish(s);
        } else {
            s->fl_toggle ^= 0x40;
            v = s->fl_toggle;
            if (s->fl_op == FLOP_PROGRAM) {
                v |= ~s->fl_data & 0x80;                /* DQ7 is data# */
            } else {
                v |= 0x08 | (s->fl_toggle >> 4 & 0x04); /* DQ3, DQ2 toggles */
            }
            return v;
        }
    }
    switch (s->fl_mode) {
    case FL_AUTO:
        return w == 0 ? FL_ID_MAN : w == 1 ? FL_ID_DEV : 0;
    case FL_CFI:
        return fl_cfi(w) ;
    default:
        memcpy(&v, s->flash + off, 2);
        return v;
    }
}

static uint32_t fl_read(bf531 *s, uint32_t addr, unsigned size)
{
    uint32_t v;

    if (size == 4) {
        v = fl_read16(s, addr);
        return v | (uint32_t)fl_read16(s, addr + 2) << 16;
    }
    v = fl_read16(s, addr);
    return size == 1 ? (v >> (8 * (addr & 1))) & 0xFF : v;
}

static void fl_busy(bf531 *s, int op, uint16_t data)
{
    static const uint64_t cycles[] = { 2000, 400000, 8000000 };

    s->fl_mode = FL_BUSY;
    s->fl_op = op;
    s->fl_data = data;
    s->fl_reads = 0;
    s->fl_toggle = 0;
    s->fl_until = bfin_cycles(s->core) + cycles[op];
    fl_sync(s);
}

/* Top-boot geometry: 31 x 64 KiB, then 8 x 8 KiB boot sectors from 0x1F0000.
 * The GUI keeps its journal in the 8 KiB sector at 0x1FC000 and erases the
 * one at 0x1FA000 on its own; a uniform 64 KiB top sector wiped them all. */
#define FL_BOOT       0x1F0000u
#define FL_BOOT_SEC   0x2000u

static void fl_erase(bf531 *s, uint32_t off)
{
    uint32_t len = off >= FL_BOOT ? FL_BOOT_SEC : FL_SECTOR;

    off &= ~(len - 1);
    memset(s->flash + off, 0xFF, len);
    flog(s, "flash erase 0x%08x +0x%x", ASYNC_BASE + off, len);
    s->fl_sectors++;
}

static void fl_write16(bf531 *s, uint32_t addr, uint16_t val)
{
    uint32_t off = (addr - ASYNC_BASE) & ~1u;
    unsigned w = (off >> 1) & 0x7FF, v = val & 0xFF;

    if (fl_logging()) {
        flog(s, "flash w 0x%08x = 0x%04x (cycle %u, mode %d, pc 0x%08x)",
             addr, val, s->fl_cyc, s->fl_mode, bfin_get_pc(s->core));
    }

    if (s->fl_mode == FL_BUSY) {
        fl_read16(s, addr);     /* busy ends by time as well as reads */
        if (s->fl_mode == FL_BUSY) {
            return;             /* writes during a program or erase are ignored */
        }
    }
    if (s->fl_cyc == 6) {       /* the program cycle: the AND of old and new */
        uint16_t old;

        memcpy(&old, s->flash + off, 2);
        old &= val;
        memcpy(s->flash + off, &old, 2);
        s->fl_words++;
        s->fl_cyc = 0;
        fl_busy(s, FLOP_PROGRAM, old);
        return;
    }
    if (v == 0xF0) {
        s->fl_cyc = 0;
        fl_finish(s);
        return;
    }
    if (v == 0x98 && w == 0x55 && s->fl_cyc == 0) {
        s->fl_mode = FL_CFI;
        fl_sync(s);
        return;
    }
    switch (s->fl_cyc) {
    case 0: case 3:
        s->fl_cyc = w == 0x555 && v == 0xAA ? s->fl_cyc + 1 : 0;
        break;
    case 1: case 4:
        s->fl_cyc = w == 0x2AA && v == 0x55 ? s->fl_cyc + 1 : 0;
        break;
    case 2:
        s->fl_cyc = 0;
        if (w == 0x555 && v == 0x90) {
            s->fl_mode = FL_AUTO;
            fl_sync(s);
        } else if (w == 0x555 && v == 0xA0) {
            s->fl_cyc = 6;
        } else if (w == 0x555 && v == 0x80) {
            s->fl_cyc = 3;
        }
        break;
    case 5:
        s->fl_cyc = 0;
        if (v == 0x30) {
            fl_erase(s, off);
            fl_busy(s, FLOP_SECTOR, 0);
        } else if (w == 0x555 && v == 0x10) {
            for (uint32_t o = 0; o < FLASH_SIZE;
                 o += o >= FL_BOOT ? FL_BOOT_SEC : FL_SECTOR) {
                fl_erase(s, o);
            }
            fl_busy(s, FLOP_CHIP, 0);
        }
        break;
    default:
        s->fl_cyc = 0;
    }
}

static void fl_write(bf531 *s, uint32_t addr, uint32_t val, unsigned size)
{
    if (size == 4) {
        fl_write16(s, addr, val);
        fl_write16(s, addr + 2, val >> 16);
    } else if (size == 2) {
        fl_write16(s, addr, val);
    } else {
        flog(s, "flash byte write 0x%08x = 0x%x ignored (x16 part)", addr, val);
    }
}

int bf531_flash_load(bf531 *s, const uint8_t *img, size_t len)
{
    if (len != FLASH_SIZE) {
        return -1;
    }
    memcpy(s->flash, img, len);
    return 0;
}

int bf531_flash_save(const bf531 *s, const char *path)
{
    FILE *f = fopen(path, "wb");
    int ok = f && fwrite(s->flash, 1, FLASH_SIZE, f) == FLASH_SIZE;

    if (f && fclose(f)) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

void bf531_flash_report(bf531 *s)
{
    flog(s, "flash %u words programmed, %u sectors erased", s->fl_words, s->fl_sectors);
}

void bf531_code_range(bf531 *s, uint32_t lo, uint32_t hi)
{
    s->code_range_on = true;
    s->code_lo = lo;
    s->code_hi = hi;
    bfin_exec_track(s->core);
}

void bf531_code_range_report(bf531 *s)
{
    if (s->code_range_on) {
        flog(s, "RANGE 0x%x:0x%x lines_run=%u", s->code_lo, s->code_hi,
             bfin_exec_lines(s->core, s->code_lo, s->code_hi));
    }
}

uint32_t bf531_bus_read(bf531 *s, uint32_t addr, unsigned size)
{
    return mem_read(s, addr, size);
}

void bf531_bus_write(bf531 *s, uint32_t addr, uint32_t val, unsigned size)
{
    bus_write(s, addr, val, size);
}

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
    uint64_t now = bfin_cycles(s->core);
    d->due = now + lines * (BF531_CCLK_HZ / 60 / 262);
    if (d == &s->dma[0]) {
        bf531_ppi_injection *i = &s->ppi_injection;
        s->ppi_started = true;
        if (i->delay_us && i->applied_count < i->requested_count &&
            bfin_timing_started(s->core)) {
            uint64_t delay = i->delay_us * (BF531_CCLK_HZ / 1000000);
            /* Never wrap a test delay into an immediate completion. */
            if (d->due >= now && d->due <= UINT64_MAX - delay) {
                i->scheduled_cycle = now;
                i->original_due_cycle = d->due;
                d->due += delay;
                i->delayed_due_cycle = d->due;
                i->applied_count++;
            }
        }
    }
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
    /* Short bursts leave the current counts for firmware to poll. In 2D,
     * pump one row per event so each DI_SEL interrupt can be serviced. */
    bf531_dma *d = &s->dma[ch];
    uint32_t *r = d->reg;
    bool two_d = r[D_CONFIG / 4] & DMA2D;
    size_t room = (d->reg[D_CURR_X / 4] & 0xFFFF) * 2;
    size_t len;
    uint8_t *to;

    if (two_d) {
        unsigned y = r[D_CURR_Y / 4] & 0xFFFF;

        room = y ? room + (size_t)(y - 1) * (r[D_XCOUNT / 4] & 0xFFFF) * 2 : 0;
    }
    len = s->sport1_rx_len < room ? s->sport1_rx_len : room;
    d->due = NEVER;
    /* A record goes in whole or waits for a receive that holds it. The
     * stock GUI arms each receive for the record it expects (32 words for a
     * header, 1024 for a 2 KB update record) and polls with a 100-word probe
     * in between; splitting a 2 KB record across the probe corrupted every
     * update chunk. A GUI that arms the full 2048 bytes always fits. */
    if (s->sport1_rx_len > room) {
        s->sport1_rx_waits++;
        return;
    }
    if (two_d) {
        size_t row = (r[D_CURR_X / 4] & 0xFFFF) * 2;
        int32_t xmod = (int16_t)r[D_XMOD / 4];
        int32_t ymod = (int16_t)r[D_YMOD / 4];

        if (len > row) {
            len = row;
        }
        for (size_t i = 0; i < len; i += 2) {
            to = load_target(s, r[D_CURR_ADDR / 4], 2);
            if (to) {
                memcpy(to, s->sport1_rx + i, len - i < 2 ? 1 : 2);
            }
            r[D_CURR_X / 4]--;
            /* HRM: Y_MODIFY replaces the last X_MODIFY of a row. */
            r[D_CURR_ADDR / 4] += r[D_CURR_X / 4] ? xmod : ymod;
        }
        if (len && !r[D_CURR_X / 4]) {
            r[D_CURR_X / 4] = r[D_XCOUNT / 4] & 0xFFFF;
            r[D_CURR_Y / 4]--;
            if (r[D_CURR_Y / 4] &&
                (r[D_CONFIG / 4] & (DI_EN | DI_SEL)) == (DI_EN | DI_SEL)) {
                r[D_IRQ_STATUS / 4] |= DMA_DONE;
                s->mmr[SIC_IMASK / 4] |= 1u << (IRQ_PPI_DMA + ch);
                sic_set(s, IRQ_PPI_DMA + ch, 1);
            }
        }
    } else {
        to = load_target(s, d->reg[D_CURR_ADDR / 4], len);
        if (to) {
            memcpy(to, s->sport1_rx, len);
        }
        d->reg[D_CURR_ADDR / 4] += len;
        d->reg[D_CURR_X / 4] = (room - len) / 2;
    }
    memmove(s->sport1_rx, s->sport1_rx + len, s->sport1_rx_len - len);
    s->sport1_rx_len -= len;
    if (!s->sport1_rx_len && s->sport1_q_count) {
        unsigned h = s->sport1_q_head;

        memcpy(s->sport1_rx, s->sport1_q[h], s->sport1_q_len[h]);
        s->sport1_rx_len = s->sport1_q_len[h];
        s->sport1_q_head = (h + 1) % 16;
        s->sport1_q_count--;
    }
    if (room > len) {
        if (s->sport1_rx_len) {
            d->due = bfin_cycles(s->core);
        }
        return;
    }
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
            d->reg[D_CURR_ADDR / 4] = d->reg[D_START / 4];
            d->reg[D_CURR_X / 4] = d->reg[D_XCOUNT / 4] & 0xFFFF;
            if (v & DMA2D) {
                d->reg[D_CURR_Y / 4] = dma_rows(d);
                d->reg[D_IRQ_STATUS / 4] |= DMA_RUN;
            }
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

static uint32_t sys_read(bf531 *s, uint32_t off, unsigned size)
{
    if (off >= WDOG_CTL && off < WDOG_STAT + 4) {
        if (!wdog_access(off, size)) { slog(s, "invalid watchdog read width/address"); return 0; }
        wdog_sync(s);
        if (off == WDOG_STAT) { ownership_stat(s); }
        return off == WDOG_STAT ? s->wdog_left : s->mmr[off / 4];
    }
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
    case FIO_FLAG_D: case FIO_FLAG_C: case FIO_FLAG_S: case FIO_FLAG_T: {
        uint32_t v = s->mmr[FIO_FLAG_D / 4];

        /* PF0 is the flash's RY/BY# when it is an input: the stock flash
         * driver waits on it after every erase (low busy, high ready). */
        if (!(s->mmr[FIO_DIR / 4] & 1)) {
            v = (v & ~1u) | fl_ready(s);
        }
        return v;
    }
    case EBIU_SDSTAT: return 0x8;             /* SDRS: SDRAM powered up */
    }
    return s->mmr[off / 4];
}

static void sys_write(bf531 *s, uint32_t off, uint32_t v, unsigned size)
{
    if (off >= WDOG_CTL && off < WDOG_STAT + 4) { wdog_write(s, off, v, size); return; }
    if (off == PLL_DIV) { wdog_sync(s); s->wdog_fraction = 0; }
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
    if (off == SIC_IMASK || off == SIC_IWR || (off >= SIC_IAR0 && off < SIC_IAR0 + 12)) {
        sic_update(s);
    }
}

/* ---- bus ---------------------------------------------------------------- */

static uint32_t mem_read(bf531 *s, uint32_t addr, unsigned size)
{
    uint8_t *h = load_target(s, addr, size);
    uint32_t v = 0;

    if (!h && addr - ASYNC_BASE < FLASH_SIZE) {
        return fl_read(s, addr, size);
    }
    if (h) {
        memcpy(&v, h, size);
        return v;
    }
    if (addr - SYS_BASE < SYS_SIZE) {
        return sys_read(s, addr - SYS_BASE, size);
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
        sys_write(s, addr - SYS_BASE, val, size);
    } else if (addr - ASYNC_BASE < FLASH_SIZE) {
        fl_write(s, addr, val, size);
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
    s->fl_mapped = true;

    /* Reset values: PLL_DIV SSEL 5, the default SIC assignment (PPI DMA
     * IVG8, SPORT DMA IVG9, the timers IVG11). */
    s->mmr[PLL_DIV / 4] = 0x0005;
    s->mmr[SIC_IWR / 4] = 0xffffffffu;
    s->mmr[WDOG_CTL / 4] = WD_DISABLE;
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
    if (!data) {
        s->sport1_rx_len = 0;
        s->sport1_q_count = 0;
        return;
    }
    if (s->sport1_rx_len) {
        unsigned t;

        if (s->sport1_q_count == 16) {
            /* Fail loud: a dropped link packet is a broken run. */
            if (!s->sport1_q_dropped++) {
                fprintf(stderr, "bf531: SPORT1 RX queue full, oldest packet dropped\n");
            }
            s->sport1_q_head = (s->sport1_q_head + 1) % 16;
            s->sport1_q_count--;
        }
        t = (s->sport1_q_head + s->sport1_q_count++) % 16;
        memcpy(s->sport1_q[t], data, len);
        s->sport1_q_len[t] = len;
        return;
    }
    memcpy(s->sport1_rx, data, len);
    s->sport1_rx_len = len;
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

/* The LDR stream at img[off..len): each block loaded where it says, then the
 * core starts at the boot entry. 0, or -1 for a malformed stream. */
static int ldr_boot(bf531 *s, const uint8_t *img, size_t off, size_t len)
{
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

int bf531_load_update(bf531 *s, const uint8_t *img, size_t len)
{
    size_t off = 0x20;

    first_section(&img, &len);
    if (len < off || len - off > FLASH_SIZE - FLASH_APP) {
        return -1;
    }
    memcpy(s->flash + FLASH_APP, img + off, len - off);
    return ldr_boot(s, img, off, len);
}

int bf531_flash_boot(bf531 *s)
{
    flog(s, "booting the flash's LDR at 0x%x", FLASH_APP);
    return ldr_boot(s, s->flash, FLASH_APP, FLASH_SIZE);
}

static uint64_t next_event(bf531 *s)
{
    uint64_t due = NEVER, core = bfin_timer_due(s->core);
    uint64_t watchdog = wdog_due(s);
    if (watchdog < due) { due = watchdog; }

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
    wdog_sync(s);

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
        if (s->wdog_unsupported) { return BFIN_STOP_UNDEF; }
        uint64_t now = bfin_cycles(s->core), due = next_event(s);
        uint64_t stop_at = due < end ? due : end;
        bfin_stop stop;

        /* At least one cycle: an expired core timer is serviced by the step. */
        stop = bfin_step(s->core, stop_at > now ? stop_at - now : 1, NULL);
        if (stop == BFIN_STOP_UNDEF || stop == BFIN_STOP_BREAK ||
            stop == BFIN_STOP_WATCH) {
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
        if (s->wdog_unsupported) { return BFIN_STOP_UNDEF; }
    }
    return BFIN_STOP_BUDGET;
}

/* One instruction or bundle, then the peripherals catch up with the cycles
 * it took. An IDLE that nothing wakes just moves time to the next event. */
bfin_stop bf531_stepi(bf531 *s)
{
    if (s->wdog_unsupported) { return BFIN_STOP_UNDEF; }
    bfin_stop stop = bfin_stepi(s->core);

    if (stop == BFIN_STOP_IDLE) {
        uint64_t due = next_event(s), now = bfin_cycles(s->core);

        if (due != NEVER && due > now) {
            bfin_skip_cycles(s->core, due - now);
        }
    }
    advance(s);
    return s->wdog_unsupported ? BFIN_STOP_UNDEF : stop;
}

bfin_core *bf531_core(bf531 *s)
{
    return s->core;
}

uint64_t bf531_sport1_waits(const bf531 *s)
{
    return s->sport1_rx_waits;
}

uint64_t bf531_frames(const bf531 *s)
{
    return s->frames;
}

/* ---- snapshots ---------------------------------------------------------- */

/* The SoC's state between two runs, as (offset, length) runs of the struct:
 * the L1 banks, every register through the DMA channels, the frame count and
 * the standing SPORT1 packet. The SDRAM and the core follow them. */
#define SNAP_RUN(first, end) { offsetof(bf531, first),                                offsetof(bf531, end) - offsetof(bf531, first) }

static const struct { size_t off, len; } snap_runs[] = {
    SNAP_RUN(l1_data_a, mmr),     /* the L1 banks and the flash */
    SNAP_RUN(mmr, fb),
    { offsetof(bf531, frames), sizeof(uint64_t) },
    { offsetof(bf531, sport1_rx), sizeof(bf531) - offsetof(bf531, sport1_rx) },
};

size_t bf531_save(const bf531 *s, uint8_t *buf)
{
    size_t at = 0;

    for (size_t i = 0; i < sizeof(snap_runs) / sizeof(snap_runs[0]); i++) {
        if (buf) {
            memcpy(buf + at, (const uint8_t *)s + snap_runs[i].off, snap_runs[i].len);
        }
        at += snap_runs[i].len;
    }
    if (buf) {
        memcpy(buf + at, s->sdram, s->sdram_size);
    }
    at += s->sdram_size;
    return at + bfin_snap_save(s->core, buf ? buf + at : NULL);
}

void bf531_load(bf531 *s, const uint8_t *buf)
{
    for (size_t i = 0; i < sizeof(snap_runs) / sizeof(snap_runs[0]); i++) {
        memcpy((uint8_t *)s + snap_runs[i].off, buf, snap_runs[i].len);
        buf += snap_runs[i].len;
    }
    memcpy(s->sdram, buf, s->sdram_size);
    bfin_snap_load(s->core, buf + s->sdram_size);
    fl_sync(s);
}

/* Host view of len bytes of the chip's memory at addr (SDRAM or L1), or NULL. */
uint8_t *bf531_mem(bf531 *s, uint32_t addr, size_t len)
{
    return load_target(s, addr, len);
}

const uint8_t *bf531_sdram(const bf531 *s, uint32_t *size)
{
    *size = s->sdram_size;
    return s->sdram;
}

/* Reads without side effects for a debugger: RAM, the boot flash, core MMRs
 * and the SoC's registers (whose reads only log). Returns 0, or -1 for an
 * address nothing answers at. */
int bf531_peek(bf531 *s, uint32_t addr, uint8_t *buf, size_t len)
{
    for (size_t n = 0; n < len; n++, addr++) {
        uint8_t *h = load_target(s, addr, 1);
        uint32_t w;

        if (!h && addr - ASYNC_BASE < FLASH_SIZE) {
            h = s->flash + (addr - ASYNC_BASE);
        }
        if (h) {
            buf[n] = *h;
        } else if (addr >= 0xFFE00000) {
            w = bfin_get_mmr(s->core, addr & ~3u);
            buf[n] = w >> (8 * (addr & 3));
        } else if (addr - SYS_BASE < SYS_SIZE) {
            uint32_t off = (addr - SYS_BASE) & ~3u;
            /* Debugger peeks must not latch expiry or change SIC state. */
            if (off >= WDOG_CTL && off <= WDOG_STAT) {
                w = off == WDOG_STAT ? s->wdog_left : s->mmr[off / 4];
            } else {
                w = sys_read(s, off, 4);
            }
            buf[n] = w >> (8 * (addr & 3));
        } else {
            return -1;
        }
    }
    return 0;
}

/* Writes RAM only (no register has a side effect from a debugger). */
int bf531_poke(bf531 *s, uint32_t addr, const uint8_t *buf, size_t len)
{
    uint8_t *h = load_target(s, addr, len);

    if (!h) {
        return -1;
    }
    memcpy(h, buf, len);
    return 0;
}
