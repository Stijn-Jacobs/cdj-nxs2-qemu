/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/*
 * Status panel for the MAIN board: a diagnostic console showing port pins,
 * the SCIFA4 peer download, NOR provisioning and the firmware's RAM console
 * buffer. It is not the CDJ's LCD; that is drawn by the GUI processor
 * (sh7269gui.c). MAIN itself never touches the LCDC at 0xFE940000.
 */
#define CDJ_PANEL_W     720
#define CDJ_PANEL_H     420
#define CDJ_GLYPH_W     8
#define CDJ_GLYPH_H     16

#define COL_BG      0xFF101216
#define COL_TEXT    0xFFC8D0D8
#define COL_DIM     0xFF6A737D
#define COL_TITLE   0xFFFFFFFF
#define COL_OK      0xFF35C46A
#define COL_WARN    0xFFE0B341
#define COL_OFF     0xFF33383D
#define COL_RED     0xFFE05252
#define COL_ACCENT  0xFF4FA3FF

typedef struct CdjPanelState {
    QemuConsole *con;
    bool inited;
    uint64_t frames;
    int64_t  t0_ms;
    Notifier exit;
} CdjPanelState;

static void cdj_px(DisplaySurface *ds, int x, int y, uint32_t c)
{
    uint8_t *base;

    if (x < 0 || y < 0 || x >= surface_width(ds) || y >= surface_height(ds)) {
        return;
    }
    base = (uint8_t *)surface_data(ds) + y * surface_stride(ds) + x * 4;
    *(uint32_t *)base = c;
}

static void cdj_fill(DisplaySurface *ds, int x, int y, int w, int h, uint32_t c)
{
    int i, j;

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            cdj_px(ds, x + i, y + j, c);
        }
    }
}

static void cdj_char(DisplaySurface *ds, int x, int y, unsigned char ch,
                     uint32_t fg)
{
    const uint8_t *g = vgafont16 + (unsigned)ch * CDJ_GLYPH_H;
    int row, col;

    for (row = 0; row < CDJ_GLYPH_H; row++) {
        uint8_t bits = g[row];
        for (col = 0; col < CDJ_GLYPH_W; col++) {
            if (bits & (0x80 >> col)) {
                cdj_px(ds, x + col, y + row, fg);
            }
        }
    }
}

static void cdj_text(DisplaySurface *ds, int x, int y, const char *str,
                     uint32_t fg)
{
    for (; *str; str++, x += CDJ_GLYPH_W) {
        cdj_char(ds, x, y, (unsigned char)*str, fg);
    }
}

/* A small on/off pill, used for the pins the firmware actually toggles. */
static void cdj_lamp(DisplaySurface *ds, int x, int y, bool on, uint32_t oncol,
                     const char *label)
{
    cdj_fill(ds, x, y + 3, 10, 10, on ? oncol : COL_OFF);
    cdj_text(ds, x + 16, y, label, on ? COL_TEXT : COL_DIM);
}

static uint8_t cdj_pfc_reg(unsigned off)
{
    return cdj_pfc_singleton ? cdj_pfc_singleton->reg[off] : 0;
}

static unsigned cdj_pfc_writes(unsigned off)
{
    return cdj_pfc_singleton ? cdj_pfc_singleton->wr_count[off] : 0;
}

static void cdj_panel_update(void *opaque)
{
    CdjPanelState *s = opaque;
    DisplaySurface *ds;
    char line[128];
    uint8_t fl[16];
    uint8_t cbuf[80];
    unsigned i;
    int y;
    uint64_t tx;
    bool provisioned, erased;

    if (!s->con) {
        return;
    }
    ds = qemu_console_surface(s->con);
    if (!ds) {
        return;
    }

    cdj_fill(ds, 0, 0, CDJ_PANEL_W, CDJ_PANEL_H, COL_BG);

    cdj_text(ds, 16, 12, "Pioneer CDJ-2000NXS2  --  MAIN / Renesas SH7724",
             COL_TITLE);
    cdj_text(ds, 16, 30, "QEMU board cdj2000nxs2   (emulator state, not the "
             "device's browse LCD)", COL_DIM);
    cdj_fill(ds, 16, 50, CDJ_PANEL_W - 32, 1, COL_OFF);

    /* Booted = peer download done, device id in NOR, heartbeat pin toggling,
     * error LED off. */
    {
        uint8_t id[4];
        bool link_done, id_ok, alive, no_fault;

        cpu_physical_memory_read(0x7FE000, id, 3);
        link_done = cdj_scifa4 && cdj_scifa4->txbytes >= 54728;
        id_ok = (id[0] == 'P' && id[1] == 'D' && id[2] == 'J');
        alive = cdj_pfc_writes(0x166) > 50;   /* low: icount runs slowly */
        no_fault = (cdj_pfc_reg(0x130) & CDJ_LED_BIT) == 0;

        if (link_done && id_ok && alive && no_fault) {
            cdj_fill(ds, 16, 58, CDJ_PANEL_W - 32, 22, 0xFF14301E);
            if (cdj_scifa4 && cdj_scifa4->txbytes > 54728 * 2) {
                cdj_text(ds, 24, 60,
                         "BOOTED -- RTOS running; still retrying the SCIFA4 "
                         "peer download", COL_OK);
            } else {
                cdj_text(ds, 24, 60,
                         "BOOT COMPLETE -- RTOS running, idle (no disc / USB "
                         "/ panel attached)", COL_OK);
            }
        } else {
            cdj_fill(ds, 16, 58, CDJ_PANEL_W - 32, 22, 0xFF2A2417);
            snprintf(line, sizeof(line),
                     "BOOTING -- link:%s  id:%s  tick:%s  fault:%s",
                     link_done ? "ok" : "..", id_ok ? "ok" : "..",
                     alive ? "ok" : "..", no_fault ? "clear" : "LED ON");
            cdj_text(ds, 24, 60, line, COL_WARN);
        }
    }

    y = 90;
    cdj_text(ds, 16, y, "PIN STATE", COL_ACCENT);
    y += 22;

    /* Error LED: PJDR (+0x130) bit 3. Blinks at 1:29 duty in the fault loop. */
    cdj_lamp(ds, 24, y, (cdj_pfc_reg(0x130) & CDJ_LED_BIT) != 0, COL_RED,
             (cdj_pfc_reg(0x130) & CDJ_LED_BIT) ? "ERROR LED  ON  (fault loop)"
                                                : "ERROR LED  off (healthy)");
    y += 20;

    /* PWDR (+0x166) bit 5: heartbeat, one edge per RTOS timer tick. */
    snprintf(line, sizeof(line), "HEARTBEAT  PWDR.5   %u edges",
             cdj_pfc_writes(0x166));
    cdj_lamp(ds, 24, y, (cdj_pfc_reg(0x166) & 0x20) != 0, COL_OK, line);
    y += 20;

    snprintf(line, sizeof(line),
             "PEER READY PUDR.1 / PEDR.6  (asserted by board -- stubbed)");
    cdj_lamp(ds, 24, y, true, COL_WARN, line);
    y += 30;

    cdj_text(ds, 16, y, "SCIFA4 PEER LINK", COL_ACCENT);
    y += 22;
    tx = cdj_scifa4 ? cdj_scifa4->txbytes : 0;
    {
        /* The firmware re-sends the blob forever (the stubbed ready pins do
         * not satisfy the handshake), so show the pass count too. */
        uint64_t passes = tx / 54728;
        uint64_t cur = tx % 54728;
        int w = (int)(cur * (CDJ_PANEL_W - 80) / 54728);

        snprintf(line, sizeof(line),
                 "blob 0x093514..0x0A0ABC  pass %" PRIu64 ", %5" PRIu64
                 " / 54728 bytes", passes + 1, cur);
        cdj_text(ds, 24, y, line, COL_TEXT);
        y += 18;
        cdj_fill(ds, 24, y, CDJ_PANEL_W - 80, 8, COL_OFF);
        cdj_fill(ds, 24, y, w, 8, COL_ACCENT);
        y += 14;
        if (passes > 1) {
            snprintf(line, sizeof(line),
                     "re-sent %" PRIu64 "x -- peer never acknowledges "
                     "(handshake pins are stubbed)", passes);
            cdj_text(ds, 24, y, line, COL_WARN);
        }
    }
    y += 22;

    cdj_text(ds, 16, y, "NOR FLASH PROVISIONING  (read live from CS0)",
             COL_ACCENT);
    y += 22;
    cpu_physical_memory_read(0x7FE000, fl, 12);
    provisioned = (fl[0] == 'P' && fl[1] == 'D' && fl[2] == 'J');
    snprintf(line, sizeof(line), "device id  0x7FE000  \"%.12s\"",
             provisioned ? (char *)fl : "(unwritten)");
    cdj_text(ds, 24, y, line, provisioned ? COL_OK : COL_DIM);
    y += 18;

    cpu_physical_memory_read(0x7EFFFC, fl, 4);
    snprintf(line, sizeof(line), "block marker 0x7EFFFC \"%c%c%c%c\"",
             fl[0] >= 32 && fl[0] < 127 ? fl[0] : '.',
             fl[1] >= 32 && fl[1] < 127 ? fl[1] : '.',
             fl[2] >= 32 && fl[2] < 127 ? fl[2] : '.',
             fl[3] >= 32 && fl[3] < 127 ? fl[3] : '.');
    cdj_text(ds, 24, y, line, fl[0] == 'F' ? COL_OK : COL_DIM);
    y += 18;

    cpu_physical_memory_read(0x7F6000, fl, 4);
    erased = (fl[0] == 0xFF && fl[1] == 0xFF);
    snprintf(line, sizeof(line), "settings   0x7F6000  %s",
             erased ? "erased (0xFFFF terminator)" : "record present");
    cdj_text(ds, 24, y, line, COL_TEXT);
    y += 28;

    cdj_text(ds, 16, y, "FIRMWARE CONSOLE  (RAM buffer 0x0BFBDB60)",
             COL_ACCENT);
    y += 22;
    cpu_physical_memory_read(0x0BFBDB60, cbuf, sizeof(cbuf) - 1);
    cbuf[sizeof(cbuf) - 1] = 0;
    for (i = 0; i < sizeof(cbuf) - 1; i++) {
        if (cbuf[i] && (cbuf[i] < 0x20 || cbuf[i] >= 0x7F)) {
            cbuf[i] = '.';
        }
    }
    if (cbuf[0]) {
        snprintf(line, sizeof(line), "\"%s\"", (char *)cbuf);
        cdj_text(ds, 24, y, line, COL_TEXT);
    } else {
        cdj_text(ds, 24, y, "(empty -- firmware emits no log; "
                 "logging is gated off)", COL_DIM);
    }

    /* Redraw counter and spinner, to tell a frozen console from an idle one. */
    {
        static const char spin[] = "|/-\\";
        int64_t now = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);

        if (!s->t0_ms) {
            s->t0_ms = now;
        }
        s->frames++;
        snprintf(line, sizeof(line), "%c  redraw %" PRIu64 "   uptime %" PRId64 "s",
                 spin[s->frames & 3], s->frames, (now - s->t0_ms) / 1000);
        cdj_text(ds, CDJ_PANEL_W - 16 - (int)strlen(line) * CDJ_GLYPH_W,
                 CDJ_PANEL_H - 52, line, COL_ACCENT);
    }

    cdj_fill(ds, 16, CDJ_PANEL_H - 34, CDJ_PANEL_W - 32, 1, COL_OFF);
    cdj_text(ds, 16, CDJ_PANEL_H - 24,
             "browse LCD is driven by the GUI CPU (SH7269 / SH-2A) -- "
             "no QEMU target exists", COL_DIM);

    dpy_gfx_update_full(s->con);
}

static void cdj_panel_invalidate(void *opaque)
{
}

/* Log the frame count at exit, for headless runs; 1 means it drew once. */
static Notifier cdj_panel_exit;

static void cdj_panel_frames(Notifier *n, void *opaque)
{
    CdjPanelState *s = container_of(n, CdjPanelState, exit);

    info_report("panel: %" PRIu64 " frames rendered", s->frames);
}

static const GraphicHwOps cdj_panel_ops = {
    .invalidate = cdj_panel_invalidate,
    .gfx_update = cdj_panel_update,
};

void cdj_panel_init(void)
{
    CdjPanelState *s = g_new0(CdjPanelState, 1);
    DisplaySurface *ds;

    s->con = graphic_console_init(NULL, 0, &cdj_panel_ops, s);
    s->exit.notify = cdj_panel_frames;
    qemu_add_exit_notifier(&s->exit);
    ds = qemu_create_displaysurface(CDJ_PANEL_W, CDJ_PANEL_H);
    dpy_gfx_replace_surface(s->con, ds);
    qemu_console_resize(s->con, CDJ_PANEL_W, CDJ_PANEL_H);
}


