/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dsp_host.h"
#include "sysemu/runstate.h"
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
 * MAIN's data accesses to the core's RAM do not take the lock at all: a read
 * is served from the RAM, a write is posted and the core thread stores it
 * between two slices. On the C6747 boards MAIN makes millions of such
 * accesses a run, and each one used to wait out a slice.
 *
 * On the board the DSP follows the command pins within microseconds, so every
 * change of command runs the core in small pieces until the DSP has seen it:
 * for a command, until it answers on HINT; for the drop back to 0, until it
 * has read the pins again. Without the second, MAIN drops the command and
 * raises the next one before the loader, still waiting for the drop
 * (0x1180292C), has looked.
 *
 * CDJ_<chip>_SLACK_US=n keeps the core from falling more than n microseconds
 * behind: a virtual-clock timer holds MAIN back until it has caught up. Off,
 * the core only ever lags, by minutes once a track plays.
 */

#define QUANTUM_NS      (1000 * 1000)
#define CHUNK_NS        (100 * 1000)
#define SLICE_CYCLES    2000
#define REACT_STEP      2000
#define REACT_MAX       (4 * 1000 * 1000)
#define SLACK_POLL_NS   (1000 * 1000)
/* Stepping to a quiet point gives up after this much DSP time: 10 ms. */
#define QUIET_STEP      16
#define QUIET_MAX_NS    (10 * 1000 * 1000)

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

/* With @lock held. Posted writes land in order, their stores before any of
 * the core's notifications, as a lone host write's store and notification. */
static void host_apply_posted(CdjDspHost *h)
{
    DspPostedWrite w[DSP_HOST_MAX_POSTED];
    unsigned i, n;

    qemu_mutex_lock(&h->posted_lock);
    n = h->nposted;
    for (i = 0; i < n; i++) {
        w[i] = h->posted[i];
        stl_le_p(h->ram(h->chip, w[i].addr & ~3u), w[i].val);
    }
    h->nposted = 0;
    qemu_mutex_unlock(&h->posted_lock);
    for (i = 0; i < n; i++) {
        c66x_invalidate(h->core, w[i].addr & ~3u, 4);
    }
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
        qemu_mutex_lock(&h->lock);
        if (!qatomic_read(&h->running)) {
            qemu_mutex_unlock(&h->lock);
            break;
        }
        if (qatomic_read(&h->paused)) {
            qemu_mutex_unlock(&h->lock);
            g_usleep(1000);
            continue;
        }
        h->lag_max_ns = MAX(h->lag_max_ns, virt - h->dsp_ns);
        for (run = 0; run < h->cycles_per_chunk
                      && !qatomic_read(&h->host_waiting);
             run += h->slice_cycles) {
            done = 0;
            if (qatomic_read(&h->nposted)) {
                host_apply_posted(h);
            }
            if (host_stopped(h, c66x_step(h->core, h->slice_cycles, &done))) {
                break;
            }
            /* Nothing changes for an idle core before its next interrupt, and
             * the EDMA events come at the end of a chunk; MAIN's DSPINT lands
             * there too instead of between two slices, well inside the
             * quantum the core may run ahead anyway. */
            if (h->last_stop == C66X_STOP_IDLE) {
                if (run + done < h->cycles_per_chunk) {
                    c66x_skip_cycles(h->core, h->cycles_per_chunk - run - done);
                    h->idle_skipped += h->cycles_per_chunk - run - done;
                }
                run = h->cycles_per_chunk;
                break;
            }
        }
        qemu_mutex_unlock(&h->lock);
        /* An idle core has waited out the rest of its slices. */
        qemu_mutex_lock(&h->progress_lock);
        h->dsp_ns += CHUNK_NS * run / h->cycles_per_chunk;
        qemu_cond_broadcast(&h->progress);
        qemu_mutex_unlock(&h->progress_lock);
        if (h->after_chunk) {
            qemu_mutex_lock(&h->lock);
            if (!qatomic_read(&h->paused)) {
                h->after_chunk(h->chip, h->dsp_ns);
            }
            qemu_mutex_unlock(&h->lock);
        }
    }
    return NULL;
}

/*
 * With the BQL held, as the NXS2's lockstep tick waits: with -icount this
 * runs as a virtual-clock timer, and stopping the machine waits, holding the
 * BQL, for the running timers to finish, so a timer that let the BQL go
 * could never take it back. The waits are short: the core is at most the
 * slack behind.
 */
static void slack_tick(void *opaque)
{
    CdjDspHost *h = opaque;
    int64_t need = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - h->slack_ns;

    qemu_mutex_lock(&h->progress_lock);
    while (qatomic_read(&h->running) && h->dsp_ns < need) {
        qemu_cond_timedwait(&h->progress, &h->progress_lock, 10);
    }
    qemu_mutex_unlock(&h->progress_lock);
    timer_mod(h->slack_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                              + SLACK_POLL_NS);
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
    if (h->nposted) {
        host_apply_posted(h);
    }
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

uint32_t cdj_dsp_host_read_word(CdjDspHost *h, uint32_t addr)
{
    uint8_t *p = h->ram(h->chip, addr & ~3u);
    uint32_t val = 0;
    bool posted = false;
    unsigned i;

    /* The newest posted write to the word wins; the core thread stores a
     * batch while it holds @posted_lock, so the RAM is never read between a
     * write leaving the list and landing. */
    qemu_mutex_lock(&h->posted_lock);
    for (i = h->nposted; i > 0 && !posted; i--) {
        if (h->posted[i - 1].addr == (addr & ~3u)) {
            val = h->posted[i - 1].val;
            posted = true;
        }
    }
    if (!posted && p) {
        val = ldl_le_p(p);
    }
    qemu_mutex_unlock(&h->posted_lock);
    return val;
}

bool cdj_dsp_host_write_word(CdjDspHost *h, uint32_t addr, uint32_t val)
{
    addr &= ~3u;
    if (!h->ram(h->chip, addr)) {
        return false;
    }
    if (qatomic_read(&h->running)) {
        qemu_mutex_lock(&h->posted_lock);
        if (h->nposted < DSP_HOST_MAX_POSTED) {
            h->posted[h->nposted] = (DspPostedWrite){ addr, val };
            qatomic_set(&h->nposted, h->nposted + 1);
            qemu_mutex_unlock(&h->posted_lock);
            return true;
        }
        qemu_mutex_unlock(&h->posted_lock);
    }
    cdj_dsp_host_lock(h);
    stl_le_p(h->ram(h->chip, addr), val);
    if (h->core) {
        c66x_invalidate(h->core, addr, 4);
    }
    cdj_dsp_host_unlock(h);
    return true;
}

static bool host_answered(CdjDspHost *h, uint64_t edges, uint64_t reads)
{
    return h->command ? h->hint_edges != edges : h->pin_reads != reads;
}

static void host_react(CdjDspHost *h)
{
    uint64_t edges = h->hint_edges, reads = h->pin_reads, total = 0, done;

    /* An idle core reads no pins, so only an interrupt gets it to answer. */
    while (h->running && !host_answered(h, edges, reads) && total < REACT_MAX) {
        done = 0;
        if (host_stopped(h, c66x_step(h->core, REACT_STEP, &done))
            || h->last_stop == C66X_STOP_IDLE) {
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

/*
 * Snapshots. The chip's RAM travels with the machine's RAM, but the core
 * stores into it through host pointers that dirty tracking never sees, so the
 * core must not run a cycle once a save has started. When the machine stops,
 * the core steps on to a quiet point (see c66x_quiet) and its thread runs
 * nothing until the machine runs again.
 */
static void host_vm_state(void *opaque, bool running, RunState state)
{
    CdjDspHost *h = opaque;
    uint64_t done, run = 0, max = h->cycles_per_chunk * QUIET_MAX_NS / CHUNK_NS;

    if (running) {
        qatomic_set(&h->paused, false);
        return;
    }
    qatomic_inc(&h->host_waiting);
    qemu_mutex_lock(&h->lock);
    qatomic_dec(&h->host_waiting);
    qatomic_set(&h->paused, true);
    /* MAIN stops with the machine, so nothing is posted after these land and
     * a snapshot never has to carry the list. */
    if (h->nposted) {
        host_apply_posted(h);
    }
    /* A machine on its way out is never saved. */
    if (state != RUN_STATE_SHUTDOWN) {
        while (h->core && h->running && !c66x_quiet(h->core) && run < max) {
            done = 0;
            if (host_stopped(h, c66x_step(h->core, QUIET_STEP, &done))) {
                break;
            }
            run += QUIET_STEP;
        }
        if (h->core && h->running && !c66x_quiet(h->core)) {
            warn_report("%s: no quiet point within %" PRIu64 " cycles, pc "
                        "0x%08x: the machine cannot be saved now", h->name,
                        run, c66x_get_pc(h->core));
        }
    }
    qemu_mutex_unlock(&h->lock);
}

static int host_pre_save(void *opaque)
{
    CdjDspHost *h = opaque;

    g_byte_array_set_size(h->snap, 0);
    if (!h->core) {
        return 0;
    }
    if (!h->paused || !c66x_quiet(h->core)) {
        error_report("%s: the core is not at a quiet point, pc 0x%08x: stop "
                     "the machine before saving it", h->name,
                     c66x_get_pc(h->core));
        return -EBUSY;
    }
    g_byte_array_set_size(h->snap, c66x_save(h->core, NULL));
    c66x_save(h->core, h->snap->data);
    return 0;
}

const VMStateDescription vmstate_cdj_dsp_host = {
    .name = "cdj-dsp-host",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = host_pre_save,
    .fields = (const VMStateField[]) {
        CDJ_VMSTATE_BYTES(snap, CdjDspHost),
        VMSTATE_BOOL(running, CdjDspHost),
        VMSTATE_UINT32(trap_pc, CdjDspHost),
        VMSTATE_INT64(dsp_ns, CdjDspHost),
        VMSTATE_UINT32(hpic, CdjDspHost),
        VMSTATE_UINT32(command, CdjDspHost),
        VMSTATE_UINT64(commands, CdjDspHost),
        VMSTATE_UINT64(hint_edges, CdjDspHost),
        VMSTATE_UINT64(pin_reads, CdjDspHost),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * C66X_JIT_PROFILE is written when the core is freed, which otherwise never
 * happens: stop the thread and free it at exit, so a live run profiles the
 * DSP for tools/c14_jitgen.py without a recording.
 */
static void host_write_profile(Notifier *n, void *data)
{
    CdjDspHost *h = container_of(n, CdjDspHost, profile_exit);

    if (!h->core) {
        return;
    }
    cdj_dsp_host_lock(h);
    qatomic_set(&h->running, false);
    c66x_free(h->core);
    h->core = NULL;
    cdj_dsp_host_unlock(h);
    info_report("%s: JIT profile written to %s", h->name,
                getenv("C66X_JIT_PROFILE"));
}

void cdj_dsp_host_init(CdjDspHost *h, const char *name, const char *env_prefix,
                       unsigned default_mhz,
                       void (*set_pins)(void *chip, unsigned bits), void *chip)
{
    g_autofree char *on_var = g_strdup_printf("CDJ_%s", env_prefix);
    g_autofree char *mhz_var = g_strdup_printf("CDJ_%s_MHZ", env_prefix);
    const char *on = getenv(on_var);
    const char *mhz = getenv(mhz_var);
    g_autofree char *slack_var = g_strdup_printf("CDJ_%s_SLACK_US", env_prefix);
    const char *slack;

    h->name = name;
    h->enabled = !(on && !strcmp(on, "0"));
    h->cycles_per_chunk = (mhz ? strtoull(mhz, NULL, 0) : default_mhz)
                          * CHUNK_NS / 1000;
    h->slice_cycles = SLICE_CYCLES;
    qemu_mutex_init(&h->lock);
    qemu_mutex_init(&h->progress_lock);
    qemu_mutex_init(&h->posted_lock);
    qemu_cond_init(&h->progress);
    slack = getenv(slack_var);
    h->slack_ns = slack ? strtoull(slack, NULL, 0) * 1000 : 0;
    /* The chip's ROM boot loader raises HINT at reset to tell the host it
     * is ready; MAIN waits for that before it loads anything. */
    h->hpic = HPIC_HINT;
    h->set_pins = set_pins;
    h->chip = chip;
    h->wires = (CdjDspWires){ h, host_command, host_busy };
    h->snap = g_byte_array_new();
    qemu_add_vm_change_state_handler(host_vm_state, h);
    /* Registered before the chip's exit report, so it runs after it. */
    if (getenv("C66X_JIT_PROFILE")) {
        h->profile_exit.notify = host_write_profile;
        qemu_add_exit_notifier(&h->profile_exit);
    }
}

c66x_core *cdj_dsp_host_core(CdjDspHost *h, const c66x_bus *bus)
{
    h->core = c66x_new(bus);
    return h->core;
}

static void host_start_thread(CdjDspHost *h)
{
    qemu_thread_create(&h->thread, h->name, host_thread, h,
                       QEMU_THREAD_DETACHED);
    if (h->slack_ns) {
        h->slack_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, slack_tick, h);
        timer_mod(h->slack_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                                  + SLACK_POLL_NS);
        if (cdj_report_enabled()) {
            info_report("%s: core kept within %" PRId64 " us of the virtual "
                        "clock", h->name, h->slack_ns / 1000);
        }
    }
}

void cdj_dsp_host_run(CdjDspHost *h, uint32_t entry)
{
    c66x_reset(h->core, entry);
    h->running = true;
    h->dsp_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (cdj_report_enabled()) {
        info_report("%s: started at 0x%08x", h->name, entry);
    }
    host_start_thread(h);
}

void cdj_dsp_host_resume(CdjDspHost *h)
{
    c66x_load(h->core, h->snap->data);
    qatomic_set(&h->paused, !runstate_is_running());
    if (h->running) {
        host_start_thread(h);
    }
}

uint8_t *cdj_dsp_ram(MemoryRegion *mr, const char *name, uint64_t size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    return memory_region_get_ram_ptr(mr);
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
                " packets, last stop %d, %" PRIu64 " cycles skipped idle",
                h->name, h->running ? "running" : "stopped",
                c66x_get_pc(h->core), st.cycles, st.packets, h->last_stop,
                h->idle_skipped);
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
