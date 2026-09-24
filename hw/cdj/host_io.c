/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/*
 * Deck state out, so a DJ controller can light its LEDs from the deck.
 * Datagrams go to the socket the MIDI relay binds; with no relay the sends
 * just fail.
 *
 *   CDJ_PANEL_STATESOCK=<path>  default: CDJ_PANEL_KEYSOCK with
 *                               "panel-keys" -> "panel-state"
 *
 *   frm beat=<n> bars=<n> tempo=<n> bpm=<n>
 *       from the MAIN->GUI heartbeat, after any emulator patching.
 *   pnl <80 hex>
 *       MAIN's latest 40-byte frame to the panel micro (built by 0x0844F808
 *       from the lamp block at 0x0B54C010). Byte 0: bit 0 PLAY, bit 1 CUE;
 *       the firmware blinks them by toggling the bit.
 *
 * Sent on change, repeated every second, throttled to 50 Hz except for beat
 * steps and lamp changes.
 */
static struct {
    bool parsed;
    int fd;
    char last[2][3 * CDJ_STATEOUT_PNL_FRAME];
    int64_t sent_ms[2];
} cdj_stateout = { .fd = -1 };

/* urgent bypasses the 20 ms throttle so beat LEDs stay on the beat. */
static void cdj_stateout_send(unsigned kind, const char *msg, bool urgent)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    if (!cdj_stateout.parsed) {
        const char *p = getenv("CDJ_PANEL_STATESOCK");
        g_autofree char *derived = NULL;

        cdj_stateout.parsed = true;
        if (!p || !*p) {
            const char *keys = getenv(CDJ_PANELKEY_ENV);
            const char *at = keys ? strstr(keys, "panel-keys") : NULL;

            if (at) {
                derived = g_strdup_printf("%.*spanel-state%s", (int)(at - keys),
                                          keys, at + strlen("panel-keys"));
                p = derived;
            }
        }
        if (p && *p) {
            cdj_stateout.fd = cdj_panelsock_connect(p);
            if (cdj_stateout.fd >= 0) {
                info_report("state out: deck state datagrams to %s (udp %u)",
                            p, cdj_panelsock_port(p));
            }
        }
    }
    if (cdj_stateout.fd < 0) {
        return;
    }
    if (!strcmp(cdj_stateout.last[kind], msg)
        && now - cdj_stateout.sent_ms[kind] < 1000) {
        return;
    }
    /* A skipped change still differs from last, so a later frame sends it. */
    if (!urgent && now - cdj_stateout.sent_ms[kind] < 20) {
        return;
    }
    g_strlcpy(cdj_stateout.last[kind], msg, sizeof(cdj_stateout.last[kind]));
    cdj_stateout.sent_ms[kind] = now;
    /* Fails harmlessly when no relay is bound. */
    (void)send(cdj_stateout.fd, msg, strlen(msg), 0);
}

static uint32_t cdj_stateout_le(uint32_t addr)
{
    uint32_t v = 0;

    cpu_physical_memory_read(A7ADDR(addr), &v, 4);
    return le32_to_cpu(v);
}

void cdj_stateout_frame(unsigned ch, uint32_t sar)
{
    static uint32_t last_beat;
    char msg[160];
    uint32_t beat;

    if (ch != 0) {
        return;
    }
    beat = cdj_stateout_le(sar + 0xC0);
    snprintf(msg, sizeof(msg), "frm beat=%u bars=%u tempo=%d bpm=%u",
             beat, cdj_stateout_le(sar + 0xE4),
             (int32_t)cdj_stateout_le(sar + 0x88), cdj_stateout_le(sar + 0x8C));
    cdj_stateout_send(0, msg, beat != last_beat);
    last_beat = beat;
}

void cdj_stateout_panel(const uint8_t *frame)
{
    static uint8_t last_lamps;
    char msg[4 + 2 * CDJ_STATEOUT_PNL_FRAME + 1] = "pnl ";
    unsigned k;

    for (k = 0; k < CDJ_STATEOUT_PNL_FRAME; k++) {
        snprintf(msg + 4 + 2 * k, 3, "%02x", frame[k]);
    }
    /* Byte 0 carries the PLAY (bit 0) and CUE (bit 1) lamps. */
    cdj_stateout_send(1, msg, frame[0] != last_lamps);
    last_lamps = frame[0];
}

/*
 * CDJ_VCLOCK_FILE=<path>: write MAIN's virtual time in ms to a file every 50
 * virtual ms. Under -icount the guest runs slower than wall time, so scripts
 * that drive the deck use this to wait in virtual seconds.
 */
static QEMUTimer *cdj_vclock_timer;
static int cdj_vclock_fd = -1;

static void cdj_vclock_tick(void *opaque)
{
    int64_t ms = qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL);
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%020" PRId64 "\n", ms);

    /* lseek + write rather than pwrite, which Windows lacks. The record is
     * fixed length at offset 0. */
    if (lseek(cdj_vclock_fd, 0, SEEK_SET) != 0
        || write(cdj_vclock_fd, buf, n) != n) {
        warn_report("vclock: short write");
    }
    timer_mod(cdj_vclock_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              50 * SCALE_MS);
}

void cdj_vclock_init(void)
{
    const char *path = getenv("CDJ_VCLOCK_FILE");

    if (!path || !*path) {
        return;
    }
    cdj_vclock_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cdj_vclock_fd < 0) {
        warn_report("vclock: cannot open %s", path);
        return;
    }
    cdj_vclock_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_vclock_tick, NULL);
    cdj_vclock_tick(NULL);
    info_report("vclock: publishing MAIN's virtual ms to %s", path);
}

