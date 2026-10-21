/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
#include "cdj_getenv.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "cdj_gui_keys.h"
#include "bfin/bf531.h"
#include "bfin/bfin_gdb.h"
#include "qemu/main-loop.h"
#include "qemu/sockets.h"
#include "qapi/error.h"
#include "sysemu/runstate.h"
#include <errno.h>
/*
 * CDJ-2000/CDJ-2000NXS display processor: the ADSP-BF531 on the other end
 * of SPORT1, in its own window. The core itself is plain C with no QEMU
 * dependency (hw/cdj/bfin/); this file is the QEMU side of it, a display
 * surface and a timer that steps the chip on its own clock.
 *
 * SPORT1 is MAIN's link (display_link.c): a MAIN packet lands the moment
 * the firmware arms its receive DMA, and each answer it transmits goes
 * straight to MAIN's receive block. CDJ_BF531_SPORT1 hands it one
 * hex-encoded packet the same way, for bring-up without MAIN's sender.
 */

#define CDJ_BF531_W     480
#define CDJ_BF531_H     255

/* One PPI/DMA0 unit's worth of the display processor's own 400 MHz clock
 * (see bf531.c's dma_start_unit): stepping this much a QEMU timer tick
 * keeps its own frame rate without syncing it to MAIN's icount. */
#define CDJ_BF531_QUANTUM (BF531_CCLK_HZ / cdj_bf531_tick_hz)

/* CDJ_BF531_TICK_HZ: ticks per virtual second (default 60). A link that
 * answers frame by frame pays one or two ticks per round trip, so it needs a
 * finer quantum than the stock firmware's lockstep. */
static unsigned cdj_bf531_tick_hz = 60;

typedef struct CdjBf531 {
    bf531 *chip;
    QemuConsole *con;
    QEMUTimer *tick;
    unsigned w, h;
    uint64_t frames;
    GByteArray *snap;           /* the chip's image, only while saving or loading */
    Notifier exit;
    /* CDJ_BF531_GDB: host-side debugger state, never migrated. */
    int gdb_listen;             /* -1 when off */
    int gdb_fd;                 /* -1 when no debugger is attached */
    bool gdb_lost;              /* a send failed; dropped after the call */
    bfin_gdb *gdb;
    /* CDJ_BF531_TRACE: tracepoint records, drained to a file every tick. */
    FILE *trace_fp;
    unsigned trace_n;           /* points */
    char **trace_name;
    uint64_t *trace_count;
    uint64_t trace_total, trace_lost;
} CdjBf531;

static CdjBf531 cdj_bf531;

void cdj2000_display_send(const uint8_t *pkt, size_t len)
{
    bf531_sport1_rx(cdj_bf531.chip, pkt, len);
}

static void cdj_bf531_answer(void *opaque, const uint8_t *pkt, size_t len)
{
    cdj2000_display_link_receive(pkt, len);
}

/* PF1, which MAIN reads as bit 2 of its port latch +0x48: low once the
 * firmware's link is up, and set (FIO_FLAG_S at 0xB7C13E) just before each
 * answer it transmits. MAIN starts the link on a 0 and takes an answer only
 * when the bit is 1; any other answer it logs as the display asking for the
 * packet again (0x042134CC). */
bool cdj2000_display_pf1(void)
{
    return bf531_flags(cdj_bf531.chip) & (1u << 1);
}

static void cdj_bf531_frame(void *opaque, const uint16_t *px, unsigned w, unsigned h)
{
    CdjBf531 *s = opaque;
    DisplaySurface *ds;
    uint32_t *dst;
    unsigned n;

    if (w != s->w || h != s->h) {
        dpy_gfx_replace_surface(s->con, qemu_create_displaysurface(w, h));
        qemu_console_resize(s->con, w, h);
        s->w = w;
        s->h = h;
    }
    ds = qemu_console_surface(s->con);
    dst = (uint32_t *)surface_data(ds);
    for (n = 0; n < (unsigned)w * h; n++) {
        uint16_t p = px[n];

        dst[n] = 0xFF000000 | (((p >> 10) & 0x1F) * 255 / 31) << 16 |
                 (((p >> 5) & 0x1F) * 255 / 31) << 8 | ((p & 0x1F) * 255 / 31);
    }
    s->frames++;
    dpy_gfx_update(s->con, 0, 0, w, h);
}

static void cdj_bf531_invalidate(void *opaque)
{
}

static void cdj_bf531_update(void *opaque)
{
}

static const GraphicHwOps cdj_bf531_ops = {
    .invalidate = cdj_bf531_invalidate,
    .gfx_update = cdj_bf531_update,
};

static void cdj_bf531_gdb_drop(CdjBf531 *s);

/*
 * Debugger (CDJ_BF531_GDB=<unix socket path | tcp port | host:port>).
 *
 * While gdb is attached and the target is halted the whole machine is
 * stopped (vm_stop), so MAIN and the DSP freeze with the display processor.
 * Continue is vm_start(): the tick below runs the chip and, at a breakpoint
 * or watchpoint, reports the stop and stops the machine again. A single step
 * runs in place on the main loop, with the machine stopped, because nothing
 * but the chip's own cycle count advances the chip's peripherals.
 */
static void cdj_bf531_gdb_write(void *opaque, const uint8_t *buf, size_t len)
{
    CdjBf531 *s = opaque;

    while (len && s->gdb_fd >= 0 && !s->gdb_lost) {
        ssize_t n = send(s->gdb_fd, buf, len, 0);

        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            s->gdb_lost = true;
            return;
        }
        buf += n;
        len -= n;
    }
}

static int cdj_bf531_gdb_peek(void *opaque, uint32_t addr, uint8_t *buf,
                              size_t len)
{
    CdjBf531 *s = opaque;

    return bf531_peek(s->chip, addr, buf, len);
}

static int cdj_bf531_gdb_poke(void *opaque, uint32_t addr, const uint8_t *buf,
                              size_t len)
{
    CdjBf531 *s = opaque;

    return bf531_poke(s->chip, addr, buf, len);
}

/* After the session saw bytes or the chip stopped: act on what it wants. */
static void cdj_bf531_gdb_after(CdjBf531 *s)
{
    if (s->gdb_lost || bfin_gdb_detached(s->gdb)) {
        cdj_bf531_gdb_drop(s);
        return;
    }
    switch (bfin_gdb_run_request(s->gdb)) {
    case BFIN_GDB_CONT:
        if (bfin_gdb_interrupted(s->gdb)) {
            bfin_gdb_stopped(s->gdb, BFIN_STOP_BUDGET);
            if (runstate_is_running()) {
                vm_stop(RUN_STATE_DEBUG);
            }
        } else if (!runstate_is_running()) {
            vm_start();
        }
        break;
    case BFIN_GDB_STEP: {
        bfin_stop stop = bf531_stepi(s->chip);

        if (stop == BFIN_STOP_UNDEF) {
            error_report("cdj_bf531: unimplemented instruction 0x%" PRIx64
                         " at 0x%08x", bfin_trap_insn(bf531_core(s->chip)),
                         bfin_trap_pc(bf531_core(s->chip)));
        }
        bfin_gdb_stopped(s->gdb, stop);
        break;
    }
    default:
        break;
    }
    if (s->gdb_lost) {
        cdj_bf531_gdb_drop(s);
    }
}

static void cdj_bf531_gdb_read(void *opaque)
{
    CdjBf531 *s = opaque;
    uint8_t buf[4096];
    ssize_t n = recv(s->gdb_fd, buf, sizeof(buf), 0);

    if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
        return;
    }
    if (n <= 0) {
        cdj_bf531_gdb_drop(s);
        return;
    }
    bfin_gdb_feed(s->gdb, buf, n);
    cdj_bf531_gdb_after(s);
}

/* Detach, kill or a dropped connection: no breakpoint outlives the debugger
 * and the machine runs again. */
static void cdj_bf531_gdb_drop(CdjBf531 *s)
{
    bfin_core *core = bf531_core(s->chip);

    qemu_set_fd_handler(s->gdb_fd, NULL, NULL, NULL);
    close(s->gdb_fd);
    s->gdb_fd = -1;
    s->gdb_lost = false;
    bfin_break_clear(core);
    bfin_watch_clear(core);
    bfin_gdb_free(s->gdb);
    s->gdb = NULL;
    if (!runstate_is_running()) {
        vm_start();
    }
}

static void cdj_bf531_gdb_accept(void *opaque)
{
    CdjBf531 *s = opaque;
    bfin_gdb_ops ops = {
        .opaque = s,
        .write = cdj_bf531_gdb_write,
        .read_mem = cdj_bf531_gdb_peek,
        .write_mem = cdj_bf531_gdb_poke,
    };
    int fd = qemu_accept(s->gdb_listen, NULL, NULL);

    if (fd < 0) {
        return;
    }
    if (s->gdb) {                       /* one debugger at a time */
        close(fd);
        return;
    }
    socket_set_nodelay(fd);             /* fails on a unix socket: harmless */
    s->gdb_fd = fd;
    s->gdb_lost = false;
    s->gdb = bfin_gdb_new(bf531_core(s->chip), &ops);
    qemu_set_fd_handler(fd, cdj_bf531_gdb_read, NULL, s);
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_DEBUG);
    }
    info_report("cdj_bf531: debugger attached, machine stopped");
}

static void cdj_bf531_gdb_listen(CdjBf531 *s, const char *spec)
{
    Error *err = NULL;
    g_autofree char *str = NULL;
    SocketAddress *addr;

    if (g_ascii_isdigit(spec[0]) && !strchr(spec, ':')) {
        str = g_strdup_printf("127.0.0.1:%s", spec);
    } else if (spec[0] == '/' || spec[0] == '.') {
        str = g_strdup_printf("unix:%s", spec);
    } else {
        str = g_strdup(spec);
    }
    addr = socket_parse(str, &err);
    if (addr) {
        s->gdb_listen = socket_listen(addr, 1, &err);
        qapi_free_SocketAddress(addr);
    }
    if (s->gdb_listen < 0) {
        error_report_err(err);
        return;
    }
    qemu_socket_set_nonblock(s->gdb_listen);
    qemu_set_fd_handler(s->gdb_listen, cdj_bf531_gdb_accept, NULL, s);
    info_report("cdj_bf531: gdb listening on %s", str);
}

/*
 * Trace file (little endian): "BFTRACE1", u32 cclk_hz, u32 npoints, then per
 * point u16 length + name; then 32-byte records until EOF:
 *   u64 cycles, u32 pc, u32 r0, u32 r1, u32 r2, u16 id, u16 kind, u32 0.
 * kind 0 = a hit; kind 1 = LOST: r0|r1<<32 = hits dropped so far, cycles =
 * when the glue noticed (everything between the previous record and the first
 * hit after this one may be incomplete).
 */
typedef struct QEMU_PACKED {
    uint64_t cycles;
    uint32_t pc, r0, r1, r2;
    uint16_t id, kind;
    uint32_t pad;
} CdjTraceRec;

static void cdj_bf531_trace_drain(CdjBf531 *s)
{
    bfin_core *core = bf531_core(s->chip);
    bfin_trace_rec buf[1024];
    size_t n;

    if (!s->trace_fp) {
        return;
    }
    while ((n = bfin_trace_drain(core, buf, ARRAY_SIZE(buf))) != 0) {
        for (size_t i = 0; i < n; i++) {
            CdjTraceRec r = { buf[i].cycles, buf[i].pc, buf[i].r0, buf[i].r1,
                              buf[i].r2, buf[i].id, 0, 0 };

            fwrite(&r, sizeof(r), 1, s->trace_fp);
            if (buf[i].id < s->trace_n) {
                s->trace_count[buf[i].id]++;
            }
        }
        s->trace_total += n;
    }
    if (bfin_trace_lost(core) != s->trace_lost) {
        uint64_t lost = bfin_trace_lost(core);
        CdjTraceRec r = { bfin_cycles(core), 0, (uint32_t)lost,
                          (uint32_t)(lost >> 32), 0, 0, 1, 0 };

        if (!s->trace_lost) {
            error_report("cdj_bf531: TRACE RING OVERFLOW, hits were dropped "
                         "(the trace file has a LOST record)");
        }
        s->trace_lost = lost;
        fwrite(&r, sizeof(r), 1, s->trace_fp);
    }
    fflush(s->trace_fp);
}

/* CDJ_BF531_TRACE=<points file>:<out file>; points are "<hex addr> <name>"
 * per line, the id being the line index. */
static void cdj_bf531_trace_open(CdjBf531 *s, const char *spec)
{
    g_autofree char *spec_c = g_strdup(spec);
    char *out = strchr(spec_c, ':');
    g_autofree char *text = NULL;
    g_auto(GStrv) lines = NULL;
    bfin_core *core = bf531_core(s->chip);
    uint32_t hz = BF531_CCLK_HZ, n = 0;
    uint8_t hdr[16] = "BFTRACE1";

    if (!out) {
        error_report("cdj_bf531: CDJ_BF531_TRACE wants <points>:<out>");
        return;
    }
    *out++ = 0;
    if (!g_file_get_contents(spec_c, &text, NULL, NULL)) {
        error_report("cdj_bf531: cannot read trace points %s", spec_c);
        return;
    }
    lines = g_strsplit(text, "\n", -1);
    for (char **l = lines; *l; l++) {
        if (**l) {
            n++;
        }
    }
    if (n > BFIN_MAX_TRACE) {
        error_report("cdj_bf531: %u trace points, at most %d", n,
                     BFIN_MAX_TRACE);
        return;
    }
    s->trace_fp = fopen(out, "wb");
    if (!s->trace_fp) {
        error_report("cdj_bf531: cannot write %s", out);
        return;
    }
    s->trace_name = g_new0(char *, n + 1);
    s->trace_count = g_new0(uint64_t, n + 1);
    memcpy(hdr + 8, &hz, 4);            /* little endian host assumed */
    memcpy(hdr + 12, &n, 4);
    fwrite(hdr, sizeof(hdr), 1, s->trace_fp);
    for (char **l = lines; *l; l++) {
        char *end;
        uint32_t addr;
        uint16_t len;

        if (!**l) {
            continue;
        }
        addr = strtoul(*l, &end, 16);
        while (*end == ' ' || *end == '\t') {
            end++;
        }
        s->trace_name[s->trace_n] = g_strdup(*end ? end : "?");
        len = strlen(s->trace_name[s->trace_n]);
        fwrite(&len, 2, 1, s->trace_fp);
        fwrite(s->trace_name[s->trace_n], len, 1, s->trace_fp);
        if (bfin_trace_add(core, addr, s->trace_n) < 0) {
            error_report("cdj_bf531: cannot trace 0x%x (%s)", addr, end);
        }
        s->trace_n++;
    }
    fflush(s->trace_fp);
    info_report("cdj_bf531: tracing %u points to %s", n, out);
}

static void cdj_bf531_trace_report(CdjBf531 *s)
{
    if (!s->trace_fp) {
        return;
    }
    cdj_bf531_trace_drain(s);
    for (unsigned i = 0; i < s->trace_n; i++) {
        info_report("cdj_bf531: trace %-40s %" PRIu64, s->trace_name[i],
                    s->trace_count[i]);
    }
    info_report("cdj_bf531: trace %" PRIu64 " records, %" PRIu64 " LOST",
                s->trace_total, s->trace_lost);
    fclose(s->trace_fp);
    s->trace_fp = NULL;
}

static void cdj_bf531_tick(void *opaque)
{
    CdjBf531 *s = opaque;
    bfin_stop stop;

    /* Halted under the debugger but the machine was resumed from elsewhere
     * (the monitor): the chip stays where it stopped. */
    if (s->gdb && bfin_gdb_run_request(s->gdb) == BFIN_GDB_HALT) {
        timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND / cdj_bf531_tick_hz);
        return;
    }
    stop = bf531_run(s->chip, CDJ_BF531_QUANTUM);
    cdj_bf531_trace_drain(s);
    if (s->gdb && (stop == BFIN_STOP_BREAK || stop == BFIN_STOP_WATCH ||
                   stop == BFIN_STOP_UNDEF)) {
        if (stop == BFIN_STOP_UNDEF) {
            error_report("cdj_bf531: unimplemented instruction 0x%" PRIx64
                         " at 0x%08x", bfin_trap_insn(bf531_core(s->chip)),
                         bfin_trap_pc(bf531_core(s->chip)));
        }
        /* The timer stays armed (the saved image must find it so); the
         * stopped machine's virtual clock does not advance it. */
        timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  NANOSECONDS_PER_SECOND / cdj_bf531_tick_hz);
        bfin_gdb_stopped(s->gdb, stop);
        /* Not vm_stop(): it disables the virtual clock and waits for its
         * timers, this callback among them. The main loop stops the machine
         * right after; until then the tick sees the halt and runs nothing. */
        qemu_system_vmstop_request_prepare();
        qemu_system_vmstop_request(RUN_STATE_DEBUG);
        if (s->gdb_lost) {
            cdj_bf531_gdb_drop(s);
        }
        return;
    }
    if (stop == BFIN_STOP_UNDEF) {
        error_report("cdj_bf531: unimplemented instruction 0x%" PRIx64
                     " at 0x%08x", bfin_trap_insn(bf531_core(s->chip)),
                     bfin_trap_pc(bf531_core(s->chip)));
        return;
    }
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / cdj_bf531_tick_hz);
}

static void cdj_bf531_report(Notifier *n, void *opaque)
{
    CdjBf531 *s = container_of(n, CdjBf531, exit);

    static const struct { const char *name; uint32_t addr, len; } banks[] = {
        { "l1a.bin", 0xFF800000, 0x8000 }, { "l1b.bin", 0xFF900000, 0x8000 },
        { "l1s.bin", 0xFFB00000, 0x1000 },
    };
    const char *dir = getenv("CDJ_BF531_DUMP");

    info_report("cdj_bf531: %" PRIu64 " PPI frames rendered", s->frames);
    if (s->chip) {
        info_report("cdj_bf531: %" PRIu64 " link receives too small for the waiting record",
                    bf531_sport1_waits(s->chip));
    }
    cdj_bf531_trace_report(s);
    if (s->chip) {
        const char *flash_out = getenv("CDJ_BF531_FLASH_OUT");

        bf531_code_range_report(s->chip);
        bf531_flash_report(s->chip);
        if (flash_out && bf531_flash_save(s->chip, flash_out)) {
            error_report("cdj_bf531: cannot write %s", flash_out);
        }
    }
    /* CDJ_BF531_DUMP=<dir>: the GUI's memory at exit, for offline decoding. */
    if (dir && s->chip) {
        uint32_t size;
        const uint8_t *sdram = bf531_sdram(s->chip, &size);
        g_autofree char *p = g_build_filename(dir, "sdram.bin", NULL);

        if (!g_file_set_contents(p, (const char *)sdram, size, NULL)) {
            error_report("cdj_bf531: cannot write %s", p);
        }
        for (unsigned i = 0; i < ARRAY_SIZE(banks); i++) {
            g_autofree char *q = g_build_filename(dir, banks[i].name, NULL);
            const uint8_t *m = bf531_mem(s->chip, banks[i].addr, banks[i].len);

            if (!m || !g_file_set_contents(q, (const char *)m, banks[i].len, NULL)) {
                error_report("cdj_bf531: cannot write %s", q);
            }
        }
        g_autofree char *timing_path = g_build_filename(dir, "ownership-timing.json", NULL);
        FILE *timing = fopen(timing_path, "w");
        if (!timing) {
            error_report("cdj_bf531: cannot write %s", timing_path);
        } else {
            int failed = bf531_timing_json(s->chip, timing);
            if (fclose(timing) || failed) { error_report("cdj_bf531: incomplete %s", timing_path); }
        }
        g_autofree char *injection_path = g_build_filename(dir, "ppi-injection.json", NULL);
        FILE *injection = fopen(injection_path, "w");
        if (!injection) {
            error_report("cdj_bf531: cannot write %s", injection_path);
        } else {
            int failed = bf531_ppi_delay_json(s->chip, injection);
            if (fclose(injection) || failed) { error_report("cdj_bf531: incomplete %s", injection_path); }
        }
        info_report("cdj_bf531: memory dumped to %s", dir);
    }
}

/* The chip runs only inside the tick, on the main loop, so between two ticks
 * it is always at a point it can be saved from. Its SDRAM is in the image:
 * tens of MiB, held only while the save or load lasts. */
static int cdj_bf531_pre_save(void *opaque)
{
    CdjBf531 *s = opaque;
    size_t len = bf531_save(s->chip, NULL);

    s->snap = g_byte_array_sized_new(len);
    g_byte_array_set_size(s->snap, len);
    bf531_save(s->chip, s->snap->data);
    return 0;
}

static int cdj_bf531_post_save(void *opaque)
{
    CdjBf531 *s = opaque;

    g_byte_array_free(s->snap, true);
    s->snap = NULL;
    return 0;
}

static int cdj_bf531_post_load(void *opaque, int version_id)
{
    CdjBf531 *s = opaque;
    bool fits = s->snap && s->snap->len == bf531_save(s->chip, NULL);

    if (fits) {
        bf531_load(s->chip, s->snap->data);
    } else {
        error_report("cdj_bf531: the saved display does not fit this machine");
    }
    cdj_bf531_post_save(s);
    return fits ? 0 : -EINVAL;
}

static const VMStateDescription vmstate_cdj_bf531 = {
    .name = "cdj-bf531",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = cdj_bf531_pre_save,
    .post_save = cdj_bf531_post_save,
    .post_load = cdj_bf531_post_load,
    .fields = (const VMStateField[]) {
        CDJ_VMSTATE_BYTES(snap, CdjBf531),
        VMSTATE_TIMER_PTR(tick, CdjBf531),
        VMSTATE_UINT64(frames, CdjBf531),
        VMSTATE_END_OF_LIST()
    }
};

static size_t cdj_bf531_parse_hex(const char *s, uint8_t *out, size_t cap)
{
    size_t n = 0;

    while (s[0] && s[1] && n < cap) {
        unsigned byte;

        if (sscanf(s, "%2x", &byte) != 1) {
            break;
        }
        out[n++] = byte;
        s += 2;
    }
    return n;
}

/*
 * Host keyboard to front panel (cdj_gui_keys.c), the NXS2 window's layout on
 * this model's report bytes. The source keys sit one bit lower than on the
 * NXS2 because of the extra SD key, F. Only the keys with a bit in the
 * decoder's name table are here: no loop, slip, reverse, sync, master, tempo
 * range, master tempo or jog mode, and no REKORDBOX key (LINK is the first
 * source key). Keys pressed on the deck and seen to act are confirmed.
 */
static const CdjGuiKey cdj_bf531_keys[] = {
    { Q_KEY_CODE_SPC,           0x10, 0x01, "PLAY/PAUSE",   false },
    { Q_KEY_CODE_C,             0x10, 0x02, "CUE",          false },
    { Q_KEY_CODE_RET,           0x11, 0x01, "ROTARY PUSH",  true  },
    { Q_KEY_CODE_KP_ENTER,      0x11, 0x01, "ROTARY PUSH",  true  },
    { Q_KEY_CODE_RIGHT,         0x11, 0x01, "ROTARY PUSH",  true  },
    { Q_KEY_CODE_COMMA,         0x12, 0x02, "TRACK -",      false },
    { Q_KEY_CODE_DOT,           0x12, 0x04, "TRACK +",      false },
    { Q_KEY_CODE_BRACKET_LEFT,  0x12, 0x08, "SEARCH -",     false },
    { Q_KEY_CODE_BRACKET_RIGHT, 0x12, 0x10, "SEARCH +",     false },
    { Q_KEY_CODE_L,             0x13, 0x01, "LINK",         false },
    { Q_KEY_CODE_U,             0x13, 0x02, "USB",          true  },
    { Q_KEY_CODE_F,             0x13, 0x04, "SD",           false },
    { Q_KEY_CODE_D,             0x13, 0x08, "DISC",         false },
    { Q_KEY_CODE_B,             0x14, 0x01, "BROWSE",       true  },
    { Q_KEY_CODE_T,             0x14, 0x02, "TAG LIST",     false },
    { Q_KEY_CODE_I,             0x14, 0x04, "INFO",         false },
    { Q_KEY_CODE_M,             0x14, 0x08, "MENU",         false },
    { Q_KEY_CODE_ESC,           0x14, 0x10, "BACK",         false },
    { Q_KEY_CODE_LEFT,          0x14, 0x10, "BACK",         false },
    { Q_KEY_CODE_BACKSPACE,     0x14, 0x10, "BACK",         false },
};

/*
 * The CDJ-2000NXS decoder (the update's key-state writer) keeps the same
 * report bytes as the NXS2: payload bit to state bit, checked against the
 * name table for every key here. Its source keys are LINK, USB, SD and DISC
 * on bits 1-4 of byte 0x13 (bit 0 is the unnamed state bit +94/0x10). Left out
 * because the state bit they set carries no key name: slip, direction, sync,
 * master and the REKORDBOX key.
 */
static const CdjGuiKey cdj_bf531_nxs_keys[] = {
    { Q_KEY_CODE_SPC,           0x10, 0x01, "PLAY/PAUSE",   false },
    { Q_KEY_CODE_C,             0x10, 0x02, "CUE",          false },
    { Q_KEY_CODE_E,             0x10, 0x04, "RELOOP/EXIT",  false },
    { Q_KEY_CODE_W,             0x10, 0x08, "LOOP OUT",     false },
    { Q_KEY_CODE_Q,             0x10, 0x10, "LOOP IN",      false },
    { Q_KEY_CODE_RET,           0x11, 0x01, "ROTARY PUSH",  false },
    { Q_KEY_CODE_KP_ENTER,      0x11, 0x01, "ROTARY PUSH",  false },
    { Q_KEY_CODE_RIGHT,         0x11, 0x01, "ROTARY PUSH",  false },
    { Q_KEY_CODE_COMMA,         0x12, 0x04, "TRACK -",      false },
    { Q_KEY_CODE_DOT,           0x12, 0x08, "TRACK +",      false },
    { Q_KEY_CODE_BRACKET_LEFT,  0x12, 0x10, "SEARCH -",     false },
    { Q_KEY_CODE_BRACKET_RIGHT, 0x12, 0x20, "SEARCH +",     false },
    { Q_KEY_CODE_L,             0x13, 0x02, "LINK",         false },
    { Q_KEY_CODE_U,             0x13, 0x04, "USB",          false },
    { Q_KEY_CODE_F,             0x13, 0x08, "SD",           false },
    { Q_KEY_CODE_D,             0x13, 0x10, "DISC",         false },
    { Q_KEY_CODE_B,             0x14, 0x01, "BROWSE",       false },
    { Q_KEY_CODE_T,             0x14, 0x02, "TAG LIST",     false },
    { Q_KEY_CODE_I,             0x14, 0x04, "INFO",         false },
    { Q_KEY_CODE_M,             0x14, 0x08, "MENU",         false },
    { Q_KEY_CODE_ESC,           0x14, 0x10, "BACK",         false },
    { Q_KEY_CODE_LEFT,          0x14, 0x10, "BACK",         false },
    { Q_KEY_CODE_BACKSPACE,     0x14, 0x10, "BACK",         false },
    { Q_KEY_CODE_J,             0x15, 0x01, "JOG MODE",     false },
    { Q_KEY_CODE_P,             0x15, 0x08, "TEMPO RANGE",  false },
    { Q_KEY_CODE_K,             0x15, 0x10, "MASTER TEMPO", false },
};

const Cdj2000Display cdj2000_display = {
    .sdram_size = 16 * MiB,
    .keys = cdj_bf531_keys,
    .key_count = ARRAY_SIZE(cdj_bf531_keys),
    .lever_fwd = true,
};

const Cdj2000Display cdj2000nxs_display = {
    .sdram_size = 32 * MiB,
    .keys = cdj_bf531_nxs_keys,
    .key_count = ARRAY_SIZE(cdj_bf531_nxs_keys),
    .lever_fwd = true,
};

/* Test-injection configuration is deliberately strict: decimal integers only,
 * before any firmware runs; malformed inputs must not silently disable a test. */
static bool cdj_bf531_bounded_decimal(const char *text, uint32_t limit, uint32_t *out)
{
    uint32_t n = 0;
    if (!text || !*text) { return false; }
    for (; *text; text++) {
        if (*text < '0' || *text > '9') { return false; }
        unsigned d = *text - '0';
        if (n > limit / 10 || (n == limit / 10 && d > limit % 10)) { return false; }
        n = n * 10 + d;
    }
    *out = n;
    return true;
}

/* CDJ_BF531_UPD=<path to a Pioneer GUI .UPD section> turns the display
 * processor on; without it this board runs MAIN alone, as before. */
bool cdj2000_display_init(const Cdj2000Display *desc)
{
    CdjBf531 *s = &cdj_bf531;
    bf531_host host = { s, cdj_bf531_frame, cdj_bf531_answer };
    const char *img_path = getenv("CDJ_BF531_UPD");
    const char *flash_path = getenv("CDJ_BF531_FLASH");
    const char *code_range = getenv("CDJ_BF531_CODE_RANGE");
    const char *sport1 = getenv("CDJ_BF531_SPORT1");
    const char *tick_hz = getenv("CDJ_BF531_TICK_HZ");
    const char *ppi_delay = getenv("CDJ_BF531_PPI_DELAY_US");
    const char *ppi_count = getenv("CDJ_BF531_PPI_DELAY_COUNT");
    uint32_t delay_us = 0, delay_count = 32;
    uint8_t *img;
    gsize len;

    if (desc->init) {
        desc->init();
        return false;
    }
    if (img_path && flash_path) {
        error_report("cdj_bf531: CDJ_BF531_UPD and CDJ_BF531_FLASH are both set; pick one");
        exit(1);
    }
    if (!img_path && !flash_path) {
        return false;
    }
    if ((ppi_delay && !cdj_bf531_bounded_decimal(ppi_delay, 50000, &delay_us)) ||
        (ppi_count && (!ppi_delay || !cdj_bf531_bounded_decimal(ppi_count, 1024, &delay_count))) ||
        (delay_us && !delay_count)) {
        error_report("cdj_bf531: invalid PPI delay injection; US=0..50000, COUNT=1..1024 (default 32)");
        exit(1);
    }
    if (tick_hz) {
        unsigned hz = strtoul(tick_hz, NULL, 10);

        if (hz < 60 || hz > 100000) {
            error_report("cdj_bf531: CDJ_BF531_TICK_HZ=%s out of 60..100000", tick_hz);
            exit(1);
        }
        cdj_bf531_tick_hz = hz;
    }
    if (!g_file_get_contents(img_path ? img_path : flash_path, (gchar **)&img,
                             &len, NULL)) {
        error_report("cdj_bf531: cannot read '%s'", img_path ? img_path : flash_path);
        return false;
    }

    s->chip = bf531_new(desc->sdram_size, &host, stderr);
    if (sport1) {
        uint8_t pkt[128];
        size_t n = cdj_bf531_parse_hex(sport1, pkt, sizeof(pkt));

        bf531_sport1_rx(s->chip, pkt, n);
    }
    if (code_range) {
        unsigned lo, hi;

        if (sscanf(code_range, "%x:%x", &lo, &hi) != 2 || lo >= hi) {
            error_report("cdj_bf531: CDJ_BF531_CODE_RANGE=%s is not <lo>:<hi> in hex", code_range);
            exit(1);
        }
        bf531_code_range(s->chip, lo, hi);
    }
    if (flash_path ? bf531_flash_load(s->chip, img, len) ||
                     bf531_flash_boot(s->chip)
                   : bf531_load_update(s->chip, img, len)) {
        error_report("cdj_bf531: '%s' is not %s", img_path ? img_path : flash_path,
                     img_path ? "an LDR boot stream" : "a 2 MiB flash image with an LDR at 0x10000");
        bf531_free(s->chip);
        s->chip = NULL;
        g_free(img);
        return false;
    }
    if (getenv("CDJ_BF531_DUMP")) {
        const char *start = getenv("CDJ_BF531_TIMING_START_PC");
        char *end = NULL;
        errno = 0;
        unsigned long pc = start ? strtoul(start, &end, 0) : 0;
        if ((start && (!*start || !end || *end || errno || pc > UINT32_MAX)) ||
            bf531_timing_enable(s->chip, start != NULL, (uint32_t)pc)) {
            error_report("cdj_bf531: invalid CDJ_BF531_TIMING_START_PC");
            bf531_free(s->chip); s->chip = NULL; g_free(img); return false;
        }
    }
    if (bf531_ppi_delay_configure(s->chip, delay_us, delay_count)) {
        error_report("cdj_bf531: PPI delay configuration rejected before startup");
        bf531_free(s->chip); s->chip = NULL; g_free(img); return false;
    }
    info_report("cdj_bf531: PPI delay injection configured us=%u count=%u (runtime marker gated)",
                delay_us, delay_count);
    g_free(img);

    s->w = CDJ_BF531_W;
    s->h = CDJ_BF531_H;
    s->con = graphic_console_init(NULL, 0, &cdj_bf531_ops, s);
    dpy_gfx_replace_surface(s->con, qemu_create_displaysurface(s->w, s->h));
    qemu_console_resize(s->con, s->w, s->h);
    cdj_gui_keys_init(desc->keys, desc->key_count, "cdj_bf531");
    cdj_gui_pointer_init();

    s->exit.notify = cdj_bf531_report;
    cdj_add_exit_report(&s->exit);

    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_bf531_tick, s);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    vmstate_register_any(NULL, &vmstate_cdj_bf531, s);
    s->gdb_listen = -1;
    s->gdb_fd = -1;
    if (getenv("CDJ_BF531_GDB")) {
        cdj_bf531_gdb_listen(s, getenv("CDJ_BF531_GDB"));
    }
    if (getenv("CDJ_BF531_TRACE")) {
        cdj_bf531_trace_open(s, getenv("CDJ_BF531_TRACE"));
    }
    return true;
}
