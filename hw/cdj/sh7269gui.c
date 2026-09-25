/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Pioneer CDJ-2000NXS2 GUI processor -- Renesas SH7269 (SH-2A), big-endian.
 *
 * The deck's second CPU, which drives the 800x480 LCD. Its firmware is
 * section 1 of the .UPD. The image starts with a load table at +0x10 of
 * 12-byte [dest][x][size] records placing chunks at 0x1C000000 (SDRAM),
 * 0xFFF84000 (on-chip RAM), 0x0E500000 and 0x2FFFB000.
 *
 * The CPU is QEMU's SH-4 core built big-endian (sh4eb) plus the SH-2A 32-bit
 * instructions. SH-2A has no MMU, so MMUCR stays disabled.
 */
#include "qemu/osdep.h"
#include "qemu/units.h"
#include "qemu/timer.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "cpu.h"
#include "hw/sysbus.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/misc/unimp.h"
#include "sysemu/sysemu.h"
#include "sysemu/reset.h"
#include "exec/address-spaces.h"
#include "chardev/char-fe.h"
#include "ui/console.h"
#include "ui/input.h"
#include "cdj_panelkeys.h"


/*
 * The SPI link to MAIN. MAIN (little-endian) and the GUI (big-endian) run as
 * separate QEMU processes, so the SPI is carried over a chardev socket named
 * "spilink" on both sides:
 *
 *   GUI:   -chardev socket,id=spilink,path=/tmp/cdj-spi.sock
 *   MAIN:  -chardev socket,id=spilink,path=/tmp/cdj-spi.sock,server=on,wait=off
 *
 * Without the chardev the link is absent.
 */
typedef struct {
    CharBackend chr;
    bool present;
    GByteArray *rx;                  /* bytes from the peer, not yet consumed */
    GArray *rx_hash;                 /* one hash per COMPLETE frame in rx */
    GHashTable *rx_count;            /* hash -> how many copies are in rx */
    unsigned dedup_dropped;          /* frames evicted as duplicates */
    unsigned dupcap_dropped;         /* frames collapsed by the dup cap */
    unsigned fresh_dropped;          /* heartbeats superseded by newer ones */
    uint8_t last_hb[4096];           /* newest pure heartbeat seen (FRESH) */
    bool have_hb;
    unsigned fifo_dropped;           /* frames evicted from the front */
    uint64_t rx_in_bytes;            /* bytes the socket has delivered */
    unsigned rx_in_bucket[60];       /* ... per second of virtual time */
    unsigned poll_bucket[60];        /* can_receive calls per second   */
    /* Host time at the first event of each virtual second, to tell a stalled
     * guest from a stalled process. */
    int64_t rx_wall[60];             /* host ms at first arrival that second */
    int64_t poll_wall[60];           /* host ms at first poll that second    */
} SpiLink;

static SpiLink spilink;

/* Host monotonic milliseconds; QEMU_CLOCK_REALTIME does not warp with -icount. */
static int64_t gui_wall_ms(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME) / 1000000;
}

/* Stamp wall[sec] on first use; a large gap between stamps is a stall. */
static void gui_wall_stamp(int64_t *wall, size_t n, int64_t sec)
{
    if (sec >= 0 && sec < (int64_t)n && !wall[sec]) {
        wall[sec] = gui_wall_ms();
    }
}

/* Real time each virtual second consumed; 1000 means it cost a real second. */
static void gui_wall_report(const char *what, const int64_t *wall, size_t n)
{
    char line[60 * 8 + 1];
    unsigned i, p = 0;
    int64_t prev = 0;

    for (i = 0; i < n; i++) {
        int64_t d = 0;

        if (wall[i]) {
            d = prev ? wall[i] - prev : 0;
            prev = wall[i];
        }
        p += snprintf(line + p, sizeof(line) - p, "%" PRId64 " ", d);
    }
    info_report("dmac: link %s real ms per virtual 1 s: %s", what, line);
}

/*
 * SPI is not a queue: whatever the slave has armed when the master clocks a
 * frame is what it gets. MAIN sends far faster than the GUI reads, so only
 * the most recent frames are kept.
 */
#define SPILINK_FRAME       4096
/*
 * Keep window in frames (SPILINK_KEEP_FRAMES, default 8). Large enough for
 * the 9900-byte type-0x39 message that carries the display model.
 */
#define SPILINK_KEEP        (spilink_keep_frames() * SPILINK_FRAME)

static unsigned spilink_keep_frames(void)
{
    const char *e = getenv("SPILINK_KEEP_FRAMES");
    unsigned n = e ? (unsigned)strtoul(e, NULL, 0) : 8;

    return n ? n : 8;
}

/* CDJ_SPILINK_OVERFLOW=1: admit one frame past a full window (default off). */
static bool spilink_overflow_ok(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("CDJ_SPILINK_OVERFLOW");

        on = e && *e != '0';
    }
    return on;
}

/*
 * CDJ_GUI_LINK_IDLE_MS=<ms>: after a receive has starved this long, feed it
 * one all-zero frame (default 0, off).
 *
 * The GUI link task waits for both TX and RX TE with a ~1000-tick timeout and
 * on timeout disarms both channels without re-arming, so one starved receive
 * kills the link for the rest of the boot. Real SPI has no flow control; a
 * starved slave still clocks in idle bits. The zero frame fails the header
 * check at 0x0E5018C0 and the driver carries on.
 */
static int64_t spilink_idle_ms(void)
{
    static int64_t ms = -1;

    if (ms < 0) {
        const char *e = getenv("CDJ_GUI_LINK_IDLE_MS");

        ms = e ? (int64_t)strtoll(e, NULL, 0) : 0;
    }
    return ms;
}

static int spilink_can_receive(void *opaque)
{
    /* Report the room left in the keep window. Back-pressure is safe: MAIN
     * writes non-blocking and holds the rest of a frame. */
    unsigned room = spilink.rx ? spilink.rx->len : 0;

    /* Polls per virtual second, to tell a starved main loop from lost bytes. */
    {
        int64_t sec = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000000;

        if (sec >= 0 && sec < (int64_t)ARRAY_SIZE(spilink.poll_bucket)) {
            spilink.poll_bucket[sec]++;
        }
        gui_wall_stamp(spilink.poll_wall, ARRAY_SIZE(spilink.poll_wall), sec);
    }

    if (room >= SPILINK_KEEP) {
        /* Returning 0 forever can wedge the link: MAIN's DMA then holds a
         * frame it can never send. With CDJ_SPILINK_OVERFLOW the oldest whole
         * frame is dropped instead. */
        return spilink_overflow_ok() ? SPILINK_FRAME : 0;
    }
    return (int)(SPILINK_KEEP - room);
}

/*
 * CDJ_SPILINK_DEDUP=1: when the window overflows, evict the oldest frame that
 * has a twin before evicting a distinct one (default off). About 96% of the
 * link is a two-frame idle heartbeat, while display-model frames are rare and
 * would otherwise be dropped before the GUI reads them.
 */
static bool spilink_dedup_ok(void)
{
    static int on = -1;

    if (on < 0) {
        const char *e = getenv("CDJ_SPILINK_DEDUP");

        on = e && *e != '0';
    }
    return on;
}

static uint32_t spilink_frame_hash(const uint8_t *p)
{
    uint32_t h = 2166136261u;
    unsigned i;

    for (i = 0; i < SPILINK_FRAME; i++) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

/* Multiset of frame hashes in the window, so duplicate lookup is O(1). */
static void spilink_count_add(uint32_t h, int delta)
{
    gpointer key = GUINT_TO_POINTER(h);
    guint n = GPOINTER_TO_UINT(g_hash_table_lookup(spilink.rx_count, key));

    n = (guint)((int)n + delta);
    if (n) {
        g_hash_table_insert(spilink.rx_count, key, GUINT_TO_POINTER(n));
    } else {
        g_hash_table_remove(spilink.rx_count, key);
    }
}

static guint spilink_count_of(uint32_t h)
{
    return GPOINTER_TO_UINT(g_hash_table_lookup(spilink.rx_count,
                                                GUINT_TO_POINTER(h)));
}

/* Keep one hash per complete frame, so eviction never rehashes the window. */
static void spilink_hash_sync(void)
{
    unsigned frames = spilink.rx->len / SPILINK_FRAME;

    while (spilink.rx_hash->len > frames) {
        unsigned last = spilink.rx_hash->len - 1;

        spilink_count_add(g_array_index(spilink.rx_hash, uint32_t, last), -1);
        g_array_remove_index(spilink.rx_hash, last);
    }
    while (spilink.rx_hash->len < frames) {
        uint32_t h = spilink_frame_hash(spilink.rx->data +
                                        spilink.rx_hash->len * SPILINK_FRAME);

        g_array_append_val(spilink.rx_hash, h);
        spilink_count_add(h, 1);
    }
}

static void spilink_drop_frame(unsigned idx)
{
    g_byte_array_remove_range(spilink.rx, idx * SPILINK_FRAME, SPILINK_FRAME);
    if (idx < spilink.rx_hash->len) {
        spilink_count_add(g_array_index(spilink.rx_hash, uint32_t, idx), -1);
        g_array_remove_index(spilink.rx_hash, idx);
    }
}

/* The oldest frame with a twin in the window, or -1; the newest copy stays. */
static int spilink_oldest_duplicate(void)
{
    unsigned i, n = spilink.rx_hash->len;

    for (i = 0; i < n; i++) {
        if (spilink_count_of(g_array_index(spilink.rx_hash, uint32_t, i)) > 1) {
            return (int)i;
        }
    }
    return -1;
}

/*
 * CDJ_SPILINK_DUPCAP=<n>: on arrival, keep at most <n> copies of any
 * byte-identical frame (default 0, off). Unlike DEDUP this bounds how far new
 * content sits from the front of the queue; the GUI reads only ~3 frames a
 * second. Collapsing duplicates can empty the queue, in which case see
 * CDJ_GUI_LINK_IDLE_MS.
 */
static unsigned spilink_dupcap(void)
{
    static int cap = -1;

    if (cap < 0) {
        const char *e = getenv("CDJ_SPILINK_DUPCAP");

        cap = e ? (int)strtol(e, NULL, 0) : 0;
        if (cap < 0) {
            cap = 0;
        }
    }
    return (unsigned)cap;
}

/*
 * CDJ_SPILINK_FRESH=<n>: on arrival, keep only the newest <n> pure heartbeats
 * (default 0, off; 2 suits the two alternating deck buffers). A heartbeat is
 * a batch of one type-3 message and carries the whole deck state, so older
 * ones are worthless and only add latency. Frames carrying anything else are
 * kept. Wire layout: +0x18..+0x1B message count repeated, +0x1D first type.
 */
static unsigned spilink_fresh(void)
{
    static int keep = -1;

    if (keep < 0) {
        const char *e = getenv("CDJ_SPILINK_FRESH");

        keep = e ? MAX((int)strtol(e, NULL, 0), 0) : 0;
    }
    return (unsigned)keep;
}

static bool spilink_is_heartbeat(const uint8_t *f)
{
    return f[0x18] == 1 && f[0x19] == 1 && f[0x1a] == 1 && f[0x1b] == 1
        && f[0x1d] == 3;
}

static void spilink_supersede_heartbeats(unsigned keep)
{
    unsigned frames = spilink.rx->len / SPILINK_FRAME, seen = 0;
    int i;

    spilink_hash_sync();
    for (i = (int)frames - 1; i >= 0; i--) {
        if (!spilink_is_heartbeat(spilink.rx->data + i * SPILINK_FRAME)) {
            continue;
        }
        if (!seen) {
            const uint8_t *f = spilink.rx->data + i * SPILINK_FRAME;

            /* Handed out by the idle feeder when the queue runs dry. */
            memcpy(spilink.last_hb, f, SPILINK_FRAME);
            spilink.have_hb = true;
            /* CDJ_SPILINK_POSLOG=1: log the heartbeat's position word
             * (+0x09C, ms) whenever it changes. */
            if (getenv("CDJ_SPILINK_POSLOG")) {
                static uint32_t last_pos;
                uint32_t pos = ldl_he_p(f + 0x9c);

                if (pos != last_pos) {
                    last_pos = pos;
                    info_report("spilink: pos %08x at %" PRId64 " ms", pos,
                                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000);
                }
            }
        }
        if (++seen > keep) {
            spilink_drop_frame((unsigned)i);
            spilink.fresh_dropped++;
        }
    }
}

/* The oldest frame that has more than <cap> copies in the window, or -1. */
static int spilink_over_cap(unsigned cap)
{
    unsigned i, n = spilink.rx_hash->len;

    for (i = 0; i < n; i++) {
        if (spilink_count_of(g_array_index(spilink.rx_hash, uint32_t, i))
            > cap) {
            return (int)i;
        }
    }
    return -1;
}

/* Drop the hashes of frames the receive DMA consumed. A read that is not
 * whole frames resets the cache. */
static void spilink_consume_hashes(uint32_t n)
{
    unsigned frames = n / SPILINK_FRAME;

    unsigned i;

    if (!spilink.rx_hash) {
        return;
    }
    if (n % SPILINK_FRAME || frames > spilink.rx_hash->len) {
        g_array_set_size(spilink.rx_hash, 0);
        g_hash_table_remove_all(spilink.rx_count);
        return;
    }
    for (i = 0; i < frames; i++) {
        spilink_count_add(g_array_index(spilink.rx_hash, uint32_t, i), -1);
    }
    g_array_remove_range(spilink.rx_hash, 0, frames);
}

/* CDJ_GUI_RXDUMP=<file>: write the bytes the receive DMA hands the guest. */
static void spilink_rxdump(const uint8_t *p, uint32_t n)
{
    static FILE *f;
    static int tried;
    const char *path = getenv("CDJ_GUI_RXDUMP");

    if (!path) {
        return;
    }
    if (!f && !tried) {
        tried = 1;
        f = fopen(path, "wb");
    }
    if (f) {
        fwrite(p, 1, n, f);
        fflush(f);
    }
}

static void spilink_receive(void *opaque, const uint8_t *buf, int size)
{
    /* Bytes delivered per virtual second. */
    {
        int64_t sec = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000000;

        spilink.rx_in_bytes += size;
        if (sec >= 0 && sec < (int64_t)ARRAY_SIZE(spilink.rx_in_bucket)) {
            spilink.rx_in_bucket[sec] += size;
        }
        gui_wall_stamp(spilink.rx_wall, ARRAY_SIZE(spilink.rx_wall), sec);
    }

    /* Log where non-zero content starts in each chunk (chunk-relative, not
     * frame-aligned), when it changes. */
    {
        int i, nz = 0, first = -1;

        for (i = 0; i < size; i++) {
            if (buf[i]) {
                if (first < 0) {
                    first = i;
                }
                nz++;
            }
        }
        if (nz) {
            static int last_first = -2, last_nz;

            if (first != last_first || nz != last_nz) {
                last_first = first;
                last_nz = nz;
                info_report("spilink: RX SOCKET first non-zero at +0x%03x, "
                            "%d of %d bytes non-zero", first, nz, size);
            }
        }
    }

    /* CDJ_SPILINK_SKEW=<bytes>: drop the first <bytes> of the stream once,
     * shifting every later frame boundary (diagnostic). */
    {
        static int skew = -1;
        static int dropped;

        if (skew < 0) {
            const char *e = getenv("CDJ_SPILINK_SKEW");

            skew = e ? (int)strtol(e, NULL, 0) : 0;
            if (skew) {
                info_report("spilink: SKEW armed -- dropping the first %d "
                            "bytes to realign the frame grid", skew);
            }
        }
        if (skew > 0 && dropped < skew) {
            int drop = MIN(skew - dropped, size);

            dropped += drop;
            buf += drop;
            size -= drop;
            if (dropped >= skew) {
                info_report("spilink: SKEW complete -- %d bytes dropped, grid "
                            "shifted", dropped);
            }
            if (size <= 0) {
                return;
            }
        }
    }

    g_byte_array_append(spilink.rx, buf, size);

    if (spilink_fresh()) {
        spilink_supersede_heartbeats(spilink_fresh());
    }

    /* Collapse duplicates first, whether or not the window is full. */
    if (spilink_dupcap()) {
        unsigned cap = spilink_dupcap();
        int over;

        spilink_hash_sync();
        while ((over = spilink_over_cap(cap)) >= 0) {
            spilink_drop_frame((unsigned)over);
            spilink.dupcap_dropped++;
        }
    }

    /* Drop whole frames only, or every later read straddles two frames. */
    while (spilink.rx->len > SPILINK_KEEP) {
        int dup = -1;

        if (spilink_dedup_ok()) {
            spilink_hash_sync();
            dup = spilink_oldest_duplicate();
        }
        if (dup >= 0) {
            spilink_drop_frame((unsigned)dup);
            spilink.dedup_dropped++;
        } else {
            spilink_drop_frame(0);
            spilink.fifo_dropped++;
        }
    }
}

static void spilink_init(void)
{
    Chardev *c = qemu_chr_find("spilink");

    spilink.rx = g_byte_array_new();
    spilink.rx_hash = g_array_new(FALSE, FALSE, sizeof(uint32_t));
    spilink.rx_count = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (!c) {
        return;
    }
    qemu_chr_fe_init(&spilink.chr, c, &error_abort);
    qemu_chr_fe_set_handlers(&spilink.chr, spilink_can_receive,
                             spilink_receive, NULL, NULL, NULL, NULL, true);
    spilink.present = true;
    info_report("sh7269gui: SPI link to MAIN attached on chardev 'spilink'");
}


/*
 * Interrupt queue. SH-2A delivery carries one vector in env.sh2a_irq_vector
 * and there is no INTC model to arbitrate, so sources queue here and a pump
 * presents them one at a time.
 */
#define SH2A_IRQ_QUEUE_LEN  32

typedef struct {
    uint16_t vec;
    uint8_t level;
    bool used;
} Sh2aIrq;

static struct {
    Sh2aIrq q[SH2A_IRQ_QUEUE_LEN];
    unsigned head, tail;
    QEMUTimer *pump;
    SuperHCPU *cpu;
    unsigned dropped;
    uint16_t cur_vec;        /* vector last handed to the CPU */
    uint8_t cur_level;
    bool cur_valid;          /* ... and not yet taken or cancelled */
    unsigned stolen;         /* foreign acknowledgements refused */
    unsigned presented;
    int64_t present_ns;
    int64_t warn_ns;
    unsigned warned_stuck;
} sh2a_irq;

/*
 * Acknowledge one source. A peripheral clearing its request must only clear
 * CPU_INTERRUPT_HARD if the vector on the pin is its own; otherwise it would
 * destroy another source's pending interrupt (e.g. a DMAC DEI presented while
 * the CMI0 handler runs).
 */
static void sh2a_irq_pump(void *opaque);

/* CDJ_GUI_IRQ_FIX=0: unconditional ack, FIFO order, no keep-alive. */
static bool sh2a_fix(void)
{
    static int fix = -1;

    if (fix < 0) {
        const char *e = getenv("CDJ_GUI_IRQ_FIX");

        fix = !(e && !strcmp(e, "0"));
    }
    return fix;
}

static void sh2a_ack(SuperHCPU *cpu, uint16_t vec)
{
    CPUState *cs = CPU(cpu);

    if (!sh2a_fix()) {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
        return;
    }

    if (!(cs->interrupt_request & CPU_INTERRUPT_HARD)) {
        return;                 /* already taken -- nothing to withdraw */
    }
    if (sh2a_irq.cur_valid && sh2a_irq.cur_vec != vec) {
        /* Another source's request is standing. */
        if (sh2a_irq.stolen++ < 8) {
            info_report("sh2a: refused ack of vector %u by %u",
                        sh2a_irq.cur_vec, vec);
        }
        return;
    }
    sh2a_irq.cur_valid = false;
    cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    sh2a_irq_pump(NULL);        /* let anything queued behind it through */
}

static void sh2a_irq_pump(void *opaque)
{
    CPUState *cs;

    if (!sh2a_irq.cpu) {
        return;
    }
    cs = CPU(sh2a_irq.cpu);

    if (!(cs->interrupt_request & CPU_INTERRUPT_HARD)) {
        sh2a_irq.cur_valid = false;         /* the CPU took the last one */
    }

    /*
     * Present the highest-level pending request, not the oldest. SH-2A only
     * accepts a request above SR.IMASK; strict FIFO let a masked DEI (level 3)
     * block every CMI0 tick (level 8) queued behind it. A higher-level raise
     * preempts a standing request, which goes back in the queue.
     */
    {
        unsigned i, best = SH2A_IRQ_QUEUE_LEN;
        int best_level = -1;
        bool prio = sh2a_fix();

        for (i = sh2a_irq.head; i != sh2a_irq.tail;
             i = (i + 1) % SH2A_IRQ_QUEUE_LEN) {
            if (!sh2a_irq.q[i].used) {
                continue;
            }
            if (!prio) {
                best = i;
                break;                          /* strict FIFO */
            }
            if ((int)sh2a_irq.q[i].level > best_level) {
                best_level = sh2a_irq.q[i].level;
                best = i;
            }
        }

        if (best != SH2A_IRQ_QUEUE_LEN &&
            (cs->interrupt_request & CPU_INTERRUPT_HARD) && prio &&
            sh2a_irq.cur_valid &&
            sh2a_irq.q[best].level > sh2a_irq.cur_level) {
            /* Preempt: the standing request goes back into the queue. */
            unsigned n = (sh2a_irq.tail + 1) % SH2A_IRQ_QUEUE_LEN;

            if (n != sh2a_irq.head) {
                sh2a_irq.q[sh2a_irq.tail] = (Sh2aIrq){
                    .vec = sh2a_irq.cur_vec,
                    .level = sh2a_irq.cur_level,
                    .used = true };
                sh2a_irq.tail = n;
                sh2a_irq.cur_valid = false;
                cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
            }
        }

        if (best != SH2A_IRQ_QUEUE_LEN &&
            !(cs->interrupt_request & CPU_INTERRUPT_HARD)) {
            Sh2aIrq *e = &sh2a_irq.q[best];

            e->used = false;
            while (sh2a_irq.head != sh2a_irq.tail &&
                   !sh2a_irq.q[sh2a_irq.head].used) {
                sh2a_irq.head = (sh2a_irq.head + 1) % SH2A_IRQ_QUEUE_LEN;
            }
            sh2a_irq.cpu->env.sh2a_irq_vector = e->vec;
            sh2a_irq.cpu->env.sh2a_irq_level = e->level;
            sh2a_irq.cur_vec = e->vec;
            sh2a_irq.cur_level = e->level;
            sh2a_irq.cur_valid = true;
            sh2a_irq.presented++;
            sh2a_irq.present_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            cpu_interrupt(cs, CPU_INTERRUPT_HARD);
            /* Kick the vCPU too: a vCPU spinning in a self-chained TB does
             * not notice a new request. CDJ_GUI_IRQ_KICK=0 disables. */
            {
                const char *k = getenv("CDJ_GUI_IRQ_KICK");

                if (!(k && !strcmp(k, "0"))) {
                    qemu_cpu_kick(cs);
                }
            }
            if (getenv("CDJ_GUI_IRQ_DEBUG")) {
                info_report("sh2a: present vector %u level %u at %" PRId64 " ms",
                            e->vec, e->level, sh2a_irq.present_ns / 1000000);
            }
        }
    }

    if (sh2a_irq.cur_valid &&
        (cs->interrupt_request & CPU_INTERRUPT_HARD)) {
        /* The presented vector is still standing. */
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        /* Keep kicking until it is taken; a single kick can be lost. */
        {
            const char *k = getenv("CDJ_GUI_IRQ_KICK");

            if (!(k && !strcmp(k, "0"))) {
                qemu_cpu_kick(cs);
            }
        }

        /* Warn at most once a second, six times in all. */
        if (now - sh2a_irq.present_ns > 1000000000LL &&
            now - sh2a_irq.warn_ns > 1000000000LL &&
            sh2a_irq.warned_stuck < 6) {
            /* SR shows why: BL set, level <= IMASK, or never asked. */
            CPUSH4State *env = &sh2a_irq.cpu->env;

            sh2a_irq.warned_stuck++;
            sh2a_irq.warn_ns = now;
            warn_report("sh2a: vector %u level %u presented at %" PRId64
                        " ms still not taken at %" PRId64 " ms"
                        " -- sr=0x%08x imask=%u bl=%u sleep=%u halted=%u"
                        " pc=0x%08x ireq=0x%x exit=%d",
                        sh2a_irq.cur_vec, sh2a_irq.cur_level,
                        sh2a_irq.present_ns / 1000000, now / 1000000,
                        env->sr, (env->sr >> 4) & 0xf,
                        (env->sr >> SR_BL) & 1, env->in_sleep,
                        CPU(sh2a_irq.cpu)->halted,
                        env->pc,
                        qatomic_read(&CPU(sh2a_irq.cpu)->interrupt_request),
                        qatomic_read(&CPU(sh2a_irq.cpu)->exit_request));
        }
    }

    /*
     * Keep pumping while anything is queued or a presented vector is not yet
     * taken. 10 us: at 100 us the queue overflowed.
     */
    if (sh2a_irq.pump &&
        (sh2a_irq.head != sh2a_irq.tail ||
         (sh2a_fix() && sh2a_irq.cur_valid &&
          (cs->interrupt_request & CPU_INTERRUPT_HARD)))) {
        timer_mod(sh2a_irq.pump,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000);
    }
}

static void sh2a_raise(SuperHCPU *cpu, uint16_t vec, uint8_t level)
{
    unsigned next;

    unsigned i;

    sh2a_irq.cpu = cpu;

    /* Coalesce: hardware has one pending bit per source, not a FIFO. */
    for (i = sh2a_irq.head; i != sh2a_irq.tail;
         i = (i + 1) % SH2A_IRQ_QUEUE_LEN) {
        if (sh2a_irq.q[i].used && sh2a_irq.q[i].vec == vec) {
            return;
        }
    }

    next = (sh2a_irq.tail + 1) % SH2A_IRQ_QUEUE_LEN;
    if (next == sh2a_irq.head) {
        /* Full. */
        if (sh2a_irq.dropped++ < 8) {
            warn_report("sh2a: interrupt queue full, dropping vector %u", vec);
        }
        return;
    }
    sh2a_irq.q[sh2a_irq.tail] = (Sh2aIrq){ .vec = vec, .level = level,
                                           .used = true };
    sh2a_irq.tail = next;

    if (!sh2a_irq.pump) {
        sh2a_irq.pump = timer_new_ns(QEMU_CLOCK_VIRTUAL, sh2a_irq_pump, NULL);
    }
    sh2a_irq_pump(NULL);
}


/*
 * SH7269 compare-match timer, channel 0 only; the firmware's RTOS tick.
 * Channel 1 is never used and is left as plain storage.
 *
 *   +0x00 CMSTR    bit0 = start channel 0
 *   +0x02 CMCSR_0  bit6 CMIE, bit7 CMF, bits1:0 CKS (pclk/8,32,128,512)
 *   +0x04 CMCNT_0  up-counter
 *   +0x06 CMCOR_0  compare value
 *
 * A periodic QEMU timer fires at the CMCOR interval; CMCNT is synthesised on
 * read from the time remaining.
 */
#define SH7269_CMT_BASE     0xFFFEC000
#define SH7269_CMT_PCLK     33333333    /* peripheral clock, best estimate */
#define SH7269_CMI0_VECTOR  188         /* CMI0, hardware manual table 7.4 */

typedef struct {
    MemoryRegion iomem;
    QEMUTimer *timer;
    SuperHCPU *cpu;
    uint16_t cmstr, cmcsr, cmcnt, cmcor;
    uint64_t period_ns;
    uint64_t next_ns;
} Sh7269Cmt;

static const int cmt_div[4] = { 8, 32, 128, 512 };

static void cmt_rearm(Sh7269Cmt *s)
{
    uint64_t ticks;

    if (!(s->cmstr & 1) || s->cmcor == 0) {
        timer_del(s->timer);
        s->period_ns = 0;
        return;
    }

    ticks = (uint64_t)s->cmcor + 1;
    s->period_ns = ticks * cmt_div[s->cmcsr & 3] * 1000000000ULL / SH7269_CMT_PCLK;
    /*
     * CDJ_GUI_CMT_SPEED=<percent> (default 100): run the RTOS tick faster than
     * the hardware. The firmware uses a 1 ms tick (CMCOR 0x1045) and redraws
     * every ~30 ticks. This speeds up all of the board's timing. The period is
     * floored at 50 us.
     */
    {
        const char *sp = getenv("CDJ_GUI_CMT_SPEED");
        unsigned pct = sp ? (unsigned)strtoul(sp, NULL, 0) : 100;

        if (pct && pct != 100) {
            s->period_ns = s->period_ns * 100 / pct;
            if (s->period_ns < 50000) {
                s->period_ns = 50000;
            }
        }
    }
    if (s->period_ns == 0) {
        s->period_ns = 1000;
    }
    s->next_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->period_ns;
    if (getenv("CDJ_GUI_CMT_DEBUG")) {
        info_report("cmt: rearm cmcor=0x%04x cks=%d div=%d period=%llu ns",
                    s->cmcor, s->cmcsr & 3, cmt_div[s->cmcsr & 3],
                    (unsigned long long)s->period_ns);
    }
    timer_mod(s->timer, s->next_ns);
}

static void cmt_fire(void *opaque)
{
    Sh7269Cmt *s = opaque;

    s->cmcsr |= 0x80;                    /* CMF */

    if (getenv("CDJ_GUI_CMT_DEBUG")) {
        static int scanned;

        if (!scanned) {
            uint32_t vbr = s->cpu->env.vbr;
            int first = -1, last = -1, n = 0;
            unsigned v;

            scanned = 1;
            for (v = 64; v < 256; v++) {
                uint8_t b[4];
                uint32_t slot;

                cpu_physical_memory_read(vbr + v * 4, b, 4);
                slot = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
                       ((uint32_t)b[2] << 8) | b[3];
                if ((slot >= 0x0E500000 && slot < 0x0E600000) ||
                    (slot >= 0xFFF80000 && slot < 0xFFFC0000)) {
                    if (first < 0) {
                        first = v;
                    }
                    last = v;
                    n++;
                }
            }
            info_report("cmt: at first tick, vbr=0x%08x, %d of 192 peripheral "
                        "vectors hold code (first=%d last=%d)",
                        vbr, n, first, last);
        }
    }

    if (s->cmcsr & 0x40) {               /* CMIE */
        /* CMI0 is vector 188; the firmware sets IPR12 = 0x8000 (level 8).
         * Vector 175 is VFIELD (VDC4), not the timer. */
        sh2a_raise(s->cpu, SH7269_CMI0_VECTOR, 8);
    }

    cmt_rearm(s);
}

static uint64_t cmt_read(void *opaque, hwaddr off, unsigned size)
{
    Sh7269Cmt *s = opaque;

    switch (off) {
    case 0x00: return s->cmstr;
    case 0x02: return s->cmcsr;
    case 0x04:
        if (s->period_ns) {
            int64_t left = s->next_ns - qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

            if (left < 0) {
                left = 0;
            }
            return s->cmcor - (uint16_t)((uint64_t)left * s->cmcor / s->period_ns);
        }
        return s->cmcnt;
    case 0x06: return s->cmcor;
    default:   return 0;
    }
}

static void cmt_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Sh7269Cmt *s = opaque;

    if (getenv("CDJ_GUI_CMT_DEBUG")) {
        static const char *rn[8] = { "CMSTR", "?", "CMCSR", "?",
                                     "CMCNT", "?", "CMCOR", "?" };
        info_report("cmt: write %-5s (+0x%02x) = 0x%04x size %u",
                    off < 8 ? rn[off] : "?", (unsigned)off,
                    (unsigned)val, size);
    }

    switch (off) {
    case 0x00:
        s->cmstr = val;
        cmt_rearm(s);
        break;
    case 0x02:
        /* CMF is write-0-to-clear; the handler's ack drops the request. */
        if (!(val & 0x80)) {
            s->cmcsr &= ~0x80;
            sh2a_ack(s->cpu, SH7269_CMI0_VECTOR);
        }
        s->cmcsr = (s->cmcsr & 0x80) | (val & 0x7f);
        cmt_rearm(s);
        break;
    case 0x04: s->cmcnt = val; break;
    case 0x06: s->cmcor = val; cmt_rearm(s); break;
    default:   break;
    }
}

static const MemoryRegionOps cmt_ops = {
    .read = cmt_read,
    .write = cmt_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/*
 * SH7269 Port F. The firmware read-modify-writes PFDR0's low byte
 * (0xFFFE38B7) about 382 times a second. Control and direction registers are
 * storage, data registers read back, and the pin registers read the data
 * masked by the direction. Nothing drives the inputs, so they read 0; reads
 * and writes are counted.
 *
 *   0xFFFE38A4 PFCR5  PFCR4  PFCR3  PFCR2  PFCR1  PFCR0   pin function
 *   0xFFFE38B0 PFIOR1 PFIOR0                               direction, 1 = out
 *   0xFFFE38B4 PFDR1  PFDR0                                data
 *   0xFFFE38B8 PFPR1  PFPR0                                pin state (read-only)
 */
#define SH7269_PORTF_BASE   0xFFFE38A4
#define SH7269_PORTF_SIZE   0x18
#define PORTF_SLOTS         (SH7269_PORTF_SIZE / 2)
#define PORTF_PFIOR1        6
#define PORTF_PFDR1         8
#define PORTF_PFPR1         10

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint16_t reg[PORTF_SLOTS];
    unsigned wr[PORTF_SLOTS];
    unsigned rd[PORTF_SLOTS];
    unsigned toggle[2][16];            /* PFDR1/PFDR0 edges per bit */
    uint32_t wpc[PORTF_SLOTS][2];      /* the first two writers of each */
    Notifier exit;
} Sh7269PortF;

static Sh7269PortF *portf;

static const char *portf_name(unsigned slot)
{
    static const char *n[PORTF_SLOTS] = {
        "PFCR5", "PFCR4", "PFCR3", "PFCR2", "PFCR1", "PFCR0",
        "PFIOR1", "PFIOR0", "PFDR1", "PFDR0", "PFPR1", "PFPR0",
    };
    return slot < PORTF_SLOTS ? n[slot] : "?";
}

static uint16_t portf_slot_read(Sh7269PortF *s, unsigned slot)
{
    s->rd[slot]++;
    if (slot >= PORTF_PFPR1) {
        /* An output pin reads its data bit; an undriven input reads 0. */
        unsigned i = slot - PORTF_PFPR1;

        return s->reg[PORTF_PFDR1 + i] & s->reg[PORTF_PFIOR1 + i];
    }
    return s->reg[slot];
}

static void portf_slot_write(Sh7269PortF *s, unsigned slot, uint16_t val)
{
    uint32_t pc = s->cpu ? (uint32_t)s->cpu->env.pc : 0;
    unsigned k;

    if (slot >= PORTF_PFPR1) {
        return;                         /* PFPRn is read-only */
    }
    if (slot == PORTF_PFDR1 || slot == PORTF_PFDR1 + 1) {
        uint16_t changed = s->reg[slot] ^ val;

        for (k = 0; k < 16; k++) {
            if (changed & (1u << k)) {
                s->toggle[slot - PORTF_PFDR1][k]++;
            }
        }
    }
    for (k = 0; k < ARRAY_SIZE(s->wpc[slot]); k++) {
        if (s->wpc[slot][k] == pc) {
            break;
        }
        if (!s->wpc[slot][k]) {
            s->wpc[slot][k] = pc;
            break;
        }
    }
    s->wr[slot]++;
    s->reg[slot] = val;
}

static uint64_t portf_read(void *opaque, hwaddr off, unsigned size)
{
    Sh7269PortF *s = opaque;
    unsigned slot = (unsigned)off / 2;

    if (slot >= PORTF_SLOTS) {
        return 0;
    }
    switch (size) {
    case 1:
        /* big-endian: the even offset is the high byte of the register */
        return (portf_slot_read(s, slot) >> ((off & 1) ? 0 : 8)) & 0xFF;
    case 4:
        if (slot + 1 < PORTF_SLOTS) {
            return ((uint32_t)portf_slot_read(s, slot) << 16) |
                   portf_slot_read(s, slot + 1);
        }
        return (uint32_t)portf_slot_read(s, slot) << 16;
    default:
        return portf_slot_read(s, slot);
    }
}

static void portf_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Sh7269PortF *s = opaque;
    unsigned slot = (unsigned)off / 2;

    if (slot >= PORTF_SLOTS) {
        return;
    }
    switch (size) {
    case 1: {
        uint16_t cur = s->reg[slot];
        uint16_t v = (off & 1)
            ? (uint16_t)((cur & 0xFF00) | (val & 0xFF))
            : (uint16_t)((cur & 0x00FF) | ((val & 0xFF) << 8));

        portf_slot_write(s, slot, v);
        break;
    }
    case 4:
        portf_slot_write(s, slot, (uint16_t)(val >> 16));
        if (slot + 1 < PORTF_SLOTS) {
            portf_slot_write(s, slot + 1, (uint16_t)val);
        }
        break;
    default:
        portf_slot_write(s, slot, (uint16_t)val);
        break;
    }
}

static const MemoryRegionOps portf_ops = {
    .read = portf_read,
    .write = portf_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void portf_dump(Notifier *n, void *unused)
{
    Sh7269PortF *s = container_of(n, Sh7269PortF, exit);
    unsigned slot, k;

    for (slot = 0; slot < PORTF_SLOTS; slot++) {
        if (!s->rd[slot] && !s->wr[slot]) {
            continue;
        }
        info_report("portf: %-6s = 0x%04x  reads=%u writes=%u  writers 0x%08x"
                    " 0x%08x", portf_name(slot), s->reg[slot], s->rd[slot],
                    s->wr[slot], s->wpc[slot][0], s->wpc[slot][1]);
    }
    for (slot = 0; slot < 2; slot++) {
        char line[16 * 12 + 1];
        int p = 0;

        for (k = 0; k < 16; k++) {
            if (s->toggle[slot][k]) {
                p += snprintf(line + p, sizeof(line) - p, "bit%u:%u ", k,
                              s->toggle[slot][k]);
            }
        }
        if (p) {
            info_report("portf: %s edges  %s",
                        portf_name(PORTF_PFDR1 + slot), line);
        }
    }
}

static void portf_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Sh7269PortF *s;

    if (getenv("CDJ_GUI_PORTF_OFF")) {
        return;                         /* leave it unimplemented */
    }
    s = g_new0(Sh7269PortF, 1);
    s->cpu = cpu;
    portf = s;
    memory_region_init_io(&s->iomem, NULL, &portf_ops, s, "sh7269.portf",
                          SH7269_PORTF_SIZE);
    /* priority 1: the sh7269.cpg unimplemented region covers this range */
    memory_region_add_subregion_overlap(sysmem, SH7269_PORTF_BASE,
                                        &s->iomem, 1);
    s->exit.notify = portf_dump;
    qemu_add_exit_notifier(&s->exit);
}

static void cmt_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Sh7269Cmt *s;

    /* CDJ_GUI_CMT_OFF=1 leaves the timer unimplemented (no RTOS tick). */
    if (getenv("CDJ_GUI_CMT_OFF")) {
        create_unimplemented_device("sh7269.cmt", SH7269_CMT_BASE, 0x1000);
        return;
    }

    s = g_new0(Sh7269Cmt, 1);

    s->cpu = cpu;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cmt_fire, s);
    memory_region_init_io(&s->iomem, NULL, &cmt_ops, s, "sh7269.cmt", 0x10);
    memory_region_add_subregion(sysmem, SH7269_CMT_BASE, &s->iomem);
}


/* LCD frame memory; the DMAC marks a new frame when a transfer lands here. */
#define SH7269_LCD_BASE     0x2C000000
#define SH7269_LCD_SIZE     (4 * MiB)

static void lcd_mark_dirty(void);


/*
 * DMAC at 0xFFFE1000. The link to MAIN is two channels on RSPI channel 1:
 *
 *   ch4:  SAR=0x3c27c000  DAR=0xE800E804  TCR=0x400  CHCR=0x30001810
 *   ch5:  SAR=0xE800E804  DAR=0x3c27b000  TCR=0x400  CHCR=0x30004810
 *
 * 0xE800E804 is SPDR; ch4 transmits and ch5 receives, 1024 longwords (4 KB)
 * each way. DMARS2 selects RSPI1 (0x55 transmit, 0x56 receive).
 *
 * Register map (manual 11.3): channel n at +n*0x10 as SAR/DAR/TCR/CHCR, reload
 * set at +0x100+n*0x10, DMAOR at +0x200, DMARS0-7 at +0x300+n*4.
 * CHCR: DE(0) enable, TE(1) transfer end, IE(2) interrupt on TE, TS(4:3) unit
 * size, SM(13:12)/DM(15:14) address mode (0 fixed, 1 increment).
 *
 * The request handshake is not modelled: an enabled channel runs to
 * completion at once, except a link receive, which waits for data.
 */
#define SH7269_DMAC_BASE    0xFFFE1000
#define SH7269_DMAC_SIZE    0x400
#define SH7269_DMAC_CHANS   16
/* DEI0..DEI15 are vectors 108..168 in steps of 4 (manual table 7.4). */
#define SH7269_DEI_VECTOR(ch)  (108 + (ch) * 4)

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint32_t sar[SH7269_DMAC_CHANS];
    uint32_t dar[SH7269_DMAC_CHANS];
    uint32_t tcr[SH7269_DMAC_CHANS];
    uint32_t chcr[SH7269_DMAC_CHANS];
    uint32_t rsar[SH7269_DMAC_CHANS];
    uint32_t rdar[SH7269_DMAC_CHANS];
    uint32_t rtcr[SH7269_DMAC_CHANS];
    uint16_t dmaor;
    uint16_t dmars[8];
    bool pending[SH7269_DMAC_CHANS];   /* waiting on the peer for RX data */
    QEMUTimer *retry;
    /* Link receive counters. */
    unsigned rx_arm;                   /* receives programmed */
    unsigned rx_done;                  /* receives that completed */
    unsigned rx_defer;                 /* receives short of data */
    unsigned rx_idle;                  /* idle frames fed to a starved receive */
    uint64_t rx_bytes;                 /* bytes drained from the link */
    unsigned rx_bucket[60];            /* completions per 1 s */
    unsigned blit_count;               /* frame-sized mem-to-mem transfers */
    uint64_t blit_bytes;
    unsigned blit_bucket[60];          /* ... per 1 s: the draw rate itself */
    uint32_t blit_pc[8];               /* which guest code asks for them */
    unsigned blit_pc_n[8];
    uint32_t blit_pr[8];               /* ...and who called it */
    unsigned blit_pr_n[8];
    uint32_t blit_up[8][8];            /* ...and the frames above that */
    int64_t blit_last_ns;              /* for the gap histogram */
    unsigned blit_gap[64];             /* gaps between blits, 1 ms bins */
    unsigned blit_up_n[8];
    int64_t rx_last_arm_ms;
    int64_t rx_last_done_ms;
    /* Guest PCs that arm the link receive, with counts and last time. */
    uint32_t rx_pc[8];
    unsigned rx_pc_n[8];
    int64_t rx_pc_last[8];
    /* Last short read of the link receive; defer_partial counts short reads
     * with a non-empty queue. */
    uint32_t defer_have, defer_need;
    uint32_t defer_partial, defer_maxhave;
    int64_t defer_ms;
    int64_t defer_start_ms;  /* stamped ONCE, when a deferral begins */
    uint32_t arm_chcr[SH7269_DMAC_CHANS];  /* CHCR as it was at arm time */
    int64_t stall_ms;   /* last 'retry does nothing' report */
    unsigned tx_run;    /* link transmit transfers executed      */
    int64_t tx_last_ms;
    Notifier exit;
} Sh7269Dmac;

static const unsigned dmac_unit[4] = { 1, 2, 4, 16 };

/* RSPI SPDR (0xE800E800 + 0x04): a channel fixed on it is the link to MAIN. */
#define SH7269_RSPI_SPDR    0xE800E804

static void dmac_run(Sh7269Dmac *s, unsigned ch);

/* Re-attempt receive channels that were short of data when they were enabled. */
static void dmac_retry(void *opaque)
{
    Sh7269Dmac *s = opaque;
    unsigned ch;
    bool still = false;

    for (ch = 0; ch < SH7269_DMAC_CHANS; ch++) {
        if (s->pending[ch]) {
            uint32_t before = s->rx_defer;

            dmac_run(s, ch);
            /* Retry made no progress: report the channel once a second. */
            if (s->pending[ch] && s->rx_defer == before) {
                int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

                if (now - s->stall_ms * 1000000LL > 1000000000LL) {
                    s->stall_ms = now / 1000000;
                    info_report("dmac: ch%u retry does nothing -- chcr=0x%08x"
                                " sar=0x%08x dar=0x%08x tcr=%u queue=%u",
                                ch, s->chcr[ch], s->sar[ch], s->dar[ch],
                                s->tcr[ch], spilink.rx ? spilink.rx->len : 0);
                }
            }
        }
        still |= s->pending[ch];
    }
    if (still) {
        timer_mod(s->retry, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
}

static void dmac_run(Sh7269Dmac *s, unsigned ch)
{
    uint32_t chcr = s->chcr[ch];
    unsigned unit = dmac_unit[(chcr >> 3) & 3];
    unsigned sm = (chcr >> 12) & 3;
    unsigned dm = (chcr >> 14) & 3;
    uint32_t sar = s->sar[ch], dar = s->dar[ch];
    uint32_t count = s->tcr[ch];
    CPUState *cs = CPU(s->cpu);
    uint8_t buf[16];
    uint32_t moved = 0;

    /*
     * Manual figure 11.2: a transfer starts only with TE (and NMIF/AE) clear
     * and DE and DME set. The firmware rewrites CHCR with TE still set.
     *
     * A deferred receive was already accepted, so it may finish even if the
     * guest has since cleared DE, and it keeps the IE it was armed with (the
     * guest clears IE in its teardown). Otherwise the completion handler at
     * 0x0E500934 never runs and the link stops. CDJ_GUI_DMA_FINISH=0 restores
     * the strict gate.
     */
    {
        const char *fin = getenv("CDJ_GUI_DMA_FINISH");

        if (s->pending[ch] && !(fin && !strcmp(fin, "0"))
            && (s->dmaor & 1) && !(chcr & 2)) {
            chcr |= 1u;                 /* treat the in-flight transfer as armed */
            chcr |= (s->arm_chcr[ch] & 4u);
        }
    }

    if (!(chcr & 1) || (chcr & 2) || !(s->dmaor & 1)) {
        /* A channel that cannot run is no longer in flight. */
        s->pending[ch] = false;
        return;
    }

    /* Transmit half of the link: send the payload down the chardev. Without a
     * link it falls through to a plain copy. */
    if (spilink.present && dar == SH7269_RSPI_SPDR && dm == 0) {
        g_autofree uint8_t *out = g_malloc(count * unit);

        /* The receive re-arm (0x0E50094C) runs in the ch4 TE handler at
         * 0x0E500934, so transmit completions matter too. */
        s->tx_run++;
        s->tx_last_ms = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;

        while (count--) {
            address_space_read(cs->as, sar, MEMTXATTRS_UNSPECIFIED,
                               out + moved * unit, unit);
            if (sm == 1) {
                sar += unit;
            } else if (sm == 2) {
                sar -= unit;
            }
            moved++;
        }
        /* qemu_chr_fe_write_all can block the vCPU thread; log slow calls. */
        {
            int64_t t0 = g_get_monotonic_time();
            int64_t took;

            qemu_chr_fe_write_all(&spilink.chr, out, moved * unit);
            took = g_get_monotonic_time() - t0;
            if (took > 1000) {
                info_report("dmac: link TX write blocked %" PRId64 " us at %"
                            PRId64 " ms", took,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000);
            }
        }
        goto done;
    }

    /*
     * Receive half of the link. The bytes only exist once MAIN has sent them,
     * so if the queue is short, leave the channel pending and retry.
     */
    if (spilink.present && sar == SH7269_RSPI_SPDR && sm == 0) {
        uint32_t need = count * unit;

        if (!s->pending[ch]) {
            int64_t ams = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
            uint32_t pc = s->cpu ? (uint32_t)s->cpu->env.pc : 0;
            unsigned k;

            s->rx_arm++;
            s->rx_last_arm_ms = ams;
            for (k = 0; k < ARRAY_SIZE(s->rx_pc); k++) {
                if (s->rx_pc_n[k] && s->rx_pc[k] != pc) {
                    continue;
                }
                s->rx_pc[k] = pc;
                s->rx_pc_n[k]++;
                s->rx_pc_last[k] = ams;
                break;
            }
        }
        if (spilink.rx->len < need) {
            if (getenv("CDJ_GUI_DMA_DEBUG") && !s->pending[ch]) {
                info_report("dmac: ch%u rx deferred, have %u of %u bytes",
                            ch, spilink.rx->len, need);
            }
            s->rx_defer++;
            s->defer_have = spilink.rx->len;
            s->defer_need = need;
            s->defer_ms = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
            if (spilink.rx->len) {
                s->defer_partial++;
                if (spilink.rx->len > s->defer_maxhave) {
                    s->defer_maxhave = spilink.rx->len;
                }
            }
            if (!s->pending[ch]) {
                s->defer_start_ms = s->defer_ms;
                s->arm_chcr[ch] = chcr;   /* remember IE/DE as armed */
            }
            s->pending[ch] = true;
            /*
             * After CDJ_GUI_LINK_IDLE_MS of starvation, feed one frame. Only
             * into an empty queue: padding a partial frame would misalign the
             * 4096-byte grid. One frame per threshold.
             */
            if (spilink_idle_ms() && !spilink.rx->len &&
                s->defer_ms - s->defer_start_ms >= spilink_idle_ms()) {
                guint at = spilink.rx->len;

                g_byte_array_set_size(spilink.rx, at + need);
                /* Prefer the newest real heartbeat, a valid deck state, over
                 * a zero frame the guest would reject. */
                if (spilink.have_hb && need == SPILINK_FRAME) {
                    memcpy(spilink.rx->data + at, spilink.last_hb, need);
                } else {
                    memset(spilink.rx->data + at, 0, need);
                }
                spilink_hash_sync();
                s->rx_idle++;
                s->defer_start_ms = s->defer_ms;
            }
            timer_mod(s->retry,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
            return;
        }

        /* Log deferrals that took over 100 ms to satisfy. */
        if (s->pending[ch] && s->defer_start_ms) {
            int64_t nowms = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;

            if (nowms - s->defer_start_ms > 100) {
                info_report("dmac: ch%u deferred rx satisfied after %" PRId64
                            " ms (deferred at %" PRId64 " ms, queue now %u,"
                            " needed %u)", ch, nowms - s->defer_start_ms,
                            s->defer_start_ms,
                            spilink.rx->len, need);
            }
        }
        /* Log where non-zero content starts at the queue front, when it
         * changes, with the destination address. */
        {
            uint32_t i, nz = 0, first = 0xFFFFFFFF;

            for (i = 0; i < need && i < spilink.rx->len; i++) {
                if (spilink.rx->data[i]) {
                    if (first == 0xFFFFFFFF) {
                        first = i;
                    }
                    nz++;
                }
            }
            if (nz) {
                static uint32_t last_first = 0xFFFFFFFE, last_nz;

                if (first != last_first || nz != last_nz) {
                    last_first = first;
                    last_nz = nz;
                    info_report("dmac: ch%u QUEUE FRONT first non-zero at "
                                "+0x%03x, %u of %u bytes non-zero, dar=0x%08x",
                                ch, first, nz, need, dar);
                }
            }
        }
        while (count--) {
            address_space_write(cs->as, dar, MEMTXATTRS_UNSPECIFIED,
                                spilink.rx->data + moved * unit, unit);
            if (dm == 1) {
                dar += unit;
            } else if (dm == 2) {
                dar -= unit;
            }
            moved++;
        }
        spilink_rxdump(spilink.rx->data, need);
        g_byte_array_remove_range(spilink.rx, 0, need);
        spilink_consume_hashes(need);
        /* Room has appeared: once can_receive has returned 0 the chardev
         * stops reading until accept_input is called. */
        qemu_chr_fe_accept_input(&spilink.chr);
        s->pending[ch] = false;
        {
            int64_t ms = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
            unsigned b = (unsigned)(ms / 1000);

            s->rx_done++;
            s->rx_bytes += need;
            s->rx_last_done_ms = ms;
            if (b < ARRAY_SIZE(s->rx_bucket)) {
                s->rx_bucket[b]++;
            }
        }
        goto done;
    }

    /*
     * Memory-to-memory with incrementing destination: copy in 64 KB chunks
     * (a fixed source is a fill, read once). Per-unit address_space calls were
     * the hottest path. Fixed or decrementing destinations are peripheral
     * registers and keep the per-unit loop below.
     */
    if (count && dm == 1 && (sm == 1 || sm == 0)) {
        uint64_t total = (uint64_t)count * unit;
        uint32_t chunk_max = 64 * KiB;
        uint8_t *big = g_malloc(MIN(total, chunk_max));

        /* Frame blit census: transfers of at least a quarter of an 800x480x2
         * frame, per second, with gaps and the calling code. */
        if (total >= 800 * 480 * 2 / 4) {
            int64_t b = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000000;
            uint32_t pc = s->cpu ? (uint32_t)s->cpu->env.pc : 0;
            unsigned j;

            s->blit_count++;
            s->blit_bytes += total;
            /* Gap between blits, 1 ms bins; bin 0 is within one picture. */
            {
                int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

                if (s->blit_last_ns) {
                    uint64_t ms = (uint64_t)(now - s->blit_last_ns) / 1000000;

                    if (ms >= ARRAY_SIZE(s->blit_gap)) {
                        ms = ARRAY_SIZE(s->blit_gap) - 1;
                    }
                    s->blit_gap[ms]++;
                }
                s->blit_last_ns = now;
            }
            if (b >= 0 && b < (int64_t)ARRAY_SIZE(s->blit_bucket)) {
                s->blit_bucket[b]++;
            }
            /* env.pc is the last translated block's PC, not the exact one. */
            for (j = 0; j < ARRAY_SIZE(s->blit_pc); j++) {
                if (s->blit_pc[j] == pc || !s->blit_pc_n[j]) {
                    s->blit_pc[j] = pc;
                    s->blit_pc_n[j]++;
                    break;
                }
            }
            /* The PC is the shared DMA-start routine (0x0E50FBE4); PR names
             * its caller. */
            if (s->cpu) {
                uint32_t pr = (uint32_t)s->cpu->env.pr;

                for (j = 0; j < ARRAY_SIZE(s->blit_pr); j++) {
                    if (s->blit_pr[j] == pr || !s->blit_pr_n[j]) {
                        s->blit_pr[j] = pr;
                        s->blit_pr_n[j]++;
                        break;
                    }
                }
            }
            /* Every 64th blit, scan the top of the guest stack for words that
             * look like code: return addresses pushed with sts.l pr. */
            if (s->cpu && (s->blit_count & 63) == 0) {
                uint32_t sp = (uint32_t)s->cpu->env.gregs[15];
                uint8_t buf[512];
                uint32_t chain[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
                unsigned found = 0, w;

                cpu_physical_memory_read(sp, buf, sizeof(buf));
                for (w = 0; w < sizeof(buf) / 4 && found < 8; w++) {
                    uint32_t v = ldl_be_p(buf + w * 4);

                    if (v >= 0x0E500000 && v < 0x0E600000) {
                        chain[found++] = v;
                    }
                }
                for (j = 0; j < ARRAY_SIZE(s->blit_up); j++) {
                    unsigned c;

                    if (s->blit_up_n[j] && (s->blit_up[j][0] != chain[0] ||
                                            s->blit_up[j][3] != chain[3] ||
                                            s->blit_up[j][7] != chain[7])) {
                        continue;
                    }
                    for (c = 0; c < 8; c++) {
                        s->blit_up[j][c] = chain[c];
                    }
                    s->blit_up_n[j]++;
                    break;
                }
            }
        }

        if (sm == 0) {
            uint32_t i, n = (uint32_t)MIN(total, chunk_max);

            address_space_read(cs->as, sar, MEMTXATTRS_UNSPECIFIED, buf, unit);
            for (i = 0; i < n; i += unit) {
                memcpy(big + i, buf, unit);
            }
        }
        while (total) {
            uint32_t n = (uint32_t)MIN(total, chunk_max);

            if (sm == 1) {
                address_space_read(cs->as, sar, MEMTXATTRS_UNSPECIFIED, big, n);
                sar += n;
            }
            address_space_write(cs->as, dar, MEMTXATTRS_UNSPECIFIED, big, n);
            dar += n;
            total -= n;
        }
        g_free(big);
        moved = count;
        count = 0;
    }
    while (count--) {
        address_space_read(cs->as, sar, MEMTXATTRS_UNSPECIFIED, buf, unit);
        address_space_write(cs->as, dar, MEMTXATTRS_UNSPECIFIED, buf, unit);
        if (sm == 1) {
            sar += unit;
        } else if (sm == 2) {
            sar -= unit;
        }
        if (dm == 1) {
            dar += unit;
        } else if (dm == 2) {
            dar -= unit;
        }
        moved++;
    }

done:

    if (moved &&
        s->dar[ch] >= SH7269_LCD_BASE &&
        s->dar[ch] < SH7269_LCD_BASE + SH7269_LCD_SIZE) {
        lcd_mark_dirty();
    }

    s->sar[ch] = sar;
    s->dar[ch] = dar;
    s->tcr[ch] = 0;
    s->chcr[ch] = (chcr & ~1u) | 2u;           /* DE clear, TE set */

    /* Reload at DMATCR = 0 (manual figure 11.2): CHCR bit 29 RLDSAR, bit 28
     * RLDDAR; RDMATCR is reloaded with either. */
    if (chcr & (1u << 29)) {
        s->sar[ch] = s->rsar[ch];
        s->tcr[ch] = s->rtcr[ch];
    }
    if (chcr & (1u << 28)) {
        s->dar[ch] = s->rdar[ch];
        s->tcr[ch] = s->rtcr[ch];
    }

    /*
     * Frame-sized transfers (ch6 is 48000 x 16 = 800x480 RGB565): log when the
     * frame first has content and optionally dump it (CDJ_GUI_FB_DUMP=<file>).
     * CDJ_GUI_FRAME_SCAN=0 skips the scan; scripts grep its log line, so it
     * defaults on.
     */
    static int frame_scan = -1;

    if (frame_scan < 0) {
        const char *e = getenv("CDJ_GUI_FRAME_SCAN");

        frame_scan = !e || strcmp(e, "0") != 0;
    }
    if (frame_scan && moved * unit >= 0x10000) {
        const char *dump = getenv("CDJ_GUI_FB_DUMP");

        uint32_t base = (sm == 1) ? sar - moved * unit : sar;
        g_autofree uint8_t *px = g_malloc(moved * unit);
        uint32_t i, nz = 0;

        address_space_read(cs->as, base, MEMTXATTRS_UNSPECIFIED,
                           px, moved * unit);
        for (i = 0; i < moved * unit; i++) {
            if (px[i]) {
                nz++;
            }
        }

        /* Log only non-blank frames, when the count changes. */
        if (nz) {
            static uint32_t last_nz;

            if (nz != last_nz) {
                last_nz = nz;
                info_report("dmac: FRAME HAS CONTENT -- %u of %u bytes "
                            "non-zero (%.2f%%)", nz, moved * unit,
                            100.0 * nz / (moved * unit));
            }
        }

        if (dump && nz) {
            FILE *fp = fopen(dump, "wb");

            if (fp) {
                fwrite(px, 1, moved * unit, fp);
                fclose(fp);
            }
        }
    }

    if (getenv("CDJ_GUI_DMA_DEBUG")) {
        /*
         * Start and end addresses (a CPU watchpoint cannot see DMA writes),
         * plus the PC and PR of the code that set DE. The DMA buffers are
         * heap allocations, not literals.
         */
        info_report("dmac: ch%u %08x -> %08x  (end %08x -> %08x)  "
                    "moved %u x %u bytes  pc=%08x "
                    "pr=%08x at %" PRId64 " ms%s", ch,
                    sm == 1 ? sar - moved * unit
                            : (sm == 2 ? sar + moved * unit : sar),
                    dm == 1 ? dar - moved * unit
                            : (dm == 2 ? dar + moved * unit : dar),
                    sar, dar, moved, unit,
                    current_cpu ? SUPERH_CPU(current_cpu)->env.pc : 0,
                    current_cpu ? (uint32_t)SUPERH_CPU(current_cpu)->env.pr : 0,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000,
                    (chcr & 4) ? ", DEI" : "");

        /* Head of the payload; a receive's SAR is SPDR, so use DAR there. */
        if (moved) {
            bool rx = (sm == 0 && sar == SH7269_RSPI_SPDR);
            /* Use the local sar/dar: s->sar/s->dar may already hold the
             * reload values for the next transfer. */
            uint32_t base = rx
                ? (dm == 1 ? dar - moved * unit
                           : (dm == 2 ? dar + moved * unit : dar))
                : (sm == 1 ? sar - moved * unit
                           : (sm == 2 ? sar + moved * unit : sar));
            char line[3 * 32 + 1];
            uint8_t peek[32];
            unsigned i;

            address_space_read(cs->as, base, MEMTXATTRS_UNSPECIFIED,
                               peek, sizeof(peek));
            for (i = 0; i < sizeof(peek); i++) {
                snprintf(line + i * 3, 4, "%02x ", peek[i]);
            }
            info_report("dmac: ch%u %s head: %s", ch, rx ? "rx" : "tx", line);

            /* First non-zero offset over the whole 4 KB link frame, logged
             * when it changes; shows a misaligned frame grid. */
            if (moved * unit == 0x1000) {
                g_autofree uint8_t *full = g_malloc(moved * unit);
                uint32_t i, nz = 0, first = 0xFFFFFFFF;

                address_space_read(cs->as, base, MEMTXATTRS_UNSPECIFIED,
                                   full, moved * unit);
                for (i = 0; i < moved * unit; i++) {
                    if (full[i]) {
                        if (first == 0xFFFFFFFF) {
                            first = i;
                        }
                        nz++;
                    }
                }
                if (nz) {
                    static uint32_t last_first[16], last_nz[16];

                    if (first != last_first[ch & 15] ||
                        nz != last_nz[ch & 15]) {
                        last_first[ch & 15] = first;
                        last_nz[ch & 15] = nz;
                        info_report("dmac: ch%u %s PAYLOAD first non-zero at "
                                    "+0x%03x, %u of %u bytes non-zero",
                                    ch, rx ? "rx" : "tx", first, nz,
                                    moved * unit);
                    }
                }
            }
        }
    }

    if (chcr & 4) {                            /* IE */
        /*
         * CDJ_GUI_DEI_PRIO=<level> (default 3). The DEI level is a guess, not
         * read from the DMAC's IPR; VLINE runs at level 5 and can outrank it.
         */
        static int dei_prio = -1;

        if (dei_prio < 0) {
            const char *e = getenv("CDJ_GUI_DEI_PRIO");

            dei_prio = e ? atoi(e) : 3;
        }
        sh2a_raise(s->cpu, SH7269_DEI_VECTOR(ch), dei_prio);
    }
}

static uint64_t dmac_read(void *opaque, hwaddr off, unsigned size)
{
    Sh7269Dmac *s = opaque;
    uint32_t v = 0;
    unsigned shift;

    if (off < 0x100) {
        unsigned ch = off / 0x10, reg = (off % 0x10) & ~3u;
        const uint32_t *bank[4] = { s->sar, s->dar, s->tcr, s->chcr };
        v = bank[reg / 4][ch];
        shift = (3 - (off & 3)) * 8;
    } else if (off < 0x200) {
        unsigned ch = (off - 0x100) / 0x10, reg = ((off - 0x100) % 0x10) & ~3u;
        const uint32_t *bank[4] = { s->rsar, s->rdar, s->rtcr, s->rtcr };
        v = bank[reg / 4][ch];
        shift = (3 - (off & 3)) * 8;
    } else if (off >= 0x200 && off < 0x204) {
        v = s->dmaor;
        shift = (1 - (off & 1)) * 8;
    } else if (off >= 0x300 && off < 0x320) {
        v = s->dmars[(off - 0x300) / 4];
        shift = (1 - (off & 1)) * 8;
    } else {
        return 0;
    }

    /* Big-endian byte lanes: a byte read of CHCR+3 is its low byte. */
    if (size == 4) {
        return v;
    }
    return (v >> shift) & ((1u << (size * 8)) - 1);
}

static void dmac_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Sh7269Dmac *s = opaque;
    uint32_t *slot = NULL;
    bool half = false;
    unsigned ch = 0;
    bool is_chcr = false;

    if (off < 0x100) {
        unsigned reg = (off % 0x10) & ~3u;
        uint32_t *bank[4];

        ch = off / 0x10;
        bank[0] = s->sar; bank[1] = s->dar;
        bank[2] = s->tcr; bank[3] = s->chcr;
        slot = &bank[reg / 4][ch];
        is_chcr = (reg == 0xc);
    } else if (off < 0x200) {
        unsigned reg = ((off - 0x100) % 0x10) & ~3u;
        uint32_t *bank[4];

        ch = (off - 0x100) / 0x10;
        bank[0] = s->rsar; bank[1] = s->rdar;
        bank[2] = s->rtcr; bank[3] = s->rtcr;
        slot = &bank[reg / 4][ch];
    } else if (off >= 0x200 && off < 0x204) {
        half = true;
    } else if (off >= 0x300 && off < 0x320) {
        half = true;
    } else {
        return;
    }

    if (half) {
        uint16_t *p = (off < 0x300) ? &s->dmaor : &s->dmars[(off - 0x300) / 4];

        if (size >= 2) {
            *p = val;
        } else {
            unsigned shift = (1 - (off & 1)) * 8;
            *p = (*p & ~(0xffu << shift)) | ((val & 0xff) << shift);
        }
        return;
    }

    if (size == 4) {
        *slot = val;
    } else {
        /* Big-endian placement for any access width; the link driver uses
         * mov.w read-modify-writes (0x0E5008D0 onwards). */
        unsigned shift = (4 - (off & 3) - size) * 8;
        uint32_t mask = (size >= 4 ? 0xffffffffu
                                   : ((1u << (size * 8)) - 1)) << shift;

        /* TE is cleared by writing back 0, which plain storage handles. */
        *slot = (*slot & ~mask) | ((val << shift) & mask);
    }

    if (getenv("CDJ_GUI_DMA_TRACE")) {
        info_report("dmac: write +0x%03x size %u = 0x%08x  pc=0x%08x",
                    (unsigned)off, size, (uint32_t)val,
                    current_cpu ? (uint32_t)cpu_env(current_cpu)->pc : 0);
    }

    if (is_chcr) {
        /* Log who writes ch5 CHCR with DE clear (stops the link receive). */
        if (ch == 5 && !(*slot & 1) && getenv("CDJ_GUI_DMA_TRACE")) {
            info_report("dmac: ch5 CHCR <- 0x%08x (DE clear) from pc 0x%08x"
                        " at %" PRId64 " ms",
                        (uint32_t)val,
                        current_cpu ? (uint32_t)cpu_env(current_cpu)->pc : 0,
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000);
        }
        if (*slot & 1) {
            dmac_run(s, ch);
        }
    }
}

static const MemoryRegionOps dmac_ops = {
    .read = dmac_read,
    .write = dmac_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void dmac_dump(Notifier *n, void *unused)
{
    Sh7269Dmac *s = container_of(n, Sh7269Dmac, exit);
    char line[60 * 8 + 1];
    unsigned k, p = 0;

    if (!s->rx_arm && !s->rx_done) {
        return;
    }
    info_report("dmac: link rx armed=%u done=%u deferred=%u bytes=%" PRIu64
                " last_arm=%" PRId64 " ms last_done=%" PRId64 " ms",
                s->rx_arm, s->rx_done, s->rx_defer, s->rx_bytes,
                s->rx_last_arm_ms, s->rx_last_done_ms);
    info_report("dmac: link rx evictions dedup=%u dupcap=%u fresh=%u fifo=%u "
                "window=%u frames",
                spilink.dedup_dropped, spilink.dupcap_dropped,
                spilink.fresh_dropped, spilink.fifo_dropped,
                spilink.rx ? spilink.rx->len / SPILINK_FRAME : 0);
    for (k = 0; k < ARRAY_SIZE(s->rx_bucket); k++) {
        p += snprintf(line + p, sizeof(line) - p, "%u ", s->rx_bucket[k]);
    }
    info_report("dmac: link rx completions per 1 s: %s", line);
    {
        char bl[60 * 5 + 1];
        int q = 0;
        unsigned z;

        for (z = 0; z < ARRAY_SIZE(s->blit_bucket); z++) {
            q += snprintf(bl + q, sizeof(bl) - q, "%u ", s->blit_bucket[z]);
        }
        info_report("dmac: FRAME BLITS total=%u bytes=%" PRIu64, s->blit_count,
                    s->blit_bytes);
        for (z = 0; z < ARRAY_SIZE(s->blit_pc); z++) {
            if (s->blit_pc_n[z]) {
                info_report("dmac: FRAME BLITS from pc 0x%08x: %u",
                            s->blit_pc[z], s->blit_pc_n[z]);
            }
        }
        for (z = 0; z < ARRAY_SIZE(s->blit_pr); z++) {
            if (s->blit_pr_n[z]) {
                info_report("dmac: FRAME BLITS called from pr 0x%08x: %u",
                            s->blit_pr[z], s->blit_pr_n[z]);
            }
        }
        for (z = 0; z < ARRAY_SIZE(s->blit_up); z++) {
            if (s->blit_up_n[z]) {
                info_report("dmac: FRAME BLITS stack %u: 0x%08x 0x%08x 0x%08x"
                            " 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x",
                            s->blit_up_n[z],
                            s->blit_up[z][0], s->blit_up[z][1],
                            s->blit_up[z][2], s->blit_up[z][3],
                            s->blit_up[z][4], s->blit_up[z][5],
                            s->blit_up[z][6], s->blit_up[z][7]);
            }
        }
        info_report("dmac: FRAME BLITS per 1 s: %s", bl);
        {
            char gl[64 * 8 + 1];
            int r = 0;
            unsigned y;

            for (y = 0; y < ARRAY_SIZE(s->blit_gap); y++) {
                if (s->blit_gap[y]) {
                    r += snprintf(gl + r, sizeof(gl) - r, "%u:%u ", y,
                                  s->blit_gap[y]);
                }
            }
            info_report("dmac: FRAME BLIT gaps (ms:count): %s", gl);
        }
    }
    info_report("dmac: link TX ran=%u last=%" PRId64 " ms",
                s->tx_run, s->tx_last_ms);
    {
        char il[60 * 8 + 1];
        int q = 0;
        unsigned z;

        for (z = 0; z < ARRAY_SIZE(spilink.rx_in_bucket); z++) {
            q += snprintf(il + q, sizeof(il) - q, "%u ",
                          spilink.rx_in_bucket[z] / SPILINK_FRAME);
        }
        info_report("dmac: link ARRIVALS per 1 s (frames): %s", il);
        q = 0;
        for (z = 0; z < ARRAY_SIZE(spilink.poll_bucket); z++) {
            q += snprintf(il + q, sizeof(il) - q, "%u ",
                          spilink.poll_bucket[z]);
        }
        info_report("dmac: link POLLS per 1 s: %s", il);
        gui_wall_report("ARRIVAL", spilink.rx_wall,
                        ARRAY_SIZE(spilink.rx_wall));
        gui_wall_report("poll", spilink.poll_wall,
                        ARRAY_SIZE(spilink.poll_wall));
    }
    info_report("dmac: link rx queue left=%u bytes, irq acks refused=%u",
                spilink.rx ? spilink.rx->len : 0, sh2a_irq.stolen);
    if (s->rx_idle) {
        info_report("dmac: link rx idle frames fed=%u (CDJ_GUI_LINK_IDLE_MS=%"
                    PRId64 ")", s->rx_idle, spilink_idle_ms());
    }
    if (s->rx_defer) {
        info_report("dmac: short reads=%u of which partial=%u (max have=%u);"
                    " last at %" PRId64 " ms had %u of %u",
                    s->rx_defer, s->defer_partial, s->defer_maxhave,
                    s->defer_ms, s->defer_have, s->defer_need);
    }
    for (k = 0; k < ARRAY_SIZE(s->rx_pc); k++) {
        if (s->rx_pc_n[k]) {
            info_report("dmac: link rx armed from pc 0x%08x  %u times,"
                        " last at %" PRId64 " ms",
                        s->rx_pc[k], s->rx_pc_n[k], s->rx_pc_last[k]);
        }
    }
}

static void dmac_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Sh7269Dmac *s = g_new0(Sh7269Dmac, 1);

    s->cpu = cpu;
    s->retry = timer_new_ns(QEMU_CLOCK_VIRTUAL, dmac_retry, s);
    s->exit.notify = dmac_dump;
    qemu_add_exit_notifier(&s->exit);
    memory_region_init_io(&s->iomem, NULL, &dmac_ops, s, "sh7269.dmac",
                          SH7269_DMAC_SIZE);
    memory_region_add_subregion(sysmem, SH7269_DMAC_BASE, &s->iomem);
}


/*
 * The LCD at 0x2C000000. The firmware sends frames with DMAC channel 6:
 *
 *   SAR=0x3C27A060  DAR=0x2C000100  TCR=0xBB80  CHCR=0x80004418  (TS = 16 byte)
 *
 * 0xBB80 * 16 = 768,000 bytes = 800 * 480 * 2. This region is display memory,
 * not an SDRAM mirror. Pixels are big-endian RGB565 starting at +0x100; the
 * first 256 bytes are not displayed.
 */
#define SH7269_LCD_OFFSET   0x100
#define SH7269_LCD_BYTES    (SH7269_LCD_WIDTH * SH7269_LCD_HEIGHT * 2)
#define SH7269_LCD_WIDTH    800
#define SH7269_LCD_HEIGHT   480

typedef struct {
    MemoryRegion iomem;
    MemoryRegion *scan;      /* the region the dirty log lives on (see lcd_init) */
    QemuConsole *con;
    uint8_t *fb;
    bool invalid;
    bool dirty;
    uint8_t *vdc_copy;       /* CDJ_GUI_VDC_SCANOUT: staged frame + previous  */
    uint8_t *vdc_prev;
} Sh7269Lcd;

static Sh7269Lcd *lcd_state;

/* Called by the DMAC when a transfer lands inside the framebuffer. */
static void lcd_mark_dirty(void)
{
    if (lcd_state) {
        lcd_state->dirty = true;
    }
}

/*
 * CDJ_GUI_VDC_SCANOUT: scan out the frame buffer the firmware programs into
 * VDC5 GR3_FLM2 (0xFFFF778C, typically 0x1C080000 / 0x1C13B800) instead of
 * LCD memory + 0x100. Set it to a register address (e.g. 0xffff760c for GR1)
 * to read another layer's base, or to any other value for GR3.
 */
#define SH7269_VDC_GR3_FLM2 0xFFFF778C

static uint32_t vdc_scanout_reg(void)
{
    const char *e = getenv("CDJ_GUI_VDC_SCANOUT");

    if (!e || !*e) {
        return 0;
    }
    if (e[0] == '0' && (e[1] == 'x' || e[1] == 'X')) {
        return (uint32_t)strtoul(e, NULL, 16);
    }
    return SH7269_VDC_GR3_FLM2;
}

static void lcd_update(void *opaque)
{
    Sh7269Lcd *s = opaque;
    DisplaySurface *surf = qemu_console_surface(s->con);
    const uint8_t *src = s->fb + SH7269_LCD_OFFSET;
    uint32_t *dst = surface_data(surf);
    DirtyBitmapSnapshot *snap;
    uint32_t reg = vdc_scanout_reg();
    bool changed;
    int x, y;

    if (reg) {
        uint32_t base = 0;

        cpu_physical_memory_read(reg, &base, 4);
        base = be32_to_cpu(base);
        if (base) {
            if (!s->vdc_copy) {
                s->vdc_copy = g_malloc0(SH7269_LCD_BYTES);
                s->vdc_prev = g_malloc0(SH7269_LCD_BYTES);
            }
            cpu_physical_memory_read(base, s->vdc_copy, SH7269_LCD_BYTES);
            /* No dirty log on this memory, so diff against the last frame. */
            if (!s->invalid
                && !memcmp(s->vdc_copy, s->vdc_prev, SH7269_LCD_BYTES)) {
                return;
            }
            memcpy(s->vdc_prev, s->vdc_copy, SH7269_LCD_BYTES);
            s->invalid = false;
            for (y = 0; y < SH7269_LCD_HEIGHT; y++) {
                const uint8_t *p = s->vdc_copy + y * SH7269_LCD_WIDTH * 2;
                for (x = 0; x < SH7269_LCD_WIDTH; x++, p += 2) {
                    uint16_t v = (p[0] << 8) | p[1];
                    uint8_t r = (v >> 11) & 0x1f;
                    uint8_t g = (v >> 5) & 0x3f;
                    uint8_t b = v & 0x1f;

                    *dst++ = 0xff000000
                           | ((r << 3 | r >> 2) << 16)
                           | ((g << 2 | g >> 4) << 8)
                           |  (b << 3 | b >> 2);
                }
            }
            dpy_gfx_update_full(s->con);
            return;
        }
    }

    if (getenv("CDJ_GUI_LCD_DEBUG")) {
        const uint32_t *p = (const uint32_t *)(s->fb + SH7269_LCD_OFFSET);
        size_t n = (size_t)SH7269_LCD_WIDTH * SH7269_LCD_HEIGHT / 2, i, nz = 0;
        static unsigned calls;
        for (i = 0; i < n; i++) {
            if (p[i]) {
                nz++;
            }
        }
        if (calls++ < 40) {
            info_report("lcd_update: call %u  src-nonzero %zu/%zu  surf=%p",
                        calls, nz, n, (void *)dst);
        }
    }

    /*
     * Repaint only when the frame changed; converting every tick starves the
     * vCPU. Use the dirty log, not a DMAC flag: ch6 is a fill (SM=0/DM=1) and
     * the drawing itself arrives as CPU stores.
     */
    /* Only the visible extent, not the whole 4 MiB aperture. */
    snap = memory_region_snapshot_and_clear_dirty(s->scan, SH7269_LCD_OFFSET,
                                                  SH7269_LCD_BYTES,
                                                  DIRTY_MEMORY_VGA);
    changed = memory_region_snapshot_get_dirty(s->scan, snap,
                                               SH7269_LCD_OFFSET,
                                               SH7269_LCD_BYTES);
    /* CDJ_GUI_LCD_FORCE: always repaint (gdbstub writes set no dirty bit). */
    if (!s->invalid && !changed && !getenv("CDJ_GUI_LCD_FORCE")) {
        g_free(snap);
        return;
    }
    g_free(snap);
    s->dirty = false;
    s->invalid = false;

    /* CDJ_GUI_LCD_TESTPATTERN: draw colour bars instead of the frame, to test
     * the console pipeline. */
    if (getenv("CDJ_GUI_LCD_TESTPATTERN")) {
        static const uint32_t bars[8] = {
            0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00,
            0xffff00ff, 0xffff0000, 0xff0000ff, 0xff202020,
        };
        int band = SH7269_LCD_HEIGHT / 8;
        for (y = 0; y < SH7269_LCD_HEIGHT; y++) {
            uint32_t c = bars[y / band < 8 ? y / band : 7];
            for (x = 0; x < SH7269_LCD_WIDTH; x++) {
                uint32_t shade = 0x40 + (x * 0xc0) / SH7269_LCD_WIDTH;
                uint8_t r = ((c >> 16) & 0xff) * shade / 255;
                uint8_t g = ((c >> 8) & 0xff) * shade / 255;
                uint8_t b = (c & 0xff) * shade / 255;
                *dst++ = 0xff000000 | (r << 16) | (g << 8) | b;
            }
        }
        dpy_gfx_update_full(s->con);
        return;
    }

    for (y = 0; y < SH7269_LCD_HEIGHT; y++) {
        for (x = 0; x < SH7269_LCD_WIDTH; x++) {
            uint16_t p = (src[0] << 8) | src[1];      /* big-endian RGB565 */
            uint8_t r = (p >> 11) & 0x1f;
            uint8_t g = (p >> 5) & 0x3f;
            uint8_t b = p & 0x1f;

            /* Expand to 8 bits, replicating the top bits into the low ones. */
            *dst++ = 0xff000000
                   | ((r << 3 | r >> 2) << 16)
                   | ((g << 2 | g >> 4) << 8)
                   |  (b << 3 | b >> 2);
            src += 2;
        }
    }
    dpy_gfx_update_full(s->con);

    /* Log once when the frame first has content (CPU stores included). */
    {
        static bool announced;

        if (!announced) {
            const uint32_t *p = (const uint32_t *)(s->fb + SH7269_LCD_OFFSET);
            size_t n = (size_t)SH7269_LCD_WIDTH * SH7269_LCD_HEIGHT / 2;
            size_t i, nz = 0;

            for (i = 0; i < n; i++) {
                if (p[i]) {
                    nz++;
                }
            }
            if (nz) {
                announced = true;
                info_report("lcd: PANEL HAS CONTENT -- %zu of %zu longwords "
                            "non-zero", nz, n);
            }
        }
    }
}

static void lcd_invalidate(void *opaque)
{
    ((Sh7269Lcd *)opaque)->invalid = true;
}

/*
 * Host keyboard to front panel. This board owns the window, but the panel is
 * on MAIN, so keys are forwarded over the socket in cdj_panelkeys.h. Each
 * window drives its own deck. The report bits are the ones midi/cdj_actions.py
 * lists as confirmed unless marked otherwise; keep the two tables in step.
 *
 *   Space play/pause    C cue           Q/W/E loop in/out/reloop
 *   Up/Down browse      PgUp/PgDn x10   Enter/Right load/enter  Left/Esc/Bksp back
 *   , .  track -/+      [ ]  search -/+ (held)
 *   - =  nudge slower/faster while held, Shift for a harder nudge
 *   B browse  M menu  U usb  L link  R rekordbox  D disc  T tag list  I info
 *   S sync  A master  K master tempo  P tempo range  V slip  J jog mode  Z reverse
 *
 * CDJ_PANEL_SWEEP=1 adds a probe for unnamed bits: F9/F10 pick a report
 * byte, F1-F8 press its bits.
 */
typedef struct {
    int qcode;
    unsigned off, mask;
    const char *name;
    bool confirmed;
} CdjGuiKey;

static const CdjGuiKey cdj_gui_keys[] = {
    { Q_KEY_CODE_SPC,           0x10, 0x01, "PLAY/PAUSE",   true  },
    { Q_KEY_CODE_C,             0x10, 0x02, "CUE",          true  },
    { Q_KEY_CODE_E,             0x10, 0x04, "RELOOP/EXIT",  true  },
    { Q_KEY_CODE_W,             0x10, 0x08, "LOOP OUT",     true  },
    { Q_KEY_CODE_Q,             0x10, 0x10, "LOOP IN",      true  },
    { Q_KEY_CODE_RET,           0x11, 0x01, "ROTARY PUSH",  true  },
    { Q_KEY_CODE_KP_ENTER,      0x11, 0x01, "ROTARY PUSH",  true  },
    { Q_KEY_CODE_RIGHT,         0x11, 0x01, "ROTARY PUSH",  true  },
    { Q_KEY_CODE_V,             0x11, 0x02, "SLIP",         true  },
    { Q_KEY_CODE_Z,             0x11, 0x04, "DIRECTION REV", true },
    { Q_KEY_CODE_COMMA,         0x12, 0x04, "TRACK -",      true  },
    { Q_KEY_CODE_DOT,           0x12, 0x08, "TRACK +",      true  },
    { Q_KEY_CODE_BRACKET_LEFT,  0x12, 0x10, "SEARCH -",     true  },
    { Q_KEY_CODE_BRACKET_RIGHT, 0x12, 0x20, "SEARCH +",     true  },
    { Q_KEY_CODE_R,             0x13, 0x01, "REKORDBOX",    false },
    { Q_KEY_CODE_L,             0x13, 0x02, "LINK",         false },
    { Q_KEY_CODE_U,             0x13, 0x04, "USB",          true  },
    { Q_KEY_CODE_D,             0x13, 0x10, "DISC",         false },
    { Q_KEY_CODE_B,             0x14, 0x01, "BROWSE",       true  },
    { Q_KEY_CODE_T,             0x14, 0x02, "TAG LIST",     true  },
    { Q_KEY_CODE_I,             0x14, 0x04, "INFO",         true  },
    { Q_KEY_CODE_M,             0x14, 0x08, "MENU",         true  },
    { Q_KEY_CODE_ESC,           0x14, 0x10, "BACK",         true  },
    { Q_KEY_CODE_LEFT,          0x14, 0x10, "BACK",         true  },
    { Q_KEY_CODE_BACKSPACE,     0x14, 0x10, "BACK",         true  },
    { Q_KEY_CODE_J,             0x15, 0x01, "JOG MODE",     false },
    { Q_KEY_CODE_S,             0x15, 0x02, "SYNC",         true  },
    { Q_KEY_CODE_A,             0x15, 0x04, "MASTER",       false },
    { Q_KEY_CODE_P,             0x15, 0x08, "TEMPO RANGE",  true  },
    { Q_KEY_CODE_K,             0x15, 0x10, "MASTER TEMPO", true  },
};

#define CDJ_GUI_ROTARY      0x0E        /* select knob counter byte          */

/*
 * Nudge: a platter turned by hand. The firmware's jog engine takes motion
 * from the rolling position counter in report bytes 8-9 and speed from the
 * pulse period in bytes 10-11 (27778 / P = platter speed %), so while the key
 * is held the counter is stepped and the period held. The counter has to move
 * at least 42 per 30 firmware passes before the bend engages; 2000 pulses/s
 * clears that. P 278 bent the deck to 1.06x, P 139 to 1.18x (graph
 * real-dsp-jog-path).
 */
#define CDJ_NUDGE_TICK_MS   40
#define CDJ_NUDGE_STEP      80
#define CDJ_NUDGE_PERIOD    278
#define CDJ_NUDGE_HARD      139

static struct {
    QEMUTimer *timer;
    const char *sock;
    int dir;                    /* -1 slower, +1 faster, 0 idle */
    bool hard;
    uint16_t count;
} cdj_nudge;

static void cdj_nudge_send(void)
{
    unsigned period = cdj_nudge.dir ? (cdj_nudge.hard ? CDJ_NUDGE_HARD
                                                      : CDJ_NUDGE_PERIOD) : 0;
    unsigned bits = 0x80 | (cdj_nudge.dir > 0 ? 0x40 : 0);

    if (cdj_nudge.dir) {
        cdj_nudge.count += cdj_nudge.dir * CDJ_NUDGE_STEP;
        cdj_panelkey_send_op(cdj_nudge.sock, 0x08, cdj_nudge.count >> 8, 0, "lvl");
        cdj_panelkey_send_op(cdj_nudge.sock, 0x09, cdj_nudge.count & 0xff, 0,
                             "lvl");
    }
    cdj_panelkey_send_op(cdj_nudge.sock, 0x0A, period >> 8, 0, "lvl");
    cdj_panelkey_send_op(cdj_nudge.sock, 0x0B, period & 0xff, 0, "lvl");
    if (cdj_nudge.dir) {
        cdj_panelkey_send_op(cdj_nudge.sock, 0x0F, bits, CDJ_NUDGE_TICK_MS * 3,
                             "or");
    }
}

static void cdj_nudge_tick(void *opaque)
{
    if (!cdj_nudge.dir) {
        return;
    }
    cdj_nudge_send();
    timer_mod(cdj_nudge.timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME)
                               + CDJ_NUDGE_TICK_MS);
}

static void cdj_nudge_set(const char *sock, int dir, bool hard)
{
    if (dir == cdj_nudge.dir && hard == cdj_nudge.hard) {
        return;
    }
    if (!cdj_nudge.timer) {
        cdj_nudge.timer = timer_new_ms(QEMU_CLOCK_REALTIME, cdj_nudge_tick, NULL);
    }
    cdj_nudge.sock = sock;
    cdj_nudge.dir = dir;
    cdj_nudge.hard = hard;
    if (dir) {
        cdj_nudge_tick(NULL);
    } else {
        timer_del(cdj_nudge.timer);
        cdj_nudge_send();       /* period 0: the platter has stopped */
    }
}

static unsigned cdj_gui_sweep_off = 0x15;   /* byte under F1-F8 */

static void cdj_gui_key_event(DeviceState *dev, QemuConsole *src,
                              InputEvent *evt)
{
    static bool shift, nudge_down[2];
    const char *sock = getenv(CDJ_PANELKEY_ENV);
    InputKeyEvent *k = evt->u.key.data;
    int qcode = qemu_input_key_value_to_qcode(k->key);
    unsigned i;

    if (!sock) {
        return;
    }
    if (qcode == Q_KEY_CODE_SHIFT || qcode == Q_KEY_CODE_SHIFT_R) {
        shift = k->down;
        if (cdj_nudge.dir) {
            cdj_nudge_set(sock, cdj_nudge.dir, shift);
        }
        return;
    }
    if (qcode == Q_KEY_CODE_MINUS || qcode == Q_KEY_CODE_EQUAL) {
        nudge_down[qcode == Q_KEY_CODE_EQUAL] = k->down;
        cdj_nudge_set(sock, nudge_down[1] - nudge_down[0], shift);
        return;
    }
    /*
     * Held keys send hold on key-down and rel on key-up, so a key is down as
     * long as the finger is; host auto-repeat is swallowed.
     */
    for (i = 0; i < ARRAY_SIZE(cdj_gui_keys); i++) {
        static bool held[ARRAY_SIZE(cdj_gui_keys)];

        if (cdj_gui_keys[i].qcode != qcode) {
            continue;
        }
        if (k->down == held[i]) {
            return;                     /* auto-repeat, or a stray release */
        }
        held[i] = k->down;
        cdj_panelkey_send_op(sock, cdj_gui_keys[i].off, cdj_gui_keys[i].mask,
                             0, k->down ? "hold" : "rel");
        if (k->down) {
            info_report("panel key: %s (report[0x%02x] 0x%02x)%s",
                        cdj_gui_keys[i].name, cdj_gui_keys[i].off,
                        cdj_gui_keys[i].mask,
                        cdj_gui_keys[i].confirmed ? "" : "   [unverified]");
        }
        return;
    }
    if (!k->down) {
        return;                         /* the rest are taps, not held keys */
    }
    /* The select knob repeats with the host's auto-repeat, like a turn. */
    if (qcode == Q_KEY_CODE_UP || qcode == Q_KEY_CODE_DOWN ||
        qcode == Q_KEY_CODE_PGUP || qcode == Q_KEY_CODE_PGDN) {
        int step = (qcode == Q_KEY_CODE_PGUP || qcode == Q_KEY_CODE_PGDN)
                   ? 10 : 1;

        if (qcode == Q_KEY_CODE_UP || qcode == Q_KEY_CODE_PGUP) {
            step = -step;
        }
        cdj_panelkey_send_op(sock, CDJ_GUI_ROTARY, step, 0, "rot");
        return;
    }
    if (!getenv("CDJ_PANEL_SWEEP")) {
        return;
    }
    if (qcode == Q_KEY_CODE_F9 || qcode == Q_KEY_CODE_F10) {
        cdj_gui_sweep_off += (qcode == Q_KEY_CODE_F10) ? 1 : -1;
        cdj_gui_sweep_off &= 0x1F;
        info_report("panel sweep: byte is now 0x%02x (F1-F8 press its bits)",
                    cdj_gui_sweep_off);
        return;
    }
    if (qcode >= Q_KEY_CODE_F1 && qcode <= Q_KEY_CODE_F8) {
        unsigned mask = 1u << (qcode - Q_KEY_CODE_F1);

        cdj_panelkey_send(sock, cdj_gui_sweep_off, mask, 150);
        info_report("panel sweep: report[0x%02x] |= 0x%02x",
                    cdj_gui_sweep_off, mask);
    }
}

static QemuInputHandler cdj_gui_kbd = {
    .name  = "CDJ front panel",
    .mask  = INPUT_EVENT_MASK_KEY,
    .event = cdj_gui_key_event,
};

static void cdj_gui_keys_init(void)
{
    if (!getenv(CDJ_PANELKEY_ENV)) {
        return;
    }
    qemu_input_handler_register(NULL, &cdj_gui_kbd);
    info_report("sh7269gui: keyboard live -- Space play/pause, C cue, "
                "Up/Down browse, Enter load, Esc back, - = nudge "
                "(emulator/README.md lists every key)");
}

/*
 * Touch screen from the host mouse (CDJ_TOUCH=1): left button is the finger.
 * Changes are forwarded to MAIN's key socket (CDJ_TOUCH_FWD overrides the
 * name), which holds short clicks long enough for the firmware's filter.
 * Off by default because registering an ABS handler switches the gtk window
 * to absolute pointer mode.
 */
typedef struct {
    CdjTouch cur;
    CdjTouch sent;              /* last state forwarded                      */
    const char *fwd;
    Notifier exit;
} GuiTouch;

static GuiTouch *gui_touch;

static void gui_touch_changed(GuiTouch *s)
{
    bool edge = s->cur.down != s->sent.down;

    /* Forward presses, drags and releases, not hovering. */
    if (!edge && (!s->cur.down
                  || (s->cur.x == s->sent.x && s->cur.y == s->sent.y))) {
        return;
    }
    if (edge) {
        info_report("touch: %s at (%u, %u)", s->cur.down ? "DOWN" : "UP",
                    s->cur.x, s->cur.y);
    }
    s->sent = s->cur;
    cdj_touch_send(s->fwd, &s->cur);
}

static void gui_touch_event(DeviceState *dev, QemuConsole *src,
                            InputEvent *evt)
{
    GuiTouch *s = gui_touch;

    if (!s || !lcd_state || src != lcd_state->con) {
        return;
    }
    switch (evt->type) {
    case INPUT_EVENT_KIND_ABS: {
        InputMoveEvent *m = evt->u.abs.data;

        /* The axis spans the 800x480 console at any window zoom. */
        if (m->axis == INPUT_AXIS_X) {
            s->cur.x = qemu_input_scale_axis(m->value, INPUT_EVENT_ABS_MIN,
                                             INPUT_EVENT_ABS_MAX, 0,
                                             CDJ_TOUCH_W - 1);
        } else if (m->axis == INPUT_AXIS_Y) {
            s->cur.y = qemu_input_scale_axis(m->value, INPUT_EVENT_ABS_MIN,
                                             INPUT_EVENT_ABS_MAX, 0,
                                             CDJ_TOUCH_H - 1);
        }
        break;
    }
    case INPUT_EVENT_KIND_BTN: {
        InputBtnEvent *b = evt->u.btn.data;

        if (b->button == INPUT_BUTTON_LEFT) {
            s->cur.down = b->down;
            s->cur.events++;
        }
        break;
    }
    default:
        break;
    }
}

/* A move and its button arrive as one batch; act once the batch is whole. */
static void gui_touch_sync(DeviceState *dev)
{
    if (gui_touch) {
        gui_touch_changed(gui_touch);
    }
}

static QemuInputHandler gui_touch_handler = {
    .name  = "CDJ touch screen",
    .mask  = INPUT_EVENT_MASK_ABS | INPUT_EVENT_MASK_BTN,
    .event = gui_touch_event,
    .sync  = gui_touch_sync,
};

static void gui_touch_summary(Notifier *n, void *opaque)
{
    GuiTouch *s = container_of(n, GuiTouch, exit);

    info_report("touch: %" PRIu64 " button events from the window",
                s->cur.events);
}

static void gui_touch_init(void)
{
    const char *fwd = getenv("CDJ_TOUCH_FWD");
    GuiTouch *s;

    if (!cdj_touch_enabled()) {
        return;
    }
    if (!fwd || !*fwd) {
        fwd = getenv(CDJ_PANELKEY_ENV);
    }
    if (!fwd || !*fwd) {
        warn_report("touch: CDJ_TOUCH is set but neither CDJ_TOUCH_FWD nor "
                    CDJ_PANELKEY_ENV " names MAIN's socket -- clicks go "
                    "nowhere");
        return;
    }
    s = g_new0(GuiTouch, 1);
    s->fwd = fwd;
    gui_touch = s;
    qemu_input_handler_register(NULL, &gui_touch_handler);
    s->exit.notify = gui_touch_summary;
    qemu_add_exit_notifier(&s->exit);
    info_report("sh7269gui: touch screen live -- left button is a finger, "
                "forwarded to %s", fwd);
}

static const GraphicHwOps lcd_ops = {
    .gfx_update = lcd_update,
    .invalidate = lcd_invalidate,
};

/*
 * 0x2C000000-0x2FFFFFFF is an uncached view of the CS3 SDRAM at 0x0C000000
 * (manual table 10.2), so the LCD is an alias of the SDRAM and CPU stores and
 * DMA land in the same place. 0x1C000000 is separate on-chip RAM (manual
 * table 47.2). CDJ_GUI_LEGACY_MAP=1 gives 0x2C its own RAM block instead.
 */
static void lcd_init(MemoryRegion *sysmem, MemoryRegion *sdram)
{
    Sh7269Lcd *s = g_new0(Sh7269Lcd, 1);

    if (getenv("CDJ_GUI_LEGACY_MAP")) {
        memory_region_init_ram(&s->iomem, NULL, "sh7269.lcd", SH7269_LCD_SIZE,
                               &error_fatal);
        memory_region_add_subregion(sysmem, SH7269_LCD_BASE, &s->iomem);
        s->fb = memory_region_get_ram_ptr(&s->iomem);
        memory_region_set_log(&s->iomem, true, DIRTY_MEMORY_VGA);
        s->scan = &s->iomem;
    } else {
        memory_region_init_alias(&s->iomem, NULL, "sh7269.sdram-2c", sdram, 0,
                                 memory_region_size(sdram));
        memory_region_add_subregion(sysmem, SH7269_LCD_BASE, &s->iomem);
        s->fb = memory_region_get_ram_ptr(sdram);
        memory_region_set_log(sdram, true, DIRTY_MEMORY_VGA);
        s->scan = sdram;
    }

    s->con = graphic_console_init(NULL, 0, &lcd_ops, s);
    qemu_console_resize(s->con, SH7269_LCD_WIDTH, SH7269_LCD_HEIGHT);
    lcd_state = s;
    cdj_gui_keys_init();
    gui_touch_init();
}


/* RSPI channel 1: register storage only; the data moves via the DMAC. */
#define SH7269_RSPI_BASE    0xE800E800

typedef struct {
    MemoryRegion iomem;
    uint8_t reg[0x100];
} Sh7269Rspi;

static uint64_t rspi_read(void *opaque, hwaddr off, unsigned size)
{
    Sh7269Rspi *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) & 0xff];
    }

    if (getenv("CDJ_GUI_SPI_DEBUG")) {
        static unsigned n;

        if (n++ < 100000) {
            info_report("rspi: read  +0x%02x size %u -> 0x%0*llx",
                        (unsigned)off, size, size * 2,
                        (unsigned long long)v);
        }
    }
    return v;
}

static void rspi_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Sh7269Rspi *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->reg[(off + size - 1 - i) & 0xff] = val >> (8 * i);
    }

    if (getenv("CDJ_GUI_SPI_DEBUG")) {
        static unsigned n;

        if (n++ < 100000) {
            info_report("rspi: write +0x%02x size %u = 0x%0*llx",
                        (unsigned)off, size, size * 2,
                        (unsigned long long)val);
        }
    }
}

static const MemoryRegionOps rspi_ops = {
    .read = rspi_read,
    .write = rspi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void rspi_init(MemoryRegion *sysmem)
{
    Sh7269Rspi *s = g_new0(Sh7269Rspi, 1);

    memory_region_init_io(&s->iomem, NULL, &rspi_ops, s, "sh7269.rspi", 0x100);
    memory_region_add_subregion(sysmem, SH7269_RSPI_BASE, &s->iomem);
}


/*
 * INTC at 0xFFFE0800, as plain storage so the firmware's read-modify-writes
 * of the IPRs (IPR10 = 0x0555 for VDC4, IPR12 = 0x8000 for CMI0) read back.
 * Interrupt delivery does not consult these.
 */
#define SH7269_INTC_BASE    0xFFFE0800
#define SH7269_INTC_SIZE    0x800

typedef struct {
    MemoryRegion iomem;
    uint8_t reg[SH7269_INTC_SIZE];
} Sh7269Intc;

static uint64_t intc_read(void *opaque, hwaddr off, unsigned size)
{
    Sh7269Intc *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) & (SH7269_INTC_SIZE - 1)];
    }
    return v;
}

static void intc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Sh7269Intc *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->reg[(off + size - 1 - i) & (SH7269_INTC_SIZE - 1)] = val >> (8 * i);
    }
}

static const MemoryRegionOps intc_ops = {
    .read = intc_read,
    .write = intc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void intc_init(MemoryRegion *sysmem)
{
    Sh7269Intc *s = g_new0(Sh7269Intc, 1);

    memory_region_init_io(&s->iomem, NULL, &intc_ops, s, "sh7269.intc",
                          SH7269_INTC_SIZE);
    /* priority 1: the CPG stub (0xFFFE0000 + 0x8000) covers this range */
    memory_region_add_subregion_overlap(sysmem, SH7269_INTC_BASE, &s->iomem, 1);
}


/*
 * VDC4 CLUT tables at 0xFFFF6000 (three blocks: 6000-63FF, 6400-67FF,
 * 6800-6BFF), as storage. Inside the sh7269.onchip-hi RAM window, so mapped
 * at a higher priority.
 */
#define SH7269_VDC4_BASE    0xFFFF6000
#define SH7269_VDC4_SIZE    0x0C00

typedef struct {
    MemoryRegion iomem;
    uint8_t reg[SH7269_VDC4_SIZE];
} Sh7269Vdc;

static uint64_t vdc_read(void *opaque, hwaddr off, unsigned size)
{
    Sh7269Vdc *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) % SH7269_VDC4_SIZE];
    }
    if (getenv("CDJ_GUI_VDC_DEBUG")) {
        static unsigned n;
        if (n++ < 200) {
            info_report("vdc: read  +0x%03x size %u -> 0x%llx",
                        (unsigned)off, size, (unsigned long long)v);
        }
    }
    return v;
}

static void vdc_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Sh7269Vdc *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->reg[(off + size - 1 - i) % SH7269_VDC4_SIZE] = val >> (8 * i);
    }
    if (getenv("CDJ_GUI_VDC_DEBUG")) {
        static unsigned n;
        if (n++ < 200) {
            info_report("vdc: write +0x%03x size %u = 0x%llx",
                        (unsigned)off, size, (unsigned long long)val);
        }
    }
}

static const MemoryRegionOps vdc_ops = {
    .read = vdc_read,
    .write = vdc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void vdc_init(MemoryRegion *sysmem)
{
    Sh7269Vdc *s = g_new0(Sh7269Vdc, 1);

    memory_region_init_io(&s->iomem, NULL, &vdc_ops, s, "sh7269.vdc4",
                          SH7269_VDC4_SIZE);
    /* priority 1: must win over sh7269.onchip-hi, which covers this range */
    memory_region_add_subregion_overlap(sysmem, SH7269_VDC4_BASE,
                                        &s->iomem, 1);
}

/*
 * VDC4 system controller at 0xFFFF7A80 (manual table 37.7). The firmware
 * programs:
 *
 *      SYSCNT_INT2      = 0x00001000     INT_STA3    -- VLINE armed
 *      SYSCNT_INT4      = 0x00001000     INT_OUT3_ON -- VLINE output enabled
 *      SYSCNT_PANEL_CLK = 0x2102         (reset value is 0x0001)
 *
 * VLINE is vector 174 (IPR10 bits 7-4), handler at 0xFFF800D4.
 *
 *   +0x00 SYSCNT_INT1   +0x04 SYSCNT_INT2   +0x08 SYSCNT_INT3
 *   +0x0C SYSCNT_INT4   +0x10 PANEL_CLK (16) +0x12 CLUT (16, read-only)
 *
 * INT_STA*: write 0 clears the status, write 1 starts acceptance; reads 1
 * when an interrupt has occurred.
 *
 * CDJ_GUI_VLINE_HZ sets the frame rate (default 60); CDJ_GUI_VLINE_OFF=1
 * stops the timer.
 */
#define SH7269_VDCSYS_BASE      0xFFFF7A80
#define SH7269_VDCSYS_SIZE      0x20
#define SH7269_VLINE_VECTOR     174         /* manual table 7.4 */
#define SH7269_VLINE_PRIO       5           /* firmware writes IPR10 = 0x0555 */
#define VDCSYS_VLINE_BIT        (1u << 12)  /* INT_STA3 / INT_OUT3_ON */

/*
 * Video interrupt vectors 171-179. SYSCNT_INT2 packs INT_STA0..7 at a 4-bit
 * stride (manual 37.2.2) and each handler returns unless both its enable bit
 * (SYSCNT_INT4) and status bit are set:
 *
 *   171 bit 0  INT_STA0  VI_VSYNC     video input vsync (unused on this deck)
 *   172 bit 4  INT_STA1  LO_VSYNC     LCD output vsync: start of frame
 *   173 bit 8  INT_STA2  VSYNCERR     error
 *   174 bit 12 INT_STA3  VLINE        mid-frame scanline
 *   175 bit 16 INT_STA4  VFIELD       field toggle, interlaced only
 *   176 bit 20 INT_STA5  VBUFERR1     error
 *   177 bit 24 INT_STA6  VBUFERR2     error
 *   178 bit 28 INT_STA7  VBUFERR3     error
 *   179 SYSCNT_INT1 bit 0 INT_STA8    VBUFERR4  error (a second register pair)
 *
 * Only LO_VSYNC, VLINE and VFIELD are raised; the rest are faults.
 */
#define SH7269_LO_VSYNC_VECTOR  172
#define VDCSYS_LO_VSYNC_BIT     (1u << 4)   /* INT_STA1 / INT_OUT1_ON */
#define SH7269_VFIELD_VECTOR    175
#define VDCSYS_VFIELD_BIT       (1u << 16)  /* INT_STA4 / INT_OUT4_ON */

typedef struct {
    MemoryRegion iomem;
    QEMUTimer *timer;
    SuperHCPU *cpu;
    uint32_t int1, int2, int3, int4;
    uint16_t panel_clk, clut;
    uint64_t period_ns;
    unsigned fired;
} Sh7269VdcSys;

static void vdcsys_rearm(Sh7269VdcSys *s)
{
    timer_mod(s->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->period_ns);
}

static void vdcsys_fire(void *opaque)
{
    Sh7269VdcSys *s = opaque;

    /* Raise only the sources whose output the firmware has enabled. */
    static const struct { uint32_t bit; int vec; const char *name; } sources[] = {
        { VDCSYS_LO_VSYNC_BIT, SH7269_LO_VSYNC_VECTOR, "LO_VSYNC" },
        { VDCSYS_VLINE_BIT,    SH7269_VLINE_VECTOR,    "VLINE"    },
        { VDCSYS_VFIELD_BIT,   SH7269_VFIELD_VECTOR,   "VFIELD"   },
    };
    unsigned i;

    /* In frame order: start of frame, scanline, field. */
    for (i = 0; i < ARRAY_SIZE(sources); i++) {
        if (!(s->int4 & sources[i].bit)) {
            continue;
        }
        s->int2 |= sources[i].bit;           /* INT_STAn: an interrupt occurred */
        sh2a_raise(s->cpu, sources[i].vec, SH7269_VLINE_PRIO);
        if (getenv("CDJ_GUI_VLINE_DEBUG") && s->fired < 40) {
            info_report("vdcsys: %s #%u vec=%d int2=0x%08x int4=0x%08x",
                        sources[i].name, s->fired, sources[i].vec,
                        s->int2, s->int4);
        }
        s->fired++;
    }
    vdcsys_rearm(s);
}

static uint64_t vdcsys_read(void *opaque, hwaddr off, unsigned size)
{
    Sh7269VdcSys *s = opaque;
    uint64_t v;

    switch (off) {
    case 0x00: v = s->int1; break;
    case 0x04: v = s->int2; break;
    case 0x08: v = s->int3; break;
    case 0x0C: v = s->int4; break;
    case 0x10: v = s->panel_clk; break;
    case 0x12: v = s->clut; break;
    default:   v = 0; break;
    }
    if (getenv("CDJ_GUI_VDC_DEBUG")) {
        info_report("vdcsys: read  +0x%02x size %u -> 0x%llx",
                    (unsigned)off, size, (unsigned long long)v);
    }
    return v;
}

static void vdcsys_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    Sh7269VdcSys *s = opaque;

    switch (off) {
    case 0x00: s->int1 = val; break;
    case 0x08: s->int3 = val; break;
    case 0x04:
        /* Writing 0 (clear) or 1 (start acceptance) both leave the status
         * low until the next event. */
        s->int2 &= ~(uint32_t)val;
        break;
    case 0x0C: s->int4 = val; break;
    case 0x10: s->panel_clk = val; break;
    default: break;
    }
    if (getenv("CDJ_GUI_VDC_DEBUG")) {
        info_report("vdcsys: write +0x%02x size %u = 0x%llx",
                    (unsigned)off, size, (unsigned long long)val);
    }
}

static const MemoryRegionOps vdcsys_ops = {
    .read = vdcsys_read,
    .write = vdcsys_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void vdcsys_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Sh7269VdcSys *s = g_new0(Sh7269VdcSys, 1);
    const char *hz = getenv("CDJ_GUI_VLINE_HZ");
    unsigned rate = hz ? atoi(hz) : 60;

    if (rate == 0) {
        rate = 60;
    }
    s->cpu = cpu;
    s->panel_clk = 0x0001;                  /* manual: initial value */
    s->period_ns = 1000000000ULL / rate;
    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, vdcsys_fire, s);

    memory_region_init_io(&s->iomem, NULL, &vdcsys_ops, s, "sh7269.vdc4-syscnt",
                          SH7269_VDCSYS_SIZE);
    /* priority 1: sh7269.onchip-hi covers this range and would swallow it */
    memory_region_add_subregion_overlap(sysmem, SH7269_VDCSYS_BASE,
                                        &s->iomem, 1);
    if (!getenv("CDJ_GUI_VLINE_OFF")) {
        vdcsys_rearm(s);
    }
}

#define GUI_SDRAM_BASE      0x0C000000
#define GUI_SDRAM_SIZE      (64 * MiB)      /* CS3 SDRAM                       */
#define GUI_OCRAM_BASE      0x1C000000      /* on-chip large-capacity RAM      */
#define GUI_OCRAM_SIZE      0x280000        /* 2.5 MB, manual table 47.2       */
#define GUI_IMAGE_BASE      0x0E500000      /* a load-table destination        */
#define GUI_ONCHIP_BASE     0xFFF80000
/*
 * On-chip RAM stops at 0xFFFC0000, where the peripherals begin; a RAM region
 * there would shadow them. The load table only uses 0xFFF84000 and
 * 0xFFF88000, and VBR is 0xFFF82000.
 */
#define GUI_ONCHIP_SIZE     (256 * KiB)
#define GUI_LOADTAB_OFF     0x10
#define GUI_LOADTAB_MAX     20

typedef struct Sh7269ResetData {
    SuperHCPU *cpu;
    uint32_t pc;
    uint32_t sp;
} Sh7269ResetData;

static void sh7269_reset(void *opaque)
{
    Sh7269ResetData *s = opaque;
    CPUSH4State *env = &s->cpu->env;

    cpu_reset(CPU(s->cpu));
    env->pc = s->pc;
    env->gregs[15] = s->sp;
    /* SH-2A has no MMU and runs privileged from reset. */
    env->sr = (1u << SR_MD);
    env->mmucr = 0;
}

/*
 * Scatter the image per its load table: twenty 12-byte [dest][src][size]
 * records at 0x10..0x100, code from 0x100. Destinations include 0x1C000000,
 * 0x2FFFB000, 0x0FFECDE4, 0xFFF84000 and 0x0E500000. The meaning of fields 1
 * and 2 is not fully understood (the last record of each run of four holds
 * 0x30010, 0x30040, ... which look like links, not sizes), so the result is
 * a plausible scatter rather than an exact model of the format.
 */
static int sh7269_load_image(const uint8_t *img, size_t len)
{
    int applied = 0;
    int i;

    for (i = 0; i < GUI_LOADTAB_MAX; i++) {
        size_t off = GUI_LOADTAB_OFF + i * 12;
        uint32_t dest, src, size;

        if (off + 12 > len) {
            break;
        }
        dest = ldl_be_p(img + off);
        src  = ldl_be_p(img + off + 4);
        size = ldl_be_p(img + off + 8);

        /*
         * Field 1 is a source offset biased by 0x30000 (the image starts at
         * flash offset 0x30000). With the bias, the 257 literal pointers into
         * 0x1C000000 land on function prologues. Smaller values are left
         * as-is; record 15 is not understood.
         */
        if (src >= 0x30000) {
            src -= 0x30000;
        }

        if (size == 0) {
            continue;               /* padding entry */
        }
        if (src != 0 && (size_t)src + size > len) {
            info_report("sh7269: record %d src 0x%x size 0x%x runs past the "
                        "image -- table ends here", i, src, size);
            break;
        }

        if (src == 0) {
            g_autofree uint8_t *zero = g_malloc0(size);
            cpu_physical_memory_write(dest, zero, size);
            info_report("sh7269: zero-fill 0x%08x + 0x%08x", dest, size);
        } else {
            cpu_physical_memory_write(dest, img + src, size);
            info_report("sh7269: copy image+0x%06x -> 0x%08x size 0x%08x",
                        src, dest, size);
        }
        applied++;
    }
    return applied;
}

/*
 * CDJ_GUI_MPOKE="<addr>:<val>[:<size>],...": hold GUI RAM words at the given
 * values, rewritten every 2 ms (size 1, 2 or 4, default 4). Debug aid.
 */
#define GUI_MPOKE_MAX 8

static struct { uint32_t addr, val; unsigned size; } gui_mpoke[GUI_MPOKE_MAX];
static unsigned gui_mpoke_n;
static QEMUTimer *gui_mpoke_timer;
static uint64_t gui_mpoke_writes;

static void gui_peek_report(void);

static void gui_mpoke_tick(void *opaque)
{
    unsigned i;

    for (i = 0; i < gui_mpoke_n; i++) {
        /* The guest is big-endian. */
        uint8_t b[4];
        unsigned k;
        uint32_t v = gui_mpoke[i].val;

        for (k = 0; k < gui_mpoke[i].size; k++) {
            b[k] = (v >> (8 * (gui_mpoke[i].size - 1 - k))) & 0xFF;
        }
        cpu_physical_memory_write(gui_mpoke[i].addr, b, gui_mpoke[i].size);
        gui_mpoke_writes++;
    }
    gui_peek_report();
    timer_mod(gui_mpoke_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2 * 1000000);
}

/*
 * CDJ_GUI_PEEK=<addr>[:<words>][,<addr>[:<words>]...]: log GUI RAM words
 * (big-endian, up to 8 per entry) whenever they change, sampled on the
 * CDJ_GUI_MPOKE timer.
 */
#define GUI_PEEK_MAX 8

static struct { uint32_t addr, words, last[8]; bool seen; } gui_peek[GUI_PEEK_MAX];
static unsigned gui_peek_n;

static void gui_peek_report(void)
{
    unsigned i, k;

    for (i = 0; i < gui_peek_n; i++) {
        uint32_t v[8];
        bool diff = !gui_peek[i].seen;

        for (k = 0; k < gui_peek[i].words; k++) {
            uint8_t b[4];

            cpu_physical_memory_read(gui_peek[i].addr + 4 * k, b, 4);
            v[k] = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16)
                 | ((uint32_t)b[2] << 8) | (uint32_t)b[3];
            if (v[k] != gui_peek[i].last[k]) {
                diff = true;
            }
        }
        if (!diff) {
            continue;
        }
        gui_peek[i].seen = true;
        for (k = 0; k < gui_peek[i].words; k++) {
            gui_peek[i].last[k] = v[k];
            info_report("gui peek: *0x%08x = 0x%08x (%d) at %" PRId64 " ms",
                        gui_peek[i].addr + 4 * k, v[k], (int32_t)v[k],
                        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        }
    }
}

static void gui_peek_init(void)
{
    const char *e = getenv("CDJ_GUI_PEEK");

    while (e && *e && gui_peek_n < GUI_PEEK_MAX) {
        const char *start = e;
        uint32_t a = strtoul(e, (char **)&e, 0);
        uint32_t n = 1;

        if (*e == ':') {
            n = strtoul(e + 1, (char **)&e, 0);
        }
        if (e == start) {
            break;
        }
        if (!n || n > 8) {
            n = 1;
        }
        gui_peek[gui_peek_n].addr = a;
        gui_peek[gui_peek_n].words = n;
        gui_peek_n++;
        info_report("gui peek: watching 0x%08x .. 0x%08x for CHANGES",
                    a, a + 4 * n - 4);
        e += *e == ',';
    }
}

static void gui_mpoke_init(void)
{
    const char *e = getenv("CDJ_GUI_MPOKE");

    while (e && *e && gui_mpoke_n < GUI_MPOKE_MAX) {
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
        if (e == start) {           /* unparsable */
            break;
        }
        if (sz != 1 && sz != 2 && sz != 4) {
            sz = 4;
        }
        gui_mpoke[gui_mpoke_n].addr = a;
        gui_mpoke[gui_mpoke_n].val = v;
        gui_mpoke[gui_mpoke_n].size = sz;
        gui_mpoke_n++;
        e += *e == ',';
    }
    gui_peek_init();
    /* The timer also drives CDJ_GUI_PEEK. */
    if (!gui_mpoke_n && !gui_peek_n) {
        return;
    }
    {
        unsigned i;

        for (i = 0; i < gui_mpoke_n; i++) {
            info_report("gui mpoke: holding *0x%08x = 0x%x (%u bytes, "
                        "big-endian) -- EXPERIMENT, not a fix",
                        gui_mpoke[i].addr, gui_mpoke[i].val,
                        gui_mpoke[i].size);
        }
    }
    gui_mpoke_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gui_mpoke_tick, NULL);
    timer_mod(gui_mpoke_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 2 * 1000000);
}


static void sh7269gui_init(MachineState *machine)
{
    MemoryRegion *sysmem = get_system_memory();
    MemoryRegion *sdram = g_new(MemoryRegion, 1);
    MemoryRegion *sdram_mirror = g_new(MemoryRegion, 1);
    MemoryRegion *imgmem = g_new(MemoryRegion, 1);
    MemoryRegion *onchip = g_new(MemoryRegion, 1);
    Sh7269ResetData *reset_info;
    SuperHCPU *cpu;
    uint8_t *img;
    gsize len;
    uint32_t entry;

    cpu = SUPERH_CPU(cpu_create(machine->cpu_type));

    gui_mpoke_init();

    /* SH-2A is always privileged; without this the first LDC to SR drops
     * to user mode and the cache init at 0x0E51C4D2 faults. */
    cpu->env.features |= SH_FEATURE_SH2A;

    memory_region_init_ram(sdram, NULL, "sh7269.sdram", GUI_SDRAM_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, GUI_SDRAM_BASE, sdram);

    /*
     * 0x1C000000 (cached) / 0x3C000000 (uncached) is the 2.5 MB on-chip
     * large-capacity RAM (manual table 47.2); the SPI buffers sit in its last
     * page at 0x3C27B000/0x3C27C000. CDJ_GUI_LEGACY_MAP=1 aliases them to
     * SDRAM instead.
     */
    {
        MemoryRegion *m3c = g_new0(MemoryRegion, 1);

        if (getenv("CDJ_GUI_LEGACY_MAP")) {
            memory_region_init_alias(sdram_mirror, NULL, "sh7269.sdram-1c",
                                     sdram, 0, GUI_SDRAM_SIZE);
            memory_region_init_alias(m3c, NULL, "sh7269.sdram-3c", sdram, 0,
                                     GUI_SDRAM_SIZE);
        } else {
            memory_region_init_ram(sdram_mirror, NULL, "sh7269.ocram",
                                   GUI_OCRAM_SIZE, &error_fatal);
            memory_region_init_alias(m3c, NULL, "sh7269.ocram-3c",
                                     sdram_mirror, 0, GUI_OCRAM_SIZE);
        }
        memory_region_add_subregion(sysmem, GUI_OCRAM_BASE, sdram_mirror);
        memory_region_add_subregion(sysmem, 0x3C000000, m3c);
    }

    memory_region_init_ram(imgmem, NULL, "sh7269.image", 32 * MiB,
                           &error_fatal);
    memory_region_add_subregion(sysmem, GUI_IMAGE_BASE, imgmem);

    memory_region_init_ram(onchip, NULL, "sh7269.onchip", GUI_ONCHIP_SIZE,
                           &error_fatal);
    memory_region_add_subregion(sysmem, GUI_ONCHIP_BASE, onchip);

    /* Zero-filled load-table destinations around 0x0FFEC000 and 0x2FFFB000. */
    {
        MemoryRegion *r0 = g_new(MemoryRegion, 1);
        MemoryRegion *r1 = g_new(MemoryRegion, 1);

        memory_region_init_ram(r0, NULL, "sh7269.ram-0ff", 32 * MiB,
                               &error_fatal);
        memory_region_add_subregion(sysmem, 0x0FF00000, r0);
        memory_region_init_ram(r1, NULL, "sh7269.ram-2ff", 32 * MiB,
                               &error_fatal);
        memory_region_add_subregion(sysmem, 0x2FF00000, r1);
    }

    /* CS0: the GUI's boot flash. The firmware reads back into it (0x0,
     * 0x4300), so it must be backed. */
    {
        MemoryRegion *cs0 = g_new(MemoryRegion, 1);

        memory_region_init_ram(cs0, NULL, "sh7269.cs0", 16 * MiB,
                               &error_fatal);
        memory_region_add_subregion(sysmem, 0x00000000, cs0);
    }

    /*
     * RSPI at 0xE800E800: the MAIN <-> GUI link (SH2_RSPCK1 / SH2_SSL10 /
     * SH2_MOSI1 / SH2_MISO1 to MAIN's MSIOF1). Registers as initialised:
     *     +0x00 SPCR   +0x01 SSLP  +0x02 SPPCR  +0x03 SPSR (status)
     *     +0x04 SPDR   +0x0A SPBR (bit rate = 0x02)  +0x0B SPDCR (0x60)
     *     +0x10 SPCMD0 (16-bit, 0x0201)  +0x20 SPBFCR (0x32)
     * Storage only; CDJ_GUI_SPI_DEBUG=1 logs accesses.
     */
    /* On-chip RAM above the peripherals (0xFFFF0000-0xFFFFFFFF). */
    {
        MemoryRegion *hi = g_new(MemoryRegion, 1);

        memory_region_init_ram(hi, NULL, "sh7269.onchip-hi", 64 * KiB,
                               &error_fatal);
        memory_region_add_subregion(sysmem, 0xFFFF0000, hi);
    }

    rspi_init(sysmem);
    vdc_init(sysmem);
    vdcsys_init(sysmem, cpu);

    cmt_init(sysmem, cpu);
    portf_init(sysmem, cpu);

    spilink_init();
    lcd_init(sysmem, sdram);
    dmac_init(sysmem, cpu);

    /* Unmodelled peripheral blocks. */
    create_unimplemented_device("sh7269.cpg",   0xFFFE0000, 0x8000);
    create_unimplemented_device("sh7269.bsc",   0xFFFC0000, 0x1000);
    /* INTC ends at IPR12 (0xFFFE0C0C); the DMAC starts at 0xFFFE1000. */
    intc_init(sysmem);
    create_unimplemented_device("sh7269.scif",  0xE8007000, 0x1000);
    /*
     * Cache address/data arrays as plain RAM: no cache is modelled, and as
     * MMIO the firmware's frequent purges forced TB recompiles.
     * CDJ_GUI_CACHE_UNIMP=1 maps an unimplemented-device stub instead.
     */
    if (getenv("CDJ_GUI_CACHE_UNIMP")) {
        create_unimplemented_device("sh7269.cache", 0xF0000000, 0x1000000);
    } else {
        MemoryRegion *cache = g_new(MemoryRegion, 1);

        memory_region_init_ram(cache, NULL, "sh7269.cache-sink", 0x1000000,
                               &error_fatal);
        memory_region_add_subregion(sysmem, 0xF0000000, cache);
    }
    /* Placeholder; the firmware never references 0xFFFD0000. */
    create_unimplemented_device("sh7269.vdc-guess", 0xFFFD0000, 0x10000);

    if (!machine->kernel_filename) {
        error_report("sh7269gui: pass the unpacked GUI image with -kernel "
                     "(gui_decode.py output)");
        exit(1);
    }
    if (!g_file_get_contents(machine->kernel_filename, (gchar **)&img, &len,
                             NULL)) {
        error_report("sh7269gui: cannot read '%s'", machine->kernel_filename);
        exit(1);
    }

    /* Do what the boot ROM does: scatter the image per its table. */
    if (sh7269_load_image(img, len) == 0) {
        error_report("sh7269gui: load table produced no chunks");
        exit(1);
    }

    /*
     * CDJ_GUI_CS0_FLASH=1: also place the image in CS0 at 0x30000, its flash
     * offset (the first 0x30000 bytes, the boot ROM, are not in the image).
     */
    if (getenv("CDJ_GUI_CS0_FLASH")) {
        cpu_physical_memory_write(0x00030000, img, len);
        info_report("sh7269gui: CS0 flash image 0x%zx bytes at 0x00030000",
                    (size_t)len);
    }

    /*
     * Glyph resource window 0x0DF00000 + 0x57C099: six blobs declared by the
     * resource table at 0x0E598DC8 and pointed at by the font slots at
     * 0x0F682210. Nothing in the load table fills it.
     *
     * CDJ_GUI_FONTFILL=<byte>  fill the whole window with a constant.
     * CDJ_GUI_FONTBLOB=<path>  load real resource bytes at 0x0DF00000.
     */
    {
        const char *fill = getenv("CDJ_GUI_FONTFILL");
        const char *blob = getenv("CDJ_GUI_FONTBLOB");

        if (fill && *fill) {
            size_t span = 0x57C099;
            g_autofree uint8_t *buf = g_malloc(span);

            memset(buf, (int)(strtoul(fill, NULL, 0) & 0xFF), span);
            cpu_physical_memory_write(0x0DF00000, buf, span);
            info_report("sh7269gui: glyph window 0x0DF00000+0x%zx filled with "
                        "0x%02lx", span, strtoul(fill, NULL, 0) & 0xFF);
        }
        if (blob && *blob) {
            uint8_t *bd;
            gsize bl;

            if (!g_file_get_contents(blob, (gchar **)&bd, &bl, NULL)) {
                error_report("sh7269gui: cannot read font blob '%s'", blob);
                exit(1);
            }
            cpu_physical_memory_write(0x0DF00000, bd, bl);
            info_report("sh7269gui: glyph blob '%s' 0x%zx bytes at 0x0DF00000",
                        blob, (size_t)bl);
            g_free(bd);
        }
    }

    /*
     * CDJ_GUI_ARTBLOB=<path>: load the artwork resources at 0x0CD00000.
     * 0x0E517992 (resource lookup, 1435 records of 44 bytes) resolves pixels
     * at 0x0CD00000 + record[+0x20]; no load segment fills it. The archive is
     * at file offset 0x150000 of the unpacked GUI image. Capped at 0x1200000
     * so it cannot reach the glyph window.
     */
    {
        const char *art = getenv("CDJ_GUI_ARTBLOB");

        if (art && *art) {
            uint8_t *ad;
            gsize al;

            if (!g_file_get_contents(art, (gchar **)&ad, &al, NULL)) {
                error_report("sh7269gui: cannot read art blob '%s'", art);
                exit(1);
            }
            if (al > 0x1200000) {
                al = 0x1200000;         /* never touch the glyph window */
            }
            cpu_physical_memory_write(0x0CD00000, ad, al);
            info_report("sh7269gui: art blob '%s' 0x%zx bytes at 0x0CD00000",
                        art, (size_t)al);
            g_free(ad);
        }
    }

    /*
     * Header is [entry][payload start]: word 0 is the entry point (0x0E510790,
     * which starts by configuring the pin function controller), word 1 is
     * 0x000300D0. CDJ_GUI_ENTRY=<addr> overrides the entry.
     */
    entry = ldl_be_p(img);
    {
        const char *e = getenv("CDJ_GUI_ENTRY");

        if (e && *e) {
            entry = (uint32_t)strtoul(e, NULL, 0);
        }
    }

    reset_info = g_new0(Sh7269ResetData, 1);
    reset_info->cpu = cpu;
    reset_info->pc = entry;
    /* Top of SDRAM (not 0x1C..., which is on-chip RAM holding SPI buffers). */
    reset_info->sp = GUI_SDRAM_BASE + GUI_SDRAM_SIZE - 16;
    qemu_register_reset(sh7269_reset, reset_info);

    info_report("sh7269gui: %zu bytes at 0x%08x, entry 0x%08x, sp 0x%08x",
                (size_t)len, GUI_IMAGE_BASE, reset_info->pc, reset_info->sp);
    g_free(img);
}

static void sh7269gui_machine_init(MachineClass *mc)
{
    mc->desc = "Pioneer CDJ-2000NXS2 GUI processor (Renesas SH7269 / SH-2A)";
    mc->init = sh7269gui_init;
    mc->default_cpu_type = TYPE_SH7785_CPU;   /* SH-4A core, built big-endian */
    mc->default_ram_size = 64 * MiB;
    mc->no_floppy = 1;
    mc->no_cdrom = 1;
    mc->no_parallel = 1;
}

DEFINE_MACHINE("sh7269gui", sh7269gui_machine_init)
