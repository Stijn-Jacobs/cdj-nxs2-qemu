/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj.h"
#include "../cdj_getenv.h"
/*
 * Held memory writes applied from QEMU timers. These do the job of a
 * debugger poke without the gdbstub, whose attach halts MAIN long enough to
 * break the GUI link. Experiment knobs, all off by default.
 *
 * CDJ_DIRTY_MS=<ms>: set the display frame-dirty flag 0x10DC1510 every <ms>.
 * The tick at 0x0838E952 rebuilds the tx frame only while it reads 1; the
 * firmware sets it once at init (0x0838E764) and clears it at 0x0838EE20.
 */
#define CDJ_DIRTY_FLAG_ADDR  0x10DC1510

static QEMUTimer *cdj_dirty_timer;
static uint64_t cdj_dirty_writes;

static int64_t cdj_dirty_ms(void)
{
    static int64_t ms = -1;

    if (ms < 0) {
        const char *e = getenv("CDJ_DIRTY_MS");

        ms = e ? (int64_t)strtoll(e, NULL, 0) : 0;
    }
    return ms;
}

/*
 * CDJ_MPOKE=<addr>:<val>[:<size>],...: hold up to 8 words (size 1, 2 or 4,
 * default 4), re-written every CDJ_MPOKE_MS (default 50).
 * Example: CDJ_MPOKE=0x0A35F71C:1:4 marks the DSP subsystem ready in the
 * status table at 0x0A35F718.
 */
#define CDJ_MPOKE_MAX 8

static struct { uint32_t addr, val; unsigned size; } cdj_mpoke[CDJ_MPOKE_MAX];
static unsigned cdj_mpoke_n;
static bool cdj_mpoke_parsed;

static void cdj_mpoke_parse(void)
{
    const char *e = getenv("CDJ_MPOKE");

    cdj_mpoke_parsed = true;
    while (e && *e && cdj_mpoke_n < CDJ_MPOKE_MAX) {
        const char *start = e;
        uint32_t a = strtoul(e, (char **)&e, 0);
        uint32_t v = 0;
        unsigned sz = 4;

        if (*e == ':') {
            v = strtoul(e + 1, (char **)&e, 0);
        }
        if (*e == ':') {
            sz = strtoul(e + 1, (char **)&e, 0);
        }
        if (e == start) {           /* unparsable -- stop rather than spin */
            break;
        }
        if (sz != 1 && sz != 2 && sz != 4) {
            sz = 4;
        }
        cdj_mpoke[cdj_mpoke_n].addr = a;
        cdj_mpoke[cdj_mpoke_n].val = v;
        cdj_mpoke[cdj_mpoke_n].size = sz;
        cdj_mpoke_n++;
        e += *e == ',';
    }
    if (cdj_mpoke_n) {
        unsigned i;

        for (i = 0; i < cdj_mpoke_n; i++) {
            info_report("mpoke: holding *0x%08x = 0x%x (%u bytes)",
                        cdj_mpoke[i].addr, cdj_mpoke[i].val,
                        cdj_mpoke[i].size);
        }
    }
}

/*
 * CDJ_MCOPY=<src>:<dst>:<len>[:i],...: copy <len> bytes (up to 128 KiB) from
 * <src> to <dst> on the CDJ_MPOKE timer. With ":i" <src> holds a pointer to
 * the source, read at copy time. Example: CDJ_MCOPY=0x0B531EB8:0x0B52C284:8
 * copies the CueWave waveform pointer and size into the track context.
 */
#define CDJ_MCOPY_MAX 4
#define CDJ_MCOPY_MAXLEN (128 * 1024)

static struct {
    uint32_t src, dst;
    unsigned len;
    bool indirect;
} cdj_mcopy[CDJ_MCOPY_MAX];
static unsigned cdj_mcopy_n;
static uint8_t *cdj_mcopy_buf;

static void cdj_mcopy_parse(void)
{
    const char *e = getenv("CDJ_MCOPY");

    while (e && *e && cdj_mcopy_n < CDJ_MCOPY_MAX) {
        const char *start = e;
        uint32_t src = strtoul(e, (char **)&e, 0);
        uint32_t dst = (*e == ':') ? strtoul(e + 1, (char **)&e, 0) : 0;
        unsigned len = (*e == ':') ? strtoul(e + 1, (char **)&e, 0) : 0;
        bool ind = false;

        if (e[0] == ':' && (e[1] == 'i' || e[1] == 'I')) {
            ind = true;
            e += 2;
        }
        if (e == start) {
            break;                          /* unparsable -- stop, not spin */
        }
        if (dst && len && len <= CDJ_MCOPY_MAXLEN) {
            cdj_mcopy[cdj_mcopy_n++] = (typeof(cdj_mcopy[0])) {
                .src = src, .dst = dst, .len = len, .indirect = ind,
            };
            info_report("mcopy: holding *0x%08x = %s0x%08x (%u bytes)",
                        dst, ind ? "**" : "*", src, len);
        } else {
            warn_report("mcopy: bad entry '%s' (want <src>:<dst>:<len>[:i], "
                        "len <= %u)", start, CDJ_MCOPY_MAXLEN);
        }
        e += *e == ',';
    }
    if (cdj_mcopy_n) {
        cdj_mcopy_buf = g_malloc(CDJ_MCOPY_MAXLEN);
    }
}

/*
 * CDJ_MFILL=<addr>:<len>:<val>,...: fill a range with a 32-bit value on the
 * CDJ_MPOKE timer. Example: CDJ_MFILL=0x0B568C9C:0x1A8:2 fills the deck-state
 * block. MAIN republishes that block faster than 50 ms, so use a short
 * CDJ_MPOKE_MS.
 */
#define CDJ_MFILL_MAX 4
#define CDJ_MFILL_MAXLEN (64 * 1024)

static struct { uint32_t addr, val; unsigned len; } cdj_mfill[CDJ_MFILL_MAX];
static unsigned cdj_mfill_n;

static void cdj_mfill_parse(void)
{
    const char *e = getenv("CDJ_MFILL");

    while (e && *e && cdj_mfill_n < CDJ_MFILL_MAX) {
        char *end;
        uint32_t a = strtoul(e, &end, 0);
        unsigned len, val;

        if (end == e || *end != ':') {
            break;
        }
        e = end + 1;
        len = strtoul(e, &end, 0);
        if (end == e || *end != ':') {
            break;
        }
        e = end + 1;
        val = strtoul(e, &end, 0);
        if (end == e) {
            break;
        }
        e = end;
        if (!len || len > CDJ_MFILL_MAXLEN) {
            warn_report("mfill: %#x:%u ignored (len must be 1..%u)",
                        a, len, CDJ_MFILL_MAXLEN);
        } else {
            cdj_mfill[cdj_mfill_n].addr = a;
            cdj_mfill[cdj_mfill_n].len = len & ~3u;
            cdj_mfill[cdj_mfill_n].val = val;
            cdj_mfill_n++;
            info_report("mfill: holding *0x%08x .. +0x%x = 0x%x (32-bit words)",
                        a, len & ~3u, val);
        }
        e += *e == ',';
    }
}

/*
 * CDJ_LOADBIN=<addr>:<path>,...: copy a host file (up to 256 KiB) into guest
 * RAM, re-applied on the CDJ_MPOKE timer. Used to supply a beat grid for the
 * time-to-beat mapper 0x0831E77C, which reads *(track_object + 0x124): count
 * at +4, 20-byte entries from +0x1C with the time in ms at entry +4.
 */
#define CDJ_LOADBIN_MAX 2
#define CDJ_LOADBIN_MAXLEN (256 * 1024)

static struct { uint32_t addr; uint8_t *buf; unsigned len; } cdj_loadbin[CDJ_LOADBIN_MAX];
static unsigned cdj_loadbin_n;

static void cdj_loadbin_parse(void)
{
    const char *e = getenv("CDJ_LOADBIN");

    while (e && *e && cdj_loadbin_n < CDJ_LOADBIN_MAX) {
        char *end;
        uint32_t a = strtoul(e, &end, 0);
        const char *path, *comma;
        char *file;
        FILE *f;
        long sz;

        if (end == e || *end != ':') {
            break;
        }
        path = end + 1;
        comma = strchr(path, ',');
        file = comma ? g_strndup(path, comma - path) : g_strdup(path);
        f = fopen(file, "rb");
        if (!f) {
            warn_report("loadbin: cannot open %s", file);
        } else {
            fseek(f, 0, SEEK_END);
            sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz <= 0 || sz > CDJ_LOADBIN_MAXLEN) {
                warn_report("loadbin: %s is %ld bytes (max %u)", file, sz,
                            CDJ_LOADBIN_MAXLEN);
            } else {
                uint8_t *b = g_malloc(sz);

                if (fread(b, 1, sz, f) != (size_t)sz) {
                    warn_report("loadbin: short read on %s", file);
                    g_free(b);
                } else {
                    cdj_loadbin[cdj_loadbin_n].addr = a;
                    cdj_loadbin[cdj_loadbin_n].buf = b;
                    cdj_loadbin[cdj_loadbin_n].len = sz;
                    cdj_loadbin_n++;
                    info_report("loadbin: %s (%ld bytes) -> 0x%08x", file, sz, a);
                }
            }
            fclose(f);
        }
        g_free(file);
        e = comma ? comma + 1 : path + strlen(path);
    }
}

static void cdj_mpoke_apply(void)
{
    unsigned i;

    for (i = 0; i < cdj_mpoke_n; i++) {
        /* The guest is little-endian, so a plain host store matches its view. */
        uint32_t v = cdj_mpoke[i].val;

        cpu_physical_memory_write(cdj_mpoke[i].addr, &v, cdj_mpoke[i].size);
    }
    for (i = 0; i < cdj_loadbin_n; i++) {
        cpu_physical_memory_write(cdj_loadbin[i].addr, cdj_loadbin[i].buf,
                                  cdj_loadbin[i].len);
    }
    for (i = 0; i < cdj_mfill_n; i++) {
        uint32_t v = cdj_mfill[i].val;
        unsigned off;

        for (off = 0; off < cdj_mfill[i].len; off += 4) {
            cpu_physical_memory_write(cdj_mfill[i].addr + off, &v, 4);
        }
    }
    for (i = 0; i < cdj_mcopy_n; i++) {
        uint32_t src = cdj_mcopy[i].src;

        if (cdj_mcopy[i].indirect) {
            uint32_t p = 0;

            cpu_physical_memory_read(src, &p, 4);
            /* The firmware has not produced the buffer yet. */
            if (p < 0x08000000) {
                continue;
            }
            src = p;
        }
        cpu_physical_memory_read(src, cdj_mcopy_buf, cdj_mcopy[i].len);
        cpu_physical_memory_write(cdj_mcopy[i].dst, cdj_mcopy_buf,
                                  cdj_mcopy[i].len);
    }
}

/* CDJ_DIRTY_MS and CDJ_MPOKE_MS have separate timers so either can be used
 * alone. */
static QEMUTimer *cdj_mpoke_timer;
static uint64_t cdj_mpoke_writes;

static int64_t cdj_mpoke_ms(void)
{
    static int64_t ms = -1;

    if (ms < 0) {
        const char *e = getenv("CDJ_MPOKE_MS");

        ms = e ? (int64_t)strtoll(e, NULL, 0) : 50;
    }
    return ms > 0 ? ms : 50;
}

static void cdj_mpoke_tick(void *opaque)
{
    cdj_mpoke_apply();
    cdj_mpoke_writes++;
    timer_mod(cdj_mpoke_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + cdj_mpoke_ms() * 1000000);
}

static void cdj_dirty_tick(void *opaque)
{
    uint8_t one = 1;

    cpu_physical_memory_write(CDJ_DIRTY_FLAG_ADDR, &one, 1);
    cdj_dirty_writes++;
    timer_mod(cdj_dirty_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + cdj_dirty_ms() * 1000000);
}

static void cdj_dirty_report(Notifier *n, void *unused)
{
    if (cdj_dirty_writes) {
        info_report("display: frame-dirty flag re-asserted %" PRIu64
                    " times (CDJ_DIRTY_MS=%" PRId64 ")",
                    cdj_dirty_writes, cdj_dirty_ms());
    }
    if (cdj_mpoke_writes) {
        info_report("mpoke: %u address(es) re-asserted %" PRIu64 " times",
                    cdj_mpoke_n, cdj_mpoke_writes);
    }
}

static Notifier cdj_dirty_exit;

void cdj_dirty_init(void)
{
    if (!cdj_mpoke_parsed) {
        cdj_mpoke_parse();
    }
    cdj_mcopy_parse();
    cdj_mfill_parse();
    cdj_loadbin_parse();
    if (cdj_dirty_ms() || cdj_mpoke_n || cdj_mcopy_n || cdj_mfill_n
        || cdj_loadbin_n) {
        cdj_dirty_exit.notify = cdj_dirty_report;
        qemu_add_exit_notifier(&cdj_dirty_exit);
    }
    if (cdj_dirty_ms()) {
        cdj_dirty_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_dirty_tick,
                                       NULL);
        timer_mod(cdj_dirty_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                  + cdj_dirty_ms() * 1000000);
        info_report("display: holding the frame-dirty flag set every %" PRId64
                    " ms", cdj_dirty_ms());
    }
    /* One timer, paced by CDJ_MPOKE_MS, for pokes, copies, fills and files. */
    if (cdj_mpoke_n || cdj_mcopy_n || cdj_mfill_n || cdj_loadbin_n) {
        cdj_mpoke_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_mpoke_tick,
                                       NULL);
        timer_mod(cdj_mpoke_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                  + cdj_mpoke_ms() * 1000000);
    }
}

