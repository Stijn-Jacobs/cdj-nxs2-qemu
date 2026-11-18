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

typedef struct PanelStateReport {
    char last[5 + 2 * PNL_MAX_FRAME + 1];
    int64_t sent_ms;
} PanelStateReport;

static struct {
    bool parsed;
    int fd;
    PanelStateReport first, second;
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

static void stateout_send(PanelStateReport *report, const char *prefix,
                          const uint8_t *frame, unsigned len)
{
    char msg[sizeof(report->last)];
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    size_t header;
    unsigned k;

    if (!stateout.parsed) {
        stateout_connect();
    }
    if (stateout.fd < 0 || len > PNL_MAX_FRAME) {
        return;
    }
    header = g_strlcpy(msg, prefix, sizeof(msg));
    for (k = 0; k < len; k++) {
        snprintf(msg + header + 2 * k, 3, "%02x", frame[k]);
    }
    if (now - report->sent_ms < (strcmp(report->last, msg) ? 20 : 1000)) {
        return;
    }
    g_strlcpy(report->last, msg, sizeof(report->last));
    report->sent_ms = now;
    (void)send(stateout.fd, msg, strlen(msg), 0);
}

void cdj_pnl_stateout(const uint8_t *frame, unsigned len)
{
    stateout_send(&stateout.first, "pnl ", frame, len);
}

void cdj_pnl_stateout_page2(const uint8_t *frame, unsigned len)
{
    stateout_send(&stateout.second, "pnl2 ", frame, len);
}
