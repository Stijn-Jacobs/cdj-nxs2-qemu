/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sh7763.h"
#include "cdj_getenv.h"
#include "qemu/timer.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "bfin/bf531.h"
/*
 * CDJ-2000/CDJ-2000NXS display processor: the ADSP-BF531 on the other end
 * of SPORT1, in its own window. The core itself is plain C with no QEMU
 * dependency (hw/cdj/bfin/); this file is the QEMU side of it, a display
 * surface and a timer that steps the chip on its own clock.
 *
 * SPORT1 is not wired to MAIN yet (that is the SH7763 0xFF400000/0xFF500000
 * blocks another board file owns): CDJ_BF531_SPORT1 hands the display
 * processor one hex-encoded RX packet, delivered the moment it next arms
 * its receive DMA, standing in for MAIN's link until the two are connected.
 */

#define CDJ_BF531_SDRAM (16 * MiB)
#define CDJ_BF531_W     480
#define CDJ_BF531_H     255

/* One PPI/DMA0 unit's worth of the display processor's own 400 MHz clock
 * (see bf531.c's dma_start_unit): stepping this much a QEMU timer tick
 * keeps its own frame rate without syncing it to MAIN's icount. */
#define CDJ_BF531_QUANTUM (BF531_CCLK_HZ / 60)

typedef struct CdjBf531 {
    bf531 *chip;
    QemuConsole *con;
    QEMUTimer *tick;
    unsigned w, h;
    uint64_t frames;
    Notifier exit;
} CdjBf531;

static CdjBf531 cdj_bf531;

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

        dst[n] = 0xFF000000 | (((p >> 11) & 0x1F) * 255 / 31) << 16 |
                 (((p >> 5) & 0x3F) * 255 / 63) << 8 | ((p & 0x1F) * 255 / 31);
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

static void cdj_bf531_tick(void *opaque)
{
    CdjBf531 *s = opaque;
    bfin_stop stop = bf531_run(s->chip, CDJ_BF531_QUANTUM);

    if (stop == BFIN_STOP_UNDEF) {
        error_report("cdj_bf531: unimplemented instruction at 0x%08x",
                     bfin_trap_pc(bf531_core(s->chip)));
        return;
    }
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

static void cdj_bf531_report(Notifier *n, void *opaque)
{
    CdjBf531 *s = container_of(n, CdjBf531, exit);

    info_report("cdj_bf531: %" PRIu64 " PPI frames rendered", s->frames);
}

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

/* CDJ_BF531_UPD=<path to a Pioneer GUI .UPD section> turns the display
 * processor on; without it this board runs MAIN alone, as before. */
void cdj2000_display_init(void)
{
    CdjBf531 *s = &cdj_bf531;
    bf531_host host = { s, cdj_bf531_frame };
    const char *img_path = getenv("CDJ_BF531_UPD");
    const char *sport1 = getenv("CDJ_BF531_SPORT1");
    uint8_t *img;
    gsize len;

    if (!img_path) {
        return;
    }
    if (!g_file_get_contents(img_path, (gchar **)&img, &len, NULL)) {
        error_report("cdj_bf531: cannot read '%s'", img_path);
        return;
    }

    s->chip = bf531_new(CDJ_BF531_SDRAM, &host, stderr);
    if (sport1) {
        uint8_t pkt[128];
        size_t n = cdj_bf531_parse_hex(sport1, pkt, sizeof(pkt));

        bf531_sport1_rx(s->chip, pkt, n);
    }
    if (bf531_load_update(s->chip, img, len)) {
        error_report("cdj_bf531: '%s' is not an LDR boot stream", img_path);
        bf531_free(s->chip);
        s->chip = NULL;
        g_free(img);
        return;
    }
    g_free(img);

    s->w = CDJ_BF531_W;
    s->h = CDJ_BF531_H;
    s->con = graphic_console_init(NULL, 0, &cdj_bf531_ops, s);
    dpy_gfx_replace_surface(s->con, qemu_create_displaysurface(s->w, s->h));
    qemu_console_resize(s->con, s->w, s->h);

    s->exit.notify = cdj_bf531_report;
    qemu_add_exit_notifier(&s->exit);

    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_bf531_tick, s);
    timer_mod(s->tick, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}
