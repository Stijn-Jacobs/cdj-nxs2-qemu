/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj.h"
#include "../cdj_getenv.h"
/*
 * Diagnostics for the MAIN->GUI link (DMA1 ch0) and the DSP command link
 * (DMA1 ch4).
 *
 * CDJ_LINKCENSUS=1[:<report_ms>]: count MAIN->GUI frames per (type, length,
 * word count C) from the frame header (C at +0x18, length at +0x1C, type at
 * +0x1E), and print the table every report_ms (default 10000).
 */
#define CDJ_LINKCENSUS_MAX 48

static struct {
    bool parsed, on;
    int64_t report_ms, next_report;
    uint64_t frames, headerless;
    struct { uint16_t len; uint8_t type, c; uint64_t n; } seen[CDJ_LINKCENSUS_MAX];
    unsigned nseen;
} cdj_linkcensus;

/*
 * CDJ_LINKFRAME=<path>[:<at_ms>][:<again_ms>][:<rare>]: write a whole 4 KiB
 * MAIN->GUI frame to <path> at at_ms (default 75000), and a second one
 * again_ms later to <path>.2. With rare=1, instead write every frame after
 * at_ms whose word count C is not 1 (C=1 is the heartbeat) to <path>.cN.M,
 * up to CDJ_LINKFRAME_RARE_MAX.
 */
static struct {
    bool parsed, on, wrote_first, wrote_second;
    char *path;
    int64_t at_ms, again_ms;
    bool rare;
    unsigned nrare;
} cdj_linkframe;

#define CDJ_LINKFRAME_RARE_MAX 12

static void cdj_linkframe_write(uint32_t sar, const char *suffix)
{
    g_autofree char *name = g_strdup_printf("%s%s", cdj_linkframe.path, suffix);
    uint8_t buf[4096];
    FILE *f = fopen(name, "wb");

    if (!f) {
        warn_report("link frame: cannot write %s", name);
        return;
    }
    cpu_physical_memory_read(A7ADDR(sar), buf, sizeof(buf));
    if (fwrite(buf, 1, sizeof(buf), f) != sizeof(buf)) {
        warn_report("link frame: short write to %s", name);
    }
    fclose(f);
    info_report("link frame: wrote %zu bytes of the frame at 0x%08x to %s "
                "(virtual %" PRId64 " ms)", sizeof(buf), A7ADDR(sar), name,
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
}

void cdj_link_frame(unsigned ch, uint32_t sar)
{
    int64_t now;

    if (ch != 0) {
        return;
    }
    if (!cdj_linkframe.parsed) {
        const char *e = getenv("CDJ_LINKFRAME");

        cdj_linkframe.parsed = true;
        if (e && *e) {
            g_auto(GStrv) parts = g_strsplit(e, ":", 4);

            if (parts[0] && *parts[0]) {
                cdj_linkframe.path = g_strdup(parts[0]);
                cdj_linkframe.at_ms = parts[1] ? strtoll(parts[1], NULL, 0)
                                               : 75000;
                cdj_linkframe.again_ms = parts[2] ? strtoll(parts[2], NULL, 0)
                                                  : 0;
                cdj_linkframe.rare = parts[3] && strtoul(parts[3], NULL, 0);
                cdj_linkframe.on = true;
                if (cdj_linkframe.rare) {
                    info_report("link frame: will write every NON-HEARTBEAT "
                                "frame (word count != 1) to %s.cN.M, up to %d",
                                cdj_linkframe.path, CDJ_LINKFRAME_RARE_MAX);
                } else {
                    info_report("link frame: will write one whole frame to %s "
                                "at %" PRId64 " ms%s", cdj_linkframe.path,
                                cdj_linkframe.at_ms,
                                cdj_linkframe.again_ms ? " and a second later"
                                                       : "");
                }
            }
        }
    }
    if (!cdj_linkframe.on) {
        return;
    }
    if (cdj_linkframe.rare) {
        uint8_t c;

        if (cdj_linkframe.nrare >= CDJ_LINKFRAME_RARE_MAX) {
            return;
        }
        /* at_ms gates rare mode too, or boot traffic fills every slot. */
        if (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) < cdj_linkframe.at_ms) {
            return;
        }
        cpu_physical_memory_read(A7ADDR(sar) + 0x18, &c, 1);
        if (c == 1 || !c) {
            return;                      /* the heartbeat, or not a header */
        }
        {
            g_autofree char *suffix =
                g_strdup_printf(".c%u.%u", c, cdj_linkframe.nrare);

            cdj_linkframe.nrare++;
            cdj_linkframe_write(sar, suffix);
        }
        return;
    }
    if (cdj_linkframe.wrote_second) {
        return;
    }
    now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (!cdj_linkframe.wrote_first) {
        if (now < cdj_linkframe.at_ms) {
            return;
        }
        cdj_linkframe.wrote_first = true;
        cdj_linkframe_write(sar, "");
        if (!cdj_linkframe.again_ms) {
            cdj_linkframe.wrote_second = true;
        }
        return;
    }
    if (now < cdj_linkframe.at_ms + cdj_linkframe.again_ms) {
        return;
    }
    cdj_linkframe.wrote_second = true;
    cdj_linkframe_write(sar, ".2");
}

void cdj_link_census(unsigned ch, uint32_t sar)
{
    uint8_t d[0x20];
    uint16_t len;
    uint8_t type, c;
    unsigned i;
    int64_t now;

    if (ch != 0) {
        return;
    }
    if (!cdj_linkcensus.parsed) {
        const char *e = getenv("CDJ_LINKCENSUS");
        char *end;

        cdj_linkcensus.parsed = true;
        if (e && *e && strtoul(e, &end, 0)) {
            cdj_linkcensus.report_ms =
                (*end == ':') ? strtoll(end + 1, &end, 0) : 10000;
            if (cdj_linkcensus.report_ms < 1000) {
                cdj_linkcensus.report_ms = 10000;
            }
            cdj_linkcensus.on = true;
            info_report("link census: counting (type, length) pairs in the "
                        "MAIN->GUI frame header, reporting every %" PRId64 " ms",
                        cdj_linkcensus.report_ms);
        }
    }
    if (!cdj_linkcensus.on) {
        return;
    }

    cpu_physical_memory_read(A7ADDR(sar), d, sizeof(d));
    cdj_linkcensus.frames++;
    /* Header test, as in cdj_linkramp_calibrate(): four equal non-zero bytes
     * below 0x32 at +0x18. */
    if (!d[0x18] || d[0x18] != d[0x19] || d[0x18] != d[0x1A]
        || d[0x18] != d[0x1B] || d[0x18] >= 0x32) {
        cdj_linkcensus.headerless++;
        goto report;
    }
    len = d[0x1C] | (d[0x1D] << 8);
    type = d[0x1E];
    c = d[0x18];
    for (i = 0; i < cdj_linkcensus.nseen; i++) {
        if (cdj_linkcensus.seen[i].len == len
            && cdj_linkcensus.seen[i].type == type
            && cdj_linkcensus.seen[i].c == c) {
            cdj_linkcensus.seen[i].n++;
            goto report;
        }
    }
    if (cdj_linkcensus.nseen < CDJ_LINKCENSUS_MAX) {
        i = cdj_linkcensus.nseen++;
        cdj_linkcensus.seen[i].len = len;
        cdj_linkcensus.seen[i].type = type;
        cdj_linkcensus.seen[i].c = c;
        cdj_linkcensus.seen[i].n = 1;
        info_report("link census: NEW type %u, length %u, C=%u (first seen at "
                    "%" PRId64 " ms)", type, len, c,
                    qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
        /* Log the start of the body; 96 bytes covers the longest C seen. */
        {
            uint8_t body[96];
            char hex[3 * sizeof(body) + 1];
            unsigned k;

            cpu_physical_memory_read(A7ADDR(sar) + 0x18, body, sizeof(body));
            for (k = 0; k < sizeof(body); k++) {
                snprintf(hex + 3 * k, 4, "%02x ", body[k]);
            }
            info_report("link census:   C=%u body %s", c, hex);
        }
    }

report:
    now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (now < cdj_linkcensus.next_report) {
        return;
    }
    cdj_linkcensus.next_report = now + cdj_linkcensus.report_ms;
    info_report("link census at %" PRId64 " ms: %" PRIu64 " frames, %" PRIu64
                " with no message header, %u distinct (type,length,C)",
                now, cdj_linkcensus.frames, cdj_linkcensus.headerless,
                cdj_linkcensus.nseen);
    for (i = 0; i < cdj_linkcensus.nseen; i++) {
        info_report("link census:   type %3u  length %5u  C %3u  x%" PRIu64,
                    cdj_linkcensus.seen[i].type, cdj_linkcensus.seen[i].len,
                    cdj_linkcensus.seen[i].c, cdj_linkcensus.seen[i].n);
    }
}

/*
 * CDJ_CH4_DUMP=1 (or =<channel>): log the payload of every transfer on DMA1
 * ch4 with its timestamp. The channels:
 *
 *   ch0  -> MSIOF1 tx (0xa4c50050)  the 4 KiB MAIN->GUI frame
 *   ch1  <- MSIOF1 rx (0xa4c50060)  its return path
 *   ch4  -> MSIOF0 tx (0xa4c40050)  short DSP command frames
 *   ch5  <- MSIOF0 rx (0xa4c40060)  the DSP's answer
 */
void cdj_ch4_dump(unsigned ch, uint32_t sar, uint32_t count,
                         unsigned unit, int64_t ms)
{
    static int parsed, on, want_ch = 4;
    uint8_t buf[64];
    size_t n;
    unsigned i;
    char line[3 * sizeof(buf) + 1];

    if (!parsed) {
        const char *e = getenv("CDJ_CH4_DUMP");

        parsed = 1;
        if (e && *e) {
            on = 1;
            if (e[0] >= '0' && e[0] <= '9') {
                want_ch = strtoul(e, NULL, 0);
            }
            info_report("ch%u dump: logging every transfer's payload "
                        "-- the command link, not the frame", want_ch);
        }
    }
    if (!on || ch != want_ch) {
        return;
    }
    n = (size_t)count * unit;
    if (n > sizeof(buf)) {
        n = sizeof(buf);
    }
    cpu_physical_memory_read(A7ADDR(sar), buf, n);
    for (i = 0; i < n; i++) {
        snprintf(line + i * 3, 4, "%02x ", buf[i]);
    }
    line[n ? n * 3 - 1 : 0] = 0;
    info_report("CH%u %6" PRId64 " ms  src=0x%08x  %s", ch, ms, sar, line);
}

/*
 * CDJ_CH4_SET=<index>:<value>[,...]: rewrite the value of matching
 * parameters in op2 (write) messages on ch4 just before the DMA sends them.
 * An experiment knob; it patches the message, not the firmware.
 *
 * Message layout: 33 55 magic, opcode at +4 (op2 = write index/value pairs,
 * op4 = read, index only), pairs from +8, ended by 00 00 cc aa.
 */
#define CDJ_CH4_SET_MAX 4

void cdj_ch4_patch(unsigned ch, uint32_t sar, uint32_t count,
                          unsigned unit)
{
    static struct { uint32_t idx, val; } set[CDJ_CH4_SET_MAX];
    static unsigned n_set;
    static int parsed;
    uint8_t hdr[16];
    uint32_t idx, opc;
    unsigned i;

    if (!parsed) {
        const char *e = getenv("CDJ_CH4_SET");

        parsed = 1;
        while (e && *e && n_set < CDJ_CH4_SET_MAX) {
            const char *start = e;

            set[n_set].idx = strtoul(e, (char **)&e, 0);
            set[n_set].val = (*e == ':') ? strtoul(e + 1, (char **)&e, 0) : 0;
            if (e == start) {
                break;
            }
            info_report("ch4 set: op2 writes to index 0x%04x forced to 0x%x "
                        "-- EXPERIMENT, not a fix",
                        set[n_set].idx, set[n_set].val);
            n_set++;
            e += *e == ',';
        }
    }
    if (!n_set || ch != 4 || (uint64_t)count * unit < 16) {
        return;
    }
    cpu_physical_memory_read(A7ADDR(sar), hdr, 8);
    if (hdr[0] != 0x33 || hdr[1] != 0x55) {         /* not a framed message */
        return;
    }
    opc = hdr[4] | ((uint32_t)hdr[5] << 8);
    if (opc != 2) {                                  /* only a WRITE carries a value */
        return;
    }
    /* An op2 message carries a list of (index, value) pairs; walk them all. */
    {
        uint32_t off = 8;
        uint32_t n = (uint32_t)count * unit;

        while (off + 8 <= n && off + 8 <= 256) {
            uint8_t pair[8];

            cpu_physical_memory_read(A7ADDR(sar + off), pair, 8);
            /* the `00 00 cc aa` trailer ends the list */
            if (pair[0] == 0 && pair[1] == 0
                && pair[2] == 0xcc && pair[3] == 0xaa) {
                break;
            }
            idx = pair[0] | ((uint32_t)pair[1] << 8)
                | ((uint32_t)pair[2] << 16) | ((uint32_t)pair[3] << 24);
            for (i = 0; i < n_set; i++) {
                if (set[i].idx != idx) {
                    continue;
                }
                pair[4] = set[i].val & 0xFF;
                pair[5] = (set[i].val >> 8) & 0xFF;
                pair[6] = (set[i].val >> 16) & 0xFF;
                pair[7] = (set[i].val >> 24) & 0xFF;
                cpu_physical_memory_write(A7ADDR(sar + off + 4), pair + 4, 4);
                break;
            }
            off += 8;
        }
    }
}

