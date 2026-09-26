/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dsp_host.h"
/*
 * The core runs in slices on QEMU's main loop, so MAIN's host-port accesses
 * and the DSP never run at the same time. On the board the DSP follows the
 * command pins within microseconds, so every change of command runs the core
 * in small pieces until the DSP has seen it: for a command, until it answers
 * on HINT; for the drop back to 0, until it has read the pins again. Without
 * the second, MAIN drops the command and raises the next one before the
 * loader, still waiting for the drop (0x1180292C), has looked.
 */

#define SLICE_NS        (1000 * 1000)       /* 1 ms of DSP time per slice */
#define REACT_STEP      2000
#define REACT_MAX       (4 * 1000 * 1000)

#define MCASP_GBLCTL    0x44
#define MCASP_RGBLCTL   0x60
#define MCASP_XGBLCTL   0xA0
#define GBLCTL_R        0x001Fu
#define GBLCTL_X        0x1F00u

static bool host_stopped(CdjDspHost *h, c66x_stop stop)
{
    h->last_stop = stop;
    if (stop != C66X_STOP_UNDEF && stop != C66X_STOP_FAULT) {
        return false;
    }
    h->trap_pc = c66x_trap_pc(h->core);
    h->running = false;
    warn_report("%s: core stopped (%s) at 0x%08x", h->name,
                stop == C66X_STOP_UNDEF ? "undefined instruction" : "fault",
                h->trap_pc);
    return true;
}

static void host_slice(void *opaque)
{
    CdjDspHost *h = opaque;
    uint64_t done = 0;

    if (!h->running || host_stopped(h, c66x_step(h->core, h->cycles_per_slice,
                                                 &done))) {
        return;
    }
    timer_mod(h->slice, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SLICE_NS);
}

static bool host_answered(CdjDspHost *h, uint64_t edges, uint64_t reads)
{
    return h->command ? h->hint_edges != edges : h->pin_reads != reads;
}

static void host_react(CdjDspHost *h)
{
    uint64_t edges = h->hint_edges, reads = h->pin_reads, total = 0, done;

    while (h->running && !host_answered(h, edges, reads) && total < REACT_MAX) {
        done = 0;
        if (host_stopped(h, c66x_step(h->core, REACT_STEP, &done))) {
            break;
        }
        total += done ? done : REACT_STEP;
    }
    h->react_cycles += total;
}

static void set_hpic(CdjDspHost *h, uint32_t hpic)
{
    if ((hpic ^ h->hpic) & HPIC_HINT) {
        h->hint_edges++;
    }
    h->hpic = hpic;
}

/* The HPIC as either side reads it; the port is always ready. */
uint32_t cdj_dsp_host_hpic(const CdjDspHost *h)
{
    return h->hpic | HPIC_HRDY;
}

bool cdj_dsp_host_main_hpic(CdjDspHost *h, uint32_t val)
{
    set_hpic(h, (h->hpic & ~(val & HPIC_HINT)) | (val & HPIC_DSPINT)
                | (val & HPIC_HWOB));
    return val & HPIC_DSPINT;
}

void cdj_dsp_host_dsp_hpic(CdjDspHost *h, uint32_t val)
{
    set_hpic(h, (h->hpic & ~(val & HPIC_DSPINT)) | (val & HPIC_HINT));
}

/* The HINT pin as MAIN's latch sees it: high (busy) until the DSP raises
 * HINT. */
static bool host_busy(void *opaque)
{
    CdjDspHost *h = opaque;

    return !(h->hpic & HPIC_HINT);
}

static void host_command(void *opaque, unsigned bits)
{
    CdjDspHost *h = opaque;

    /* MAIN rewrites the latch for its other bits too. */
    if (bits == h->command) {
        return;
    }
    h->command = bits;
    h->set_pins(h->chip, bits);
    if (bits) {
        h->commands++;
    }
    host_react(h);
}

void cdj_dsp_host_init(CdjDspHost *h, const char *name, const char *env_prefix,
                       unsigned default_mhz,
                       void (*set_pins)(void *chip, unsigned bits), void *chip)
{
    g_autofree char *on_var = g_strdup_printf("CDJ_%s", env_prefix);
    g_autofree char *mhz_var = g_strdup_printf("CDJ_%s_MHZ", env_prefix);
    const char *on = getenv(on_var);
    const char *mhz = getenv(mhz_var);

    h->name = name;
    h->enabled = !(on && !strcmp(on, "0"));
    h->cycles_per_slice = (mhz ? strtoull(mhz, NULL, 0) : default_mhz) * 1000;
    h->slice = timer_new_ns(QEMU_CLOCK_VIRTUAL, host_slice, h);
    /* The chip's ROM boot loader raises HINT at reset to tell the host it
     * is ready; MAIN waits for that before it loads anything. */
    h->hpic = HPIC_HINT;
    h->set_pins = set_pins;
    h->chip = chip;
    h->wires = (CdjDspWires){ h, host_command, host_busy };
}

c66x_core *cdj_dsp_host_core(CdjDspHost *h, const c66x_bus *bus)
{
    h->core = c66x_new(bus);
    return h->core;
}

void cdj_dsp_host_run(CdjDspHost *h, uint32_t entry)
{
    c66x_reset(h->core, entry);
    h->running = true;
    info_report("%s: started at 0x%08x", h->name, entry);
    timer_mod(h->slice, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + SLICE_NS);
}

/* The GBLCTL register @addr is in, or -1; its McASP in @n. */
static int mcasp_reg(const DspMcasps *m, uint32_t addr, unsigned *n)
{
    uint32_t off = (addr - m->base) % m->stride;

    *n = (addr - m->base) / m->stride;
    if (addr < m->base || *n >= ARRAY_SIZE(m->gblctl)) {
        return -1;
    }
    return off == MCASP_GBLCTL || off == MCASP_RGBLCTL
        || off == MCASP_XGBLCTL ? off : -1;
}

uint32_t cdj_dsp_mcasp_read(const DspMcasps *m, uint32_t addr)
{
    unsigned n;

    return mcasp_reg(m, addr, &n) < 0 ? 0 : m->gblctl[n];
}

void cdj_dsp_mcasp_write(DspMcasps *m, uint32_t addr, uint32_t val)
{
    unsigned n;
    uint32_t mask;

    switch (mcasp_reg(m, addr, &n)) {
    case MCASP_RGBLCTL:
        mask = GBLCTL_R;
        break;
    case MCASP_XGBLCTL:
        mask = GBLCTL_X;
        break;
    case MCASP_GBLCTL:
        mask = GBLCTL_R | GBLCTL_X;
        break;
    default:
        return;
    }
    m->gblctl[n] = (m->gblctl[n] & ~mask) | (val & mask);
}

void cdj_dsp_dump_mem(const char *dir, const char *name, const void *p,
                      size_t len)
{
    g_autofree char *path = g_strdup_printf("%s/%s", dir, name);
    g_autoptr(GError) err = NULL;

    if (!g_file_set_contents(path, p, len, &err)) {
        warn_report("dsp: cannot write %s: %s", path, err->message);
    }
}

void cdj_dsp_count(DspAddrTable *t, uint32_t addr, uint32_t val)
{
    unsigned i;

    for (i = 0; i < t->n; i++) {
        if (t->e[i].addr == addr) {
            t->e[i].count++;
            t->e[i].last = val;
            return;
        }
    }
    if (t->n < DSP_HOST_MAX_ADDR) {
        t->e[t->n++] = (DspAddrCount){ addr, 1, val };
    }
}

static void print_counts(const char *name, const char *what,
                         const DspAddrTable *t)
{
    unsigned i;

    for (i = 0; i < t->n; i++) {
        info_report("%s: %s 0x%08x x%" PRIu64 ", last 0x%08x", name, what,
                    t->e[i].addr, t->e[i].count, t->e[i].last);
    }
}

void cdj_dsp_host_report(CdjDspHost *h)
{
    c66x_stats st;

    if (!h->core) {
        info_report("%s: never started", h->name);
        return;
    }
    c66x_get_stats(h->core, &st);
    info_report("%s: %s, pc 0x%08x, %" PRIu64 " cycles, %" PRIu64
                " packets, last stop %d", h->name,
                h->running ? "running" : "stopped", c66x_get_pc(h->core),
                st.cycles, st.packets, h->last_stop);
    if (h->trap_pc) {
        info_report("%s: trap at 0x%08x", h->name, h->trap_pc);
    }
    info_report("%s: %" PRIu64 " commands from MAIN, %" PRIu64
                " HINT edges, HPIC 0x%08x, %" PRIu64 " cycles run to answer",
                h->name, h->commands, h->hint_edges, h->hpic, h->react_cycles);
    print_counts(h->name, "bus read ", &h->busr);
    print_counts(h->name, "bus write", &h->busw);
}
