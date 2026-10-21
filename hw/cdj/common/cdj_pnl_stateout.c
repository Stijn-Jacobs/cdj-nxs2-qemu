/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_pnl_stateout.h"
#include "cdj_panelkeys.h"
#include "cdj_getenv.h"
/*
 * Same datagram the CDJ-2000NXS2 sends (boards/nxs2/host_io.c), for the
 * boards whose frame is shorter. A changed frame goes out at most every
 * 20 ms and an unchanged one is repeated every second; with no relay bound
 * the sends fail harmlessly.
 */
#define PNL_MAX_FRAME 40

static struct {
    bool parsed;
    int fd;
    char last[4 + 2 * PNL_MAX_FRAME + 1];
    int64_t sent_ms;
} stateout = { .fd = -1 };

static void stateout_connect(void)
{
    const char *p = getenv("CDJ_PANEL_STATESOCK");
    g_autofree char *derived = NULL;

    stateout.parsed = true;
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
        stateout.fd = cdj_panelsock_connect(p);
    }
}

void cdj_pnl_stateout(const uint8_t *frame, unsigned len)
{
    char msg[4 + 2 * PNL_MAX_FRAME + 1] = "pnl ";
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    unsigned k;

    if (!stateout.parsed) {
        stateout_connect();
    }
    if (stateout.fd < 0 || len > PNL_MAX_FRAME) {
        return;
    }
    for (k = 0; k < len; k++) {
        snprintf(msg + 4 + 2 * k, 3, "%02x", frame[k]);
    }
    if (!strcmp(stateout.last, msg) ? now - stateout.sent_ms < 1000
                                    : now - stateout.sent_ms < 20) {
        return;
    }
    g_strlcpy(stateout.last, msg, sizeof(stateout.last));
    stateout.sent_ms = now;
    (void)send(stateout.fd, msg, strlen(msg), 0);
}
