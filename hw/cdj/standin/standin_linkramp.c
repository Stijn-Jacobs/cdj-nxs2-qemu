/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj.h"
#include "../cdj_getenv.h"
/*
 * Stand-in display drivers on the MAIN -> GUI link (DMA0 ch0), from before the
 * real DSP was emulated. All off by default. Each patches the deck-state frame
 * at ch0's SAR just before the DMA reads it and then repairs the checksum;
 * MAIN's own memory is untouched.
 *
 *   CDJ_LINKRAMP   ramp or hold arbitrary frame words (a search tool)
 *   CDJ_BEATDRIVE  beat indicator at frame+0xC0 and bars countdown at +0xE4
 *   CDJ_BEATGRID   take the beat number from the firmware's beat grid in RAM
 *   CDJ_PITCHDRIVE tempo percentage at frame+0x88 and BPM at +0x8C
 *
 * The link byte-reverses every 32-bit word, so a value meant as a number must
 * be written little-endian (MAIN's struct at 0x0B568C9C is little-endian).
 *
 *     CDJ_LINKRAMP=<spec>[,<spec>...]   up to 8 specs, each
 *     <off>:<start>:<step>[:<width>][:<period>][:<count>][:<start_ms>][:<wrap>][:<order>]
 *
 *   width    1/2/4 bytes, default 4
 *   period   frames per step, default 50; step 0 holds the value
 *   count    consecutive fields patched, default 1
 *   start_ms virtual-clock time to start; patching from boot wedges the load
 *   wrap     cycle start..start+wrap-1 instead of climbing, 0 = no wrap
 *   order    0 = big-endian (GUI sees it reversed), 1 = little-endian
 *
 * e.g. CDJ_LINKRAMP=0xE0:1:0:4:1:1:70000:0,0x24:0:1:4:1:32:70000:8 holds the
 * beat-phase meter enable at frame+0xE0 while ramping 32 words.
 * CDJ_LINKRAMP_NOCK=1 skips the checksum repair.
 */
#define CDJ_LINKRAMP_MAX 8

typedef struct {
    uint32_t off, val, base, step, width, period, count, wrap, order;
    int64_t start_ms;
    uint64_t n;
} CdjLinkRamp;

static struct {
    bool parsed, on;
    unsigned n;
    CdjLinkRamp t[CDJ_LINKRAMP_MAX];
} cdj_linkramp;

/*
 * The GUI checks each message before any handler sees it; the last check
 * (0x0E500FC0) is a checksum, so a patched frame without a repaired checksum
 * is silently dropped.
 *
 * Frame layout on the link:
 *   +0x18  preamble, four identical bytes < 0x32
 *   +0x1C  len (16-bit little-endian), +0x1E type, +0x1F sequence
 *   +0x24  payload
 *   C = len/4 + 5 (+1 if len is not a multiple of 4) words from +0x18;
 *   word[C-2] is the checksum, word[C-1] the 0xFFFFFFFF terminator.
 * 0xFFFFFFFF also occurs in the payload, so the terminator cannot be scanned for.
 */
/* One-shot dump of the DMA source on the first patched frame. */
static void cdj_linkramp_probe(uint32_t sar)
{
    static bool done;
    uint32_t base = A7ADDR(sar);
    uint8_t d[0x100];
    char line[3 * 0x40 + 1];
    uint32_t i, term = 0;

    if (done) {
        return;
    }
    done = true;
    cpu_physical_memory_read(base, d, sizeof(d));

    for (i = 0; i < 0x40; i++) {
        snprintf(line + 3 * i, 4, "%02x ", d[i]);
    }
    info_report("link ramp PROBE: sar=%08x phys=%08x", sar, base);
    info_report("link ramp PROBE: +0x00..0x3f  %s", line);

    for (i = 0; i + 3 < 0x100; i += 4) {
        if (d[i] && d[i] == d[i + 1] && d[i] == d[i + 2] && d[i] == d[i + 3]
            && d[i] < 0x32) {
            info_report("link ramp PROBE: preamble-shaped word at +0x%02x "
                        "(byte 0x%02x), hdr would be +0x%02x type=0x%02x "
                        "len=%u", i, d[i], i + 4, d[i + 5],
                        ((unsigned)d[i + 6] << 8) | d[i + 7]);
        }
    }
    for (i = 0; i + 3 < 0x100; i += 4) {
        if (d[i] == 0xFF && d[i + 1] == 0xFF && d[i + 2] == 0xFF
            && d[i + 3] == 0xFF) {
            term = i;
            break;
        }
    }
    info_report("link ramp PROBE: first 0xFFFFFFFF in the first 0x100 bytes: %s",
                term ? "see offset below" : "NONE");
    if (term) {
        info_report("link ramp PROBE: ... at +0x%02x -- NOTE 0xFFFFFFFF occurs "
                    "as PAYLOAD DATA, so scanning for it finds the wrong end",
                    term);
    }
    /* Decode the header and dump the tail where the checksum should be. */
    {
        uint32_t len2 = (uint32_t)d[0x1C] | ((uint32_t)d[0x1D] << 8);
        uint32_t ty2 = d[0x1E], seq2 = d[0x1F];
        uint32_t c2 = len2 / 4 + 4 + (len2 % 4 ? 1 : 0);
        uint8_t tail[0x20];
        char tl[3 * 0x20 + 1];
        uint32_t off = 0x18 + 4 * (c2 - 4), k;

        info_report("link ramp PROBE: decoded len=%u type=%u seq=%u -> C=%u, "
                    "checksum should be +0x%x, terminator +0x%x",
                    len2, ty2, seq2, c2, 0x18 + 4 * (c2 - 2),
                    0x18 + 4 * (c2 - 1));
        cpu_physical_memory_read(base + off, tail, sizeof(tail));
        for (k = 0; k < sizeof(tail); k++) {
            snprintf(tl + 3 * k, 4, "%02x ", tail[k]);
        }
        info_report("link ramp PROBE: +0x%x..  %s", off, tl);
    }
}

/*
 * Checksum calibration: on an unpatched frame, sum words [0..C-3] and
 * [1..C-3] in both byte orders and latch whichever reproduces the checksum
 * MAIN stored. If none does, the repair stays off.
 */
static int cdj_linkramp_ckstart = -1;   /* 0 or 1; -2 = calibration failed */
static int cdj_linkramp_ckbe = 1;       /* 1 = big-endian words, 0 = little */

static uint32_t cdj_linkramp_c(const uint8_t *d)
{
    uint32_t len = (uint32_t)d[0x1C] | ((uint32_t)d[0x1D] << 8);

    if (!len || len > 0x4000) {
        return 0;
    }
    return len / 4 + 5 + (len % 4 ? 1 : 0);
}

static uint32_t cdj_linkramp_word(uint32_t base, uint32_t i, int be)
{
    uint8_t w[4];

    cpu_physical_memory_read(base + 4 * i, w, 4);
    if (be) {
        return ((uint32_t)w[0] << 24) | ((uint32_t)w[1] << 16)
             | ((uint32_t)w[2] << 8) | (uint32_t)w[3];
    }
    return ((uint32_t)w[3] << 24) | ((uint32_t)w[2] << 16)
         | ((uint32_t)w[1] << 8) | (uint32_t)w[0];
}

static void cdj_linkramp_calibrate(uint32_t sar)
{
    static uint32_t tries;
    uint32_t phys = A7ADDR(sar), base = phys + 0x18;
    uint32_t c, i, stored, sum, be, st;
    uint8_t d[0x20];

    if (cdj_linkramp_ckstart != -1) {
        return;
    }
    cpu_physical_memory_read(phys, d, sizeof(d));
    if (!d[0x18] || d[0x18] != d[0x19] || d[0x18] != d[0x1A]
        || d[0x18] != d[0x1B] || d[0x18] >= 0x32) {
        return;                                  /* not a header; try the next */
    }
    c = cdj_linkramp_c(d);
    if (c < 6) {
        return;
    }
    for (be = 0; be < 2; be++) {
        stored = cdj_linkramp_word(base, c - 2, be);
        if (!stored) {
            continue;
        }
        for (st = 0; st < 2; st++) {
            sum = 0;
            for (i = st; i + 2 < c; i++) {
                sum += cdj_linkramp_word(base, i, be);
            }
            if (sum == stored) {
                cdj_linkramp_ckstart = (int)st;
                cdj_linkramp_ckbe = (int)be;
                info_report("link ramp: checksum CALIBRATED after %u frame(s) "
                            "-- len-%u type-%u message, C=%u, checksum at "
                            "+0x%x = 0x%08x, reproduced by summing words "
                            "[%u..C-3] %s-endian",
                            tries + 1,
                            (unsigned)(d[0x1C] | (d[0x1D] << 8)), d[0x1E], c,
                            0x18 + 4 * (c - 2), stored, st,
                            be ? "big" : "little");
                return;
            }
        }
    }
    if (++tries >= 400) {
        cdj_linkramp_ckstart = -2;
        warn_report("link ramp: checksum calibration FAILED on 400 frames -- "
                    "no span/endianness reproduces the stored value; the "
                    "repair stays off and every ramp result is VOID");
    }
}

/* Only repairs after a successful calibration. */
static void cdj_linkramp_fix_checksum(uint32_t sar)
{
    static int nock = -1;
    static bool said;
    uint32_t phys = A7ADDR(sar), base = phys + 0x18;
    uint32_t c, i, sum = 0;
    uint8_t d[0x20], w[4];

    if (nock < 0) {
        const char *e = getenv("CDJ_LINKRAMP_NOCK");
        nock = e && *e && *e != '0';
    }
    if (nock || cdj_linkramp_ckstart < 0) {
        return;
    }
    cpu_physical_memory_read(phys, d, sizeof(d));
    if (!d[0x18] || d[0x18] != d[0x19] || d[0x18] != d[0x1A]
        || d[0x18] != d[0x1B] || d[0x18] >= 0x32) {
        return;
    }
    c = cdj_linkramp_c(d);
    if (c < 6) {
        return;
    }
    for (i = (uint32_t)cdj_linkramp_ckstart; i + 2 < c; i++) {
        sum += cdj_linkramp_word(base, i, cdj_linkramp_ckbe);
    }
    if (cdj_linkramp_ckbe) {
        w[0] = sum >> 24; w[1] = sum >> 16; w[2] = sum >> 8; w[3] = sum;
    } else {
        w[3] = sum >> 24; w[2] = sum >> 16; w[1] = sum >> 8; w[0] = sum;
    }
    cpu_physical_memory_write(base + 4 * (c - 2), w, 4);
    if (!said) {
        said = true;
        info_report("link ramp: checksum repaired at +0x%x (C=%u) -- ramped "
                    "frames should now be ACCEPTED", 0x18 + 4 * (c - 2), c);
    }
}

static void cdj_linkramp_parse(void)
{
    const char *e = getenv("CDJ_LINKRAMP");
    char *end;

    cdj_linkramp.parsed = true;
    if (!e || !*e) {
        return;
    }
    while (*e && cdj_linkramp.n < CDJ_LINKRAMP_MAX) {
        CdjLinkRamp *r = &cdj_linkramp.t[cdj_linkramp.n];

        r->off = strtoul(e, &end, 0);
        r->val = (*end == ':') ? strtoul(end + 1, &end, 0) : 0;
        r->step = (*end == ':') ? strtoul(end + 1, &end, 0) : 1;
        r->width = (*end == ':') ? strtoul(end + 1, &end, 0) : 4;
        r->period = (*end == ':') ? strtoul(end + 1, &end, 0) : 50;
        r->count = (*end == ':') ? strtoul(end + 1, &end, 0) : 1;
        /* The load needs a pristine link, so patch only after start_ms. */
        r->start_ms = (*end == ':') ? strtoll(end + 1, &end, 0) : 0;
        r->wrap = (*end == ':') ? strtoul(end + 1, &end, 0) : 0;
        r->order = (*end == ':') ? strtoul(end + 1, &end, 0) : 0;
        r->base = r->val;
        if (!r->count) {
            r->count = 1;
        }
        if (r->width != 1 && r->width != 2 && r->width != 4) {
            r->width = 4;
        }
        if (!r->period) {
            r->period = 1;
        }
        cdj_linkramp.n++;
        cdj_linkramp.on = true;
        info_report("link ramp: frame+0x%x .. +0x%x = %u += %u every %u "
                    "frames, wrapping at %u (width %u, %s), "
                    "starting at %" PRId64 " ms -- SEARCHING, not a fix",
                    r->off, r->off + (r->count - 1) * r->width,
                    r->val, r->step, r->period, r->wrap, r->width,
                    r->order ? "little-endian, so the GUI reads this value"
                             : "big-endian, so the GUI reads it REVERSED --"
                               " add :1 as a 9th field to send it straight",
                    r->start_ms);
        if (*end != ',') {
            break;
        }
        e = end + 1;
    }
}

/*
 * CDJ_BEATDRIVE=<bpm>[:<start_ms>][:<beats_per_bar>][:<bars>][:<bpm_addr>]
 *
 * frame+0xC0 lights one box of the blue beat row (GUI element 0x2c of
 * FUN_1c008fee): 1..4 selects a box, 0 none, >= 5 hides the row. With bars > 0,
 * frame+0xE4 counts down bars*beats_per_bar..1 for the Bars readout.
 *
 * bpm > 0 steps at that fixed rate from start_ms. bpm = 0 follows the deck:
 * phase from cdj_dsp_engine_pos_ms(), rate from the deck's BPM field in tenths
 * at bpm_addr (default 0x0B568D04). Nothing is written until audio has played.
 * MAIN itself never counts beats, so this phase is the emulator's.
 */
static struct {
    bool parsed, on;
    double bpm;
    int64_t start_ms;
    uint32_t per_bar, bars, bpm_addr;
    double phase;               /* beats accumulated, integrated not rescaled */
    int64_t last_pos_ms;
} cdj_beatdrive;


/* The link reverses each 32-bit word, so the frame carries these little-endian. */
static void cdj_beat_write_le(uint32_t sar, uint32_t off, uint32_t v)
{
    uint8_t b[4];

    b[0] = v & 0xFF;
    b[1] = (v >> 8) & 0xFF;
    b[2] = (v >> 16) & 0xFF;
    b[3] = (v >> 24) & 0xFF;
    cpu_physical_memory_write(A7ADDR(sar + off), b, 4);
}

/*
 * CDJ_BEATGRID=1[:<scan_lo>][:<scan_hi>][:<retry_ms>]
 *   defaults 0x0C000000, 0x0D000000, 2000 ms
 *
 * The firmware builds a beat grid in RAM from the track's analysis file (its
 * address and length go to FUN_0827C2C4 from 0x0826AFC0). The buffer is a heap
 * allocation, so it is found by scanning for its self-identifying header:
 *
 *   header, 28 bytes          entry, 20 bytes
 *     +0x00  u16 0              +0x00  u16 beat-in-bar (1..4)
 *     +0x02  u16 20  stride     +0x02  u16 tempo x100
 *     +0x04  u32 count          +0x04  u32 time in ms
 *     +0x08  u32 count*20       +0x08  u32 0xFFFFFFFF
 *     +0x0C  u32 1              +0x0C  u32 0xFFFFFFFF
 *     +0x10  u16 0, u16 1       +0x10  u32 level
 *     +0x14  u32 0
 *     +0x18  u32 base+0x1C      self-pointer
 *
 * Used by CDJ_BEATDRIVE's follow mode in place of its integrated phase.
 */
#define CDJ_BEATGRID_STRIDE 20
#define CDJ_BEATGRID_HDR    28

static struct {
    bool parsed, on, found, gave_up;
    uint32_t scan_lo, scan_hi, base, count;
    /* First entry with bar position 1; the grid need not start on a downbeat. */
    uint32_t first_downbeat;
    bool have_downbeat;
    int64_t retry_ms, next_scan_ms;
    unsigned tries;
} cdj_beatgrid;

static void cdj_beatgrid_parse(void)
{
    const char *e = getenv("CDJ_BEATGRID");
    char *end;

    cdj_beatgrid.parsed = true;
    if (!e || !*e) {
        return;
    }
    if (!strtoul(e, &end, 0)) {
        return;
    }
    cdj_beatgrid.scan_lo = (*end == ':') ? strtoul(end + 1, &end, 0) : 0x0C000000;
    cdj_beatgrid.scan_hi = (*end == ':') ? strtoul(end + 1, &end, 0) : 0x0D000000;
    cdj_beatgrid.retry_ms = (*end == ':') ? strtoll(end + 1, &end, 0) : 2000;
    if (cdj_beatgrid.scan_hi <= cdj_beatgrid.scan_lo) {
        warn_report("beat grid: empty scan range -- ignored");
        return;
    }
    if (cdj_beatgrid.retry_ms < 100) {
        cdj_beatgrid.retry_ms = 2000;
    }
    cdj_beatgrid.on = true;
    info_report("beat grid: scanning 0x%08x-0x%08x every %" PRId64 " ms for the "
                "firmware's own beat grid (stride %d, self-pointer at +0x18); "
                "the beat number and beat times become the FIRMWARE'S, the "
                "publication into the frame is still the emulator's",
                cdj_beatgrid.scan_lo, cdj_beatgrid.scan_hi,
                cdj_beatgrid.retry_ms, CDJ_BEATGRID_STRIDE);
}

/* One candidate header at `a`, fully validated. */
static bool cdj_beatgrid_header_ok(uint32_t a, uint32_t *count_out)
{
    uint8_t h[CDJ_BEATGRID_HDR];
    uint32_t stride, count, bytes, selfp;

    cpu_physical_memory_read(a, h, sizeof(h));
    stride = h[2] | (h[3] << 8);
    if (h[0] || h[1] || stride != CDJ_BEATGRID_STRIDE) {
        return false;
    }
    count = h[4] | (h[5] << 8) | (h[6] << 16) | ((uint32_t)h[7] << 24);
    bytes = h[8] | (h[9] << 8) | (h[10] << 16) | ((uint32_t)h[11] << 24);
    if (!count || count > 200000 || bytes != count * CDJ_BEATGRID_STRIDE) {
        return false;
    }
    selfp = h[24] | (h[25] << 8) | (h[26] << 16) | ((uint32_t)h[27] << 24);
    if (selfp != a + CDJ_BEATGRID_HDR) {
        return false;
    }
    *count_out = count;
    return true;
}

/* Looks at the first 32 entries only. */
static void cdj_beatgrid_find_downbeat(void)
{
    uint32_t i, lim = cdj_beatgrid.count < 32 ? cdj_beatgrid.count : 32;

    cdj_beatgrid.have_downbeat = false;
    cdj_beatgrid.first_downbeat = 0;
    for (i = 0; i < lim; i++) {
        uint8_t e[2];

        cpu_physical_memory_read(cdj_beatgrid.base + i * CDJ_BEATGRID_STRIDE,
                                 e, sizeof(e));
        if ((e[0] | (e[1] << 8)) == 1) {
            cdj_beatgrid.first_downbeat = i;
            cdj_beatgrid.have_downbeat = true;
            return;
        }
    }
}

static void cdj_beatgrid_scan(void)
{
    /* 64 KiB at a time, with CDJ_BEATGRID_HDR bytes of overlap so a header that
     * straddles a chunk boundary is still seen. */
    enum { CDJ_BEATGRID_CHUNK = 64 * 1024 };
    uint8_t *buf = g_malloc(CDJ_BEATGRID_CHUNK);
    uint32_t a;

    for (a = cdj_beatgrid.scan_lo; a + CDJ_BEATGRID_HDR <= cdj_beatgrid.scan_hi;
         a += CDJ_BEATGRID_CHUNK - CDJ_BEATGRID_HDR) {
        uint32_t n = cdj_beatgrid.scan_hi - a;
        uint32_t i;

        if (n > CDJ_BEATGRID_CHUNK) {
            n = CDJ_BEATGRID_CHUNK;
        }
        cpu_physical_memory_read(a, buf, n);
        for (i = 0; i + CDJ_BEATGRID_HDR <= n; i += 4) {
            uint32_t selfp = buf[i + 24] | (buf[i + 25] << 8)
                             | (buf[i + 26] << 16)
                             | ((uint32_t)buf[i + 27] << 24);
            uint32_t count;

            if (selfp != a + i + CDJ_BEATGRID_HDR) {
                continue;       /* the cheap test first: it rejects almost all */
            }
            if (!cdj_beatgrid_header_ok(a + i, &count)) {
                continue;
            }
            cdj_beatgrid.base = a + i + CDJ_BEATGRID_HDR;
            cdj_beatgrid.count = count;
            cdj_beatgrid.found = true;
            cdj_beatgrid_find_downbeat();
            info_report("beat grid: FOUND -- %u entries at 0x%08x (header "
                        "0x%08x), after %u scan(s); first downbeat at entry %u "
                        "%s", count, cdj_beatgrid.base, a + i,
                        cdj_beatgrid.tries, cdj_beatgrid.first_downbeat,
                        cdj_beatgrid.have_downbeat ? "" : "(NONE FOUND)");
            g_free(buf);
            return;
        }
    }
    g_free(buf);
}

/*
 * The firmware's beat-in-bar at a playback position, or 0 when the grid is not
 * found yet or the position is before its first beat.
 */
static uint32_t cdj_beatgrid_beat_at(uint64_t pos_ms, uint32_t *index_out)
{
    uint32_t lo = 0, hi, best_beat = 0;
    uint8_t e[8];

    if (index_out) {
        *index_out = 0;
    }

    if (!cdj_beatgrid.parsed) {
        cdj_beatgrid_parse();
    }
    if (!cdj_beatgrid.on) {
        return 0;
    }
    if (!cdj_beatgrid.found && !cdj_beatgrid.gave_up) {
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

        if (now >= cdj_beatgrid.next_scan_ms) {
            cdj_beatgrid.next_scan_ms = now + cdj_beatgrid.retry_ms;
            cdj_beatgrid.tries++;
            cdj_beatgrid_scan();
            if (!cdj_beatgrid.found && cdj_beatgrid.tries >= 60) {
                cdj_beatgrid.gave_up = true;
                warn_report("beat grid: no grid found in 0x%08x-0x%08x after "
                            "%u scans -- falling back to the integrated phase",
                            cdj_beatgrid.scan_lo, cdj_beatgrid.scan_hi,
                            cdj_beatgrid.tries);
            }
        }
    }
    if (!cdj_beatgrid.found) {
        return 0;
    }
    hi = cdj_beatgrid.count;
    /* Last entry whose time is <= pos_ms. */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t t;

        cpu_physical_memory_read(cdj_beatgrid.base + mid * CDJ_BEATGRID_STRIDE,
                                 e, sizeof(e));
        t = e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);
        if (t <= pos_ms) {
            best_beat = e[0] | (e[1] << 8);
            if (index_out) {
                *index_out = mid;
            }
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return best_beat;
}

void cdj_beat_drive(unsigned ch, uint32_t sar)
{
    int64_t now;
    uint32_t beat, fw_beat = 0, fw_index = 0;
    uint64_t pos = 0;

    if (ch != 0) {
        return;
    }
    if (!cdj_beatdrive.parsed) {
        const char *e = getenv("CDJ_BEATDRIVE");
        char *end;

        cdj_beatdrive.parsed = true;
        if (e && *e) {
            cdj_beatdrive.bpm = strtod(e, &end);
            cdj_beatdrive.start_ms =
                (*end == ':') ? strtoll(end + 1, &end, 0) : 70000;
            cdj_beatdrive.per_bar = (*end == ':') ? strtoul(end + 1, &end, 0) : 4;
            cdj_beatdrive.bars = (*end == ':') ? strtoul(end + 1, &end, 0) : 0;
            cdj_beatdrive.bpm_addr =
                (*end == ':') ? strtoul(end + 1, &end, 0) : 0x0B568D04;
            if (cdj_beatdrive.bpm < 0.0) {
                warn_report("beat drive: bpm cannot be negative -- ignored");
            } else {
                if (!cdj_beatdrive.per_bar || cdj_beatdrive.per_bar > 16) {
                    cdj_beatdrive.per_bar = 4;
                }
                cdj_beatdrive.on = true;
                if (cdj_beatdrive.bpm > 0.0) {
                    info_report("beat drive: frame+0xC0 = 1..%u at a TYPED "
                                "%.2f BPM from %" PRId64 " ms, little-endian; "
                                "countdown over %u bar(s) -- the EMULATOR is "
                                "producing this phase, MAIN never counts beats",
                                cdj_beatdrive.per_bar, cdj_beatdrive.bpm,
                                cdj_beatdrive.start_ms, cdj_beatdrive.bars);
                } else {
                    info_report("beat drive: frame+0xC0 = 1..%u FOLLOWING THE "
                                "DECK -- phase from the decode position, rate "
                                "from *0x%08x tenths scaled by the pitch; "
                                "countdown over %u bar(s). Nothing is "
                                "published until audio has been played",
                                cdj_beatdrive.per_bar, cdj_beatdrive.bpm_addr,
                                cdj_beatdrive.bars);
                }
            }
        }
    }
    if (!cdj_beatdrive.on) {
        return;
    }
    now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    if (now < cdj_beatdrive.start_ms) {
        return;
    }
    if (cdj_beatdrive.bpm > 0.0) {
        /* Fixed BPM from the virtual clock; needs no audio. */
        beat = (uint32_t)(((double)(now - cdj_beatdrive.start_ms) / 1000.0)
                          * cdj_beatdrive.bpm / 60.0);
    } else {
        uint32_t tenths = 0;
        double bpm;

        pos = cdj_dsp_engine_pos_ms();

        if (!pos) {
            cdj_beatdrive.phase = 0.0;      /* cued or not started: downbeat */
            cdj_beatdrive.last_pos_ms = 0;
            return;
        }
        cpu_physical_memory_read(cdj_beatdrive.bpm_addr, &tenths, 4);
        bpm = (double)le32_to_cpu(tenths) / 10.0;
        if (bpm <= 0.0) {
            return;             /* the deck has not published a BPM yet */
        }
        /*
         * Base BPM, not pitched: the position is already pitch-scaled.
         * Integrated so a fader move does not jump the lit box.
         */
        if (pos < (uint64_t)cdj_beatdrive.last_pos_ms) {
            cdj_beatdrive.phase = 0.0;      /* seeked backwards: start over */
        } else {
            cdj_beatdrive.phase += (double)(pos - cdj_beatdrive.last_pos_ms)
                                   / 1000.0 * bpm / 60.0;
        }
        cdj_beatdrive.last_pos_ms = (int64_t)pos;
        beat = (uint32_t)cdj_beatdrive.phase;
        /* Prefer the firmware's grid; the integrated phase covers its gaps. */
        fw_beat = cdj_beatgrid_beat_at(pos, &fw_index);
    }

    cdj_linkramp_calibrate(sar);        /* must happen BEFORE any patch */

    /* Log the beat source every 5 s. */
    if (fw_beat >= 1 && fw_beat <= cdj_beatdrive.per_bar) {
        static int64_t next_say;

        if (now >= next_say) {
            next_say = now + 5000;
            info_report("beat grid: position %" PRIu64 " ms -> the FIRMWARE'S "
                        "beat %u of %u (integrated phase would have said %u)",
                        pos, fw_beat, cdj_beatdrive.per_bar,
                        beat % cdj_beatdrive.per_bar + 1);
        }
        cdj_beat_write_le(sar, 0xC0, fw_beat);
    } else {
        cdj_beat_write_le(sar, 0xC0, beat % cdj_beatdrive.per_bar + 1);
    }
    if (cdj_beatdrive.bars) {
        uint32_t span = cdj_beatdrive.bars * cdj_beatdrive.per_bar;

        /* Count bars from the grid's first downbeat, not from entry 0. */
        if (fw_beat >= 1 && fw_beat <= cdj_beatdrive.per_bar
            && cdj_beatgrid.have_downbeat
            && fw_index >= cdj_beatgrid.first_downbeat) {
            uint32_t since = fw_index - cdj_beatgrid.first_downbeat;

            cdj_beat_write_le(sar, 0xE4, span - since % span);
        } else {
            cdj_beat_write_le(sar, 0xE4, span - beat % span);
        }
    }
    cdj_linkramp_fix_checksum(sar);
}

/*
 * CDJ_PITCHDRIVE=<range_pct>[:<start_ms>][:<base_bpm_tenths>][:<fader_addr>]
 *
 * frame+0x88 is the tempo percentage in signed hundredths (350 = +3.50%),
 * frame+0x8C the BPM in tenths. In the stand-in era MAIN's tempo publisher
 * (0x0844E4F0) was never entered, so the emulator published these from the
 * fader word (default 0x0B54CAE8). Percentage = (raw - 128) * range * 100 / 127.
 * base_bpm_tenths > 0 makes the BPM box track the pitch too.
 */
static struct {
    bool parsed, on, seen;
    uint32_t range, base_bpm, fader;
    int64_t start_ms;
} cdj_pitchdrive;

/*
 * The deck's tempo range. *(0x0A35DF30) (getter FUN_083FC474) is the TEMPO
 * RANGE selection index 1..5, not a percentage. The index-to-percent table is
 * unverified. A non-zero range_pct in CDJ_PITCHDRIVE overrides it.
 */
static uint32_t cdj_pitch_range(void)
{
    static const uint32_t pct[] = { 10, 6, 10, 16, 100, 100 };
    static uint32_t last = 0xFFFFFFFF;
    uint32_t sel = 0;

    if (cdj_pitchdrive.range) {
        return cdj_pitchdrive.range;        /* an explicit override */
    }
    cpu_physical_memory_read(0x0A35DF30, &sel, 4);
    sel = le32_to_cpu(sel);
    if (sel != last) {
        last = sel;
        info_report("pitch drive: the deck's tempo range selection is %u -> "
                    "+/-%u%% (provisional mapping, see cdj_pitch_range)",
                    sel, pct[sel < ARRAY_SIZE(pct) ? sel : 0]);
    }
    return pct[sel < ARRAY_SIZE(pct) ? sel : 0];
}

/* Tempo percentage in hundredths; 0 when CDJ_PITCHDRIVE is off. */
int32_t cdj_pitch_pct(void)
{
    uint32_t word = 0;
    uint8_t raw;

    if (!cdj_pitchdrive.on) {
        return 0;
    }
    /*
     * The fader word holds the panel byte << 8 (0..255, centre 128). It stays
     * 0 until the first panel report, which must not read as full negative.
     */
    cpu_physical_memory_read(cdj_pitchdrive.fader, &word, 4);
    if (!le32_to_cpu(word) && !cdj_pitchdrive.seen) {
        return 0;
    }
    cdj_pitchdrive.seen = true;
    raw = (uint8_t)((le32_to_cpu(word) >> 8) & 0xFF);
    return ((int32_t)raw - 128) * (int32_t)cdj_pitch_range() * 100 / 127;
}

void cdj_pitch_drive(unsigned ch, uint32_t sar)
{
    int32_t pct;

    if (ch != 0) {
        return;
    }
    if (!cdj_pitchdrive.parsed) {
        const char *e = getenv("CDJ_PITCHDRIVE");
        char *end;

        cdj_pitchdrive.parsed = true;
        if (e && *e) {
            cdj_pitchdrive.range = strtoul(e, &end, 0);
            cdj_pitchdrive.start_ms =
                (*end == ':') ? strtoll(end + 1, &end, 0) : 70000;
            cdj_pitchdrive.base_bpm =
                (*end == ':') ? strtoul(end + 1, &end, 0) : 0;
            cdj_pitchdrive.fader =
                (*end == ':') ? strtoul(end + 1, &end, 0) : 0x0B54CAE8;
            if (!cdj_pitchdrive.range) {
                cdj_pitchdrive.range = 10;
            }
            cdj_pitchdrive.on = true;
            info_report("pitch drive: frame+0x88 = ((*0x%08x >> 8) - 128) * %u%% "
                        "/ 127 (range 0 = the deck's own setting), from "
                        "%" PRId64 " ms; BPM box "
                        "%s -- the EMULATOR is publishing this, MAIN's tempo "
                        "publisher is never entered",
                        cdj_pitchdrive.fader, cdj_pitchdrive.range,
                        cdj_pitchdrive.start_ms,
                        cdj_pitchdrive.base_bpm ? "tracks it" : "untouched");
        }
    }
    if (!cdj_pitchdrive.on) {
        return;
    }
    if (qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) < cdj_pitchdrive.start_ms) {
        return;
    }
    pct = cdj_pitch_pct();
    if (!cdj_pitchdrive.seen) {
        return;             /* the fader has never reported: claim nothing */
    }

    cdj_linkramp_calibrate(sar);        /* must happen BEFORE any patch */
    cdj_beat_write_le(sar, 0x88, (uint32_t)pct);
    if (cdj_pitchdrive.base_bpm) {
        int64_t bpm = (int64_t)cdj_pitchdrive.base_bpm * (10000 + pct) / 10000;

        cdj_beat_write_le(sar, 0x8C, (uint32_t)bpm);
    }
    cdj_linkramp_fix_checksum(sar);
}

void cdj_link_ramp(unsigned ch, uint32_t sar)
{
    uint8_t b[4];
    uint32_t v;
    unsigned i, f, k;
    bool patched = false;

    if (ch != 0) {
        return;
    }
    if (!cdj_linkramp.parsed) {
        cdj_linkramp_parse();
    }
    if (!cdj_linkramp.on) {
        return;
    }
    for (k = 0; k < cdj_linkramp.n; k++) {
        CdjLinkRamp *r = &cdj_linkramp.t[k];

        if (r->start_ms
            && qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) < r->start_ms) {
            continue;
        }
        r->n++;
        if (r->n % r->period == 0) {
            r->val += r->step;
            if (r->wrap && r->val - r->base >= r->wrap) {
                r->val = r->base;
            }
        }
        v = r->val;
        for (i = 0; i < r->width; i++) {
            b[i] = r->order ? ((v >> (8 * i)) & 0xFF)           /* little */
                            : ((v >> (8 * (r->width - 1 - i))) & 0xFF);
        }
        if (!patched) {
            patched = true;
            cdj_linkramp_probe(sar);    /* one-shot: what is actually here? */
            cdj_linkramp_calibrate(sar);   /* must happen BEFORE any patch */
        }
        for (f = 0; f < r->count; f++) {
            cpu_physical_memory_write(A7ADDR(sar + r->off + f * r->width),
                                      b, r->width);
        }
    }
    if (patched) {
        cdj_linkramp_fix_checksum(sar);
    }
}

