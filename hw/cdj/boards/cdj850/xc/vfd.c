/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "vfd.h"
#include "ui/console.h"
#include "ui/surface.h"
#include "trace.h"

/* DATA FL layout reconstruction; SCIF1 text comes from panel_peer.c.
 * SSU0 traffic is audio-DSP traffic, retained only in vfd_text diagnostics.
 * See notes/cdj850-display-roadmap.md for reference photos and limitations. */
#include "vfd_render.h"

#define VFD_COLS        40
#define VFD_W           FL_WIDTH
#define VFD_H           FL_HEIGHT

static struct {
    QemuConsole *con;
    FlState display;
    char line[2][VFD_COLS + 1];
    bool dirty;
    bool has_panel_text;
    bool synced;
    uint8_t frame[VFD_FRAME_LEN];
    uint32_t nframe;
    uint64_t frames;
} vfd;

static void vfd_draw(DisplaySurface *ds)
{
    FlCanvas canvas = {
        .pixels = surface_data(ds), .width = surface_width(ds),
        .height = surface_height(ds), .stride = surface_stride(ds),
    };
    fl_render(&canvas, &vfd.display);
}

static bool vfd_gfx_update(void *opaque)
{
    (void)opaque;
    if (!vfd.dirty || !vfd.con) {
        return true;
    }
    vfd.dirty = false;
    vfd_draw(qemu_console_surface(vfd.con));
    qemu_console_update(vfd.con, 0, 0, VFD_W, VFD_H);
    return true;
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

static int vfd_post_load(void *opaque, int version_id)
{
    (void)opaque;
    (void)version_id;
    vfd.dirty = true;
    return 0;
}

static const VMStateDescription vmstate_cdj850_vfd = {
    .name = "cdj850-vfd",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = vfd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_UNSAFE(display, typeof(vfd), 0, sizeof(vfd.display)),
        VMSTATE_BUFFER_UNSAFE(line, typeof(vfd), 0, sizeof(vfd.line)),
        VMSTATE_BOOL(has_panel_text, typeof(vfd)),
        VMSTATE_BOOL(synced, typeof(vfd)),
        VMSTATE_UINT8_ARRAY(frame, typeof(vfd), VFD_FRAME_LEN),
        VMSTATE_UINT32(nframe, typeof(vfd)),
        VMSTATE_UINT64(frames, typeof(vfd)),
        VMSTATE_END_OF_LIST()
    }
};

void vfd_init(void)
{
    if (vfd.con) {
        return;
    }
    vfd.has_panel_text = false;
    snprintf(vfd.line[0], sizeof(vfd.line[0]), "CDJ-850 PANEL LINK -- BOOT");
    snprintf(vfd.line[1], sizeof(vfd.line[1]), "FL: waiting for frames");
    vfd.dirty = true;
    vfd.con = qemu_graphic_console_create(NULL, 0, &vfd_ops, NULL);
    qemu_console_set_surface(vfd.con,
                             qemu_create_displaysurface(VFD_W, VFD_H));
    qemu_console_resize(vfd.con, VFD_W, VFD_H);
    vmstate_register_any(NULL, &vmstate_cdj850_vfd, &vfd);
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

void vfd_note_panel_text(uint8_t mode, const char *l0, const char *l1,
                         const char *l2)
{
    const char *s0 = (l0 && l0[0]) ? l0 : "";
    const char *s1 = (l1 && l1[0]) ? l1 : "";
    const char *s2 = (l2 && l2[0]) ? l2 : "";

    vfd.display.has_text = true;
    vfd.display.mode = mode;
    snprintf(vfd.display.rows[0], sizeof(vfd.display.rows[0]), "%s", s0);
    snprintf(vfd.display.rows[1], sizeof(vfd.display.rows[1]), "%s", s1);
    snprintf(vfd.display.rows[2], sizeof(vfd.display.rows[2]), "%s", s2);
    vfd.has_panel_text = true;
    if (!s0[0] && s1[0] && !s2[0]) {
        snprintf(vfd.line[0], sizeof(vfd.line[0]), "%.*s", VFD_COLS, s1);
        snprintf(vfd.line[1], sizeof(vfd.line[1]), "SCIF1 mode 0x%02x", mode);
    } else {
        snprintf(vfd.line[0], sizeof(vfd.line[0]), "%.*s", VFD_COLS,
                 s0[0] ? s0 : (s1[0] ? s1 : "-"));
        if (s1[0] && s2[0]) {
            snprintf(vfd.line[1], sizeof(vfd.line[1]), "%.*s | %.*s",
                     18, s1, 18, s2);
        } else {
            snprintf(vfd.line[1], sizeof(vfd.line[1]), "%.*s", VFD_COLS,
                     s1[0] ? s1 : s2);
        }
    }
    vfd.dirty = true;
}

void vfd_note_track_number(bool known, uint8_t number)
{
    vfd.display.track_known = known;
    vfd.display.track_number = number;
    vfd.dirty = true;
}

void vfd_note_time(const Cdj850Time *time)
{
    vfd.display.time = *time;
    vfd.dirty = true;
}

void vfd_note_bpm(bool known, uint16_t bpm)
{
    vfd.display.bpm_known = known;
    vfd.display.bpm = bpm;
    vfd.dirty = true;
}

void vfd_note_pitch(const Cdj850Pitch *pitch)
{
    vfd.display.pitch = *pitch;
    vfd.dirty = true;
}

void vfd_note_master_tempo(bool enabled)
{
    vfd.display.master_tempo = enabled;
    vfd.dirty = true;
}

void vfd_note_auto_cue(bool enabled)
{
    vfd.display.auto_cue = enabled;
    vfd.dirty = true;
}

void vfd_note_info(bool enabled)
{
    vfd.display.info_lamp = enabled;
    vfd.dirty = true;
}

void vfd_note_browse(bool enabled)
{
    vfd.display.browse_lamp = enabled;
    vfd.dirty = true;
}

void vfd_note_menu(bool enabled)
{
    vfd.display.menu_lamp = enabled;
    vfd.dirty = true;
}

void vfd_note_tag_list(bool enabled)
{
    vfd.display.tag_list_lamp = enabled;
    vfd.dirty = true;
}

void vfd_note_playing(const Cdj850Playing *playing)
{
    vfd.display.playing = *playing;
    vfd.dirty = true;
}

void vfd_note_panel_tags(const uint8_t tags[3])
{
    memcpy(vfd.display.tags, tags, sizeof(vfd.display.tags));
    vfd.dirty = true;
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
        if (!vfd.has_panel_text) {
            snprintf(vfd.line[1], sizeof(vfd.line[1]),
                     "TX %02X %02X %02X %02X %02X %02X #%" PRIu64,
                     vfd.frame[0], vfd.frame[1], vfd.frame[2],
                     vfd.frame[3], vfd.frame[4], vfd.frame[5], vfd.frames);
            vfd.dirty = true;
        }
    }
}
