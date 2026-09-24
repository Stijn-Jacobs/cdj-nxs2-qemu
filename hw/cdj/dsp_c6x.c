/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/*
 * CDJ_C6X: board glue for the emulated DSP, IC301 (a TMS320C6655), running the
 * program MAIN uploads to it. The core is c6x/c66x_core.c and the peripherals
 * c6x/soc_*.c; this file wires them to MAIN:
 *
 *   PTH0   (PHDR bit 0, +0x12E)  DSP reset: low holds it, a rise re-arms the ROM
 *   IIC1   slave 0x30            ROM I2C passive boot: the first-stage table
 *   PUDR.3 (+0x162)              <- DSP GPIO24, "ready for the next window"
 *   PVDR.1 (+0x164)              -> DSP GPIO21, MAIN's ack
 *   PZDR.6 (+0x16C)              <- DSP GPIO25, low = "booted"
 *   DMA1 ch3 -> CS6              the FPGA forwards every word into uPP channel A
 *   MSIOF0 ch4/ch5               the DSP's SPI master exchange, MAIN the slave
 *   McBSP0                       PCM towards the FPGA and the DAC
 *
 * The DSP is stepped against QEMU's virtual clock, so a slow host makes the
 * chip slower, never the machine's time wrong. CDJ_C6X_MHZ sets the core
 * cycles per virtual second; the SoC's timers, SPI and McBSP run at their real
 * rates on virtual time. With CDJ_C6X unset none of this runs.
 */
CdjC6x cdj_c6x;
CdjDma1State *cdj_dma1_singleton;

bool cdj_c6x_on(void)
{
    return cdj_c6x.on;
}

static uint64_t cdj_c6x_thread_cpu_ns(void)
{
#ifdef CLOCK_THREAD_CPUTIME_ID
    struct timespec ts;

    if (!clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts)) {
        return ts.tv_sec * 1000000000ull + ts.tv_nsec;
    }
#endif
    return 0;
}

static uint8_t *cdj_c6x_ram(uint32_t addr, unsigned size)
{
    if (addr - C6X_L2_ALIAS < C6X_L2_SIZE) {
        addr -= C6X_L2_ALIAS - C6X_L2_BASE;
    }
    if (addr - C6X_L2_BASE <= C6X_L2_SIZE - size) {
        return cdj_c6x.l2 + (addr - C6X_L2_BASE);
    }
    if (addr - C6X_DDR_BASE <= C6X_DDR_SIZE - size) {
        return cdj_c6x.ddr + (addr - C6X_DDR_BASE);
    }
    return NULL;
}

static uint32_t cdj_c6x_bus_read(void *opaque, uint32_t addr, unsigned size)
{
    uint8_t *p = cdj_c6x_ram(addr, size);
    uint32_t v = 0;

    if (p) {
        for (unsigned i = 0; i < size; i++) {
            v |= (uint32_t)p[i] << (8 * i);
        }
        return v;
    }
    if (c6655_soc_owns(addr)) {
        return c6655_soc_read(cdj_c6x.soc, addr, size);
    }
    qemu_log_mask(LOG_UNIMP, "c6x: unmapped read 0x%08x pc 0x%08x\n", addr,
                  cdj_c6x.core ? c66x_get_exec_pc(cdj_c6x.core) : 0);
    return 0;
}

static void cdj_c6x_bus_write(void *opaque, uint32_t addr, uint32_t val,
                              unsigned size)
{
    uint8_t *p = cdj_c6x_ram(addr, size);

    if (p) {
        for (unsigned i = 0; i < size; i++) {
            p[i] = val >> (8 * i);
        }
        /* Only the SoC's DMA masters store through here; the core writes its
         * mapped RAM itself, so this is the one place decode can go stale. */
        if (cdj_c6x.core) {
            if (cdj_c6x.prof) {
                int64_t p0 = get_clock();

                c66x_invalidate(cdj_c6x.core, addr, size);
                cdj_c6x.prof_inval_ns += get_clock() - p0;
                cdj_c6x.prof_inval++;
            } else {
                c66x_invalidate(cdj_c6x.core, addr, size);
            }
        }
        return;
    }
    if (c6655_soc_owns(addr)) {
        /* EVTFLAG0-3 are read-only; count stores there for the summary. */
        if (addr - 0x01800000u < 0x20) {
            cdj_c6x.intc_flag_writes++;
        }
        /* Log audio output start: McBSP0/1 SPCR (XRST/FRST) and EDMA3 event
         * enables (EESR/EESRH, global and shadow regions). Bounded. */
        {
            uint32_t eoff = addr - 0x02741000u;
            bool spcr = addr == 0x021B4008u || addr == 0x021B8008u;
            bool eesr = eoff < 0x2000 && ((eoff & 0x1FF) == 0x030 ||
                                          (eoff & 0x1FF) == 0x034);

            if ((spcr || eesr) && cdj_c6x.out_start_logs < 80) {
                cdj_c6x.out_start_logs++;
                info_report("c6x: output-start write *0x%08x = 0x%08x pc 0x%08x "
                            "at %" PRId64 " ms", addr, val,
                            cdj_c6x.core ? c66x_get_exec_pc(cdj_c6x.core) : 0,
                            qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
            }
        }
        c6655_soc_write(cdj_c6x.soc, addr, val, size);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "c6x: unmapped write 0x%08x = 0x%08x pc 0x%08x\n",
                  addr, val, cdj_c6x.core ? c66x_get_exec_pc(cdj_c6x.core) : 0);
}

static void cdj_c6x_link_kick(void);

/*
 * One 16-bit SPI word. The DSP is the master and MAIN's MSIOF0 the slave, so
 * this is a wire in both directions at once. The DSP builds a 64-halfword
 * window and scans it for 0x5533, so MAIN's frame starts at a window boundary
 * to land whole inside one. MAIN hunts its receive stream one word at a time
 * for the 0xAACC trailer (DEI5 ISR 0x08326E62).
 */
static uint16_t cdj_c6x_spi_xfer(void *opaque, uint16_t tx, unsigned bits,
                                 unsigned csnr, int cshold)
{
    CdjC6x *c = &cdj_c6x;
    uint64_t now = c6655_soc_now_ns(c->soc);
    uint16_t out = 0;
    bool kick;
    int64_t p0 = c->prof ? get_clock() : 0;

    c->prof_spi++;
    c->csnr_seen[csnr & 0xFF]++;
    if ((csnr & 0xFF) != 0x02 && (csnr & 0xFF) != 0xFE) {
        c->other_cs_words++;
        return 0;
    }
    qemu_mutex_lock(&c->link_lock);
    if (c->wpos >= C6X_WINDOW || now - c->last_word_ns > C6X_WINDOW_GAP_NS) {
        c->wpos = 0;
        c->rx_windows++;
    }
    c->last_word_ns = now;

    if (c->rx_windows == 50 || c->rx_windows == 2000) {
        c->dump[c->wpos & 63] = tx;
        if (c->wpos == 63) {
            GString *g = g_string_new("");

            for (unsigned i = 0; i < 64; i++) {
                g_string_append_printf(g, " %04x", c->dump[i]);
            }
            info_report("c6x: DSP exchange %" PRIu64 " sends:%s", c->rx_windows,
                        g->str);
            /* The frame as built (u16 at 0x008A6CB8) and as queued for EDMA
             * (u32 at 0x008A6FD0). */
            g_string_truncate(g, 0);
            for (unsigned i = 0; i < 64; i++) {
                uint8_t *m = cdj_c6x_ram(0x008A6CB8 + 2 * i, 2);
                g_string_append_printf(g, " %04x", m ? m[0] | m[1] << 8 : 0);
            }
            info_report("c6x:   built frame 0x008A6CB8:%s", g->str);
            g_string_truncate(g, 0);
            for (unsigned i = 60; i < 64; i++) {
                uint8_t *m = cdj_c6x_ram(0x008A6FD0 + 4 * i, 4);
                g_string_append_printf(g, " %08x",
                                       m ? m[0] | m[1] << 8 | m[2] << 16 |
                                       (uint32_t)m[3] << 24 : 0);
            }
            info_report("c6x:   EDMA words 60..63 at 0x008A6FD0:%s", g->str);
            g_string_free(g, true);
        }
    }
    if (tx == 0xAACC || tx == 0xCCAA || tx == 0x5533 || tx == 0x3355) {
        c->magic[(tx == 0xCCAA) | (tx == 0x5533) << 1 | (tx == 0x3355) * 3]++;
    }
    if (c->rx_len < C6X_RXQ) {
        c->rxq[(c->rx_head + c->rx_len) % C6X_RXQ] = tx;
        c->rx_stamp[(c->rx_head + c->rx_len) % C6X_RXQ] = now;
        c->rx_len++;
    } else {
        c->rx_dropped++;
    }
    c->rx_words++;

    if (c->wpos == 0 && c->tx_cur < 0 && c->tx_count && !c->ship_pending &&
        now >= c->tx_hold_until_ns) {
        c->tx_cur = c->tx_head;
        c->tx_pos = 0;
        if (c->tx_frames_sent < 3) {
            info_report("c6x: DSP clocks MAIN frame %" PRIu64 " in exchange %"
                        PRIu64 " at DSP %.6f s (DSP pc 0x%08x, virt %" PRId64
                        " us)", c->tx_frames_sent + 1, c->rx_windows, now / 1e9,
                        c66x_get_exec_pc(c->core),
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000);
        }
    }
    if (c->wpos == 0 && c->rx_windows <= 16) {
        info_report("c6x: exchange %" PRIu64 " starts at DSP %.6f s, INT9 so "
                    "far %u, MAIN frames queued %u", c->rx_windows, now / 1e9,
                    0u, c->tx_count);
    }
    if (c->tx_cur >= 0) {
        out = c->txf[c->tx_cur][c->tx_pos++];
        if (c->tx_pos >= c->txf_len[c->tx_cur]) {
            c->tx_head = (c->tx_head + 1) % C6X_TXFRAMES;
            c->tx_count--;
            c->tx_cur = -1;
            c->tx_frames_sent++;
        }
    } else {
        c->tx_idle_words++;
    }
    c->wpos++;
    /* Paced, a held frame's release time is fixed once its last word is in,
     * so only that word kicks; later words would only re-arm the same timer. */
    kick = (c->rx_want && (c->rx_pace ? c->rx_len == c->rx_want
                                      : c->rx_len >= c->rx_want)) ||
           (c->tx_waiting && c->tx_count < C6X_TXFRAMES);
    qemu_mutex_unlock(&c->link_lock);
    if (kick) {
        /* The DMA completion touches guest memory and the INTC, so from the
         * DSP thread it has to run under the BQL, in the main loop. */
        if (c->threaded) {
            c->prof_bh++;
            qemu_bh_schedule(c->kick);
        } else {
            cdj_c6x_link_kick();
        }
    }
    if (c->prof) {
        c->prof_spi_ns += get_clock() - p0;
    }
    return out;
}

/*
 * DSP-side diagnostics, off by default (a per-packet trace costs throughput):
 *   CDJ_C6X_PCHITS=<pc>[,<pc>...]  execute-packet hit counts, reported at exit
 *   CDJ_C6X_WATCH=<lo>:<hi>[:<after_ms>]  the first 40 stores into [lo, hi]
 */
static void cdj_c6x_pchit_trace(c66x_core *core, void *opaque, uint32_t pc)
{
    CdjC6x *c = opaque;

    for (unsigned i = 0; i < c->pchit_n; i++) {
        if (c->pchit_pc[i] == pc) {
            c->pchit_count[i]++;
            /* B3 (reg 35) is the C6000 return address: the caller. */
            c->pchit_b3[i] = c66x_get_reg(core, 35);
            return;
        }
    }
}

/* CDJ_C6X_SAMPLE=<addr>:<size>[,...]: log DSP RAM words (size 1, 2 or 4) every
 * 100 virtual ms whenever any of them changed.
 * CDJ_C6X_SAMPLE_OPT=<every_ms>:<max_lines>:<after_ms> (default 100:400:0). */
static void cdj_c6x_sample(CdjC6x *c)
{
    int64_t ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    GString *g;
    bool changed = false;

    if (!c->sample_n || ms < c->sample_next_ms || ms < c->sample_after_ms) {
        return;
    }
    c->sample_next_ms = ms + c->sample_every_ms;
    g = g_string_new("");
    for (unsigned i = 0; i < c->sample_n; i++) {
        uint8_t *p = cdj_c6x_ram(c->sample_addr[i], c->sample_size[i]);
        uint32_t v = 0;

        for (unsigned k = 0; p && k < c->sample_size[i]; k++) {
            v |= (uint32_t)p[k] << (8 * k);
        }
        changed |= v != c->sample_last[i];
        c->sample_last[i] = v;
        g_string_append_printf(g, " %08x=%x", c->sample_addr[i], v);
    }
    if (changed && c->sample_logged++ < c->sample_max) {
        info_report("c6x: sample %" PRId64 " ms:%s", ms, g->str);
    }
    g_string_free(g, true);
}

static void cdj_c6x_watch_store(c66x_core *core, void *opaque, uint32_t addr,
                                uint32_t val, unsigned size)
{
    CdjC6x *c = opaque;

    if (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) < c->watch_after_ms) {
        return;
    }
    if (c->watch_stores++ < 40) {
        info_report("c6x: store *0x%08x = 0x%08x (%u) pc 0x%08x at %" PRId64 " ms",
                    addr, val, size, c66x_get_exec_pc(core),
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
}

static void cdj_c6x_diag_arm(CdjC6x *c)
{
    const char *e = getenv("CDJ_C6X_PCHITS");
    unsigned long lo, hi;

    c->pchit_n = 0;
    while (e && *e && c->pchit_n < ARRAY_SIZE(c->pchit_pc)) {
        char *end;

        uint32_t pc = strtoul(e, &end, 0);

        if (end == e) {
            break;
        }
        c->pchit_pc[c->pchit_n] = pc;
        c->pchit_count[c->pchit_n++] = 0;
        e = *end == ',' ? end + 1 : end;
    }
    if (c->pchit_n) {
        c66x_set_trace(c->core, cdj_c6x_pchit_trace, c);
        info_report("c6x: counting %u DSP pcs (per-packet trace on)", c->pchit_n);
    }
    e = getenv("CDJ_C6X_SAMPLE");
    c->sample_n = 0;
    while (e && *e && c->sample_n < ARRAY_SIZE(c->sample_addr)) {
        char *end;
        unsigned long a = strtoul(e, &end, 16), sz = 4;

        if (end == e) {
            break;
        }
        if (*end == ':') {
            sz = strtoul(end + 1, &end, 0);
        }
        c->sample_addr[c->sample_n] = a;
        c->sample_size[c->sample_n++] = sz == 1 || sz == 2 ? sz : 4;
        e = *end == ',' ? end + 1 : end;
    }
    c->sample_every_ms = 100;
    c->sample_max = 400;
    c->sample_after_ms = 0;
    e = getenv("CDJ_C6X_SAMPLE_OPT");
    if (e) {
        unsigned long every = 100, max = 400, after = 0;

        sscanf(e, "%lu:%lu:%lu", &every, &max, &after);
        c->sample_every_ms = every ? every : 100;
        c->sample_max = max;
        c->sample_after_ms = after;
    }
    e = getenv("CDJ_C6X_WATCH");
    c->watch_after_ms = 0;
    if (e && sscanf(e, "%lx:%lx:%" SCNd64, &lo, &hi, &c->watch_after_ms) >= 2) {
        c66x_watch_writes(c->core, lo, hi, cdj_c6x_watch_store, c);
        info_report("c6x: watching stores into 0x%08lx..0x%08lx", lo, hi);
    }
}

/*
 * CDJ_C6X_GPIOLOG=<mask>: log every DSP GPIO set/clear touching <mask>, with
 * the virtual time.
 */
static uint32_t cdj_c6x_gpiolog_mask(void)
{
    static int64_t m = -1;

    if (m < 0) {
        const char *e = getenv("CDJ_C6X_GPIOLOG");

        m = e ? (int64_t)strtoul(e, NULL, 0) : 0;
    }
    return (uint32_t)m;
}

static void cdj_c6x_gpio_setclr(void *opaque, uint32_t mask, int set)
{
    CdjC6x *c = &cdj_c6x;

    if (mask & cdj_c6x_gpiolog_mask()) {
        info_report("c6x gpio: %s 0x%08x at %" PRId64 " ms (pc 0x%08x)",
                    set ? "SET" : "CLR", mask,
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                    c->core ? c66x_get_exec_pc(c->core) : 0);
    }

    /* Per-mask set/clear census. The main loop writes SET_DATA every pass
     * (pc 0x00801808); that write is only idempotent if nothing clears the
     * same pins in between. */
    for (unsigned i = 0; i < ARRAY_SIZE(c->gpio_census); i++) {
        if (!c->gpio_census[i].n[0] && !c->gpio_census[i].n[1]) {
            c->gpio_census[i].mask = mask;
        }
        if (c->gpio_census[i].mask == mask) {
            c->gpio_census[i].n[set ? 1 : 0]++;
            c->gpio_census[i].last_pc[set ? 1 : 0] =
                c->core ? c66x_get_exec_pc(c->core) : 0;
            break;
        }
    }

    if (!set && (mask >> C6X_PIN_BOOTED) & 1 && !c->booted_latch) {
        c->booted_latch = true;
        c->booted_virt_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        info_report("c6x: DSP GPIO25 cleared -- the first stage reports BOOTED "
                    "after %u uPP windows, %.3f s after it started "
                    "(pc 0x%08x, %" PRIu64 " cycles)", c->windows_seen,
                    (c->booted_virt_ns - c->start_virt_ns) / 1e9,
                    c66x_get_exec_pc(c->core), c->cycles);
        if (c->mhz != c->mhz_run) {
            info_report("c6x: CDJ_C6X_MHZ_BOOT: %" PRIu64 " -> %" PRIu64 " MHz",
                        c->mhz, c->mhz_run);
            c->mhz = c->mhz_run;
            c->cyc_rem = 0;
        }
    }
}

/* Pin levels MAIN reads are published here, so the PFC (vCPU thread) never
 * touches the SoC while the DSP thread runs it. */
static void cdj_c6x_gpio_out(void *opaque, unsigned pin, int level)
{
    if (pin == C6X_PIN_READY) {
        qatomic_set(&cdj_c6x.ready_level, level);
        if (!level) {
            cdj_c6x.windows_seen++;
        }
    } else if (pin == C6X_PIN_BOOTED) {
        qatomic_set(&cdj_c6x.booted_level, level);
    }
}

static void cdj_c6x_soc_log(void *opaque, const char *msg)
{
    static unsigned n;

    if (n++ < 64) {
        c66x_core *core = cdj_c6x.core;

        /* PC and call context identify which driver touched an unmodelled
         * register. */
        info_report("c6x: %s  [pc 0x%08x b3 0x%08x a4 0x%08x b4 0x%08x a6 0x%08x "
                    "at %" PRId64 " ms]", msg,
                    core ? c66x_get_exec_pc(core) : 0,
                    core ? c66x_get_reg(core, 32 + 3) : 0,
                    core ? c66x_get_reg(core, 4) : 0,
                    core ? c66x_get_reg(core, 32 + 4) : 0,
                    core ? c66x_get_reg(core, 6) : 0,
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    }
}

static void cdj_c6x_tick(void *opaque);

/* Park the DSP. Returns with no chunk in progress, so L2 may be rewritten. */
static void cdj_c6x_stop(void)
{
    CdjC6x *c = &cdj_c6x;

    qemu_mutex_lock(&c->run_lock);
    qatomic_set(&c->running, false);
    if (c->sync) {
        qemu_cond_broadcast(&c->done_cond);
    }
    qemu_mutex_unlock(&c->run_lock);
    if (c->slack_ns) {
        qemu_sem_post(&c->done_sem);
    }
}

/* Fresh chip, program in L2, PC at the entry: what the ROM does at the end of
 * an I2C boot. A new core each time, so no predecode survives a reload. */
static void cdj_c6x_start(uint32_t entry)
{
    CdjC6x *c = &cdj_c6x;
    c66x_bus bus = { c, cdj_c6x_bus_read, cdj_c6x_bus_write };
    c6655_soc_config cfg = {
        .mem = { c, cdj_c6x_bus_read, cdj_c6x_bus_write },
        .opaque = c,
        .spi_xfer = cdj_c6x_spi_xfer,
        .mcbsp_tx = cdj_c6x_mcbsp_tx,
        .gpio_out = cdj_c6x_gpio_out,
        .gpio_setclr = cdj_c6x_gpio_setclr,
        .log = cdj_c6x_soc_log,
    };

    qemu_mutex_lock(&c->run_lock);
    if (c->soc) {
        c6655_soc_free(c->soc);
    }
    if (c->core) {
        c66x_free(c->core);
    }
    c->core = c66x_new(&bus);
    /* CDJ_C6X_RECORD=<path>: record everything the core receives, for
     * c6x/c6xreplay (offline benchmark and bit-exact check of the core). */
    if (c66x_record_open(c->core, getenv("CDJ_C6X_RECORD"))) {
        warn_report("c6x: cannot open CDJ_C6X_RECORD %s", getenv("CDJ_C6X_RECORD"));
    }
    c66x_map_ram(c->core, C6X_L2_BASE, C6X_L2_SIZE, c->l2);
    c66x_map_ram(c->core, C6X_L2_ALIAS, C6X_L2_SIZE, c->l2);
    c66x_map_ram(c->core, C6X_DDR_BASE, C6X_DDR_SIZE, c->ddr);
    {
        /* CDJ_C6X_IDLE=<head>:<stack_lo>:<stack_hi>[:<reads>], "0" off: the
         * core's busy-wait skip for the app's polling main loop. The default
         * stack range is the 64 KB below the data page stage 1 sets (B15
         * 0x008BC668, B14 0x008BC670); a range that included a global would
         * make a real store look idle. */
        const char *idle = getenv("CDJ_C6X_IDLE");
        unsigned long h = 0x80076F00, lo = 0x008AC668, hi = 0x008BC670;
        int reads = 0;

        if (idle && !strcmp(idle, "0")) {
            h = 0;
        } else if (idle) {
            sscanf(idle, "%lx:%lx:%lx:%d", &h, &lo, &hi, &reads);
        }
        if (h) {
            c66x_set_idle_loop(c->core, h, lo, hi, reads);
            /* CDJ_C6X_IDLE_IDEMP=1: treat the main loop's GPIO SET_DATA write
             * (0x02320018, pc 0x00801808) as side-effect free. Default off. */
            if (getenv("CDJ_C6X_IDLE_IDEMP") &&
                strcmp(getenv("CDJ_C6X_IDLE_IDEMP"), "0")) {
                c66x_idle_idempotent_write(c->core, 0x02320018);
            }
        }
    }
    cdj_c6x_diag_arm(c);
    cfg.core = c->core;
    c->soc = c6655_soc_new(&cfg);
    c6655_soc_gpio_set_input(c->soc, C6X_PIN_ACK, c->ack);
    /* An undriven input reads high, but READY must read low until the first
     * stage drives it, or MAIN ships all its windows before the stage runs. */
    c6655_soc_gpio_set_input(c->soc, C6X_PIN_READY, 0);
    c66x_reset(c->core, entry);

    qemu_mutex_lock(&c->link_lock);
    c->rx_len = 0;
    c->tx_count = 0;
    c->tx_cur = -1;
    c->wpos = 0;
    qemu_mutex_unlock(&c->link_lock);
    qatomic_set(&c->ready_level, 0);
    qatomic_set(&c->booted_level, 1);
    c->booted_latch = false;
    c->windows_seen = 0;
    c->cycles = 0;
    c->cyc_rem = 0;
    c->halted = false;
    c->start_virt_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    c->epoch_ns = c->start_virt_ns;
    c->sync_target = 0;
    qatomic_set(&c->dsp_now_pub, 0);
    c->report_at = c->start_virt_ns + 5 * NANOSECONDS_PER_SECOND;
    qatomic_set(&c->running, true);
    qemu_mutex_unlock(&c->run_lock);
    info_report("c6x: DSP released at 0x%08x, %" PRIu64 " MHz on the virtual "
                "clock, %" PRId64 " us quantum, %s", entry, c->mhz,
                c->quantum_ns / 1000,
                c->threaded ? "own thread" : "stepped in the main loop");
    if (!c->threaded || c->sync) {
        timer_mod(c->tick, c->start_virt_ns + c->quantum_ns);
    }
}

static void cdj_c6x_rom_reset(void)
{
    CdjC6x *c = &cdj_c6x;

    g_byte_array_set_size(c->i2c, 0);
    g_byte_array_set_size(c->table, 0);
    c->i2c_blocks = 0;
    c->loaded = false;
}

/* MAIN's checksum at 0x08326474: end-around-carry sum of BE u16, inverted. */
static uint16_t cdj_c6x_sum16(const uint8_t *p, size_t n)
{
    uint32_t s = 0;

    for (size_t i = 0; i + 1 < n; i += 2) {
        s += (uint32_t)p[i] << 8 | p[i + 1];
        s = (s & 0xFFFF) + (s >> 16);
    }
    if (n & 1) {
        s += (uint32_t)p[n - 1] << 8;
        s = (s & 0xFFFF) + (s >> 16);
    }
    return ~s & 0xFFFF;
}

static uint32_t cdj_c6x_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

/* The accumulated payload as a big-endian TI boot table. Returns true once it
 * walks to its terminator, having loaded every section. */
static bool cdj_c6x_try_boot_table(void)
{
    CdjC6x *c = &cdj_c6x;
    const uint8_t *t = c->table->data;
    size_t len = c->table->len, p = 4;
    uint32_t entry;
    unsigned nsec = 0;

    if (len < 8) {
        return false;
    }
    entry = cdj_c6x_be32(t);
    for (;;) {
        uint32_t size, padded;

        if (p + 4 > len) {
            return false;
        }
        size = cdj_c6x_be32(t + p);
        if (!size) {
            break;
        }
        if (p + 8 > len) {
            return false;
        }
        padded = (size + 3) & ~3u;
        if (p + 8 + padded > len) {
            return false;
        }
        p += 8 + padded;
        nsec++;
    }
    /* Complete: load it. Each 32-bit big-endian word is stored as the value the
     * ROM writes into little-endian memory. */
    cdj_c6x_stop();
    memset(c->l2, 0, C6X_L2_SIZE);
    p = 4;
    for (unsigned k = 0; k < nsec; k++) {
        uint32_t size = cdj_c6x_be32(t + p);
        uint32_t addr = cdj_c6x_be32(t + p + 4);
        uint32_t padded = (size + 3) & ~3u;

        for (uint32_t w = 0; w < padded; w += 4) {
            uint8_t *m = cdj_c6x_ram(addr + w, 4);
            uint32_t v = cdj_c6x_be32(t + p + 8 + w);

            if (m) {
                m[0] = v; m[1] = v >> 8; m[2] = v >> 16; m[3] = v >> 24;
            }
        }
        info_report("c6x: ROM I2C boot: section 0x%08x..0x%08x (%u bytes)",
                    addr, addr + size, size);
        p += 8 + padded;
    }
    info_report("c6x: ROM I2C boot: %u blocks (%u bad checksums), %u sections, "
                "entry 0x%08x, PTH0 %s", c->i2c_blocks, c->i2c_bad_ck, nsec,
                entry, c->pth0 ? "high" : "LOW");
    c->loaded = true;
    cdj_c6x_start(entry);
    return true;
}

/* A byte MAIN wrote to IIC1 slave 0x30. Blocks are self-delimiting
 * (u16 BE length including the header, u16 BE ~sum16), so the stream needs no
 * start/stop bookkeeping. The first two are parameter tables. */
void cdj_c6x_i2c_byte(uint8_t v)
{
    CdjC6x *c = &cdj_c6x;
    unsigned blen;

    if (!c->on || c->preload) {
        return;
    }
    if (c->loaded) {
        cdj_c6x_rom_reset();        /* a second upload: the ROM starts over */
    }
    g_byte_array_append(c->i2c, &v, 1);
    if (c->i2c->len < 4) {
        return;
    }
    blen = c->i2c->data[0] << 8 | c->i2c->data[1];
    if (blen < 4) {
        g_byte_array_set_size(c->i2c, 0);
        return;
    }
    if (c->i2c->len < blen) {
        return;
    }
    {
        uint16_t ck = c->i2c->data[2] << 8 | c->i2c->data[3];

        c->i2c->data[2] = c->i2c->data[3] = 0;
        if (cdj_c6x_sum16(c->i2c->data, blen) != ck) {
            c->i2c_bad_ck++;
        }
    }
    if (c->i2c_blocks >= 2) {
        g_byte_array_append(c->table, c->i2c->data + 4, blen - 4);
    }
    c->i2c_blocks++;
    g_byte_array_set_size(c->i2c, 0);
    if (c->i2c_blocks > 2) {
        cdj_c6x_try_boot_table();
    }
}

static bool cdj_c6x_preload_stage1(void)
{
    static const struct { const char *file; uint32_t addr; } s1[] = {
        { "s00_00800000.bin", 0x00800000 },
        { "s01_00800200.bin", 0x00800200 },
        { "s02_00812ae8.bin", 0x00812ae8 },
    };
    const char *dir = getenv("CDJ_C6X_STAGE1");

    cdj_c6x_stop();
    memset(cdj_c6x.l2, 0, C6X_L2_SIZE);
    for (unsigned i = 0; i < ARRAY_SIZE(s1); i++) {
        g_autofree char *path = g_strdup_printf("%s/%s",
                                                dir ? dir : "lab/c3_out/stage1",
                                                s1[i].file);
        g_autofree gchar *data = NULL;
        gsize n;

        if (!g_file_get_contents(path, &data, &n, NULL) || n > C6X_L2_SIZE) {
            warn_report("c6x: cannot read the first stage %s", path);
            return false;
        }
        memcpy(cdj_c6x.l2 + (s1[i].addr - C6X_L2_BASE), data, n);
    }
    return true;
}

/* PTH0 and PVDR.1, after the PFC has stored the byte. */
void cdj_c6x_pfc_write(const uint8_t *reg)
{
    CdjC6x *c = &cdj_c6x;
    int pth0 = reg[C6X_PFC_PHDR] & 0x01;
    int ack = !!(reg[C6X_PFC_PVDR] & 0x02);

    if (pth0 != c->pth0) {
        c->pth0 = pth0;
        info_report("c6x: PTH0 (DSP reset) -> %s at %" PRId64 " ms", pth0 ? "high" : "low",
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        if (!pth0) {
            cdj_c6x_stop();
        }
        cdj_c6x_rom_reset();
        if (pth0 && c->preload && cdj_c6x_preload_stage1()) {
            cdj_c6x_start(0x00800200);
        }
    }
    if (ack != c->ack) {
        c->ack = ack;
        qemu_mutex_lock(&c->run_lock);
        if (c->soc) {
            c6655_soc_gpio_set_input(c->soc, C6X_PIN_ACK, ack);
        }
        qemu_mutex_unlock(&c->run_lock);
    }
}

/* What MAIN reads on the three DSP input lines. */
uint8_t cdj_c6x_pfc_read(hwaddr off, uint8_t b)
{
    CdjC6x *c = &cdj_c6x;

    if (off == C6X_PFC_PUDR) {
        b &= ~0x08;
        if (qatomic_read(&c->running) && qatomic_read(&c->ready_level)) {
            b |= 0x08;
        }
    } else if (off == C6X_PFC_PZDR) {
        b |= 0x40;
        if (qatomic_read(&c->running) && qatomic_read(&c->booted_latch) &&
            !qatomic_read(&c->booted_level)) {
            b &= ~0x40;
        }
    }
    return b;
}

/* Every word MAIN's DMA1 ch3 put on CS6, forwarded by the FPGA into uPP. The
 * FPGA takes it at once; MAIN does not wait on this path. */
void cdj_c6x_upp_ship(uint32_t sar, uint32_t bytes)
{
    CdjC6x *c = &cdj_c6x;
    g_autofree uint8_t *buf = NULL;

    if (!bytes || bytes > 1 * MiB) {
        return;
    }
    buf = g_malloc(bytes);
    cpu_physical_memory_read(A7ADDR(sar), buf, bytes);
    /*
     * Delivered at DMA completion, not queued behind the DSP thread: MAIN's
     * next request (the 0x40/0x18 record arm) must not reach a DSP that has
     * not seen the data, or it answers busy and parks for good at 0x800744FA.
     * Taking run_lock waits out the current chunk; MAIN's frames are then held
     * back for ship_hold_ns of DSP time so the uPP ISR runs first.
     */
    if (c->ship_async && c->soc) {
        qemu_mutex_lock(&c->link_lock);
        g_queue_push_tail(c->upp_in, g_byte_array_new_take(g_steal_pointer(&buf),
                                                           bytes));
        c->ship_pending++;
        qemu_mutex_unlock(&c->link_lock);
        c->upp_bytes += bytes;
        c->upp_ships++;
        return;
    }
    {
        int64_t w0 = get_clock();

        qemu_mutex_lock(&c->run_lock);
        c->ship_wait_ns += get_clock() - w0;
    }
    if (c->soc) {
        size_t q = c6655_soc_upp_rx(c->soc, buf, bytes);

        if (q > c->upp_peak_queue) {
            c->upp_peak_queue = q;
        }
        qemu_mutex_lock(&c->link_lock);
        c->tx_hold_until_ns = c6655_soc_now_ns(c->soc) + c->ship_hold_ns;
        qemu_mutex_unlock(&c->link_lock);
        if (c->upp_ships >= 87 && c->ship_logged < 120) {
            c->ship_logged++;
            info_report("c6x: ship %" PRIu64 " landed %u B at %" PRId64 " ms "
                        "(DSP time %.3f s, lag %.1f ms, queued %zu, dropped %zu)",
                        c->upp_ships, bytes, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                        c6655_soc_now_ns(c->soc) / 1e9,
                        (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - c->epoch_ns -
                         (int64_t)c6655_soc_now_ns(c->soc)) / 1e6, q,
                        c6655_soc_upp_dropped(c->soc));
        }
    } else {
        qemu_mutex_lock(&c->link_lock);
        g_queue_push_tail(c->upp_in, g_byte_array_new_take(g_steal_pointer(&buf),
                                                           bytes));
        qemu_mutex_unlock(&c->link_lock);
    }
    qemu_mutex_unlock(&c->run_lock);
    c->upp_bytes += bytes;
    c->upp_ships++;
}

/*
 * CDJ_C6X_RX_PACE engages only once the link is up: the boot exchange is
 * timing-sensitive and the beat position only matters during playback. Runs
 * on the vCPU thread, rx_goodframes' only writer.
 */
static bool cdj_c6x_pacing(CdjC6x *c)
{
    return c->rx_pace && c->rx_goodframes >= C6X_PACE_AFTER_FRAMES;
}

/*
 * 0 when the first `want` queued words may go to MAIN at virtual time `virt`,
 * else the virtual time they may. The last word decides, because MAIN's
 * transfer completes on it. Caller holds link_lock and has checked
 * rx_len >= want >= 1. epoch_ns never slips in sync mode.
 */
static int64_t cdj_c6x_rx_hold_until(CdjC6x *c, uint32_t want, int64_t virt)
{
    int64_t due;

    if (!cdj_c6x_pacing(c)) {
        return 0;
    }
    due = c->epoch_ns +
          (int64_t)c->rx_stamp[(c->rx_head + want - 1) % C6X_RXQ] -
          (int64_t)c->rx_pace_grain_ns;
    return due > virt ? due : 0;
}

/* A ch5 transfer MAIN armed on MSIOF0's receive FIFO. True when it completed. */
bool cdj_c6x_link_rx(CdjDma1State *s, unsigned ch, uint32_t dar,
                            unsigned dm, uint32_t count, uint32_t *dar_out)
{
    CdjC6x *c = &cdj_c6x;

    uint16_t first = 0, last = 0;
    uint16_t fr[64];
    /* Read only when pacing, so the default path is untouched. */
    int64_t virt = c->rx_pace ? qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) : 0;
    int64_t hold;

    c->dma = s;
    c->rx_ch = ch;
    qemu_mutex_lock(&c->link_lock);
    if (c->rx_len < count) {
        c->rx_want = count;
        qemu_mutex_unlock(&c->link_lock);
        return false;
    }
    hold = cdj_c6x_rx_hold_until(c, count, virt);
    if (hold) {
        c->rx_want = count;
        c->rx_held++;
        qemu_mutex_unlock(&c->link_lock);
        timer_mod(c->rx_timer, hold);
        return false;
    }
    if (cdj_c6x_pacing(c) && count > 1) {
        int64_t late = virt - c->epoch_ns -
                       (int64_t)c->rx_stamp[(c->rx_head + count - 1) % C6X_RXQ];

        if (late > 1000000) {
            c->rx_late++;
        }
        if (late > 0 && (uint64_t)late > c->rx_late_max_ns) {
            c->rx_late_max_ns = late;
        }
    }
    for (uint32_t i = 0; i < count; i++) {
        uint16_t w = c->rxq[c->rx_head];
        uint8_t le[2] = { w & 0xFF, w >> 8 };

        if (i < 64) {
            fr[i] = w;
        }
        if (!i) {
            first = w;
        }
        if (i == 1 && count > 1) {
            c->rx_kind[(w & 0xFF) & 0x3F]++;
        }
        last = w;
        if (w == 0xAACC) {
            c->rx_aacc++;
        }
        c->rx_head = (c->rx_head + 1) % C6X_RXQ;
        c->rx_len--;
        cpu_physical_memory_write(A7ADDR(dar), le, 2);
        if (dm == 1) {
            dar += 2;
        }
    }
    if (count == 1) {
        c->rx_arm1++;
    } else {
        c->rx_armn++;
        if (first == 0x5533 && last == 0xAACC) {
            c->rx_goodframes++;
        }
        /* The DSP's answer to a lane-A 0x0AF0 read: region 2 of the frame,
         * after the kind's status vector. */
        if (first == 0x5533 && count >= 8 && count <= 64) {
            static const unsigned vec[7] = { 0, 32, 42, 52, 38, 4, 20 };
            unsigned kind = fr[1] & 0x3F, len2 = fr[1] >> 8;
            unsigned r = 2 + (kind < 7 ? vec[kind] : 0);

            /* id-1 p[10] is the DSP->MAIN event word (DSP 0x008FFFE8); the
             * handler latches the first non-zero one and fires event flag 1. */
            /* id-1 p[2]/p[3] fill 0x0994416C/0x09944170 (DSP idx 0x154/0x158),
             * whose sum *(0x09944220) the manager's play loop waits on.
             * Logged on change. */
            if (kind == 1 && count > 5 &&
                (fr[4] != c->id1_p23[0] || fr[5] != c->id1_p23[1])) {
                c->id1_p23[0] = fr[4];
                c->id1_p23[1] = fr[5];
                if (c->id1_p23_logged++ < 40) {
                    info_report("c6x: id1 p[2] 0x%04x p[3] 0x%04x at %" PRId64 " ms",
                                fr[4], fr[5], qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
                }
            }
            if (kind == 1 && count > 12 && fr[12]) {
                c->id1_p10_nonzero++;
                if (c->id1_p10_nonzero <= 20) {
                    info_report("c6x: id1 p[10] = 0x%04x at %" PRId64 " ms (#%" PRIu64
                                ")", fr[12], qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                                c->id1_p10_nonzero);
                }
            }
            /* Every RPC reply after a watched read: kind, tag and length, to
             * show a client parked on a tag that never comes back. */
            if ((c->watch_tag & 0x80000000u) && len2 >= 4 && r + len2 <= count &&
                c->watch_logged < 60) {
                c->watch_logged++;
                info_report("c6x: after-watch reply kind %u tag 0x%04x len2 %u "
                            "val 0x%08x at %" PRId64 " ms%s", kind, fr[r + 1], len2,
                            fr[r + 2] | (uint32_t)fr[r + 3] << 16,
                            qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                            fr[r + 1] == (c->watch_tag & 0xFFFF) ? "  <-- the watched tag" : "");
            }
            if ((kind == 4 || kind == 5) && c->rx_kind[kind] <= 10) {
                GString *g = g_string_new("");

                for (unsigned k = 2; k < MIN(count, 2u + vec[kind] + 12); k++) {
                    g_string_append_printf(g, " %04x", fr[k]);
                }
                info_report("c6x: reply id%u #%" PRIu64 " at %" PRId64 " ms "
                            "(len2 %u):%s", kind, c->rx_kind[kind],
                            qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), len2, g->str);
                g_string_free(g, true);
            }
            if (kind && kind < 7 && len2 >= 4 && r + len2 <= count) {
                for (unsigned k = 0; k < 16; k++) {
                    if (c->arm_tag[k] && (c->arm_tag[k] & 0xFFFF) == fr[r + 1]) {
                        info_report("c6x: arm reply %" PRId64 " ms tag 0x%04x -> "
                                    "0x%08x (%s)",
                                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), fr[r + 1],
                                    fr[r + 2] | (uint32_t)fr[r + 3] << 16,
                                    fr[r + 2] ? "BUSY" : "armed");
                        c->arm_tag[k] = 0;
                    }
                }
                for (unsigned k = 0; k < 16; k++) {
                    uint32_t t = c->lane_a_read_tag[k];

                    if ((t & 0x80000000u) && (t & 0xFFFF) == fr[r + 1]) {
                        unsigned ai = (t >> 16) & 0x7FFF;
                        unsigned o = r + 2 + 2 * ai;
                        uint32_t v = o + 1 < r + len2
                                     ? fr[o] | (uint32_t)fr[o + 1] << 16 : 0;
                        uint8_t *gate = cdj_c6x_ram(0x008BCCE8, 1);

                        info_report("c6x: laneA %" PRId64 " ms tag 0x%04x reply "
                                    "0x0AF0 -> 0x%08x  (DSP b14+1656 = 0x%02x)",
                                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                                    fr[r + 1], v, gate ? *gate : 0);
                        c->lane_a_read_tag[k] = 0;
                    }
                }
            }
        }
        if (c->rx_armn <= 6) {
            info_report("c6x: MAIN read a %u-word frame: first 0x%04x last 0x%04x "
                        "at %" PRId64 " ms", count, first, last,
                        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    }
    c->rx_want = 0;
    qemu_mutex_unlock(&c->link_lock);
    *dar_out = dar;
    return true;
}

/*
 * Lane A: every write to 0x0AF0/0x0AF4/0x0B18 (and neighbours) and every read
 * of 0x0AF0, in order, with the DSP-side gate byte at b14+1656 (0x008BCCE8)
 * that lane-A commands are checked against. Payload: [type][tag][u32 args].
 */
static bool cdj_c6x_lane_a(uint32_t idx)
{
    return idx == 0x0AF0 || idx == 0x0AF4 || idx == 0x0B18 || idx == 0x0B1C ||
           idx == 0x0B74 || idx == 0x0AFC || idx == 0x0B08 || idx == 0x0B0C ||
           (idx >= 0x0B84 && idx <= 0x0B8C) || (idx >= 0x0AB0 && idx <= 0x0AD4);
}

static void cdj_c6x_log_lane_a(const uint16_t *w, unsigned n)
{
    static unsigned logged;
    const uint16_t *pl;
    unsigned len, type, tag, nargs, i;
    uint32_t a[32];
    uint8_t *gate = cdj_c6x_ram(0x008BCCE8, 1);
    int64_t ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

    if (n < 4 || w[0] != 0x5533 || logged >= 20000) {
        return;
    }
    len = w[1] >> 8;
    pl = w + 2 + ((w[1] & 0xFF) ? 8 : 0);
    if (pl + len > w + n || len < 2) {
        return;
    }
    type = pl[0];
    tag = pl[1];
    nargs = MIN((len - 2) / 2, (unsigned)ARRAY_SIZE(a));
    for (i = 0; i < nargs; i++) {
        a[i] = pl[2 + 2 * i] | (uint32_t)pl[3 + 2 * i] << 16;
    }
    /* Every list request, whole, so what MAIN polls is visible too. */
    if (type >= 2 && type <= 10 && logged < 20000) {
        GString *g = g_string_new("");

        for (i = 0; i < nargs; i++) {
            g_string_append_printf(g, " %x", a[i]);
        }
        logged++;
        info_report("c6x: req %" PRId64 " ms tag 0x%04x type %u args:%s", ms,
                    tag, type, g->str);
        g_string_free(g, true);
    }
#define LANE_A_LOG(what, idx, val)                                            \
    do {                                                                      \
        logged++;                                                             \
        info_report("c6x: laneA %" PRId64 " ms tag 0x%04x type %u %s 0x%04x = "\
                    "0x%08x  (DSP b14+1656 = 0x%02x)", ms, tag, type, what,   \
                    (idx), (val), gate ? *gate : 0);                          \
    } while (0)
    switch (type) {
    case 2:
        for (i = 0; i + 1 < nargs; i += 2) {
            if (cdj_c6x_lane_a(a[i])) {
                LANE_A_LOG("write", a[i], a[i + 1]);
            }
        }
        break;
    case 3:
        for (i = 1; i < nargs; i++) {
            if (nargs && cdj_c6x_lane_a(a[0] + 4 * (i - 1))) {
                LANE_A_LOG("write", a[0] + 4 * (i - 1), a[i]);
            }
        }
        break;
    case 10:
        for (i = 0; i + 1 < nargs;) {
            uint32_t idx = a[i], cnt = a[i + 1];

            for (uint32_t k = 0; k < cnt && i + 2 + k < nargs; k++) {
                if (cdj_c6x_lane_a(idx + 4 * k)) {
                    LANE_A_LOG("write", idx + 4 * k, a[i + 2 + k]);
                }
            }
            i += 2 + cnt;
        }
        break;
    case 6:
    case 12:
        cdj_c6x.arm_tag[cdj_c6x.arm_n++ % 16] = tag | 0x80000000u;
        break;
    case 4:
        /* The manager's load handler parks after reading idx 0x377E0 (the
         * DSP's top-of-L2 output-enable word); trace the reply tag. */
        for (i = 0; i < nargs; i++) {
            if (a[i] >= 0x30000) {
                cdj_c6x.watch_tag = tag | 0x80000000u;
                cdj_c6x.watch_logged = 0;
                info_report("c6x: watch read idx 0x%x tag 0x%04x at %" PRId64 " ms",
                            a[i], tag, ms);
            }
        }
        for (i = 0; i < nargs; i++) {
            if (a[i] == 0x0AF0 || a[i] == 0x0B1C || a[i] == 0x0B14) {
                LANE_A_LOG("read ", a[i], 0);
                cdj_c6x.lane_a_read_tag[cdj_c6x.lane_a_read_n++ % 16] =
                    tag | (i << 16) | 0x80000000u;
            }
        }
        break;
    default:
        break;
    }
#undef LANE_A_LOG
}

/* A ch4 transfer MAIN pointed at MSIOF0's transmit FIFO: one frame. */
bool cdj_c6x_link_tx(CdjDma1State *s, unsigned ch, uint32_t sar,
                            unsigned sm, uint32_t count, uint32_t *sar_out)
{
    CdjC6x *c = &cdj_c6x;
    unsigned slot;

    c->dma = s;
    c->tx_ch = ch;
    qemu_mutex_lock(&c->link_lock);
    if (c->tx_count >= C6X_TXFRAMES) {
        c->tx_waiting = true;
        qemu_mutex_unlock(&c->link_lock);
        return false;
    }
    slot = (c->tx_head + c->tx_count) % C6X_TXFRAMES;
    c->txf_len[slot] = MIN(count, (uint32_t)C6X_TXWORDS);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t le[2];

        cpu_physical_memory_read(A7ADDR(sar), le, 2);
        if (i < C6X_TXWORDS) {
            c->txf[slot][i] = le[0] | le[1] << 8;
        }
        if (sm == 1) {
            sar += 2;
        }
    }
    if (c->txf_len[slot]) {
        cdj_c6x_log_lane_a(c->txf[slot], c->txf_len[slot]);
        c->tx_count++;
        c->tx_frames++;
        if (c->tx_frames <= 40 || c->tx_frames % 500 == 0) {
            GString *g = g_string_new("");

            for (unsigned i = 0; i < MIN(c->txf_len[slot], 16u); i++) {
                g_string_append_printf(g, " %04x", c->txf[slot][i]);
            }
            info_report("c6x: MAIN frame %" PRIu64 " (%u hw) at %" PRId64 " ms:%s",
                        c->tx_frames, c->txf_len[slot],
                        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), g->str);
            g_string_free(g, true);
        }
    }
    c->tx_waiting = false;
    qemu_mutex_unlock(&c->link_lock);
    *sar_out = sar;
    return true;
}

/* A transfer MAIN is waiting on may now be able to finish. */
static void cdj_c6x_link_kick(void)
{
    CdjC6x *c = &cdj_c6x;

    bool rx, tx;
    int64_t hold = 0;

    if (!c->dma) {
        return;
    }
    qemu_mutex_lock(&c->link_lock);
    rx = c->rx_want && c->rx_len >= c->rx_want;
    if (rx && c->rx_pace) {
        hold = cdj_c6x_rx_hold_until(c, c->rx_want,
                                     qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
        rx = !hold;
    }
    tx = c->tx_waiting && c->tx_count < C6X_TXFRAMES;
    qemu_mutex_unlock(&c->link_lock);
    if (hold) {
        timer_mod(c->rx_timer, hold);
    }
    if (rx && c->dma->pending[c->rx_ch]) {
        cdj_dma1_run(c->dma, c->rx_ch);
    }
    if (tx && c->dma->pending[c->tx_ch]) {
        cdj_dma1_run(c->dma, c->tx_ch);
    }
}

static void cdj_c6x_kick_bh(void *opaque)
{
    cdj_c6x_link_kick();
}

static void cdj_c6x_report(const char *why)
{
    CdjC6x *c = &cdj_c6x;
    c66x_stats st = { 0 };
    int64_t virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    double run_s = c->running || c->core ? (virt - c->start_virt_ns) / 1e9 : 0;

    if (c->core) {
        c66x_get_stats(c->core, &st);
    }
    info_report("c6x[%s]: %.1f s run, %" PRIu64 " cycles (%.1f M/virtual s, "
                "%.1f M/host s), pc 0x%08x, stop %d, irqs %" PRIu64
                " [INT4 %u INT5 %u INT6 %u INT7 %u INT8 %u INT9 %u], "
                "slipped %.3f s, idle skips %" PRIu64 ", windows %u, "
                "uPP queued %zu, INTC EVTFLAG stores %" PRIu64 ", booted %s",
                why, run_s, c->cycles,
                run_s > 0 ? c->cycles / run_s / 1e6 : 0,
                c->host_ns ? c->cycles / (c->host_ns / 1e9) / 1e6 : 0,
                c->core ? c66x_get_exec_pc(c->core) : 0, (int)c->last_stop,
                st.interrupts, st.irq_edges[4], st.irq_edges[5], st.irq_edges[6],
                st.irq_edges[7], st.irq_edges[8], st.irq_edges[9],
                c->slipped_ns / 1e9, c->idle_skips, c->windows_seen,
                c->soc ? c6655_soc_upp_queued(c->soc) : 0,
                c->intc_flag_writes, c->booted_latch ? "yes" : "no");
    if (c->prof) {
        static int64_t wall0, virt0;
        int64_t wall = get_clock_realtime();

        if (!wall0) {
            wall0 = wall;
            virt0 = virt;
        }
        info_report("c6x[%s] prof: virtual/wall %.3f; run_to %.2f s host (core step "
                    "%.2f s in %" PRIu64 " calls, %.0f cycles/call; SoC advance "
                    "%.2f s); McBSP sink %.2f s, SPI %.2f s, DMA invalidates %.2f s",
                    why, wall > wall0 ? (double)(virt - virt0) / (wall - wall0) : 0,
                    c->host_ns / 1e9, c->prof_step_ns / 1e9, c->prof_steps,
                    c->prof_steps ? (double)c->cycles / c->prof_steps : 0,
                    c->prof_soc_ns / 1e9, c->prof_pcm_ns / 1e9,
                    c->prof_spi_ns / 1e9, c->prof_inval_ns / 1e9);
    }
    if (c->sync) {
        /* Cumulative; difference consecutive lines for per-interval values. */
        info_report("c6x[%s] sync: virt %.3f wall %.3f MAIN waited %.3f vCPU cpu %.3f "
                    "DSP cpu %.3f DSP idle %.3f DSP run_to %.3f cycles %" PRIu64
                    " quanta %" PRIu64 " ships %" PRIu64 " ship wait %.3f",
                    why, virt / 1e9, get_clock_realtime() / 1e9,
                    c->sync_waits_ns / 1e9, c->vcpu_cpu_ns / 1e9,
                    qatomic_read(&c->dsp_cpu_ns) / 1e9, c->dsp_idle_ns / 1e9,
                    c->host_ns / 1e9, c->cycles, c->chunks_run, c->upp_ships,
                    c->ship_wait_ns / 1e9);
    }
    info_report("c6x[%s]: link DSP->MAIN %" PRIu64 " words in %" PRIu64
                " exchanges, %" PRIu64 " dropped, %u queued; MAIN->DSP %" PRIu64
                " frames queued, %" PRIu64 " sent, %" PRIu64 " idle words; "
                "other CS %" PRIu64 " words; uPP %" PRIu64 " bytes in %" PRIu64
                " ships (peak queue %" PRIu64 "); McBSP0 %" PRIu64 " words",
                why, c->rx_words, c->rx_windows, c->rx_dropped, c->rx_len,
                c->tx_frames, c->tx_frames_sent, c->tx_idle_words,
                c->other_cs_words, c->upp_bytes, c->upp_ships,
                c->upp_peak_queue, c->pcm_words[0]);
    if (c->rx_pace) {
        /* Cumulative. late = a multi-word frame MAIN got over 1 ms after its
         * DSP time. */
        info_report("c6x[%s] rx pace: held %" PRIu64 " late %" PRIu64
                    " late max %.3f ms", why, c->rx_held, c->rx_late,
                    c->rx_late_max_ns / 1e6);
    }
    if (c->core) {
        c66x_idle_info ii;
        int64_t vs = virt - c->start_virt_ns;

        c66x_get_idle_info(c->core, &ii);
        info_report("c6x[%s]: idle loop: %" PRIu64 " iterations, %" PRIu64
                    " fixed points, %.3f s of %.3f s skipped; last non-idle "
                    "reason %u at 0x%08x (pc 0x%08x); host %.1f s for the DSP "
                    "(%.2f of virtual)", why, ii.iterations, ii.idle_hits,
                    c->idle_skipped_ns / 1e9, vs / 1e9, ii.last_reason,
                    ii.last_addr, ii.last_pc, c->host_ns / 1e9,
                    vs > 0 ? (double)c->host_ns / vs : 0);
        {
            char jr[256];

            c66x_jit_report(c->core, jr, sizeof jr);
            if (jr[0]) {
                info_report("c6x[%s]: jit: %s", why, jr);
            }
        }
    }
    /* MAIN's own view of the DSP link and the DSP's shared struct. */
    {
        uint32_t up = 0, frames = 0, st1 = 0, lvl = 0, stat = 0;
        uint16_t ver[2] = { 0, 0 };

        cpu_physical_memory_read(0x0995A138, &up, 4);
        cpu_physical_memory_read(0x09959F20, &frames, 4);
        cpu_physical_memory_read(0x0A35F71C, &st1, 4);
        cpu_physical_memory_read(0x09944170, &lvl, 4);
        cpu_physical_memory_read(0x09944168, &stat, 4);
        cpu_physical_memory_read(0x10DBFC7C, ver, 4);
        {
            uint8_t *s158 = cdj_c6x_ram(0x008C8800 + 0x158, 4);
            uint8_t *b34 = cdj_c6x_ram(0x008C8800 + 0xB34, 4);
            uint8_t *b54 = cdj_c6x_ram(0x008C8800 + 0xB54, 4);
            uint8_t *b74 = cdj_c6x_ram(0x008C8800 + 0xB74, 4);
            uint8_t *af0 = cdj_c6x_ram(0x008C8800 + 0xAF0, 4);
            uint8_t *rec = cdj_c6x_ram(0x008D8F94, 4);
            /* b14+1156: the decoded-frame counter. */
            uint8_t *frm = cdj_c6x_ram(0x008BC670 + 1156, 4);
            uint8_t *s154 = cdj_c6x_ram(0x008C8800 + 0x154, 4);
            /* outMode handshake: MAIN writes idx 0, the audio-buffer routine
             * 0x00809DE4 latches the mode, publishes idx 0x58 and clears
             * idx 0; 0x008BCA4C is the play sub-state. */
            uint8_t *i00 = cdj_c6x_ram(0x008C8800, 4);
            uint8_t *i58 = cdj_c6x_ram(0x008C8800 + 0x58, 4);
            uint8_t *mode = cdj_c6x_ram(0x008BCC74, 4);
            uint8_t *psub = cdj_c6x_ram(0x008BCA4C, 4);
#define RD32(p) ((p) ? (p)[0] | (p)[1] << 8 | (p)[2] << 16 | (uint32_t)(p)[3] << 24 : 0)
            info_report("c6x[%s]: DSP struct: 0x154 0x%08x 0x158 0x%08x 0x0B34 "
                        "0x%08x 0x0B54 0x%08x 0x0B74 0x%08x 0x0AF0 0x%08x record[1] "
                        "0x%08x decoded frames 0x%08x; nonzero PCM words %" PRIu64
                        "; parked at 0x800744FA %s",
                        why, RD32(s154), RD32(s158), RD32(b34), RD32(b54), RD32(b74),
                        RD32(af0), RD32(rec), RD32(frm), c->pcm_nonzero,
                        c->core && (c66x_get_exec_pc(c->core) & ~0xFu) ==
                        0x800744F0u ? "YES" : "no");
            info_report("c6x[%s]: outMode: idx0 0x%08x idx0x58 0x%08x latched mode "
                        "0x%08x play sub-state 0x%08x; L2 top block E0 0x%08x E4 "
                        "0x%08x E8 0x%08x; id1 p[10] non-zero %" PRIu64, why,
                        RD32(i00), RD32(i58), RD32(mode), RD32(psub),
                        RD32(cdj_c6x_ram(0x008FFFE0, 4)),
                        RD32(cdj_c6x_ram(0x008FFFE4, 4)),
                        RD32(cdj_c6x_ram(0x008FFFE8, 4)), c->id1_p10_nonzero);
#undef RD32
        }
        info_report("c6x[%s]: MAIN: link up %u, frames %u, DSP registrar st[1] "
                    "%u, version 0x%04x.%04x, status 0x150 0x%08x, level 0x158 "
                    "0x%08x", why, up, frames, st1, ver[0], ver[1], stat, lvl);
    }
    {
        GString *g = g_string_new("");

        for (unsigned i = 0; i < 64; i++) {
            if (c->rx_kind[i]) {
                g_string_append_printf(g, " id%u:%" PRIu64, i, c->rx_kind[i]);
            }
        }
        info_report("c6x[%s]: reply ids MAIN read:%s; uPP bytes dropped by the "
                    "SoC %zu", why, g->str,
                    c->soc ? c6655_soc_upp_dropped(c->soc) : 0);
        g_string_free(g, true);
    }
    info_report("c6x[%s]: MAIN's receive: %" PRIu64 " one-word hunt reads, %"
                PRIu64 " frame reads (%" PRIu64 " 0x5533..0xAACC), %" PRIu64
                " trailers delivered; DSP sent AACC %" PRIu64 " CCAA %" PRIu64
                " 5533 %" PRIu64 " 3355 %" PRIu64, why, c->rx_arm1, c->rx_armn,
                c->rx_goodframes, c->rx_aacc, c->magic[0], c->magic[1],
                c->magic[2], c->magic[3]);
}

/* Bytes MAIN shipped over CS6 while the DSP thread was running, handed to the
 * SoC between chunks so its uPP queue has one writer. */
static void cdj_c6x_drain_upp_in(void)
{
    CdjC6x *c = &cdj_c6x;

    /* One c6655_soc_upp_rx per DMA transfer, never merged: the SoC recognises
     * a ship that overshoots its window only at the transfer's boundary. */
    for (;;) {
        GByteArray *ship;
        size_t q;

        qemu_mutex_lock(&c->link_lock);
        ship = g_queue_pop_head(c->upp_in);
        qemu_mutex_unlock(&c->link_lock);
        if (!ship) {
            break;
        }
        q = c6655_soc_upp_rx(c->soc, ship->data, ship->len);
        if (q > c->upp_peak_queue) {
            c->upp_peak_queue = q;
        }
        g_byte_array_free(ship, true);
        if (c->ship_async) {
            qemu_mutex_lock(&c->link_lock);
            c->tx_hold_until_ns = c6655_soc_now_ns(c->soc) + c->ship_hold_ns;
            c->ship_pending--;
            qemu_mutex_unlock(&c->link_lock);
        }
    }
}

/* Step the core and the SoC to DSP time `target`. Caller holds run_lock. */
static void cdj_c6x_run_to(uint64_t target)
{
    CdjC6x *c = &cdj_c6x;
    uint64_t now = c6655_soc_now_ns(c->soc);
    struct timespec h0, h1;

    clock_gettime(CLOCK_MONOTONIC, &h0);
    while (now < target && c->running && !c->halted) {
        uint64_t next;

        if (c->ship_async && qatomic_read(&c->ship_pending)) {
            cdj_c6x_drain_upp_in();
        }
        next = c6655_soc_next_event_ns(c->soc);
        uint64_t until = MIN(next, target);
        uint64_t budget, n = 0, adv;
        c66x_stop stop;

        if (until <= now) {
            c6655_soc_advance(c->soc, 0);
            until = MIN(c6655_soc_next_event_ns(c->soc), target);
            if (until <= now) {
                until = now + 1;
            }
        }
        budget = ((until - now) * c->mhz + 999) / 1000;
        /* CDJ_C6X_JITTER=<seed>: cut steps at random points, to tell a
         * cross-thread race from a step-boundary dependence in the models. */
        if (c->jitter && budget > 1) {
            budget = g_rand_int_range(c->jitter, 1, (gint32)MIN(budget, 1u << 30) + 1);
        }
        if (c->prof) {
            int64_t p0 = get_clock();

            stop = c66x_step(c->core, budget ? budget : 1, &n);
            c->prof_step_ns += get_clock() - p0;
        } else {
            stop = c66x_step(c->core, budget ? budget : 1, &n);
        }
        c->prof_steps++;
        c->cycles += n;
        c->cyc_rem += n * 1000;
        adv = c->cyc_rem / c->mhz;
        c->cyc_rem %= c->mhz;
        c->last_stop = stop;
        if (stop == C66X_STOP_IDLE) {
            /* A fixed point until the next event: the cycles it would have
             * spun still count on the time-stamp counter. */
            uint64_t wake = MIN(c6655_soc_next_irq_ns(c->soc), target);
            uint64_t skip = wake > now ? wake - now : 0;

            c->idle_skips++;
            if (skip > adv) {
                c66x_skip_cycles(c->core, (skip - adv) * c->mhz / 1000);
                c->idle_skipped_ns += skip - adv;
                adv = skip;
            }
            c->cyc_rem = 0;
        } else if (stop != C66X_STOP_BUDGET) {
            warn_report("c6x: the DSP stopped (%d) at pc 0x%08x, trap 0x%08x",
                        (int)stop, c66x_get_pc(c->core), c66x_trap_pc(c->core));
            c->halted = true;
            break;
        }
        if (!adv) {
            if (n) {
                continue;
            }
            adv = 1;
        }
        if (c->prof) {
            int64_t p0 = get_clock();

            c6655_soc_advance(c->soc, adv);
            c->prof_soc_ns += get_clock() - p0;
        } else {
            c6655_soc_advance(c->soc, adv);
        }
        now = c6655_soc_now_ns(c->soc);
    }
    clock_gettime(CLOCK_MONOTONIC, &h1);
    c->host_ns += (h1.tv_sec - h0.tv_sec) * 1000000000ull + h1.tv_nsec - h0.tv_nsec;
}

/*
 * Single-thread stepping (CDJ_C6X_THREAD unset or 0): deterministic, but every
 * DSP cycle is paid for inside QEMU's main loop, which caps the chip at a few
 * tens of MHz. Also the report timer in threaded mode.
 */
static void cdj_c6x_tick(void *opaque)
{
    CdjC6x *c = &cdj_c6x;
    int64_t virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (c->sync && c->slack_ns) {
        int64_t w0 = get_clock();
        uint64_t now = virt > c->epoch_ns ? (uint64_t)(virt - c->epoch_ns) : 0;
        uint64_t need = now > c->slack_ns ? now - c->slack_ns : 0;

        if (!qatomic_read(&c->running) || c->halted) {
            return;
        }
        while (!qatomic_read(&c->halted) && qatomic_read(&c->running) &&
               qatomic_read(&c->dsp_now_pub) < need) {
            qemu_sem_timedwait(&c->done_sem, 1);
        }
        c->sync_waits_ns += get_clock() - w0;
        c->vcpu_cpu_ns = cdj_c6x_thread_cpu_ns();
        c->ticks++;
        qatomic_set(&c->sync_target, now + c->quantum_ns +
                    (cdj_c6x_pacing(c) ? c->quantum_ns : 0));
        qemu_sem_post(&c->go_sem);
        if (c->halted || virt >= c->report_at) {
            qemu_mutex_lock(&c->run_lock);
            cdj_c6x_sample(c);
            c->report_at = virt + 10 * NANOSECONDS_PER_SECOND;
            cdj_c6x_report(c->halted ? "halt" : "tick");
            qemu_mutex_unlock(&c->run_lock);
        }
        if (!c->halted) {
            timer_mod(c->tick, virt + c->quantum_ns);
        }
        return;
    }
    if (c->sync) {
        int64_t w0;

        if (!c->running || c->halted) {
            return;
        }
        w0 = get_clock();
        qemu_mutex_lock(&c->run_lock);
        while (!c->halted && qatomic_read(&c->running) &&
               c6655_soc_now_ns(c->soc) < c->sync_target) {
            qemu_cond_wait(&c->done_cond, &c->run_lock);
        }
        c->sync_waits_ns += get_clock() - w0;
        c->vcpu_cpu_ns = cdj_c6x_thread_cpu_ns();
        if (!c->tick_tid_logged) {
            c->tick_tid_logged = true;
            info_report("c6x: the lockstep tick runs on host thread %d",
                        qemu_get_thread_id());
            cdj_mprof_arm();
        }
        cdj_c6x_sample(c);
        c->ticks++;
        if (c->halted) {
            cdj_c6x_report("halt");
        } else if (virt >= c->report_at) {
            c->report_at = virt + 10 * NANOSECONDS_PER_SECOND;
            cdj_c6x_report("tick");
        }
        {
            uint64_t now = virt > c->epoch_ns ? (uint64_t)(virt - c->epoch_ns) : 0;

            /* Paced, the DSP runs [now + q, now + 2q) beside MAIN's
             * [now, now + q), so its frames are early and can be held. */
            c->sync_target = now + c->quantum_ns +
                             (cdj_c6x_pacing(c) ? c->quantum_ns : 0);
        }
        qemu_cond_signal(&c->go_cond);
        qemu_mutex_unlock(&c->run_lock);
        if (!c->halted) {
            timer_mod(c->tick, virt + c->quantum_ns);
        }
        return;
    }
    if (c->threaded) {
        qemu_mutex_lock(&c->run_lock);
        if (c->core && virt >= c->report_at) {
            c->report_at = virt + 10 * NANOSECONDS_PER_SECOND;
            cdj_c6x_report(c->halted ? "halt" : "tick");
        }
        qemu_mutex_unlock(&c->run_lock);
        timer_mod(c->tick, virt + NANOSECONDS_PER_SECOND);
        return;
    }
    if (!c->running || c->halted) {
        return;
    }
    {
        uint64_t now = c6655_soc_now_ns(c->soc);
        uint64_t target = virt > c->epoch_ns ? (uint64_t)(virt - c->epoch_ns) : 0;

        if (target > now + c->max_catchup_ns) {
            /* The host fell behind: give the lost time up rather than bursting. */
            uint64_t lost = target - now - c->max_catchup_ns;

            c->slipped_ns += lost;
            c->epoch_ns += lost;
            target -= lost;
        }
        cdj_c6x_drain_upp_in();
        cdj_c6x_run_to(target);
        cdj_c6x_sample(c);
    }
    c->ticks++;
    if (c->halted) {
        cdj_c6x_report("halt");
    }
    if (virt >= c->report_at) {
        c->report_at = virt + 10 * NANOSECONDS_PER_SECOND;
        cdj_c6x_report("tick");
    }
    if (!c->halted) {
        timer_mod(c->tick, virt + c->quantum_ns);
    }
}

/*
 * The DSP's own thread. It may run at most one quantum ahead of QEMU's virtual
 * clock and waits when it gets there, so a stalled vCPU (or a paused VM, whose
 * virtual clock stops) holds the DSP back instead of letting it slip. When the
 * host cannot keep up it lags; the lag is reported, never given away.
 */
static void *cdj_c6x_thread(void *opaque)
{
    CdjC6x *c = &cdj_c6x;

    /* Named so it can be told apart in a host profile. */
    info_report("c6x: DSP thread is host thread %d", qemu_get_thread_id());
    if (c->sync && c->slack_ns) {
        /* run_lock is held per chunk only, so the tick never queues behind a
         * chunk unless the DSP is more than the slack behind. */
        while (!qatomic_read(&c->quit)) {
            uint64_t tgt = qatomic_read(&c->sync_target);
            bool ran = false;
            int64_t i0;

            qemu_mutex_lock(&c->run_lock);
            if (c->running && !c->halted && c6655_soc_now_ns(c->soc) < tgt) {
                cdj_c6x_drain_upp_in();
                cdj_c6x_run_to(tgt);
                c->chunks_run++;
                qatomic_set(&c->dsp_now_pub, c6655_soc_now_ns(c->soc));
                qatomic_set(&c->dsp_cpu_ns, cdj_c6x_thread_cpu_ns());
                ran = true;
            }
            qemu_mutex_unlock(&c->run_lock);
            if (ran) {
                qemu_sem_post(&c->done_sem);
                continue;
            }
            i0 = get_clock();
            qemu_sem_timedwait(&c->go_sem, 10);
            if (qatomic_read(&c->running)) {
                c->dsp_idle_ns += get_clock() - i0;
            }
        }
        return NULL;
    }
    if (c->sync) {
        qemu_mutex_lock(&c->run_lock);
        while (!qatomic_read(&c->quit)) {
            if (!c->running || c->halted || c6655_soc_now_ns(c->soc) >= c->sync_target) {
                int64_t i0 = get_clock();

                qemu_cond_timedwait(&c->go_cond, &c->run_lock, 10);
                if (c->running && !c->halted) {
                    c->dsp_idle_ns += get_clock() - i0;
                }
                continue;
            }
            cdj_c6x_drain_upp_in();
            cdj_c6x_run_to(c->sync_target);
            c->chunks_run++;
            qatomic_set(&c->dsp_cpu_ns, cdj_c6x_thread_cpu_ns());
            qemu_cond_broadcast(&c->done_cond);
        }
        qemu_mutex_unlock(&c->run_lock);
        return NULL;
    }
    while (!qatomic_read(&c->quit)) {
        int64_t virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        uint64_t sleep_us = 0;

        qemu_mutex_lock(&c->run_lock);
        if (!c->running || c->halted) {
            sleep_us = 1000;
        } else {
            uint64_t vt = virt > c->epoch_ns ? (uint64_t)(virt - c->epoch_ns) : 0;
            uint64_t now = c6655_soc_now_ns(c->soc);

            if (vt > now && vt - now > c->lag_max_ns) {
                c->lag_max_ns = vt - now;
            }
            /*
             * Bounded lag. A DSP that falls behind receives MAIN's version
             * request within its first few SPI exchanges and its SPI chain
             * stops for good. A real DSP is never behind MAIN, so give the
             * time up instead.
             */
            if (vt > now + c->max_catchup_ns) {
                uint64_t lost = vt - now - c->max_catchup_ns;

                c->slipped_ns += lost;
                c->epoch_ns += lost;
                vt -= lost;
            }
            /* Hysteresis: run a whole quantum ahead, then wait until the clock
             * has passed us; topping up to the moving limit spins the lock. */
            if (now >= vt) {
                c->waits++;
                sleep_us = MAX((now - vt) / 1000, 50);
            } else {
                int64_t p0 = get_clock();

                cdj_c6x_drain_upp_in();
                c->prof_drain_ns += get_clock() - p0;
                cdj_c6x_run_to(MIN(vt + c->ahead_ns, now + 20 * c->quantum_ns));
                c->ticks++;
            }
        }
        qemu_mutex_unlock(&c->run_lock);
        if (sleep_us) {
            g_usleep(sleep_us);
        }
    }
    return NULL;
}

static void cdj_c6x_summary(Notifier *n, void *unused)
{
    CdjC6x *c = &cdj_c6x;
    GString *cs = g_string_new("");

    if (c->threaded) {
        qatomic_set(&c->quit, true);
        if (c->sync) {
            qemu_mutex_lock(&c->run_lock);
            qemu_cond_broadcast(&c->go_cond);
            qemu_mutex_unlock(&c->run_lock);
            if (c->slack_ns) {
                qemu_sem_post(&c->go_sem);
            }
            info_report("c6x[exit]: synchronised DSP thread: %" PRIu64 " quanta, MAIN "
                        "waited %.2f s for the DSP", c->chunks_run, c->sync_waits_ns / 1e9);
        }
        qemu_thread_join(&c->thread);
        info_report("c6x[exit]: DSP thread: %" PRIu64 " chunks, %" PRIu64
                    " waits for the virtual clock, worst lag %.3f s",
                    c->ticks, c->waits, c->lag_max_ns / 1e9);
    }
    /* Lockstep too: the stepper is the whole machine's speed limit there. */
    if (c->threaded || c->prof) {
        info_report("c6x[exit]: profile: host %.2f s in run_to; core step %.2f s "
                    "(%" PRIu64 " calls, %.0f cycles/call), SoC advance %.2f s, "
                    "uPP handoff %.3f s, %" PRIu64 " link kicks to the main loop, "
                    "%" PRIu64 " McBSP0 words; inside the SoC: %" PRIu64
                    " DMA-store invalidates %.2f s, %" PRIu64 " SPI words %.2f s, McBSP sink callback %.2f s",
                    c->host_ns / 1e9,
                    c->prof_step_ns / 1e9, c->prof_steps,
                    c->prof_steps ? (double)c->cycles / c->prof_steps : 0,
                    c->prof_soc_ns / 1e9, c->prof_drain_ns / 1e9, c->prof_bh,
                    c->pcm_words[0], c->prof_inval, c->prof_inval_ns / 1e9,
                    c->prof_spi, c->prof_spi_ns / 1e9, c->prof_pcm_ns / 1e9);
    }
    cdj_c6x_report("exit");
    for (unsigned i = 0; i < c->pchit_n; i++) {
        info_report("c6x[exit]: DSP pc 0x%08x hit %" PRIu64 " times (last B3 0x%08x)",
                    c->pchit_pc[i], c->pchit_count[i], c->pchit_b3[i]);
    }
    if (getenv("CDJ_C6X_WATCH")) {
        info_report("c6x[exit]: watched stores %" PRIu64, c->watch_stores);
    }
    for (unsigned i = 0; i < ARRAY_SIZE(c->gpio_census); i++) {
        if (c->gpio_census[i].n[0] || c->gpio_census[i].n[1]) {
            info_report("c6x[exit]: GPIO mask 0x%08x: SET %" PRIu64 " (last pc "
                        "0x%08x), CLR %" PRIu64 " (last pc 0x%08x)",
                        c->gpio_census[i].mask, c->gpio_census[i].n[1],
                        c->gpio_census[i].last_pc[1], c->gpio_census[i].n[0],
                        c->gpio_census[i].last_pc[0]);
        }
    }
    for (unsigned i = 0; i < 256; i++) {
        if (c->csnr_seen[i]) {
            g_string_append_printf(cs, " 0x%02x:%u", i, c->csnr_seen[i]);
        }
    }
    info_report("c6x[exit]: SPI chip-select values seen:%s; I2C blocks %u "
                "(%u bad), loaded %s", cs->str, c->i2c_blocks, c->i2c_bad_ck,
                c->loaded ? "yes" : "no");
    g_string_free(cs, true);
    if (c->pcm) {
        fclose(c->pcm);
    }
    /*
     * C66X_JIT_PROFILE is written by c66x_free(), which otherwise only runs
     * when the core is re-created. The DSP thread is joined above, so free
     * the core here to flush the profile.
     */
    if (c->core && getenv("C66X_JIT_PROFILE")) {
        c66x_free(c->core);
        c->core = NULL;
        info_report("c6x[exit]: JIT profile written to %s",
                    getenv("C66X_JIT_PROFILE"));
    }
}

void cdj_c6x_init(void)
{
    CdjC6x *c = &cdj_c6x;
    const char *on = getenv("CDJ_C6X");
    const char *e;

    if (!on || !strcmp(on, "0")) {
        return;
    }
    c->on = true;
    e = getenv("CDJ_C6X_MHZ");
    c->mhz = e ? strtoull(e, NULL, 0) : 20;
    if (!c->mhz) {
        c->mhz = 1;
    }
    /* CDJ_C6X_MHZ_BOOT: clock rate until GPIO25 reports booted. Stage 1
     * busy-waits in its GPIO handshake, and at high rates that stall outruns
     * MAIN's handshake timeouts. */
    c->mhz_run = c->mhz;
    e = getenv("CDJ_C6X_MHZ_BOOT");
    if (e && strtoull(e, NULL, 0)) {
        c->mhz = strtoull(e, NULL, 0);
    }
    e = getenv("CDJ_C6X_QUANTUM_US");
    c->quantum_ns = (e ? strtoll(e, NULL, 0) : 1000) * 1000;
    if (c->quantum_ns <= 0) {
        c->quantum_ns = 1000000;
    }
    e = getenv("CDJ_C6X_CATCHUP_MS");
    c->max_catchup_ns = (e ? strtoull(e, NULL, 0) : 20) * 1000000ull;
    c->preload = getenv("CDJ_C6X_PRELOAD") && strcmp(getenv("CDJ_C6X_PRELOAD"), "0");
    e = getenv("CDJ_C6X_PCM");
    if (e && *e) {
        c->pcm = fopen(e, "wb");
    }
    c->l2 = g_malloc0(C6X_L2_SIZE);
    c->ddr = g_malloc0(C6X_DDR_SIZE);
    c->i2c = g_byte_array_new();
    c->table = g_byte_array_new();
    c->upp_in = g_queue_new();
    c->tx_cur = -1;
    c->booted_level = 1;
    qemu_mutex_init(&c->run_lock);
    qemu_mutex_init(&c->link_lock);
    c->kick = qemu_bh_new(cdj_c6x_kick_bh, c);
    c->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_c6x_tick, c);
    c->prof = getenv("CDJ_C6X_PROF") != NULL;
    e = getenv("CDJ_C6X_AHEAD_US");
    c->ahead_ns = e ? strtoull(e, NULL, 0) * 1000ull : (uint64_t)c->quantum_ns;
    e = getenv("CDJ_C6X_JITTER");
    if (e && strcmp(e, "0")) {
        c->jitter = g_rand_new_with_seed((guint32)strtoul(e, NULL, 0));
    }
    e = getenv("CDJ_C6X_SHIP_HOLD_US");
    c->ship_hold_ns = (e ? strtoull(e, NULL, 0) : 2000) * 1000ull;
    /* CDJ_C6X_THREAD: unset/0 steps in the main loop (default), 1 runs a
     * free-running DSP thread, 2 a lockstep thread (see CDJ_C6X_SLACK_US).
     * The free-running thread occasionally loses the SPI link at boot. */
    e = getenv("CDJ_C6X_THREAD");
    c->threaded = e && strcmp(e, "0");
    c->sync = e && !strcmp(e, "2");
    e = getenv("CDJ_C6X_SLACK_US");
    c->slack_ns = c->sync && e ? strtoull(e, NULL, 0) * 1000ull : 0;
    e = getenv("CDJ_C6X_SHIP_ASYNC");
    c->ship_async = c->sync && ((e && strcmp(e, "0")) || c->slack_ns);
    if (c->sync) {
        info_report("c6x: lockstep slack %" PRIu64 " us, uPP ships %s",
                    c->slack_ns / 1000, c->ship_async ? "queued to the DSP thread"
                    : "delivered under run_lock");
    }
    /* Pacing needs the DSP ahead of MAIN, and only the lockstep modes keep
     * epoch_ns fixed, so a stamp maps to one virtual time for good. */
    e = getenv("CDJ_C6X_RX_PACE");
    c->rx_pace = c->sync && e && strcmp(e, "0");
    if (e && strcmp(e, "0") && !c->sync) {
        warn_report("c6x: CDJ_C6X_RX_PACE needs CDJ_C6X_THREAD=2; ignored");
    }
    if (c->rx_pace) {
        e = getenv("CDJ_C6X_RX_PACE_GRAIN_US");
        c->rx_pace_grain_ns = (e ? strtoull(e, NULL, 0) : 500) * 1000ull;
        c->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_c6x_kick_bh, c);
        info_report("c6x: DSP->MAIN words paced by DSP time (grain %" PRIu64
                    " us), DSP leads by one extra quantum, from good frame %u",
                    c->rx_pace_grain_ns / 1000, C6X_PACE_AFTER_FRAMES);
    }
    if (c->threaded) {
        qemu_cond_init(&c->go_cond);
        qemu_cond_init(&c->done_cond);
        qemu_sem_init(&c->go_sem, 0);
        qemu_sem_init(&c->done_sem, 0);
        qemu_thread_create(&c->thread, "cdj-c6655", cdj_c6x_thread, c,
                           QEMU_THREAD_JOINABLE);
        if (!c->sync) {
            timer_mod(c->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      NANOSECONDS_PER_SECOND);
        }
    }
    c->exit.notify = cdj_c6x_summary;
    qemu_add_exit_notifier(&c->exit);
    info_report("c6x: the real DSP is modelled (CDJ_C6X): %" PRIu64 " MHz, boot "
                "via %s, %s; the stand-in DSP peer is bypassed on MSIOF0, CS6 "
                "and the handshake lines", c->mhz,
                c->preload ? "preloaded first stage" : "ROM I2C passive boot",
                c->threaded ? "own thread" : "main-loop stepping");
}

