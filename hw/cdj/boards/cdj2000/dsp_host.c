/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dsp_host.h"
/*
 * The core runs on its own thread in chunks, at most one quantum ahead of
 * QEMU's virtual clock. Run on the main loop instead, a chunk of the
 * interpreted core takes several times its own length in host time and every
 * QEMU timer, MAIN's tick among them, fires late and in bursts. When the host
 * cannot keep up the core lags; the worst lag is reported at exit.
 *
 * MAIN's host-port accesses and a chunk never run at the same time (@lock).
 * On the chip the UHPI is a bus master of its own: a host access takes no CPU
 * cycles and is served while the CPU runs, so the core runs a chunk in slices
 * and hands the lock to a waiting MAIN between two. With whole chunks under
 * the lock each of MAIN's accesses waited about half a millisecond: on the
 * CDJ-2000, MAIN spent 286 s of a 366 s run waiting, its player task
 * (priority 4, in the window on every pass) looked busy for all of it, and the
 * lower-priority media manager took five minutes to read the USB stick's
 * database instead of seconds.
 *
 * On the board the DSP follows the command pins within microseconds, so every
 * change of command runs the core in small pieces until the DSP has seen it:
 * for a command, until it answers on HINT; for the drop back to 0, until it
 * has read the pins again. Without the second, MAIN drops the command and
 * raises the next one before the loader, still waiting for the drop
 * (0x1180292C), has looked.
 */

#define QUANTUM_NS      (1000 * 1000)
#define CHUNK_NS        (100 * 1000)
#define SLICE_CYCLES    2000
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

static void *host_thread(void *opaque)
{
    CdjDspHost *h = opaque;
    uint64_t done, run;

    while (qatomic_read(&h->running)) {
        int64_t virt = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        if (qatomic_read(&h->host_waiting)) {
            g_usleep(0);
            continue;
        }
        if (h->dsp_ns >= virt + QUANTUM_NS) {
            g_usleep(1000);
            continue;
        }
        h->lag_max_ns = MAX(h->lag_max_ns, virt - h->dsp_ns);
        qemu_mutex_lock(&h->lock);
        for (run = 0; run < h->cycles_per_chunk
                      && !qatomic_read(&h->host_waiting); run += SLICE_CYCLES) {
            done = 0;
            if (host_stopped(h, c66x_step(h->core, SLICE_CYCLES, &done))) {
                break;
            }
        }
        qemu_mutex_unlock(&h->lock);
        /* An idle core has waited out the rest of its slices. */
        h->dsp_ns += CHUNK_NS * run / h->cycles_per_chunk;
    }
    return NULL;
}

/*
 * MAIN waits for a chunk to end, and then may run the core itself
 * (host_react), for milliseconds of host time. Holding the BQL all that
 * while stalls the main loop and every QEMU timer with it: MAIN's 1 kHz
 * tick underflowed in bursts of four and the RTOS counted half its ticks.
 * Nothing behind @lock needs the BQL, so MAIN's side lets it go.
 */
void cdj_dsp_host_lock(CdjDspHost *h)
{
    bool had_bql = bql_locked();
    int64_t t0 = get_clock();

    if (had_bql) {
        bql_unlock();
    }
    qatomic_inc(&h->host_waiting);
    qemu_mutex_lock(&h->lock);
    qatomic_dec(&h->host_waiting);
    h->host_had_bql = had_bql;
    h->host_locks++;
    h->host_wait_ns += get_clock() - t0;
}

void cdj_dsp_host_unlock(CdjDspHost *h)
{
    bool had_bql = h->host_had_bql;

    qemu_mutex_unlock(&h->lock);
    if (had_bql) {
        bql_lock();
    }
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
    if (h->core && h->dspint_line && (hpic ^ h->hpic) & HPIC_DSPINT) {
        c66x_set_irq(h->core, h->dspint_line, hpic & HPIC_DSPINT);
    }
    qatomic_set(&h->hpic, hpic);
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

    return !(qatomic_read(&h->hpic) & HPIC_HINT);
}

static void host_command(void *opaque, unsigned bits)
{
    CdjDspHost *h = opaque;

    /* MAIN rewrites the latch for its other bits too. */
    if (bits == h->command) {
        return;
    }
    cdj_dsp_host_lock(h);
    h->command = bits;
    h->set_pins(h->chip, bits);
    if (bits) {
        h->commands++;
    }
    host_react(h);
    cdj_dsp_host_unlock(h);
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
    h->cycles_per_chunk = (mhz ? strtoull(mhz, NULL, 0) : default_mhz)
                          * CHUNK_NS / 1000;
    qemu_mutex_init(&h->lock);
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
    h->dsp_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    info_report("%s: started at 0x%08x", h->name, entry);
    qemu_thread_create(&h->thread, h->name, host_thread, h,
                       QEMU_THREAD_DETACHED);
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
    cdj_dsp_host_lock(h);
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
    info_report("%s: at most %" PRId64 " ms behind the virtual clock",
                h->name, h->lag_max_ns / SCALE_MS);
    info_report("%s: MAIN waited %" PRId64 " ms for the core over %" PRIu64
                " host-port accesses", h->name, h->host_wait_ns / SCALE_MS,
                h->host_locks);
    print_counts(h->name, "bus read ", &h->busr);
    print_counts(h->name, "bus write", &h->busw);
    cdj_dsp_host_unlock(h);
}
