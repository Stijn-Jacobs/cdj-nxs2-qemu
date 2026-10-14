/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "vfd.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "ui/vgafont.h"
#include "trace.h"

/*
 * CDJ-850 VFD text display: the deck window, same single-window approach
 * as the CDJ-900's display_m16c.c but text, not pixels. The panel CPU
 * renders MAIN's frames onto the fluorescent glass itself; MAIN only sends
 * frame bytes (captured from the SSU TX stream by panel_link.c), so this
 * file keeps the decoded text lines and draws them with the 8x16 VGA font.
 *
 * Frame format (Task VFD-1, see vfd.h + notes/cdj850-bringup.md): the wire
 * carries binary control frames, not text -- the 6-byte panel exchange
 * (TX[0].bit7 always set by the TX-prep helper at 0x0C005D54) plus 16-bit
 * command words. The panel CPU holds the font; MAIN sends indices only.
 * This window therefore passes the exchange frame bytes through as hex,
 * synced on the builder-proven TX[0].bit7 invariant. Upload-phase bytes
 * (panel-init blob) and 16-bit words interleave in the same stream: bytes
 * that cannot continue a frame are ignored, and any bit7-set byte
 * resyncs, so the window locks onto exchange frames once polling runs.
 *
 * Glyph policy: the FL character map comes from MAIN's firmware FL tables
 * as each frame format is mapped; bytes outside the known map render as
 * '?' and are logged once (unknown_glyphs below). The current hex
 * passthrough only ever emits 0-9/A-F/#/space, all in-map, so '?' never
 * fires on present traffic; the policy stays for the DisplayTask future.
 */

#define VFD_COLS        40
#define VFD_ROWS        2
#define VFD_GW          8
#define VFD_GH          16
#define VFD_W           (VFD_COLS * VFD_GW)
#define VFD_H           (VFD_ROWS * VFD_GH + 8)

#define VFD_BG          0xFF0A0A12
#define VFD_FG          0xFF9FE8FF
#define VFD_DIM         0xFF5A6A7A

static struct {
    QemuConsole *con;
    char line[2][VFD_COLS + 1];
    bool dirty;
    bool unknown_logged[256];
    bool synced;
    uint8_t frame[VFD_FRAME_LEN];
    unsigned nframe;
    unsigned long frames;
} vfd;

static void vfd_px(DisplaySurface *ds, int x, int y, uint32_t c)
{
    uint8_t *base;

    if (x < 0 || y < 0 || x >= surface_width(ds) ||
        y >= surface_height(ds)) {
        return;
    }
    base = (uint8_t *)surface_data(ds) + y * surface_stride(ds) + x * 4;
    *(uint32_t *)base = c;
}

static void vfd_char(DisplaySurface *ds, int x, int y, unsigned char ch,
                     uint32_t fg)
{
    const uint8_t *g = vgafont16 + (unsigned)ch * 16;
    int row, col;

    if (ch < 32 || ch > 126) {
        if (!vfd.unknown_logged[ch]) {
            vfd.unknown_logged[ch] = true;
            info_report("cdj850: vfd unknown glyph 0x%02x rendered as '?'",
                        ch);
        }
        ch = '?';
        g = vgafont16 + (unsigned)ch * 16;
    }
    for (row = 0; row < 16; row++) {
        uint8_t bits = g[row];

        for (col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                vfd_px(ds, x + col, y + row, fg);
            }
        }
    }
}

static void vfd_string(DisplaySurface *ds, int x, int y, const char *str,
                       uint32_t fg)
{
    for (; *str; str++, x += VFD_GW) {
        vfd_char(ds, x, y, (unsigned char)*str, fg);
    }
}

static void vfd_draw(DisplaySurface *ds)
{
    unsigned r;

    for (int y = 0; y < surface_height(ds); y++) {
        for (int x = 0; x < surface_width(ds); x++) {
            vfd_px(ds, x, y, VFD_BG);
        }
    }
    for (r = 0; r < VFD_ROWS; r++) {
        vfd_string(ds, 4, 4 + r * (VFD_GH + 2), vfd.line[r],
                   r == 0 ? VFD_FG : VFD_DIM);
    }
}

static void vfd_gfx_update(void *opaque)
{
    (void)opaque;
    if (!vfd.dirty || !vfd.con) {
        return;
    }
    vfd.dirty = false;
    vfd_draw(qemu_console_surface(vfd.con));
    dpy_gfx_update(vfd.con, 0, 0, VFD_W, VFD_H);
}

static void vfd_invalidate(void *opaque)
{
    (void)opaque;
    vfd.dirty = true;
}

static const GraphicHwOps vfd_ops = {
    .invalidate = vfd_invalidate,
    .gfx_update = vfd_gfx_update,
};

void vfd_init(void)
{
    if (vfd.con) {
        return;
    }
    snprintf(vfd.line[0], sizeof(vfd.line[0]), "CDJ-850 PANEL LINK -- BOOT");
    snprintf(vfd.line[1], sizeof(vfd.line[1]), "FL: waiting for frames");
    vfd.dirty = true;
    vfd.con = graphic_console_init(NULL, 0, &vfd_ops, NULL);
    dpy_gfx_replace_surface(vfd.con,
                            qemu_create_displaysurface(VFD_W, VFD_H));
    qemu_console_resize(vfd.con, VFD_W, VFD_H);
    vfd.dirty = true;
}

void vfd_text(char *line0, unsigned n0, char *line1, unsigned n1)
{
    if (line0 && n0) {
        snprintf(line0, n0, "%s", vfd.line[0]);
    }
    if (line1 && n1) {
        snprintf(line1, n1, "%s", vfd.line[1]);
    }
}

void vfd_note_tx(uint8_t b)
{
    if (b & VFD_TX0_VALID) {
        /*
         * Candidate frame start: the TX-prep helper (MAIN 0x0C005D54)
         * sets TX[0].bit7 unconditionally, so a bit7-set byte opens a
         * new 6-byte exchange window, aborting any partial one.
         */
        vfd.nframe = 0;
        vfd.synced = true;
    }
    if (!vfd.synced || vfd.nframe >= VFD_FRAME_LEN) {
        /*
         * Not in a frame: pre-sync upload bytes, completed-frame
         * 16-bit interleave, or anything past a full window. Ignored;
         * the next bit7-set byte resyncs.
         */
        return;
    }
    vfd.frame[vfd.nframe++] = b;
    if (vfd.nframe == VFD_FRAME_LEN) {
        vfd.frames++;
        snprintf(vfd.line[1], sizeof(vfd.line[1]),
                 "TX %02X %02X %02X %02X %02X %02X #%lu",
                 vfd.frame[0], vfd.frame[1], vfd.frame[2],
                 vfd.frame[3], vfd.frame[4], vfd.frame[5], vfd.frames);
        vfd.dirty = true;
    }
}
