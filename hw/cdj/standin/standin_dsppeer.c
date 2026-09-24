/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../cdj.h"
#include "../cdj_getenv.h"
/*
 * Stand-in DSP peer on MSIOF0, from before the real C6655 was emulated.
 * CDJ_DSP_LINK=1 enables it; off by default.
 *
 * tsk_DSPspi_frameDriver (0x08326B14) arms a one-word receive on DMA1 ch5 from
 * MSIOF0's RX FIFO into 0x09072A24 and blocks; without an answer 0x0995A138 is
 * never set and tsk_DSP_startup fails with E-7010. The framing, from the driver:
 *
 *     sync    one u16, 0xAACC (checked at 0x08326E7E)
 *     frame   n u16 words, n = *(0x0995A13C) (0x2C, toggled to 0x40 at
 *             0x08326C88 when a frame fails); word[0] must be 0x5533
 *             (0x08326C4A) and word[n-1] 0xAACC (0x08326C66)
 *
 * 0x08326CB2 wants three good frames in a row before setting the ready flag.
 * By default the payload is zero; the CDJ_DSP_REPLY* and CDJ_DSP_PARAM* knobs
 * below fill it. Words are written little-endian; CDJ_DSP_SWAP=1 swaps them.
 */
CdjDspPeer cdj_dsp;

/*
 * CDJ_DSP_PARAM=1: a parameter store answering reads per index.
 *
 * Command frames (DMA1 ch4 -> MSIOF0 SITFDR, i.e. cdj_dsp_take):
 *
 *     33 55 | 00 04 | 04 00 | 4f 01 | 1c 0b 00 00 | 00 00 cc aa
 *     SOF   | words | op    | tag   | index/value | EOF
 *
 *     op2 = WRITE, a list of (u32 index, u32 value) pairs
 *     op4 = READ,  a list of u32 indices
 *
 *     client 0x082E4D5A  buf = {replyMbfId, 0, u16 type=4, u16 tag, u32 code}
 *     reply  0x082E4956  matches the reply's tag against the pending entry
 *     reply frame        [w0 hdr][w1 count/id/level][w2 type][w3 tag][w4.. value]
 *
 * Indices MAIN never writes are DSP-owned outputs; CDJ_DSP_PARAM_SET / _RAMP
 * supply values for them.
 */
#define CDJ_DSP_PARAM_MAX 512
#define CDJ_DSP_PARAM_NONE 0xFFFFFFFFu

typedef struct CdjDspParam {
    uint32_t idx, val;
    uint64_t writes, reads;
    bool used;
    bool written;        /* MAIN has written it -> it is not a DSP-owned output */
} CdjDspParam;

static CdjDspParam cdj_dsp_pstore[CDJ_DSP_PARAM_MAX];
static uint16_t cdj_dsp_taglog_last = 0xFFFF;
/* CDJ_DSP_PARAM_SKIP="<idx>,..." -- never answer these, even under _RAMP_ALL.
 * Answering 0x0154 errors the deck. */
static uint32_t cdj_dsp_skip[32];
static unsigned cdj_dsp_skip_n;

static bool cdj_dsp_param_skipped(uint32_t idx)
{
    unsigned i;

    for (i = 0; i < cdj_dsp_skip_n; i++) {
        if (cdj_dsp_skip[i] == idx) {
            return true;
        }
    }
    return false;
}
static uint32_t *cdj_dsp_tagidx;        /* tag -> index of the pending read */

/*
 * Recent op4 read lists, keyed by tag. The largest read asks for fourteen
 * indices once per audio block:
 *
 *   0x0AF0 0x0150 0x0154 0x0158 0x0B30 0x0B34 0x0B38 0x0B3C
 *                        0x0B40 0x0B44 0x0B48 0x0B50 0x0B54 0x0B58
 *
 * A reply frame is 44 words, room for 19 32-bit values.
 */
#define CDJ_DSP_READLIST_MAX 20         /* longest list seen is 14             */
#define CDJ_DSP_RLIST_N      16         /* recent requests kept                */

typedef struct CdjDspReadList {
    uint16_t tag;
    uint8_t n;
    uint8_t cursor;                     /* how far CDJ_DSP_PARAM_SEQ has drained */
    bool used;
    uint32_t idx[CDJ_DSP_READLIST_MAX];
} CdjDspReadList;

static CdjDspReadList cdj_dsp_rlist[CDJ_DSP_RLIST_N];
static unsigned cdj_dsp_rlist_head;

/* Newest first: a tag can be reused, and the latest request is the live one. */
static const CdjDspReadList *cdj_dsp_rlist_find(uint16_t tag)
{
    unsigned i;

    for (i = 0; i < CDJ_DSP_RLIST_N; i++) {
        const CdjDspReadList *e =
            &cdj_dsp_rlist[(cdj_dsp_rlist_head + CDJ_DSP_RLIST_N - 1 - i)
                           % CDJ_DSP_RLIST_N];

        if (e->used && e->tag == tag) {
            return e;
        }
    }
    return NULL;
}

/* CDJ_DSP_PARAM_SET="<idx>:<val>[,...]" -- override; wins over MAIN's own write.
 * Up to 24 entries, enough for the fourteen-index read. */
#define CDJ_DSP_PSET_MAX 24
static struct { uint32_t idx, val; } cdj_dsp_pset[CDJ_DSP_PSET_MAX];
static unsigned cdj_dsp_pset_n;

/* CDJ_DSP_PARAM_RAMP="<idx>:<start>:<step>:<period_ms>[:<wrap>]" -- an
 * advancing answer, starting at CDJ_DSP_REPLY_MS. */
static struct {
    uint32_t idx, start, step, wrap;
    int64_t period_ms, from_ms;
    bool on;
} cdj_dsp_pramp;

static CdjDspParam *cdj_dsp_param_slot(uint32_t idx, bool create)
{
    unsigned i;

    for (i = 0; i < CDJ_DSP_PARAM_MAX; i++) {
        if (cdj_dsp_pstore[i].used && cdj_dsp_pstore[i].idx == idx) {
            return &cdj_dsp_pstore[i];
        }
        if (!cdj_dsp_pstore[i].used && create) {
            cdj_dsp_pstore[i].used = true;
            cdj_dsp_pstore[i].idx = idx;
            return &cdj_dsp_pstore[i];
        }
    }
    return NULL;
}

/* An index MAIN has never written is a DSP-owned output. */
static bool cdj_dsp_param_owned(uint32_t idx)
{
    CdjDspParam *e = cdj_dsp_param_slot(idx, false);

    return !e || !e->written;
}

/* The answer for <idx>: an override, a ramp, or (with _ECHO) what MAIN last
 * wrote. False when nothing is known. */
static bool cdj_dsp_param_value(uint32_t idx, uint32_t *out)
{
    unsigned i;
    CdjDspParam *slot;

    if (cdj_dsp_param_skipped(idx)) {
        return false;
    }

    for (i = 0; i < cdj_dsp_pset_n; i++) {
        if (cdj_dsp_pset[i].idx == idx) {
            *out = cdj_dsp_pset[i].val;
            return true;
        }
    }
    if (cdj_dsp_pramp.on
        && (cdj_dsp_pramp.idx == idx
            || (cdj_dsp.param_ramp_all && cdj_dsp_param_owned(idx)))) {
        int64_t ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) - cdj_dsp_pramp.from_ms;
        uint32_t v;

        if (ms < 0) {
            ms = 0;
        }
        v = cdj_dsp_pramp.start
            + (uint32_t)(ms / cdj_dsp_pramp.period_ms) * cdj_dsp_pramp.step;
        if (cdj_dsp_pramp.wrap) {
            v %= cdj_dsp_pramp.wrap;
        }
        *out = v;
        return true;
    }
    slot = cdj_dsp_param_slot(idx, false);
    if (cdj_dsp.param_echo && slot && slot->written) {
        *out = slot->val;
        return true;
    }
    return false;
}

/* Parse one command frame off the wire and keep what it says. */
static void cdj_dsp_param_take(const uint8_t *in, unsigned words)
{
    unsigned n = (unsigned)words * 2, off;
    uint32_t op, tag;

    if (!cdj_dsp.param || n < 12 || in[0] != 0x33 || in[1] != 0x55) {
        return;
    }
    op = in[4] | ((uint32_t)in[5] << 8);
    tag = in[6] | ((uint32_t)in[7] << 8);
    if (op == 2) {                       /* WRITE: a list of (index, value) */
        for (off = 8; off + 8 <= n; off += 8) {
            uint32_t idx, val;
            CdjDspParam *slot;

            if (!in[off] && !in[off + 1]
                && in[off + 2] == 0xcc && in[off + 3] == 0xaa) {
                break;                   /* the trailer ends the list */
            }
            idx = in[off] | ((uint32_t)in[off + 1] << 8)
                | ((uint32_t)in[off + 2] << 16) | ((uint32_t)in[off + 3] << 24);
            val = in[off + 4] | ((uint32_t)in[off + 5] << 8)
                | ((uint32_t)in[off + 6] << 16) | ((uint32_t)in[off + 7] << 24);
            if (cdj_dsp.param_log) {
                info_report("dsp param wr %7" PRId64 " ms  idx 0x%04x = 0x%x",
                            qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), idx, val);
            }
            slot = cdj_dsp_param_slot(idx, true);
            if (slot) {
                slot->val = val;
                slot->written = true;
                slot->writes++;
                cdj_dsp.param_writes++;
            }
        }
    } else if (op == 4) {                /* READ: a LIST of indices */
        /*
         * All entries share the tag; the tag -> index map keeps the last one,
         * and the full list goes into cdj_dsp_rlist.
         */
        unsigned entries = 0;
        CdjDspReadList *rl = &cdj_dsp_rlist[cdj_dsp_rlist_head];

        cdj_dsp_rlist_head = (cdj_dsp_rlist_head + 1) % CDJ_DSP_RLIST_N;
        rl->tag = (uint16_t)tag;
        rl->n = 0;
        rl->cursor = 0;
        rl->used = true;

        for (off = 8; off + 4 <= n; off += 4) {
            uint32_t idx;
            CdjDspParam *e;

            if (!in[off] && !in[off + 1]
                && in[off + 2] == 0xcc && in[off + 3] == 0xaa) {
                break;                   /* the trailer ends the list */
            }
            idx = in[off] | ((uint32_t)in[off + 1] << 8)
                | ((uint32_t)in[off + 2] << 16) | ((uint32_t)in[off + 3] << 24);
            e = cdj_dsp_param_slot(idx, true);
            if (e) {
                e->reads++;
            }
            if (cdj_dsp_tagidx) {
                cdj_dsp_tagidx[tag & 0xFFFF] = idx;
            }
            cdj_dsp.param_last_read = idx;
            cdj_dsp.param_reads++;
            if (rl->n < CDJ_DSP_READLIST_MAX) {
                rl->idx[rl->n++] = idx;
            }
            entries++;
            if (cdj_dsp.param_log) {
                info_report("dsp param rd %7" PRId64 " ms  idx 0x%04x  tag 0x%04x"
                            "  [%u]", qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL),
                            idx, tag, entries);
            }
        }
        if (entries) {
            cdj_dsp.param_pending = true;
        }
    }
}


bool cdj_dsp_present(void)
{
    return cdj_dsp.present;
}

int64_t cdj_dsp_ready_at(void)
{
    return cdj_dsp.ready_at;
}

void cdj_dsp_sent(int64_t now)
{
    cdj_dsp.ready_at = now + cdj_dsp.frame_ns;
}

void cdj_dsp_defer(void)
{
    cdj_dsp.deferred++;
}

static void cdj_dsp_summary(Notifier *n, void *unused)
{
    if (!cdj_dsp.present) {
        return;
    }
    info_report("dsp: %" PRIu64 " syncs, %" PRIu64 " response frames, "
                "%" PRIu64 " command frames (%" PRIu64 " words), "
                "%" PRIu64 " deferrals at %" PRId64 " us/frame",
                cdj_dsp.syncs, cdj_dsp.frames, cdj_dsp.txframes,
                cdj_dsp.txwords, cdj_dsp.deferred, cdj_dsp.frame_ns / 1000);
    /* So a CDJ_DSP_REPLY_MS gate past the end of the run is visible. */
    info_report("dsp: virtual clock at exit %" PRId64 " ms",
                qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL));
    if (cdj_dsp.reply) {
        info_report("dsp: %" PRIu64 " dispatchable replies (id %u, %u payload "
                    "words)", cdj_dsp.replies, cdj_dsp.reply_id,
                    cdj_dsp.reply_words);
    }
    {
        /* Reply sizes seen; `words` is MAIN's own RX DMA count. */
        unsigned i;

        for (i = 0; i < ARRAY_SIZE(cdj_dsp.fill_hist); i++) {
            if (cdj_dsp.fill_hist[i]) {
                info_report("dsp fill:  %2u words  %6u replies  "
                            "%6u with a read outstanding  -> room for %d "
                            "32-bit values", i, cdj_dsp.fill_hist[i],
                            cdj_dsp.fill_hist_pending[i],
                            (int)(i >= 5 ? (i - 5) / 2 : 0));
            }
        }
        if (cdj_dsp.fill_over) {
            info_report("dsp fill:  >64 words  %6u replies", cdj_dsp.fill_over);
        }
    }
    if (cdj_dsp.param) {
        unsigned i;

        info_report("dsp param: %" PRIu64 " writes stored, %" PRIu64 " reads "
                    "seen, %" PRIu64 " replies answered from the store "
                    "(last index 0x%04x = 0x%x)",
                    cdj_dsp.param_writes, cdj_dsp.param_reads,
                    cdj_dsp.param_answered, cdj_dsp.param_last_idx,
                    cdj_dsp.param_last_val);
        info_report("dsp param: %" PRIu64 " replies whose tag named no read",
                    cdj_dsp.param_tag_miss);
        if (cdj_dsp.param_vec) {
            info_report("dsp param: VEC on -- %" PRIu64 " value slots filled, "
                        "%" PRIu64 " replies too small for the whole list",
                        cdj_dsp.param_vec_slots, cdj_dsp.param_vec_short);
        }
        for (i = 0; i < CDJ_DSP_PARAM_MAX; i++) {
            if (cdj_dsp_pstore[i].used) {
                info_report("dsp param:   0x%04x = 0x%-10x %" PRIu64 " writes "
                            "%" PRIu64 " reads%s", cdj_dsp_pstore[i].idx,
                            cdj_dsp_pstore[i].val, cdj_dsp_pstore[i].writes,
                            cdj_dsp_pstore[i].reads,
                            cdj_dsp_pstore[i].written
                            ? "" : "   <-- DSP-OWNED, we invent it");
            }
        }
    }
    if (cdj_dsp.tag) {
        info_report("dsp: %" PRIu64 " tagged replies, last tag 0x%04x "
                    "(from *0x%08x, written at reply byte %u)",
                    cdj_dsp.tag_echoes, cdj_dsp.tag_last, cdj_dsp.tag_addr,
                    cdj_dsp.tag_off);
    }
}

void cdj_dsp_init(void)
{
    const char *on = getenv("CDJ_DSP_LINK");

    cdj_dsp.present = on && strcmp(on, "0");
    if (!cdj_dsp.present) {
        return;
    }
    cdj_dsp.swap = getenv("CDJ_DSP_SWAP") != NULL;
    cdj_dsp.debug = getenv("CDJ_DSP_DEBUG") != NULL;
    cdj_dsp.echo = getenv("CDJ_DSP_ECHO") != NULL;
    /*
     * CDJ_DSP_REPLY=1 builds a reply the response consumer (0x08326CB6) will
     * dispatch:
     *
     *     byte[2] bits 5..0   handler id; 0 skips dispatch, >6 returns -1
     *     byte[2] bit 7       a level; its 1->0 transition fires the event
     *                         flag the 3 ms task blocks on, so it toggles
     *     byte[3]             payload byte count; >>1 must be > 0
     *
     * CDJ_DSP_REPLY_ID (default 4, dispatch entry 0x082D78DA) and
     * CDJ_DSP_REPLY_WORDS (default 2) set the id and declared payload words.
     */
    cdj_dsp_engine_init();
    cdj_dsp.reply = getenv("CDJ_DSP_REPLY") != NULL;
    cdj_dsp.reply_n = getenv("CDJ_DSP_REPLY_N")
        ? strtoull(getenv("CDJ_DSP_REPLY_N"), NULL, 0) : 0;
    if (cdj_dsp.reply_n) {
        info_report("dsp: only the first %" PRIu64 " replies are dispatchable; "
                    "the rest fall back to the rejected id 0",
                    cdj_dsp.reply_n);
    }
    cdj_dsp.reply_prime = getenv("CDJ_DSP_REPLY_PRIME")
        ? strtoull(getenv("CDJ_DSP_REPLY_PRIME"), NULL, 0) : 0;
    cdj_dsp.reply_every = getenv("CDJ_DSP_REPLY_EVERY")
        ? strtoull(getenv("CDJ_DSP_REPLY_EVERY"), NULL, 0) : 0;
    if (cdj_dsp.reply_every > 1) {
        warn_report("dsp: a valid reply id only every %" PRIu64 " frames",
                    cdj_dsp.reply_every);
    }
    cdj_dsp.reply_skip = getenv("CDJ_DSP_REPLY_SKIP")
        ? strtoull(getenv("CDJ_DSP_REPLY_SKIP"), NULL, 0) : 0;
    cdj_dsp.clocklog = getenv("CDJ_DSP_CLOCKLOG") != NULL;
    cdj_dsp.key_off = cdj_dsp.key_val = -1;
    if (getenv("CDJ_DSP_REPLY_AFTER_KEY")) {
        unsigned off, val;

        if (sscanf(getenv("CDJ_DSP_REPLY_AFTER_KEY"), "%i:%i", &off, &val) == 2) {
            cdj_dsp.key_off = (int)off;
            cdj_dsp.key_val = (int)val;
            info_report("dsp: replies keep the rejected id 0 until panel key "
                        "0x%02x:0x%02x is pressed", off, val);
        } else {
            warn_report("dsp: CDJ_DSP_REPLY_AFTER_KEY wants <off>:<val>, e.g. "
                        "0x10:0x01 for PLAY -- ignoring '%s'",
                        getenv("CDJ_DSP_REPLY_AFTER_KEY"));
        }
    }
    if (cdj_dsp.reply_skip) {
        info_report("dsp: the first %" PRIu64 " response frames keep the "
                    "rejected id 0; the load runs on the old behaviour",
                    cdj_dsp.reply_skip);
    }
    cdj_dsp.reply_ms = getenv("CDJ_DSP_REPLY_MS")
        ? strtoll(getenv("CDJ_DSP_REPLY_MS"), NULL, 0) : 0;
    if (cdj_dsp.reply_ms) {
        info_report("dsp: replies use handler id 0 (rejected) until %" PRId64
                    " ms, then the configured id -- the load runs on the old "
                    "behaviour", cdj_dsp.reply_ms);
    }
    cdj_dsp.reply_id = getenv("CDJ_DSP_REPLY_ID")
        ? strtoul(getenv("CDJ_DSP_REPLY_ID"), NULL, 0) : 4;
    cdj_dsp.reply_words = getenv("CDJ_DSP_REPLY_WORDS")
        ? strtoul(getenv("CDJ_DSP_REPLY_WORDS"), NULL, 0) : 2;
    cdj_dsp.reply_vary = getenv("CDJ_DSP_REPLY_VARY") != NULL;
    cdj_dsp.reply_cnt = getenv("CDJ_DSP_REPLY_CNT")
        ? strtoul(getenv("CDJ_DSP_REPLY_CNT"), NULL, 0) : 0;
    cdj_dsp.reply_fill = getenv("CDJ_DSP_REPLY_FILL")
        ? strtoul(getenv("CDJ_DSP_REPLY_FILL"), NULL, 0) : 0;
    cdj_dsp.msg20 = getenv("CDJ_DSP_MSG20")
        ? strtoul(getenv("CDJ_DSP_MSG20"), NULL, 0) : 0;
    if (cdj_dsp.msg20) {
        info_report("dsp: msg+20 (frame+24) answered with 0x%04X -- the "
                    "transport request gate", cdj_dsp.msg20);
    }
    cdj_dsp.param = getenv("CDJ_DSP_PARAM") != NULL;
    cdj_dsp.param_type = getenv("CDJ_DSP_PARAM_TYPE") != NULL;
    cdj_dsp.param_last = getenv("CDJ_DSP_PARAM_LAST") != NULL;
    cdj_dsp.param_always = getenv("CDJ_DSP_PARAM_ALWAYS") != NULL;
    cdj_dsp.param_ro = getenv("CDJ_DSP_PARAM_RO") != NULL;
    cdj_dsp.param_log = getenv("CDJ_DSP_PARAM_LOG") != NULL;
    cdj_dsp.param_vec = getenv("CDJ_DSP_PARAM_VEC") != NULL;
    cdj_dsp.param_seq = getenv("CDJ_DSP_PARAM_SEQ") != NULL;
    cdj_dsp.veclen_exact = getenv("CDJ_DSP_VECLEN_EXACT") != NULL;
    cdj_dsp.taglog = getenv("CDJ_DSP_TAGLOG") != NULL;
    cdj_dsp.param_pair = getenv("CDJ_DSP_PARAM_PAIR") != NULL;
    {
        const char *sk = getenv("CDJ_DSP_PARAM_SKIP");

        while (sk && *sk && cdj_dsp_skip_n < ARRAY_SIZE(cdj_dsp_skip)) {
            cdj_dsp_skip[cdj_dsp_skip_n++] = strtoul(sk, (char **)&sk, 0);
            while (*sk == ',' || *sk == ' ') {
                sk++;
            }
        }
        if (cdj_dsp_skip_n) {
            warn_report("dsp param: skipping %u index(es), first 0x%04X",
                        cdj_dsp_skip_n, cdj_dsp_skip[0]);
        }
    }
    if (cdj_dsp.veclen_exact) {
        warn_report("dsp param: VECLEN_EXACT on -- declared W = 2 + values");
    }
    if (cdj_dsp.param_seq) {
        cdj_dsp.param_vec = true;       /* SEQ needs the read lists VEC keeps */
    }
    cdj_dsp.framemap = getenv("CDJ_DSP_FRAMEMAP") != NULL;
    cdj_dsp.param_echo = getenv("CDJ_DSP_PARAM_ECHO") != NULL;
    cdj_dsp.param_ramp_all = getenv("CDJ_DSP_PARAM_RAMP_ALL") != NULL;
    cdj_dsp.param_last_read = CDJ_DSP_PARAM_NONE;
    if (cdj_dsp.param) {
        const char *e = getenv("CDJ_DSP_PARAM_SET");
        unsigned i;

        cdj_dsp_tagidx = g_new(uint32_t, 0x10000);
        for (i = 0; i < 0x10000; i++) {
            cdj_dsp_tagidx[i] = CDJ_DSP_PARAM_NONE;
        }
        while (e && *e && cdj_dsp_pset_n < CDJ_DSP_PSET_MAX) {
            const char *start = e;

            cdj_dsp_pset[cdj_dsp_pset_n].idx = strtoul(e, (char **)&e, 0);
            cdj_dsp_pset[cdj_dsp_pset_n].val =
                (*e == ':') ? strtoul(e + 1, (char **)&e, 0) : 0;
            if (e == start) {
                break;
            }
            info_report("dsp param: index 0x%04x answered with 0x%x",
                        cdj_dsp_pset[cdj_dsp_pset_n].idx,
                        cdj_dsp_pset[cdj_dsp_pset_n].val);
            cdj_dsp_pset_n++;
            e += *e == ',';
        }
        if (e && *e) {
            warn_report("dsp param: CDJ_DSP_PARAM_SET truncated at %u entries "
                        "-- \"%s\" was NOT accepted", CDJ_DSP_PSET_MAX, e);
        }
        e = getenv("CDJ_DSP_PARAM_RAMP");
        if (e && *e) {
            cdj_dsp_pramp.idx = strtoul(e, (char **)&e, 0);
            cdj_dsp_pramp.start = (*e == ':') ? strtoul(e + 1, (char **)&e, 0) : 0;
            cdj_dsp_pramp.step = (*e == ':') ? strtoul(e + 1, (char **)&e, 0) : 1;
            cdj_dsp_pramp.period_ms =
                (*e == ':') ? strtoll(e + 1, (char **)&e, 0) : 1000;
            cdj_dsp_pramp.wrap = (*e == ':') ? strtoul(e + 1, (char **)&e, 0) : 0;
            if (cdj_dsp_pramp.period_ms <= 0) {
                cdj_dsp_pramp.period_ms = 1000;
            }
            cdj_dsp_pramp.from_ms = cdj_dsp.reply_ms;
            cdj_dsp_pramp.on = true;
            info_report("dsp param ramp: index 0x%04x = %u += %u every %"
                        PRId64 " ms, wrap %u, from %" PRId64 " ms",
                        cdj_dsp_pramp.idx, cdj_dsp_pramp.start,
                        cdj_dsp_pramp.step, cdj_dsp_pramp.period_ms,
                        cdj_dsp_pramp.wrap, cdj_dsp_pramp.from_ms);
        }
        info_report("dsp param: the peer now REMEMBERS index/value writes and "
                    "answers reads per index%s",
                    cdj_dsp.param_type ? " (stamping w2 = 4)" : "");
    }
    cdj_dsp.tag = getenv("CDJ_DSP_TAG") != NULL;
    cdj_dsp.tag_addr = getenv("CDJ_DSP_TAG_ADDR")
        ? strtoul(getenv("CDJ_DSP_TAG_ADDR"), NULL, 0) : 0x09944B7C;
    cdj_dsp.tag_off = getenv("CDJ_DSP_TAG_OFF")
        ? strtoul(getenv("CDJ_DSP_TAG_OFF"), NULL, 0) : 6;
    /* CDJ_DSP_FRAME_US: wire time per response frame, default 5000. The MSIOF0
     * clock is unknown; much faster starves the guest. */
    cdj_dsp.frame_ns = 1000LL * (getenv("CDJ_DSP_FRAME_US")
                                 ? strtoll(getenv("CDJ_DSP_FRAME_US"), NULL, 0)
                                 : 5000);
    cdj_dsp.exit.notify = cdj_dsp_summary;
    qemu_add_exit_notifier(&cdj_dsp.exit);
    info_report("dsp: MSIOF0 peer active%s", cdj_dsp.swap ? " (byte-swapped)"
                                                          : "");
}

/*
 * Set by the panel when the CDJ_DSP_REPLY_AFTER_KEY=<off>:<val> key goes down;
 * until then replies keep the rejected id 0.
 */
bool cdj_dsp_key_armed;

static void cdj_dsp_put_word(uint8_t *at, uint16_t w)
{
    if (cdj_dsp.swap) {
        at[0] = w >> 8;
        at[1] = w & 0xFF;
    } else {
        at[0] = w & 0xFF;
        at[1] = w >> 8;
    }
}

/* Fill `words` u16 stages of a receive request, sized to the request because
 * the driver toggles its frame length between 0x2C and 0x40 (0x08326C88). */
void cdj_dsp_fill(uint8_t *out, unsigned words)
{
    memset(out, 0, (size_t)words * 2);
    if (words == 1) {
        cdj_dsp_put_word(out, 0xAACC);
        cdj_dsp.syncs++;
        return;
    }
    cdj_dsp_put_word(out, 0x5533);
    /*
     * CDJ_DSP_ECHO=1 reflects the last command's payload into the reply. It
     * does not work: the echoed word lands on byte[2..3] as id 8 with a zero
     * count, which the consumer rejects. Kept for reference only.
     */
    if (words < ARRAY_SIZE(cdj_dsp.fill_hist)) {
        cdj_dsp.fill_hist[words]++;
        if (cdj_dsp.param_pending) {
            cdj_dsp.fill_hist_pending[words]++;
        }
    } else {
        cdj_dsp.fill_over++;
    }
    if (cdj_dsp.reply && words >= 4) {
        unsigned n = cdj_dsp.reply_words;
        unsigned id = cdj_dsp.reply_id & 0x3F;
        const CdjDspReadList *vec_rl = NULL;

        /*
         * byte[3] = n * 2 declares the payload length. A longer declared length
         * on every reply errors the deck, so widen only the reply that answers
         * an outstanding read list. Looked up here, before the count word.
         */
        if (cdj_dsp.param_vec && cdj_dsp.param && !cdj_dsp.param_ro
            && cdj_dsp_tagidx
            && (cdj_dsp.param_pending || cdj_dsp.param_always)) {
            uint16_t t = 0;

            cpu_physical_memory_read(cdj_dsp.tag_addr, &t, 2);
            vec_rl = cdj_dsp_rlist_find(t);
            /*
             * CDJ_DSP_TAGLOG=1: log each change of the expected tag
             * *(0x09944B7C) with the outstanding count *(0x09944CA0).
             */
            if (cdj_dsp.taglog && t != cdj_dsp_taglog_last) {
                uint32_t outstanding = 0;

                cpu_physical_memory_read(0x09944CA0, &outstanding, 4);
                info_report("dsp tag: 0x%04X outstanding %u list %u",
                            t, outstanding, vec_rl ? vec_rl->n : 0);
                if (vec_rl && vec_rl->n >= 6) {
                    char line[512];
                    unsigned k, o = 0;

                    for (k = 0; k < vec_rl->n && o < sizeof(line) - 8; k++) {
                        o += snprintf(line + o, sizeof(line) - o, " 0x%04X",
                                      vec_rl->idx[k]);
                    }
                    info_report("dsp tag: 0x%04X list of %u:%s",
                                t, vec_rl->n, line);
                }
                cdj_dsp_taglog_last = t;
            }
            if (vec_rl && vec_rl->n) {
                unsigned k, last = 0;
                uint32_t v;

                /* Widen only as far as the last index we can answer. */
                for (k = 0; k < vec_rl->n; k++) {
                    if (cdj_dsp_param_value(vec_rl->idx[k], &v)) {
                        last = k + 1;
                    }
                }
                /*
                 * The consumer 0x082E4956 (called from 0x08326D12, r5 = W =
                 * byte[3] >> 1):
                 *
                 *   0x082E4960  tst r5,r5        W == 0            -> return
                 *   0x082E497E  cmp/gt #1,W-1    W <= 2            -> error 4
                 *   0x082E499E  mov.w @(2,r14)   tag at frame + 6  -> error 2 on miss
                 *   0x082E49E2  shll2 r13        r6 = (W - 2) * 4  = byte count
                 *   0x082E49F6  jsr  0x08517738  snd_mbf(*(rec), frame + 8, r6, 0)
                 *
                 * so W = 2 + number of 32-bit values. CDJ_DSP_VECLEN_EXACT=1
                 * declares that; without it the older 2 + 2*values is used.
                 * SEQ never widens.
                 */
                if (cdj_dsp.param_pair) {
                    /* CDJ_DSP_PARAM_PAIR=1: a reply carries one (index,
                     * value) pair, mirroring the op2 write format. */
                    unsigned k;

                    for (k = vec_rl->cursor; k < vec_rl->n; k++) {
                        if (cdj_dsp_param_value(vec_rl->idx[k], &v)) {
                            n = 4;
                            break;
                        }
                    }
                } else if (last && !cdj_dsp.param_seq) {
                    unsigned want = cdj_dsp.veclen_exact
                                    ? 2 + last : 2 + last * 2;

                    if (cdj_dsp.veclen_exact || want > n) {
                        n = want;
                    }
                }
            }
        }

        /*
         * Gates that keep the rejected id 0 so the load is not disturbed; a
         * valid id makes the dispatcher 0x082D7036 run on every reply.
         *   CDJ_DSP_REPLY_MS=<ms>      until this virtual time
         *   CDJ_DSP_REPLY_SKIP=<n>     for the first n response frames
         *   CDJ_DSP_REPLY_EVERY=<k>    valid only every k-th eligible frame,
         *                              after CDJ_DSP_REPLY_PRIME valid replies
         *                              (the dispatcher discards its first 500,
         *                              0x082D7044)
         *   CDJ_DSP_REPLY_AFTER_KEY    until the panel key is pressed
         *   CDJ_DSP_REPLY_N=<n>        only the first n valid replies
         */
        if (cdj_dsp.reply_ms
            && qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) < cdj_dsp.reply_ms) {
            id = 0;
        }
        if (cdj_dsp.reply_skip && cdj_dsp.frames < cdj_dsp.reply_skip) {
            id = 0;
        }
        if (id && cdj_dsp.reply_every > 1
            && cdj_dsp.reply_used >= cdj_dsp.reply_prime
            && (cdj_dsp.reply_seen++ % cdj_dsp.reply_every) != 0) {
            id = 0;
        }
        if (cdj_dsp.key_off >= 0 && !cdj_dsp_key_armed) {
            id = 0;
        }
        if (id && cdj_dsp.reply_n) {
            if (cdj_dsp.reply_used >= cdj_dsp.reply_n) {
                id = 0;
            } else {
                cdj_dsp.reply_used++;
            }
        }
        /* CDJ_DSP_CLOCKLOG=1: one line per dispatched reply with the clock. */
        if (id && cdj_dsp.clocklog) {
            info_report("dsp: dispatch #%" PRIu64 " at %" PRId64 " ms "
                        "(frame %" PRIu64 ")", cdj_dsp.reply_used,
                        qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL), cdj_dsp.frames);
        }

        if (n > words - 3) {
            n = words - 3;          /* header, count word and trailer */
        }
        cdj_dsp.reply_level = !cdj_dsp.reply_level;
        cdj_dsp_put_word(out + 2, (uint16_t)(((n * 2) << 8)
                                             | (cdj_dsp.reply_level ? 0x80 : 0)
                                             | id));
        /*
         * CDJ_DSP_TAG=1 echoes the request's tag into the reply. 0x082E4956
         * (called on every frame) is the only setter of event flag 0x5A, the
         * flag the DSP serial server waits on; its setters (0x082E49DA,
         * 0x082E4A48, 0x082E4AAA) compare a u16 in the reply with the expected
         * tag *(u16)0x09944B7C, a runtime sequence number. The tag is written
         * last, after the payload fills below.
         * CDJ_DSP_TAG_ADDR (default 0x09944B7C) / CDJ_DSP_TAG_OFF (default 6).
         *
         * *(0x09944CA0) is the outstanding-request counter: sends need it < 2
         * (0x082E4664), replies need it > 0 (0x082E4996).
         */
        /*
         * CDJ_DSP_REPLY_VARY=1 changes payload word[1] every frame. Handler 1
         * (the one that signals, 0x082D71F2) copies it to *(s+0xD0) and skips
         * the signal if it equals the previous value at *(s+0x1BC) (0x082D71CA).
         */
        if (cdj_dsp.reply_vary && n >= 2) {
            cdj_dsp_put_word(out + 6, (uint16_t)++cdj_dsp.vary);
        }
        /*
         * CDJ_DSP_REPLY_CNT=<v>: the pending count handler 1 gates on.
         * 0x082D716A computes *(s+0x17C) = *(s+0xC8) + *(s+0xCC) from payload
         * words [2] and [3]; 0x082D71D6 requires it > 0.
         */
        if (cdj_dsp.reply_cnt && n >= 4) {
            cdj_dsp_put_word(out + 8, (uint16_t)cdj_dsp.reply_cnt);
            cdj_dsp_put_word(out + 10, 0);
        }
        /*
         * Payload word[4], stored to *(s+0x178). 0x082D7174 also requires
         * *(s+0xA4) == 0; the handler sets it to 1 itself after signalling.
         */
        if (cdj_dsp.reply_cnt && n >= 5) {
            cdj_dsp_put_word(out + 12, (uint16_t)cdj_dsp.reply_cnt);
        }
        /*
         * Payload word[10]: 0x082D714A loads it << 16 into r4 and 0x082D7178
         * tests it; zero leaves the signal mask empty.
         */
        if (cdj_dsp.reply_cnt && n >= 11) {
            cdj_dsp_put_word(out + 24, (uint16_t)cdj_dsp.reply_cnt);
        }
        /*
         * CDJ_DSP_MSG20=<v>: the transport request gate at msg+20 (msg = frame
         * + 4, so out+24), read at 0x082D714A. Written without widening the
         * declared length; the handler reads the buffer directly.
         */
        if (cdj_dsp.msg20 && words * 2 >= 26) {
            cdj_dsp_put_word(out + 24, (uint16_t)cdj_dsp.msg20);
        }
        /* CDJ_DSP_REPLY_FILL=<v>: every declared payload word set to v. */
        if (cdj_dsp.reply_fill) {
            unsigned i;

            for (i = 0; i < n; i++) {
                cdj_dsp_put_word(out + 4 + i * 2, (uint16_t)cdj_dsp.reply_fill);
            }
            if (cdj_dsp.reply_vary) {
                cdj_dsp_put_word(out + 6, (uint16_t)cdj_dsp.vary);
            }
        }
        /*
         * CDJ_DSP_PARAM: answer the read this tag names from the store, once
         * per request. Answering every read (CDJ_DSP_PARAM_ECHO) stops PLAY's
         * work path (0x082FBB1A/0x082FBDA6), so only named indices are
         * answered by default. After the payload fills, before the tag echo.
         * Modes: _PAIR (index, value) per reply, _SEQ one value per reply
         * draining the list, _VEC value k at bytes 8 + 4k, else scalar at w4.
         */
        if (cdj_dsp.param && !cdj_dsp.param_ro && n >= 4 && cdj_dsp_tagidx
            && (cdj_dsp.param_pending || cdj_dsp.param_always)) {
            uint16_t t = 0;
            uint32_t idx, v;

            cpu_physical_memory_read(cdj_dsp.tag_addr, &t, 2);
            idx = cdj_dsp_tagidx[t];
            if (idx == CDJ_DSP_PARAM_NONE) {
                cdj_dsp.param_tag_miss++;
                if (cdj_dsp.param_last) {
                    idx = cdj_dsp.param_last_read;
                }
            }
            /*
             * A 4-word payload is [type][seq][value_lo][value_hi]. The cursor
             * lives on the request; pending clears when the list is drained.
             */
            if (cdj_dsp.param_pair) {
                CdjDspReadList *rl = (CdjDspReadList *)vec_rl;

                while (rl && rl->cursor < rl->n) {
                    uint32_t widx = rl->idx[rl->cursor++];

                    if (!cdj_dsp_param_value(widx, &v)) {
                        continue;
                    }
                    if (cdj_dsp.param_type) {
                        cdj_dsp_put_word(out + 4, 4);
                    }
                    cdj_dsp_put_word(out + 8, (uint16_t)(widx & 0xFFFF));
                    cdj_dsp_put_word(out + 10, (uint16_t)(widx >> 16));
                    cdj_dsp_put_word(out + 12, (uint16_t)(v & 0xFFFF));
                    cdj_dsp_put_word(out + 14, (uint16_t)(v >> 16));
                    cdj_dsp.param_answered++;
                    cdj_dsp.param_vec_slots++;
                    cdj_dsp.param_last_idx = widx;
                    cdj_dsp.param_last_val = v;
                    break;
                }
                if (rl && rl->cursor >= rl->n) {
                    cdj_dsp.param_pending = false;
                }
            } else if (cdj_dsp.param_seq) {
                CdjDspReadList *rl = (CdjDspReadList *)vec_rl;

                while (rl && rl->cursor < rl->n) {
                    uint32_t widx = rl->idx[rl->cursor++];

                    if (!cdj_dsp_param_value(widx, &v)) {
                        continue;       /* nothing to say about this one */
                    }
                    if (cdj_dsp.param_type) {
                        cdj_dsp_put_word(out + 4, 4);
                    }
                    cdj_dsp_put_word(out + 8, (uint16_t)(v & 0xFFFF));
                    cdj_dsp_put_word(out + 10, (uint16_t)(v >> 16));
                    cdj_dsp.param_answered++;
                    cdj_dsp.param_vec_slots++;
                    cdj_dsp.param_last_idx = widx;
                    cdj_dsp.param_last_val = v;
                    break;
                }
                if (rl && rl->cursor >= rl->n) {
                    cdj_dsp.param_pending = false;
                }
            } else if (cdj_dsp.param_vec) {
                const CdjDspReadList *rl = vec_rl;
                unsigned k, answered = 0;
                unsigned room = words * 2 >= 10 ? (words * 2 - 2 - 8) / 4 : 0;

                if (rl) {
                    for (k = 0; k < rl->n && k < room; k++) {
                        if (!cdj_dsp_param_value(rl->idx[k], &v)) {
                            continue;
                        }
                        cdj_dsp_put_word(out + 8 + k * 4,
                                         (uint16_t)(v & 0xFFFF));
                        cdj_dsp_put_word(out + 10 + k * 4,
                                         (uint16_t)(v >> 16));
                        answered++;
                        cdj_dsp.param_last_idx = rl->idx[k];
                        cdj_dsp.param_last_val = v;
                    }
                    if (rl->n > room) {
                        cdj_dsp.param_vec_short++;
                    }
                }
                if (answered) {
                    if (cdj_dsp.param_type) {
                        cdj_dsp_put_word(out + 4, 4);
                    }
                    cdj_dsp.param_answered++;
                    cdj_dsp.param_vec_slots += answered;
                    cdj_dsp.param_pending = false;
                }
            } else if (idx != CDJ_DSP_PARAM_NONE
                       && cdj_dsp_param_value(idx, &v)) {
                if (cdj_dsp.param_type) {
                    cdj_dsp_put_word(out + 4, 4);
                }
                cdj_dsp_put_word(out + 8, (uint16_t)(v & 0xFFFF));
                cdj_dsp_put_word(out + 10, (uint16_t)(v >> 16));
                cdj_dsp.param_answered++;
                cdj_dsp.param_pending = false;
                cdj_dsp.param_last_idx = idx;
                cdj_dsp.param_last_val = v;
            }
        }
        /*
         * CDJ_DSP_FRAMEMAP=1: a debugging instrument. Stamp every 32-bit slot
         * of a reply that answers a read with 0xB0B0<offset>, then search
         * MAIN's RAM for the stamps to see which offsets it consumes. The deck
         * will error.
         */
        if (cdj_dsp.framemap && vec_rl) {
            unsigned off;

            for (off = 4; off + 4 <= words * 2 - 2; off += 4) {
                cdj_dsp_put_word(out + off, (uint16_t)off);
                cdj_dsp_put_word(out + off + 2, 0xB0B0);
            }
            cdj_dsp.framemaps++;
        }
        /* Last, so no payload fill can overwrite it. */
        if (cdj_dsp.tag && cdj_dsp.tag_off + 2 <= words * 2) {
            uint16_t t = 0;

            cpu_physical_memory_read(cdj_dsp.tag_addr, &t, 2);
            cdj_dsp_put_word(out + cdj_dsp.tag_off, t);
            cdj_dsp.tag_last = t;
            cdj_dsp.tag_echoes++;
        }
        cdj_dsp.replies++;
        if (cdj_dsp.debug && cdj_dsp.replies <= 8) {
            info_report("dsp: reply id=%u level=%d payload=%u bytes tag=0x%04x",
                        id, cdj_dsp.reply_level, n * 2, cdj_dsp.tag_last);
        }
    } else if (cdj_dsp.echo && cdj_dsp.cmdwords && words > cdj_dsp.cmdwords + 2) {
        unsigned i;

        for (i = 0; i < cdj_dsp.cmdwords; i++) {
            cdj_dsp_put_word(out + (i + 1) * 2, cdj_dsp.cmd[i]);
        }
    }
    cdj_dsp_put_word(out + (words - 1) * 2, 0xAACC);
    cdj_dsp.frames++;
}

void cdj_dsp_take(const uint8_t *in, unsigned words)
{
    static FILE *cap;

    cdj_capture_write(&cap, "msiof", in, (size_t)words * 2);
    cdj_dsp.txframes++;
    cdj_dsp.txwords += words;
    cdj_dsp_param_take(in, words);
    /* Keep the payload for CDJ_DSP_ECHO; the word count is byte 3. */
    if (words >= 4 && words <= 64) {
        unsigned n = in[3], i;

        if (n && n + 3 <= words && n <= ARRAY_SIZE(cdj_dsp.cmd)) {
            for (i = 0; i < n; i++) {
                cdj_dsp.cmd[i] = in[(i + 2) * 2] | (in[(i + 2) * 2 + 1] << 8);
            }
            cdj_dsp.cmdwords = n;
        }
    }
    if (cdj_dsp.debug) {
        char line[3 * 12 + 1];
        unsigned i, n = MIN(words * 2, 12u);

        for (i = 0; i < n; i++) {
            snprintf(line + i * 3, 4, "%02x ", in[i]);
        }
        info_report("dsp: command frame #%" PRIu64 ", %u words: %s",
                    cdj_dsp.txframes, words, line);
    }
    /*
     * CDJ_DSP_TXDUMP=<path>: every command frame MAIN sends, as records of
     * u32 frame index, u32 word count, then the words as sent.
     */
    {
        static FILE *fp;
        static int tried;
        const char *path = getenv("CDJ_DSP_TXDUMP");

        if (path && !fp && !tried) {
            tried = 1;
            fp = fopen(path, "wb");
        }
        if (fp) {
            uint32_t hdr[2] = { (uint32_t)cdj_dsp.txframes, words };

            fwrite(hdr, sizeof(hdr), 1, fp);
            fwrite(in, 1, (size_t)words * 2, fp);
            fflush(fp);
        }
    }
}

