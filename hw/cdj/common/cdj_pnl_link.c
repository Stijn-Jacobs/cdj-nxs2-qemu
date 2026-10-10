/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_pnl_link.h"
#include "cdj_getenv.h"
#include "cdj_panelkeys.h"
#include "qemu/timer.h"

#define CDJ_PNL_LINK_SIZE 0x1000
/* Reply bytes a second device can have queued at once. */
#define CDJ_PNL_LINK_FIFO 128

#define CDJ_PNL_LINK_SCSMR       0x00
#define CDJ_PNL_LINK_SCBRR       0x04
#define CDJ_PNL_LINK_SCSCR       0x08
#define CDJ_PNL_LINK_SCFTDR      0x0C  /* write-only: transmit FIFO data    */
#define CDJ_PNL_LINK_SCFSR       0x10
#define CDJ_PNL_LINK_SCFRDR      0x14  /* read-only: receive FIFO data      */
#define CDJ_PNL_LINK_SCFCR       0x18
#define CDJ_PNL_LINK_SCFDR       0x1C  /* read-only: FIFO fill levels        */
#define CDJ_PNL_LINK_SCLSR       0x24

#define CDJ_PNL_LINK_SCFSR_RDF   0x0002
#define CDJ_PNL_LINK_SCFSR_TDFE  0x0020
#define CDJ_PNL_LINK_SCFSR_TEND  0x0040
/* Status bits the peer owns; a guest write may clear them but never set them. */
#define CDJ_PNL_LINK_SCFSR_HW    0x009F  /* ER|BRK|FER|PER|RDF|DR            */

#define CDJ_PNL_LINK_SCFCR_RFRST 0x0002
#define CDJ_PNL_LINK_SCFCR_TFRST 0x0004

/* One scheduled key press, from CDJ_PANEL_PRESS=<off>:<mask>:<at_ms>:<dur_ms>
 * (comma-separated list) or the live key socket. Offsets are into the
 * payload. */
typedef struct CdjPnlLinkPress {
    unsigned off;
    uint8_t mask;
    int64_t at_ms;
    int64_t dur_ms;
    bool seen;
    /* Down until a matching "rel"; dur_ms is ignored meanwhile. A separate
     * flag because a zero-duration press must stay a no-op. */
    bool held;
} CdjPnlLinkPress;

struct CdjPnlLinkState {
    MemoryRegion iomem;
    uint16_t reg[CDJ_PNL_LINK_SIZE / 4];
    unsigned frame_len;
    unsigned payload_len;
    uint8_t sync;
    uint8_t *rx;
    unsigned rxhead;                /* next byte the guest will pop        */
    unsigned rxlen;                 /* bytes still queued                  */
    unsigned txlen;                 /* bytes staged toward the next frame  */
    uint8_t *tx;
    CdjPnlLinkPress *press;
    unsigned npress;
    int keyfd;                      /* live key socket; -2 = not opened yet */
    unsigned rot_off;               /* report byte the select knob counts in */
    uint8_t rot_val;
    bool rot_live;
    uint8_t *lvl_val;                /* analogue fields: absolute, held     */
    bool *lvl_live;
    uint64_t frames_sent;            /* replies the panel loaded            */
    uint64_t frames_recv;            /* complete frames MAIN transmitted    */
    uint64_t starved;                /* SCFRDR reads with an empty FIFO     */
    uint64_t reads;
    uint64_t writes;
    Notifier exit;
    const CdjPnlLinkHooks *hooks;
    void *extra;
    bool reglog;
    uint64_t dbg_first, dbg_n;
    bool fill_on;
    uint8_t fill;
    unsigned fill_lo, fill_hi;
    uint64_t wcount[64], wnz[64];
};

/* 8-bit sum with end-around carry, as every board's validator computes it. */
static uint8_t cdj_pnl_link_checksum(const CdjPnlLinkState *s,
                                     const uint8_t *frame)
{
    unsigned sum = 0;
    unsigned i;

    for (i = 0; i < s->payload_len; i++) {
        sum += frame[i];
        if (sum & 0x100) {
            sum = (sum & 0xFF) + 1;
        }
    }
    return (uint8_t)sum;
}

void cdj_pnl_link_press(CdjPnlLinkState *s, unsigned off, uint8_t mask,
                        int64_t at_ms, int64_t dur_ms)
{
    s->press = g_renew(CdjPnlLinkPress, s->press, s->npress + 1);
    s->press[s->npress++] = (CdjPnlLinkPress) {
        .off = off, .mask = mask, .at_ms = at_ms, .dur_ms = dur_ms,
    };
}

static void cdj_pnl_link_parse_presses(CdjPnlLinkState *s)
{
    const char *spec = getenv("CDJ_PANEL_PRESS");
    g_auto(GStrv) items = NULL;
    unsigned i;

    if (!spec || !*spec) {
        return;
    }
    items = g_strsplit(spec, ",", 0);
    for (i = 0; items[i]; i++) {
        g_auto(GStrv) f = g_strsplit(items[i], ":", 0);
        unsigned off;

        if (!f[0] || !f[1] || !f[2] || !f[3]) {
            warn_report("panel: ignoring malformed CDJ_PANEL_PRESS entry '%s' "
                        "(want <off>:<mask>:<at_ms>:<dur_ms>)", items[i]);
            continue;
        }
        off = (unsigned)strtoul(f[0], NULL, 0);
        if (off >= s->payload_len) {
            warn_report("panel: CDJ_PANEL_PRESS offset 0x%x is outside the "
                        "%u-byte report, ignored", off, s->payload_len);
            continue;
        }
        s->press = g_renew(CdjPnlLinkPress, s->press, s->npress + 1);
        s->press[s->npress++] = (CdjPnlLinkPress) {
            .off    = off,
            .mask   = (uint8_t)strtoul(f[1], NULL, 0),
            .at_ms  = (int64_t)strtoll(f[2], NULL, 0),
            .dur_ms = (int64_t)strtoll(f[3], NULL, 0),
        };
    }
}

/* Live presses from the key socket (cdj_panelkeys.h), drained once per
 * frame. */
static void cdj_pnl_link_drain_keys(CdjPnlLinkState *s, int64_t now)
{
    char msg[64];
    ssize_t n;

    if (s->keyfd == -2) {
        s->keyfd = cdj_panelkey_bind(getenv(CDJ_PANELKEY_ENV));
        if (s->keyfd >= 0) {
            info_report("panel: live keys on %s (udp %u)",
                        getenv(CDJ_PANELKEY_ENV),
                        cdj_panelsock_port(getenv(CDJ_PANELKEY_ENV)));
        } else if (getenv(CDJ_PANELKEY_ENV)) {
            /* Usually a port inside a Windows reserved range. */
            warn_report("panel: could NOT bind %s (udp %u) -- this deck will "
                        "ignore every key. On Windows check "
                        "'netsh int ipv4 show excludedportrange protocol=udp'",
                        getenv(CDJ_PANELKEY_ENV),
                        cdj_panelsock_port(getenv(CDJ_PANELKEY_ENV)));
        }
    }
    if (s->keyfd < 0) {
        return;
    }
    while ((n = recv(s->keyfd, msg, sizeof(msg) - 1, 0)) > 0) {
        char op[16] = "or";
        unsigned off, dur;
        int val;

        msg[n] = '\0';
        if (s->hooks && s->hooks->drain_extra
            && s->hooks->drain_extra(s, s->extra, msg, now)) {
            continue;
        }
        if (sscanf(msg, "%i:%i:%u:%15s", &off, &val, &dur, op) < 3) {
            warn_report("panel: bad live key '%s'", msg);
            continue;
        }
        if (off >= s->payload_len) {
            warn_report("panel: live key offset 0x%x is outside the report", off);
            continue;
        }
        info_report("panel: live key '%s' at %" PRId64 " ms", msg, now);
        if (!strcmp(op, "rot")) {
            /* The select knob is a counter the firmware diffs between
             * frames, so it persists. Moving it to a new offset restarts it. */
            if (s->rot_off != off) {
                s->rot_off = off;
                s->rot_val = 0;
                s->rot_live = true;
                info_report("panel: rotary now on report[0x%02x]", off);
            }
            s->rot_val = (uint8_t)(s->rot_val + val);
            info_report("panel: rotary %+d -> report[0x%02x] = 0x%02x",
                        val, off, s->rot_val);
            continue;
        }
        if (!strcmp(op, "lvl")) {
            /* Analogue field: an absolute byte value held until changed. */
            s->lvl_val[off] = (uint8_t)val;
            if (!s->lvl_live[off]) {
                s->lvl_live[off] = true;
                info_report("panel: report[0x%02x] is now an analogue field",
                            off);
            }
            continue;
        }
        /*
         * Held keys (CUE held, search scrub):
         *   op "hold"  press and keep it down (dur_ms ignored)
         *   op "rel"   release it
         */
        if (!strcmp(op, "rel")) {
            unsigned j;

            for (j = 0; j < s->npress; j++) {
                CdjPnlLinkPress *p = &s->press[j];

                if (p->held && p->off == off && p->mask == (uint8_t)val) {
                    p->held = false;
                    /* At least 25 ms, so a tap shorter than a panel frame
                     * is still seen. */
                    p->dur_ms = MAX(now - p->at_ms, 25);
                    info_report("panel: report[0x%02x] &= ~0x%02x at %" PRId64
                                " ms (held %" PRId64 " ms)",
                                p->off, p->mask, now, p->dur_ms);
                }
            }
            if (s->hooks && s->hooks->key_up) {
                s->hooks->key_up(s, s->extra, off, (unsigned)val, now);
            }
            continue;
        }
        if (s->hooks && s->hooks->key_down) {
            s->hooks->key_down(s, s->extra, off, (unsigned)val, dur,
                               !strcmp(op, "hold"), now);
        }
        s->press = g_renew(CdjPnlLinkPress, s->press, s->npress + 1);
        s->press[s->npress++] = (CdjPnlLinkPress) {
            .off = off, .mask = (uint8_t)val, .at_ms = now,
            .dur_ms = dur ? dur : 120,
            .held = !strcmp(op, "hold"),
        };
    }
}

/* Build the panel's answer to the frame MAIN just sent. */
static void cdj_pnl_link_build_reply(CdjPnlLinkState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / SCALE_MS;
    unsigned i;

    cdj_pnl_link_drain_keys(s, now);
    if (s->hooks && s->hooks->poll) {
        s->hooks->poll(s, s->extra, now);
    }
    memset(s->rx, 0, s->frame_len);
    /* Board defaults and live analogue overrides go before the keys, which
     * OR into the payload. */
    if (s->hooks && s->hooks->build_defaults) {
        s->hooks->build_defaults(s, s->extra, s->rx);
    }
    for (i = 0; i < s->payload_len; i++) {
        if (s->lvl_live[i]) {
            s->rx[i] = s->lvl_val[i];
        }
    }
    for (i = 0; i < s->npress; i++) {
        CdjPnlLinkPress *p = &s->press[i];

        /* A held key is down from at_ms until its "rel" arrives. */
        if (now >= p->at_ms && (p->held || now < p->at_ms + p->dur_ms)) {
            s->rx[p->off] |= p->mask;
            if (!p->seen) {
                p->seen = true;
                info_report("panel: report[0x%02x] |= 0x%02x at %" PRId64 " ms",
                            p->off, p->mask, now);
            }
        }
    }
    if (s->rot_live && s->rot_off < s->payload_len) {
        s->rx[s->rot_off] = s->rot_val;
    }
    /*
     * CDJ_PANEL_FILL=<byte>[:<lo>:<hi>]: fill zero reply bytes in [lo,hi)
     * with <byte>, for finding undocumented fields.
     */
    if (s->fill_on) {
        unsigned lo;

        for (lo = s->fill_lo; lo < s->fill_hi && lo < s->payload_len; lo++) {
            if (!s->rx[lo]) {
                s->rx[lo] = s->fill;
            }
        }
    }
    if (s->hooks && s->hooks->build_last) {
        s->hooks->build_last(s, s->extra, s->rx, now);
    }
    s->rx[s->payload_len] = cdj_pnl_link_checksum(s, s->rx);
    s->rx[s->payload_len + 1] = s->sync;
    s->rxhead = 0;
    s->rxlen = s->frame_len;
    s->frames_sent++;
}

static uint64_t cdj_pnl_link_read(void *opaque, hwaddr off, unsigned size)
{
    CdjPnlLinkState *s = opaque;
    unsigned idx = (off & (CDJ_PNL_LINK_SIZE - 1)) >> 2;

    s->reads++;
    switch (off & 0xFFC) {
    case CDJ_PNL_LINK_SCFRDR:
        if (!s->rxlen) {
            /* Nothing queued; MAIN's frame will fail its sync check. */
            s->starved++;
            return 0;
        }
        s->rxlen--;
        return s->rx[s->rxhead++];

    case CDJ_PNL_LINK_SCFSR:
        return s->reg[idx] | (s->rxlen ? CDJ_PNL_LINK_SCFSR_RDF : 0);

    case CDJ_PNL_LINK_SCFDR:
        /* RX count in bits 6:0, TX count in bits 14:8 (TX always empty). */
        return s->rxlen & 0x7F;

    default:
        return s->reg[idx];
    }
}

static void cdj_pnl_link_write(void *opaque, hwaddr off, uint64_t val,
                               unsigned size)
{
    CdjPnlLinkState *s = opaque;
    unsigned idx = (off & (CDJ_PNL_LINK_SIZE - 1)) >> 2;

    s->writes++;
    /* CDJ_PANEL_REGLOG: per-register write census, printed at exit. */
    if (s->reglog) {
        unsigned o = (off & 0xFFC) >> 2;

        if (o < 64) {
            s->wcount[o]++;
            if (val) {
                s->wnz[o]++;
            }
        }
    }
    switch (off & 0xFFC) {
    case CDJ_PNL_LINK_SCFTDR:
        if (s->hooks && s->hooks->link_selected &&
            s->hooks->link_selected(s->extra)) {
            if (!s->rxlen) {
                s->rxhead = 0;
            }
            s->rx[s->rxhead + s->rxlen++] = s->hooks->link_byte(s->extra, val);
            return;
        }
        s->tx[s->txlen++] = (uint8_t)val;
        if (s->txlen == s->frame_len) {
            s->txlen = 0;
            s->frames_recv++;
            /* CDJ_PANEL_DEBUG=<first>[:<count>]: log MAIN frames from
             * #first (default count 3). */
            if (s->dbg_n
                && s->frames_recv >= s->dbg_first
                && s->frames_recv < s->dbg_first + s->dbg_n) {
                g_autofree char *hex = g_malloc(3 * s->frame_len + 1);
                unsigned k;

                for (k = 0; k < s->frame_len; k++) {
                    snprintf(hex + 3 * k, 4, "%02x ", s->tx[k]);
                }
                info_report("panel: MAIN frame #%" PRIu64 ": %s",
                            s->frames_recv, hex);
            }
            if (s->hooks && s->hooks->on_frame) {
                s->hooks->on_frame(s, s->extra, s->tx);
            }
            cdj_pnl_link_build_reply(s);
        }
        return;

    case CDJ_PNL_LINK_SCFSR:
        /* Hardware-owned bits are write-0-to-clear only. */
        s->reg[idx] &= (uint16_t)val | (uint16_t)~CDJ_PNL_LINK_SCFSR_HW;
        /* TX drains instantly, so TEND and TDFE stay set. */
        s->reg[idx] |= CDJ_PNL_LINK_SCFSR_TEND | CDJ_PNL_LINK_SCFSR_TDFE;
        return;

    case CDJ_PNL_LINK_SCFCR:
        if (val & CDJ_PNL_LINK_SCFCR_RFRST) {
            s->rxlen = 0;
            s->rxhead = 0;
        }
        if (val & CDJ_PNL_LINK_SCFCR_TFRST) {
            s->txlen = 0;
        }
        s->reg[idx] = (uint16_t)val;
        return;

    case CDJ_PNL_LINK_SCLSR:
        s->reg[idx] &= (uint16_t)val;       /* ORER is write-0-to-clear */
        return;

    default:
        s->reg[idx] = (uint16_t)val;
        return;
    }
}

static const MemoryRegionOps cdj_pnl_link_ops = {
    .read = cdj_pnl_link_read,
    .write = cdj_pnl_link_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void cdj_pnl_link_summary(Notifier *n, void *unused)
{
    CdjPnlLinkState *s = container_of(n, CdjPnlLinkState, exit);
    unsigned i;

    if (s->reglog) {
        for (i = 0; i < 64; i++) {
            if (s->wcount[i]) {
                info_report("panel reg: off 0x%03X  %" PRIu64 " writes, "
                            "%" PRIu64 " non-zero", i * 4,
                            s->wcount[i], s->wnz[i]);
            }
        }
    }
    info_report("panel: %" PRIu64 " reads, %" PRIu64 " writes, "
                "%" PRIu64 " frames from MAIN, %" PRIu64 " replies loaded, "
                "%" PRIu64 " starved SCFRDR reads",
                s->reads, s->writes, s->frames_recv, s->frames_sent,
                s->starved);
    if (s->hooks && s->hooks->summary) {
        s->hooks->summary(s, s->extra);
    }
}

static const VMStateDescription vmstate_cdj_pnl_press = {
    .name = "cdj-pnl-press",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(off, CdjPnlLinkPress),
        VMSTATE_UINT8(mask, CdjPnlLinkPress),
        VMSTATE_INT64(at_ms, CdjPnlLinkPress),
        VMSTATE_INT64(dur_ms, CdjPnlLinkPress),
        VMSTATE_BOOL(seen, CdjPnlLinkPress),
        VMSTATE_BOOL(held, CdjPnlLinkPress),
        VMSTATE_END_OF_LIST()
    }
};

/* Every live key appends a press, so the saved list is longer than the one
 * a fresh machine starts with: the count comes first and the list is
 * allocated to it. */
static int cdj_pnl_link_pre_load(void *opaque)
{
    CdjPnlLinkState *s = opaque;

    g_free(s->press);
    s->press = NULL;
    return 0;
}

static const VMStateDescription vmstate_cdj_pnl_link = {
    .name = "cdj-pnl-link",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = cdj_pnl_link_pre_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16_ARRAY(reg, CdjPnlLinkState, CDJ_PNL_LINK_SIZE / 4),
        VMSTATE_UINT8(sync, CdjPnlLinkState),
        VMSTATE_VBUFFER_UINT32(rx, CdjPnlLinkState, 0, NULL, frame_len),
        VMSTATE_UINT32(rxhead, CdjPnlLinkState),
        VMSTATE_UINT32(rxlen, CdjPnlLinkState),
        VMSTATE_UINT32(txlen, CdjPnlLinkState),
        VMSTATE_VBUFFER_UINT32(tx, CdjPnlLinkState, 0, NULL, frame_len),
        VMSTATE_UINT32(npress, CdjPnlLinkState),
        {
            .name = "press",
            .vmsd = &vmstate_cdj_pnl_press,
            .num_offset = vmstate_offset_value(CdjPnlLinkState, npress, uint32_t),
            .size = sizeof(CdjPnlLinkPress),
            .flags = VMS_STRUCT | VMS_VARRAY_UINT32 | VMS_ALLOC | VMS_POINTER,
            .offset = vmstate_offset_pointer(CdjPnlLinkState, press, CdjPnlLinkPress),
        },
        VMSTATE_UINT8(rot_val, CdjPnlLinkState),
        VMSTATE_BOOL(rot_live, CdjPnlLinkState),
        VMSTATE_VBUFFER_UINT32(lvl_val, CdjPnlLinkState, 0, NULL, payload_len),
        VMSTATE_VBUFFER_UINT32(lvl_live, CdjPnlLinkState, 0, NULL, payload_len),
        VMSTATE_END_OF_LIST()
    }
};

void *cdj_pnl_link_init(MemoryRegion *sysmem, hwaddr addr, const char *name,
                        unsigned frame_len, uint8_t sync,
                        const CdjPnlLinkHooks *hooks, size_t extra_size)
{
    CdjPnlLinkState *s = g_new0(CdjPnlLinkState, 1);
    MemoryRegion *alias = g_new(MemoryRegion, 1);
    g_autofree char *alias_name = g_strdup_printf("%s-a7", name);

    s->frame_len = frame_len;
    s->payload_len = frame_len - 2;
    s->sync = sync;
    s->hooks = hooks;
    s->extra = extra_size ? g_malloc0(extra_size) : NULL;
    s->rx = g_new0(uint8_t, MAX(frame_len, CDJ_PNL_LINK_FIFO));
    s->tx = g_new0(uint8_t, frame_len);
    s->lvl_val = g_new0(uint8_t, s->payload_len);
    s->lvl_live = g_new0(bool, s->payload_len);
    s->keyfd = -2;                  /* bound lazily, on the first panel frame */
    s->reglog = getenv("CDJ_PANEL_REGLOG") != NULL;
    {
        const char *f = getenv("CDJ_PANEL_FILL");

        if (f) {
            s->fill_on = true;
            s->fill = (uint8_t)strtoul(f, (char **)&f, 0);
            s->fill_hi = 0xFFFF;
            if (*f == ':') {
                s->fill_lo = strtoul(f + 1, (char **)&f, 0);
                if (*f == ':') {
                    s->fill_hi = strtoul(f + 1, NULL, 0);
                }
            }
            warn_report("panel: filling reply bytes [%u,%u) with 0x%02X",
                        s->fill_lo, s->fill_hi, s->fill);
        }
    }
    {
        const char *d = getenv("CDJ_PANEL_DEBUG");

        if (d) {
            s->dbg_first = strtoull(d, (char **)&d, 0);
            if (!s->dbg_first) {
                s->dbg_first = 1;
            }
            s->dbg_n = (*d == ':') ? strtoull(d + 1, NULL, 0) : 3;
            if (!s->dbg_n) {
                s->dbg_n = 3;
            }
        } else {
            s->dbg_first = 1;
        }
    }
    cdj_pnl_link_parse_presses(s);
    vmstate_register_any(NULL, &vmstate_cdj_pnl_link, s);
    s->reg[CDJ_PNL_LINK_SCFSR >> 2] = CDJ_PNL_LINK_SCFSR_TEND
                                     | CDJ_PNL_LINK_SCFSR_TDFE;
    s->exit.notify = cdj_pnl_link_summary;
    cdj_add_exit_report(&s->exit);

    memory_region_init_io(&s->iomem, NULL, &cdj_pnl_link_ops, s, name,
                          CDJ_PNL_LINK_SIZE);
    memory_region_add_subregion(sysmem, addr, &s->iomem);
    /* Needed: cdj_dmac_run() addresses through A7ADDR(). */
    memory_region_init_alias(alias, NULL, alias_name, &s->iomem,
                             0, CDJ_PNL_LINK_SIZE);
    memory_region_add_subregion(sysmem, A7ADDR(addr), alias);
    return s->extra;
}
