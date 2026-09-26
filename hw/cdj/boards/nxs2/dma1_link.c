/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/*
 * DMA1 (0xFDC08000) and the SPI link to the GUI processor.
 *
 * The firmware runs two channels against MSIOF1, 1024 longwords (4 KB) each
 * way, mirroring the GUI's DMAC against its RSPI:
 *
 *   ch0:  SAR=RAM             DAR=0xA4C50050 (SITFDR)  TCR=0x400  CHCR=0x40001810
 *   ch1:  SAR=0xA4C50060 (SIRFDR)  DAR=RAM             TCR=0x400  CHCR=0x40004810
 *
 * The GUI runs in a separate (big-endian) QEMU process, so the link is a
 * chardev socket named "spilink" on both sides. Without it the transfers run
 * as plain memory copies. Channels 4 and 5 carry the DSP link over MSIOF0.
 *
 * CHCR bits used: DE(0) enable, TE(1) transfer end, IE(2) interrupt on TE,
 * TS(4:3) unit size, SM(13:12)/DM(15:14) address mode (0 fixed, 1 increment).
 */
/*
 * DMA1's channels are not evenly spaced: channels 0-3 start at +0x20 in steps
 * of 0x10, DMAOR is at +0x60, and channels 4 and 5 are at +0x70 and +0x80.
 * The DSP driver uses 4 and 5: 0x08325EA8 pushes a command frame into
 * MSIOF0's transmit FIFO and 0x08325EF0 pulls the response out.
 */
static inline unsigned cdj_dma1_chbase(unsigned ch)
{
    return ch < 4 ? CDJ_DMA1_CH0 + ch * 0x10 : 0x70 + (ch - 4) * 0x10;
}


/* Channel owning a register offset, or -1. */
static inline int cdj_dma1_ch_of(hwaddr off)
{
    if (off >= CDJ_DMA1_CH0 && off < CDJ_DMA1_CH0 + 4 * 0x10) {
        return (off - CDJ_DMA1_CH0) / 0x10;
    }
    if (off >= 0x70 && off < 0x70 + 2 * 0x10) {
        return 4 + (off - 0x70) / 0x10;
    }
    return -1;
}

typedef struct {
    CharBackend chr;
    bool present;
    GByteArray *rx;
    uint64_t tx_frames;          /* frames pushed to the GUI over MSIOF1 */
    uint64_t thin_dropped;       /* type-3 frames never written (THIN3) */
    unsigned tx_bucket[60];      /* ... per second of virtual time       */
    /*
     * Virtual-to-wall clock map: the host time at which each virtual second
     * was first touched, so the exit report shows what each virtual second
     * cost. poll_* is stamped from can_receive, which the main loop calls
     * even while the guest is stalled.
     */
    int64_t tx_wall[60];         /* host ms at first TX of that virtual second */
    unsigned poll_bucket[60];    /* can_receive calls per virtual second       */
    int64_t poll_wall[60];       /* host ms at first poll of that second       */
} CdjSpiLink;

/* Host monotonic milliseconds; QEMU_CLOCK_REALTIME does not warp with -icount. */
static int64_t cdj_wall_ms(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME) / 1000000;
}

/* Stamp bucket[sec] on first use; a stall shows as a large gap between two
 * adjacent stamps. */
static void cdj_wall_stamp(int64_t *wall, size_t n, int64_t sec)
{
    if (sec >= 0 && sec < (int64_t)n && !wall[sec]) {
        wall[sec] = cdj_wall_ms();
    }
}

/* Print the real ms each virtual second took: 1000 is real time, 0 means the
 * second was never reached, a large value is a stall. */
static void cdj_wall_report(const char *what, const int64_t *wall, size_t n)
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
    info_report("spilink: %s real ms per virtual 1 s: %s", what, line);
}

static CdjSpiLink cdj_spilink;

/* SPI has no queue: keep only the most recent frames, or each side reads
 * stale data. */
#define CDJ_SPILINK_FRAME   4096
/*
 * Receive window, in 4 KB DMA chunks. MAIN's frame budget is 0x4000 bytes and
 * a large message spans several chunks (the type-0x39 display model is 9900
 * bytes). CDJ_SPILINK_KEEP_FRAMES=<n> sets it (default 8).
 */
#define CDJ_SPILINK_KEEP    (cdj_spilink_keep_frames() * CDJ_SPILINK_FRAME)

static unsigned cdj_spilink_keep_frames(void)
{
    const char *e = getenv("CDJ_SPILINK_KEEP_FRAMES");
    unsigned n = e ? (unsigned)strtoul(e, NULL, 0) : 8;

    return n ? n : 8;
}

/* CDJ_SPILINK_QUEUE=1: complete a transmit the socket would not take yet and
 * hold the remainder in order, instead of stalling the channel. Default off. */
static bool cdj_spilink_queue(void)
{
    const char *e = getenv("CDJ_SPILINK_QUEUE");

    return e && strcmp(e, "0");
}

/*
 * CDJ_SPILINK_QUEUE_MAX: backlog in bytes (default 65536) above which a queued
 * transmit stalls for real. Completing everything instantly lets MAIN outrun
 * the GUI; stalling on every short write wedges the channel.
 */
static unsigned cdj_spilink_queue_max(void)
{
    const char *e = getenv("CDJ_SPILINK_QUEUE_MAX");
    unsigned n = e ? (unsigned)strtoul(e, NULL, 0) : 65536;

    return n ? n : 65536;
}

static int cdj_spilink_can_receive(void *opaque)
{
    /* The main loop calls this every pass whatever the guest is doing, so it
     * is where the virtual-to-wall clock map is taken. */
    int64_t sec = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000000;

    if (sec >= 0 && sec < (int64_t)ARRAY_SIZE(cdj_spilink.poll_bucket)) {
        cdj_spilink.poll_bucket[sec]++;
    }
    cdj_wall_stamp(cdj_spilink.poll_wall, ARRAY_SIZE(cdj_spilink.poll_wall),
                   sec);

    /* Generous on purpose: throttling the reader stalls the peer's vCPU in
     * qemu_chr_fe_write_all(). */
    return 64 * 1024;
}

static void cdj_spilink_receive(void *opaque, const uint8_t *buf, int size)
{
    g_byte_array_append(cdj_spilink.rx, buf, size);

    /* Whole frames only: a partial drop misaligns the stream permanently. */
    while (cdj_spilink.rx->len > CDJ_SPILINK_KEEP) {
        g_byte_array_remove_range(cdj_spilink.rx, 0, CDJ_SPILINK_FRAME);
    }
}

void cdj_spilink_init(void)
{
    Chardev *c = qemu_chr_find("spilink");

    cdj_spilink.rx = g_byte_array_new();
    if (!c) {
        return;
    }
    qemu_chr_fe_init(&cdj_spilink.chr, c, &error_abort);
    qemu_chr_fe_set_handlers(&cdj_spilink.chr, cdj_spilink_can_receive,
                             cdj_spilink_receive, NULL, NULL, NULL, NULL, true);
    cdj_spilink.present = true;
    info_report("cdj2000nxs2: SPI link to GUI attached on chardev 'spilink'");
}

static int64_t cdj_spilink_pace_ns(void)
{
    static int64_t ns = -1;

    if (ns < 0) {
        const char *e = getenv("CDJ_SPILINK_PACE_US");

        ns = e ? (int64_t)strtoll(e, NULL, 0) * 1000 : 0;
    }
    return ns;
}

static const unsigned cdj_dma1_unit[4] = { 1, 2, 4, 16 };

/* TX message-type census, filled in by the wire scanner in cdj_dma1_run(). */
static uint32_t *cdj_tx_type_census;
static int64_t *cdj_tx_type_first, *cdj_tx_type_last;


static void cdj_dma1_retry(void *opaque)
{
    CdjDma1State *s = opaque;
    unsigned ch;
    bool still = false;

    /* Drain the queued tail first, in order. Under CDJ_SPILINK_QUEUE the
     * transfer that produced it has already completed, so only this timer
     * moves it. */
    if (s->txhold && s->txhold->len && !s->txdefer) {
        int w = qemu_chr_fe_write(&cdj_spilink.chr, s->txhold->data,
                                  s->txhold->len);

        if (w > 0) {
            g_byte_array_remove_range(s->txhold, 0, w);
            s->drained += w;
        }
        still |= (s->txhold->len != 0);
    }

    for (ch = 0; ch < CDJ_DMA1_CHANS; ch++) {
        if (s->pending[ch]) {
            cdj_dma1_run(s, ch);
        }
        still |= s->pending[ch];
    }
    if (still) {
        timer_mod(s->retry, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    }
}

/*
 * SPI shifts words out MSB first, so the link is big-endian. MAIN is
 * little-endian and every element is reversed as it crosses the socket; the
 * GUI's own memory order already matches the wire.
 *
 * CDJ_SPILINK_THIN3=<n> (default 0, off): send only one type-3 frame (the
 * 508-byte deck-state heartbeat, most of the link's traffic) in every <n>. The
 * others still complete as transfers but never reach the socket, which is
 * what an unheard SPI frame looks like. Only single-message batches are
 * dropped, whole, so the word and batch alignment stays intact.
 */
static unsigned cdj_spilink_thin3(void)
{
    static int n = -1;

    if (n < 0) {
        const char *e = getenv("CDJ_SPILINK_THIN3");

        n = e ? (int)strtol(e, NULL, 0) : 0;
        if (n < 0) {
            n = 0;
        }
    }
    return (unsigned)n;
}

static void cdj_spilink_swap(uint8_t *buf, uint32_t elems, unsigned unit)
{
    /* Reverse in 32-bit groups: the 16-byte burst unit is four SPI words. */
    unsigned group = unit >= 4 ? 4 : unit;
    size_t total = (size_t)elems * unit;
    static int noswap = -1;

    /* CDJ_SPI_NOSWAP=1 sends raw bytes. */
    if (noswap < 0) {
        noswap = getenv("CDJ_SPI_NOSWAP") != NULL;
    }
    if (noswap || group < 2) {
        return;
    }
    for (size_t off = 0; off + group <= total; off += group) {
        uint8_t *p = buf + off;

        for (unsigned a = 0, b = group - 1; a < b; a++, b--) {
            uint8_t t = p[a];

            p[a] = p[b];
            p[b] = t;
        }
    }
}

void cdj_dma1_run(CdjDma1State *s, unsigned ch)
{
    unsigned base = cdj_dma1_chbase(ch) / 4;
    uint32_t sar = s->reg[base + 0];
    uint32_t dar = s->reg[base + 1];
    uint32_t count = s->reg[base + 2];
    uint32_t chcr = s->reg[base + 3];
    unsigned unit = cdj_dma1_unit[(chcr >> 3) & 3];
    unsigned sm = (chcr >> 12) & 3;
    unsigned dm = (chcr >> 14) & 3;
    uint32_t moved = 0;

    /* A deferred transmit owns the channel until its tail has gone out; only
     * then does the transfer complete and DEI rise. */
    if (s->txdefer && s->txdefer_ch == ch) {
        if (ch < CDJ_DMA1_CHANS) {
            s->defer_hit[ch]++;
        }
        int w = s->txhold->len
            ? qemu_chr_fe_write(&cdj_spilink.chr, s->txhold->data,
                                s->txhold->len)
            : 0;

        if (w > 0) {
            g_byte_array_remove_range(s->txhold, 0, w);
        }
        if (s->txhold->len) {
            /* Still held: retry. A GUI that stops reading keeps this channel
             * wedged, unlike real SPI, which has no flow control. */
            timer_mod(s->retry,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
            return;
        }
        s->txdefer = false;
        s->pending[ch] = false;
        s->reg[base + 0] = s->txdefer_sar;
        s->reg[base + 2] = 0;
        s->reg[base + 3] = (chcr & ~1u) | 2u;
        if ((chcr & 4) && s->dei[ch]) {
            s->dei_raised[ch]++;
            qemu_set_irq(s->dei[ch], 1);
        }
        return;
    }

    /* TE must be clear for a transfer to start. */
    if (!(chcr & 1) || (chcr & 2) || !(s->reg[CDJ_DMA1_DMAOR / 4] & 1)) {
        if (ch < CDJ_DMA1_CHANS) {
            if (!(chcr & 1)) {
                s->no_de[ch]++;
            } else if (chcr & 2) {
                s->no_te[ch]++;
            } else {
                s->no_dmaor[ch]++;
            }
        }
        return;
    }

    if (ch < CDJ_DMA1_CHANS) {
        int64_t ms = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
        unsigned b = (unsigned)(ms / 1000);

        /* ch0 is the link transmitter, so its SAR is MAIN's deck-state frame
         * buffer. Print it once. */
        if (!s->sar_reported[ch]) {
            s->sar_reported[ch] = true;
            info_report("dma1: ch%u first run: SAR=0x%08x DAR=0x%08x count=%u"
                        "%s", ch, sar, dar, count,
                        ch == 0 ? "   <-- the MAIN->GUI frame buffer" : "");
        }
        /* Report when the content of ch0's outgoing frame changes
         * (deduplicated on first non-zero offset and count). */
        if (ch == 0 && count && unit) {
            uint32_t bytes = count * unit;
            g_autofree uint8_t *fr = g_malloc(bytes);
            uint32_t i, nz = 0, first = 0xFFFFFFFF;

            /* sar is a P1/P2 virtual address; cpu_physical_memory_read needs
             * the physical one, or it silently reads zeros. */
            cpu_physical_memory_read(A7ADDR(sar), fr, bytes);
            for (i = 0; i < bytes; i++) {
                if (fr[i]) {
                    if (first == 0xFFFFFFFF) {
                        first = i;
                    }
                    nz++;
                }
            }
            {
                static uint32_t last_first = 0xFFFFFFFE, last_nz;
                static unsigned empty_runs;

                if (!nz) {
                    empty_runs++;
                } else if (first != last_first || nz != last_nz) {
                    last_first = first;
                    last_nz = nz;
                    info_report("dma1: ch0 TX PAYLOAD first non-zero at "
                                "+0x%03x, %u of %u bytes non-zero "
                                "(after %u all-zero frames)",
                                first, nz, bytes, empty_runs);
                }
            }
        }
        /*
         * ch3's first run each boot is SAR=0xa80034e0 DAR=0xb8000000
         * count=8192: MAIN pushing the DSP program from its own image into CS6
         * (the DSP's window); the DSP has no program storage of its own. Later
         * the same channel carries audio.
         *
         * CDJ_DMA1_COPY=1 actually performs the transfers (they are otherwise
         * only inspected). Pair it with CDJ_AREA6=1 or ch3's bytes land in an
         * unmapped window.
         */
        if (cdj_c6x_on() && ch == 3 && A7ADDR(dar) == CDJ_AREA6_PHYS) {
            cdj_c6x_upp_ship(sar, count * unit);
        }
        /* Audio tap, read-only, before anything is moved. */
        cdj_audio_out(ch, sar, count * unit);
        if (getenv("CDJ_DMA1_COPY")) {
            uint32_t n = count * unit;

            if (n && n <= 1 * MiB) {
                g_autofree uint8_t *buf = g_malloc(n);

                cpu_physical_memory_read(A7ADDR(sar), buf, n);
                cpu_physical_memory_write(A7ADDR(dar), buf, n);
                /* CDJ_CAPTURE: ch3's destination is a fixed port, so the image
                 * only exists as a stream; <prefix>-dma1ch<n>.bin. */
                {
                    char suffix[32];

                    snprintf(suffix, sizeof(suffix), "dma1ch%u", ch);
                    cdj_capture_write(&s->cap[ch], suffix, buf, n);
                }
                if (!s->copy_reported[ch]) {
                    s->copy_reported[ch] = true;
                    info_report("dma1: ch%u COPY %u bytes 0x%08x -> 0x%08x",
                                ch, n, A7ADDR(sar), A7ADDR(dar));
                }
            }
        }
        s->ran[ch]++;
        s->last_run_ms[ch] = ms;
        if (b < ARRAY_SIZE(s->run_bucket[ch])) {
            s->run_bucket[ch][b]++;
        }
        cdj_audio_queue_ship(ch);
        cdj_dsp_engine_ship(ch, sar, count * unit);
        /* The census runs before the ramp so it counts what MAIN sent. */
        cdj_link_census(ch, sar);
        cdj_link_frame(ch, sar);
        cdj_link_ramp(ch, sar);
        cdj_beat_drive(ch, sar);
        cdj_pitch_drive(ch, sar);
        cdj_stateout_frame(ch, sar);
        cdj_ch4_patch(ch, sar, count, unit);
        cdj_ch4_dump(ch, sar, count, unit, ms);
    }

    /*
     * MSIOF0, the DSP link. Both directions complete synchronously: transmit
     * hands the frame to the peer, receive has the peer build an answer.
     */
    /* With CDJ_C6X the emulated DSP owns MSIOF0, see cdj_c6x_spi_xfer. */
    if (cdj_c6x_on() && dar == CDJ_MSIOF0_SITFDR && dm == 0 && unit == 2) {
        uint32_t nsar = sar;

        if (count && !cdj_c6x_link_tx(s, ch, sar, sm, count, &nsar)) {
            s->pending[ch] = true;
            return;
        }
        sar = nsar;
        moved = count;
        goto done;
    }
    if (cdj_c6x_on() && sar == CDJ_MSIOF0_SIRFDR && sm == 0 && unit == 2) {
        uint32_t ndar = dar;

        if (count && !cdj_c6x_link_rx(s, ch, dar, dm, count, &ndar)) {
            s->pending[ch] = true;
            return;
        }
        dar = ndar;
        moved = count;
        goto done;
    }

    if (cdj_dsp_present() && dar == CDJ_MSIOF0_SITFDR && dm == 0 && unit == 2) {
        g_autofree uint8_t *out = g_malloc((size_t)count * unit);

        while (count--) {
            cpu_physical_memory_read(A7ADDR(sar), out + moved * unit, unit);
            if (sm == 1) {
                sar += unit;
            }
            moved++;
        }
        cdj_dsp_take(out, moved);
        goto done;
    }

    if (cdj_dsp_present() && sar == CDJ_MSIOF0_SIRFDR && sm == 0 && unit == 2) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        g_autofree uint8_t *in = NULL;

        /*
         * Rate limit. A peer that answers instantly is an infinitely fast DSP
         * and floods the guest; the wait models the frame's time on the wire.
         */
        if (now < cdj_dsp_ready_at()) {
            cdj_dsp_defer();
            s->pending[ch] = true;
            timer_mod(s->retry, cdj_dsp_ready_at());
            return;
        }
        cdj_dsp_sent(now);
        in = g_malloc((size_t)count * unit);
        cdj_dsp_fill(in, count);
        while (count--) {
            cpu_physical_memory_write(A7ADDR(dar), in + moved * unit, unit);
            if (dm == 1) {
                dar += unit;
            }
            moved++;
        }
        goto done;
    }

    if (cdj_spilink.present && dar == CDJ_MSIOF1_SITFDR && dm == 0) {
        g_autofree uint8_t *out = g_malloc((size_t)count * unit);

        /* Count transmits to MSIOF1 itself (the link by definition), per
         * second. */
        {
            int64_t sec = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000000;

            cdj_spilink.tx_frames++;
            if (sec >= 0 && sec < (int64_t)ARRAY_SIZE(cdj_spilink.tx_bucket)) {
                cdj_spilink.tx_bucket[sec]++;
            }
            cdj_wall_stamp(cdj_spilink.tx_wall,
                           ARRAY_SIZE(cdj_spilink.tx_wall), sec);
        }

        while (count--) {
            cpu_physical_memory_read(A7ADDR(sar), out + moved * unit, unit);
            if (sm == 1) {
                sar += unit;
            }
            moved++;
        }
        cdj_spilink_swap(out, moved, unit);
        /* Log changes in what goes on the wire, after the endian swap
         * (deduplicated on first non-zero offset and count). */
        {
            size_t wb = (size_t)moved * unit;
            size_t i, nz = 0, first = (size_t)-1;

            for (i = 0; i < wb; i++) {
                if (out[i]) {
                    if (first == (size_t)-1) {
                        first = i;
                    }
                    nz++;
                }
            }
            if (nz) {
                static size_t last_first = (size_t)-2, last_nz;

                if (first != last_first || nz != last_nz) {
                    last_first = first;
                    last_nz = nz;
                    info_report("spilink: TX WIRE first non-zero at +0x%03x, "
                                "%zu of %zu bytes non-zero",
                                (unsigned)first, nz, wb);
                }
            }
            /*
             * CDJ_LINKWATCH=<off>[:<width>]: log the frame field at <off>
             * (width 1, 2 or 4, default 4) whenever its value on the wire
             * changes, with the virtual time and a frame count. Useful because
             * the frame is block-copied, so there is no single store to watch.
             */
            {
                static int lw_off = -2;
                static unsigned lw_width = 4;
                static uint32_t lw_last;
                static bool lw_seen;
                static uint64_t lw_frames;

                if (lw_off == -2) {
                    const char *e = getenv("CDJ_LINKWATCH");
                    char *end;

                    lw_off = -1;
                    if (e && *e) {
                        lw_off = (int)strtol(e, &end, 0);
                        if (*end == ':') {
                            lw_width = strtoul(end + 1, &end, 0);
                        }
                        if (lw_width != 1 && lw_width != 2 && lw_width != 4) {
                            lw_width = 4;
                        }
                        info_report("link watch: frame+0x%x width %u -- "
                                    "logging every CHANGE on the wire",
                                    lw_off, lw_width);
                    }
                }
                if (lw_off >= 0 && (unsigned)lw_off + lw_width <= wb) {
                    uint32_t v = 0;
                    unsigned q;

                    for (q = 0; q < lw_width; q++) {
                        v = (v << 8) | out[lw_off + q];
                    }
                    lw_frames++;
                    if (!lw_seen || v != lw_last) {
                        lw_seen = true;
                        lw_last = v;
                        info_report("link watch: frame+0x%x = 0x%08x (%d) at "
                                    "%" PRId64 " ms, frame %" PRIu64,
                                    lw_off, v, (int32_t)v,
                                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                                        / 1000000,
                                    lw_frames);
                    }
                }
            }
            if (wb >= 0xB0) {
                static uint8_t last[16];
                static bool seen;

                if (!seen || memcmp(last, out + 0xA0, 16)) {
                    char hex[3 * 16 + 1];
                    unsigned k;

                    seen = true;
                    memcpy(last, out + 0xA0, 16);
                    for (k = 0; k < 16; k++) {
                        snprintf(hex + 3 * k, 4, "%02x ", last[k]);
                    }
                    info_report("spilink: TX WIRE +0xA0: %s", hex);
                }
            }
            if (wb >= 0x20) {
                static uint32_t type_n[256];
                static int64_t type_first[256], type_last[256];
                static bool type_reg;
                int64_t nowms = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
                uint8_t magic = out[0x18], ty = out[0x1D];

                /* Per-type message census. Only frames whose magic is a real
                 * preamble count, so mid-stream bytes are not mistaken for a
                 * header. */
                if (magic && magic == out[0x19] && magic == out[0x1a]
                    && magic == out[0x1b] && magic < 0x32) {
                    /*
                     * The magic byte is the message count; walk every message
                     * in the batch. From the message base: +5 type, +6/+7
                     * big-endian payload length; the message is len/4 + 5
                     * words (+1 for a ragged tail) including magic, header,
                     * checksum and terminator.
                     */
                    unsigned m, mb = 0x18;

                    for (m = 0; m < magic && mb + 8 <= wb; m++) {
                        unsigned mlen = ((unsigned)out[mb + 6] << 8)
                                      | (unsigned)out[mb + 7];
                        unsigned mc = mlen / 4 + 5 + (mlen % 4 ? 1 : 0);
                        uint8_t mty = out[mb + 5];

                        if (!mlen || mlen > 0x4000) {
                            break;
                        }
                        if (!type_n[mty]) {
                            type_first[mty] = nowms;
                        }
                        type_n[mty]++;
                        type_last[mty] = nowms;
                        mb += 4 * mc;
                    }
                    (void)ty;
                }
                if (!type_reg) {
                    type_reg = true;
                    cdj_tx_type_census = type_n;
                    cdj_tx_type_first = type_first;
                    cdj_tx_type_last = type_last;
                }
            }
        }
        /*
         * CDJ_SPILINK_DUMP=<path>: append every byte put on the wire (after
         * the endian swap) to a file.
         */
        {
            /* The handle stays open; fopen/fclose per transmit is slow enough
             * to change timing. CDJ_SPILINK_DUMP_SYNC=1 does it anyway. */
            static FILE *dumpfp;
            static int dumpsync = -1;
            const char *path = getenv("CDJ_SPILINK_DUMP");

            if (dumpsync < 0) {
                dumpsync = getenv("CDJ_SPILINK_DUMP_SYNC") ? 1 : 0;
            }
            if (path && dumpsync) {
                FILE *fp = fopen(path, "ab");

                if (fp) {
                    fwrite(out, 1, moved * unit, fp);
                    fclose(fp);
                }
            } else if (path) {
                if (!dumpfp) {
                    dumpfp = fopen(path, "ab");
                }
                if (dumpfp) {
                    fwrite(out, 1, moved * unit, dumpfp);
                }
            }
        }
        /* The heartbeat thinner, after the census and dump so both still see
         * every frame. */
        if (cdj_spilink_thin3() && (size_t)moved * unit >= 0x20) {
            uint8_t magic = out[0x18], ty = out[0x1D];

            if (magic == 1 && magic == out[0x19] && magic == out[0x1a]
                && magic == out[0x1b] && ty == 3) {
                static uint64_t seen;

                if (seen++ % cdj_spilink_thin3()) {
                    cdj_spilink.thin_dropped++;
                    goto done;          /* completes: TE set, DEI raised */
                }
            }
        }

        /*
         * Backpressure. A non-blocking write reports what the socket took; by
         * default the rest is held, DE stays set and the retry timer flushes
         * it. With CDJ_SPILINK_QUEUE the remainder is queued in order and the
         * transfer completes (the socket is transport, not the wire); later
         * frames append behind it. A partial drop is never an option: the GUI
         * reads fixed-size frames and would lose alignment.
         */
        if (cdj_spilink_queue() && s->txhold && s->txhold->len) {
            g_byte_array_append(s->txhold, out, moved * unit);
            timer_mod(s->retry,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
            if (s->txhold->len > cdj_spilink_queue_max()) {
                /* Backlog is deep: stall this one. The bytes are already at
                 * the tail of the queue. */
                if (!s->txdefer) {
                    s->txdefer = true;
                    s->txdefer_ch = ch;
                    s->txdefer_sar = sar;
                    s->defer_since_ms =
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
                    s->defer_start[ch]++;
                }
                s->pending[ch] = true;
                return;                 /* DE stays set, no TE, no DEI */
            }
            s->queued[ch]++;
            goto done;
        }

        if (cdj_spilink_pace_ns()) {
            /*
             * CDJ_SPILINK_PACE_US: block the vCPU thread for real time inside
             * the transfer. The GUI is a separate process and needs wall-clock
             * time to drain; virtual-time pacing or deferring the transfer does
             * not give it any.
             */
            g_usleep(cdj_spilink_pace_ns() / 1000);
            s->paced[ch]++;
        }

        {
            int wrote = qemu_chr_fe_write(&cdj_spilink.chr, out, moved * unit);

            if (wrote < 0) {
                wrote = 0;
            }
            if (wrote < (int)(moved * unit) && cdj_spilink_queue()) {
                if (!s->txhold) {
                    s->txhold = g_byte_array_new();
                }
                g_byte_array_append(s->txhold, out + wrote,
                                    moved * unit - wrote);
                s->queued[ch]++;
                timer_mod(s->retry,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
                goto done;              /* completes: TE set, DEI raised */
            }
            if (wrote < (int)(moved * unit)) {
                if (!s->txhold) {
                    s->txhold = g_byte_array_new();
                }
                g_byte_array_append(s->txhold, out + wrote,
                                    moved * unit - wrote);
                if (ch < CDJ_DMA1_CHANS && !s->txdefer) {
                    s->defer_start[ch]++;
                    s->defer_since_ms =
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
                }
                s->txdefer = true;
                s->txdefer_ch = ch;
                s->txdefer_sar = sar;
                s->pending[ch] = true;
                timer_mod(s->retry,
                          qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
                return;                       /* DE stays set, no TE, no DEI */
            }
        }
        goto done;
    }

    /* Receive completes only once the GUI has sent the bytes; until then DE
     * stays set and the retry timer tries again. */
    if (cdj_spilink.present && sar == CDJ_MSIOF1_SIRFDR && sm == 0) {
        uint32_t need = count * unit;

        if (cdj_spilink.rx->len < need) {
            if (getenv("CDJ_DMA1_DEBUG") && !s->pending[ch]) {
                info_report("dma1: ch%u rx deferred at %" PRId64
                            " ms, have %u of %u bytes", ch,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000,
                            cdj_spilink.rx->len, need);
            }
            s->pending[ch] = true;
            timer_mod(s->retry,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
            return;
        }
        while (count--) {
            uint8_t elem[16];

            memcpy(elem, cdj_spilink.rx->data + moved * unit, unit);
            cdj_spilink_swap(elem, 1, unit);
            cpu_physical_memory_write(A7ADDR(dar), elem, unit);
            if (dm == 1) {
                dar += unit;
            }
            moved++;
        }
        g_byte_array_remove_range(cdj_spilink.rx, 0, need);
        s->pending[ch] = false;
        goto done;
    }

    while (count--) {
        uint8_t buf[16];

        cpu_physical_memory_read(A7ADDR(sar), buf, unit);
        cpu_physical_memory_write(A7ADDR(dar), buf, unit);
        if (sm == 1) {
            sar += unit;
        }
        if (dm == 1) {
            dar += unit;
        }
        moved++;
    }

done:
    s->reg[base + 0] = sar;
    s->reg[base + 1] = dar;
    s->reg[base + 2] = 0;
    s->reg[base + 3] = (chcr & ~1u) | 2u;       /* DE clear, TE set */
    s->pending[ch] = false;

    /* DEI wakes the SPI1_DMA_END task. It is a level: it stands while TE
     * stands and the guest clears TE to acknowledge. A re-arm with TCR still
     * zero moves nothing and must not raise DEI, or it becomes an interrupt
     * storm that starves the 1 kHz tick. */
    if (moved && (chcr & 4) && ch < CDJ_DMA1_CHANS && s->dei[ch]) {
        s->dei_raised[ch]++;
        qemu_set_irq(s->dei[ch], 1);
    } else if (!moved && ch < CDJ_DMA1_CHANS) {
        s->moved0[ch]++;
    }

    if (getenv("CDJ_DMA1_DEBUG")) {
        info_report("dma1: ch%u moved %u x %u bytes at %" PRId64 " ms",
                    ch, moved, unit,
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000);

        if (moved) {
            bool rx = (sm == 0 && s->reg[base + 0] == CDJ_MSIOF1_SIRFDR);
            uint32_t at = rx
                ? (dm == 1 ? dar - moved * unit : dar)
                : (sm == 1 ? sar - moved * unit : sar);
            char line[3 * 32 + 1];
            uint8_t peek[32];
            unsigned i;

            cpu_physical_memory_read(A7ADDR(at), peek, sizeof(peek));
            for (i = 0; i < sizeof(peek); i++) {
                snprintf(line + i * 3, 4, "%02x ", peek[i]);
            }
            info_report("dma1: ch%u %s head @0x%08x: %s",
                        ch, rx ? "rx" : "tx", at, line);
        }
    }
}

static uint64_t cdj_dma1_read(void *opaque, hwaddr off, unsigned size)
{
    CdjDma1State *s = opaque;
    return s->reg[off / 4];
}

static void cdj_dma1_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    CdjDma1State *s = opaque;
    uint32_t was = s->reg[off / 4];

    s->reg[off / 4] = val;

    int wch = ((off & 0x0F) == 0x0C) ? cdj_dma1_ch_of(off) : -1;

    /* Acknowledgement: a write that clears a TE which was actually set. */
    if (wch >= 0 && (was & 0x02) && !(val & 0x02) && s->dei[wch]) {
        qemu_set_irq(s->dei[wch], 0);
    }

    /* CDJ_DMA1_IEACK=1: also dismiss DEI when the guest clears IE. On hardware
     * DEI needs both TE and IE, and the DSP module's DEI handler at 0x08326370
     * acknowledges by clearing IE in CHCR (0xFDC0805C), never TE, which
     * otherwise leaves the level stuck. Opt-in. */
    if (getenv("CDJ_DMA1_IEACK") && wch >= 0) {
        bool had = (was & 0x02) && (was & 0x04);
        bool want = (val & 0x02) && (val & 0x04);

        if (had && !want && s->dei[wch]) {
            qemu_set_irq(s->dei[wch], 0);
        }
    }

    if (getenv("CDJ_DMA1_TRACE")) {
        info_report("dma1: write +0x%03x size %u = 0x%08x",
                    (unsigned)off, size, (uint32_t)val);
    }

    if (wch >= 0 && wch < CDJ_DMA1_CHANS && (val & 0x01)) {
        s->last_arm_ms[wch] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000000;
        if (val & 0x02) {
            s->armed_te[wch]++;
        } else {
            s->armed[wch]++;
        }
    }

    if (wch >= 0 && (val & 0x01) && !(val & 0x02)) {
        cdj_dma1_run(s, wch);
    }
}

static const MemoryRegionOps cdj_dma1_ops = {
    .read = cdj_dma1_read,
    .write = cdj_dma1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_dma1_dump(Notifier *n, void *unused)
{
    CdjDma1State *s = container_of(n, CdjDma1State, exit);
    unsigned i;

    if (cdj_tx_type_census) {
        unsigned t;

        for (t = 0; t < 256; t++) {
            if (cdj_tx_type_census[t]) {
                info_report("spilink: TX message type %u sent %u times, "
                            "first at %" PRId64 " ms, last at %" PRId64 " ms",
                            t, cdj_tx_type_census[t], cdj_tx_type_first[t],
                            cdj_tx_type_last[t]);
            }
        }
    }
    if (cdj_spilink.tx_frames) {
        char tl[60 * 5 + 1];
        int p = 0;

        for (i = 0; i < ARRAY_SIZE(cdj_spilink.tx_bucket); i++) {
            p += snprintf(tl + p, sizeof(tl) - p, "%u ",
                          cdj_spilink.tx_bucket[i]);
        }
        info_report("spilink: TX to GUI frames=%" PRIu64 " thin3-dropped=%"
                    PRIu64, cdj_spilink.tx_frames, cdj_spilink.thin_dropped);
        info_report("spilink: TX per 1 s: %s", tl);

        p = 0;
        for (i = 0; i < ARRAY_SIZE(cdj_spilink.poll_bucket); i++) {
            p += snprintf(tl + p, sizeof(tl) - p, "%u ",
                          cdj_spilink.poll_bucket[i] > 9999
                          ? 9999 : cdj_spilink.poll_bucket[i]);
        }
        info_report("spilink: main-loop polls per 1 s: %s", tl);
        cdj_wall_report("TX", cdj_spilink.tx_wall,
                        ARRAY_SIZE(cdj_spilink.tx_wall));
        cdj_wall_report("poll", cdj_spilink.poll_wall,
                        ARRAY_SIZE(cdj_spilink.poll_wall));
    }

    for (i = 0; i < CDJ_DMA1_CHANS; i++) {
        if (s->dei_raised[i]) {
            info_report("dma1: ch%u DEI raised=%u", i, s->dei_raised[i]);
        }
        if (s->armed[i] || s->armed_te[i]) {
            info_report("dma1: ch%u armed=%u armed_with_TE=%u ran=%u moved0=%u"
                        " refused(de=%u te=%u dmaor=%u)"
                        " last_arm=%" PRId64 " ms last_run=%" PRId64 " ms",
                        i, s->armed[i], s->armed_te[i], s->ran[i],
                        s->moved0[i], s->no_de[i], s->no_te[i], s->no_dmaor[i],
                        s->last_arm_ms[i], s->last_run_ms[i]);
        }
        if (s->paced[i]) {
            info_report("dma1: ch%u paced=%u transfers held by"
                        " CDJ_SPILINK_PACE_US", i, s->paced[i]);
        }
        if (s->ran[i]) {
            char line[60 * 8 + 1];
            unsigned k, n = 0;

            for (k = 0; k < ARRAY_SIZE(s->run_bucket[i]); k++) {
                n += snprintf(line + n, sizeof(line) - n, "%u ",
                              s->run_bucket[i][k]);
            }
            info_report("dma1: ch%u runs per 1 s: %s", i, line);
        }
        if (s->defer_hit[i] || s->defer_start[i]) {
            info_report("dma1: ch%u txdefer: started=%u swallowed_arms=%u"
                        " dropped=%u (%u bytes) queued=%u",
                        i, s->defer_start[i], s->defer_hit[i],
                        s->defer_dropped[i], s->defer_dropped_bytes[i],
                        s->queued[i]);
        }
    }

    if (s->txdefer) {
        info_report("dma1: STUCK in txdefer on ch%u since %" PRId64
                    " ms, %u bytes still unsent",
                    s->txdefer_ch, s->defer_since_ms,
                    s->txhold ? s->txhold->len : 0);
    }
    if (s->drained || (s->txhold && s->txhold->len)) {
        info_report("dma1: spilink queue: drained=%" PRIu64 " bytes,"
                    " %u still held", s->drained,
                    s->txhold ? s->txhold->len : 0);
    }
}

/* dei carries CDJ_DMA1_CHANS lines -- DMAC1A DEI0..3 then DMAC1B DEI4/DEI5 --
 * with NULL entries for channels that should raise nothing. */
void cdj_dma1_init(MemoryRegion *sysmem, qemu_irq *dei)
{
    CdjDma1State *s = g_new0(CdjDma1State, 1);
    unsigned i;
    const char *off = getenv("CDJ_DMA1_DEI");

    for (i = 0; i < CDJ_DMA1_CHANS && dei; i++) {
        s->dei[i] = (off && !strcmp(off, "0")) ? NULL : dei[i];
    }
    s->exit.notify = cdj_dma1_dump;
    qemu_add_exit_notifier(&s->exit);
    s->retry = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_dma1_retry, s);
    cdj_dma1_singleton = s;
    memory_region_init_io(&s->iomem, NULL, &cdj_dma1_ops, s,
                          "sh7724.dma1", CDJ_DMA1_SIZE);
    memory_region_add_subregion(sysmem, CDJ_DMA1_BASE, &s->iomem);
}

