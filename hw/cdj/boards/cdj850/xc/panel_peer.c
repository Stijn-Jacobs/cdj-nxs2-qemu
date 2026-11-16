/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "intc.h"
#include "panel_peer.h"
#include "port.h"
#include "scif.h"
#include "trace.h"

/*
 * Panel CPU (IC1007) as the clock master of the SCIF1 link to MAIN.
 *
 * Established (static reverse engineering of the MAIN firmware; addresses
 * are in the MAIN image, load 0x0C000000):
 *   - Pins (schematic): PE3/SCK1 = TSCK, PE5/TXD1 = TSO, PE4/RXD1 = TSI,
 *     PE9/IRQ5 = PNL_XBUSY (panel to MAIN), PC14 = MAIN_XBUSY (MAIN to
 *     panel). MAIN runs SCIF1 as a clock-synchronous slave on external SCK.
 *   - MAIN sets MAIN_XBUSY once, at the end of the link setup 0x0C0F9F14.
 *     Before that its ISRs are not registered.
 *   - A rising edge of PNL_XBUSY runs the IRQ5 ISR 0x0C0FA05E, which
 *     restarts SCIF1 and preloads 16 TX bytes with TIE on. Each time those
 *     bytes are clocked out TXI1 (ISR 0x0C0FA13C) drains up to 16 RX bytes,
 *     validates them and, depending on its page counter, preloads another
 *     16 TX bytes with TIE on or ends the transaction with TIE off. So a
 *     transaction is 2 or 3 back-to-back 16-byte full-duplex frames.
 *   - Frame 1 must be type 1 and frame 2 type 2; frame 3 is not checked.
 *     Type 1: byte 0 = 0x01, byte 15 = sum of bytes 0..14. Type 2: byte 0 =
 *     0x02, byte 12 = sum of bytes 0..11. The sum is the 8-bit end-around
 *     carry sum of the firmware routine 0x0C0F9EE8. Type 1 bytes 1-2 are
 *     the panel firmware version, shown by MAIN as "Panel : X.YZ".
 *   - A new PNL_XBUSY edge in the middle of a transaction restarts MAIN's
 *     state machine, so this peer never raises one.
 *   - The service manual (10.14, items 7-10) shows PNL_XBUSY as one high
 *     pulse of roughly 0.6 ms and three TSCK bursts of roughly 90 us about
 *     0.75 ms apart, in separately triggered captures.
 *
 * Assumed (nothing in the firmware or the manual decides these):
 *   - The transaction period. The real rate is unknown; CDJ850_PANEL_PERIOD_MS
 *     (default 10) is the idle gap from the end of one transaction to the
 *     next PNL_XBUSY rising edge.
 *   - Intra-transaction timing: frame 1 is clocked 200 us after the edge,
 *     each further frame 750 us after the previous one (the TSCK spacing
 *     from the capture above). If MAIN's ISR has not yet loaded the TX
 *     bytes when a frame is due (emulated interrupt latency is not bounded
 *     by these delays) the peer polls every 50 us for up to 20 ms. PNL_XBUSY
 *     stays high for the whole transaction and drops once MAIN ends it
 *     (TIE off after the last TXI1), or when MAIN did not take part.
 *   - Whether a transaction has 2 or 3 frames is decided here by "MAIN
 *     reloaded 16 TX bytes with TIE set after the previous frame"; how the
 *     real panel decides is not known.
 *   - Frame contents: all analog and key fields zero (keys released, jog
 *     idle, encoder and sliders at zero) and panel version 0x0100. The
 *     version bytes are arbitrary (display only); the zero idle values for
 *     the analog-like fields are a guess.
 *   - The panel starts only once MAIN_XBUSY is high, and polls for that at
 *     the transaction period.
 */
#define PANEL_FRAME_LEN         16
#define PANEL_NFRAMES           3
#define PANEL_FIRST_DELAY_NS    200000
#define PANEL_GAP_NS            750000
#define PANEL_POLL_NS           50000     /* re-check while MAIN's ISR is late */
#define PANEL_POLL_MAX_NS       20000000  /* ... for at most this long */
#define PANEL_PERIOD_MS_DEFAULT 10
#define PNL_XBUSY_IRQ           5

typedef enum {
    PANEL_IDLE,         /* waiting for MAIN_XBUSY / the next period */
    PANEL_WAIT_FRAME,   /* PNL_XBUSY high, next frame due when the timer fires */
} PanelState;

static struct {
    QEMUTimer *timer;
    bool enabled;
    PanelState state;
    uint32_t frame;             /* frames exchanged in this transaction */
    uint64_t period_ns;
    uint32_t transactions;      /* completed (>= 2 frames) */
    uint32_t aborted;           /* no frame 1 */
    uint32_t timeouts;          /* MAIN never answered a frame */
    int64_t due_ns;             /* when the pending frame was due */
    uint32_t seq;               /* transaction number, 1-based, for logs */
    bool reported;
    uint8_t last_tx[PANEL_NFRAMES][PANEL_FRAME_LEN];
    uint8_t cur_tx[PANEL_NFRAMES][PANEL_FRAME_LEN];
} panel;

uint8_t cdj850_panel_sum8ec(const uint8_t *p, unsigned n)
{
    unsigned s = 0, i;

    for (i = 0; i < n; i++) {
        s += p[i];
        if (s > 0xFF) {
            s = (s + 1) & 0xFF;
        }
    }
    return s;
}

static void panel_frame_type1(uint8_t f[PANEL_FRAME_LEN])
{
    memset(f, 0, PANEL_FRAME_LEN);
    f[0] = 0x01;
    f[1] = 0x01;                /* panel firmware version 0x0100 */
    f[2] = 0x00;
    f[15] = cdj850_panel_sum8ec(f, 15);
}

static void panel_frame_type2(uint8_t f[PANEL_FRAME_LEN])
{
    memset(f, 0, PANEL_FRAME_LEN);
    f[0] = 0x02;
    f[12] = cdj850_panel_sum8ec(f, 12);
}

static void panel_hex(char *out, const uint8_t *f)
{
    unsigned i;

    for (i = 0; i < PANEL_FRAME_LEN; i++) {
        sprintf(out + 3 * i, "%02x ", f[i]);
    }
    out[3 * PANEL_FRAME_LEN - 1] = 0;
}

static void panel_schedule(int64_t delay_ns)
{
    timer_mod(panel.timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns);
}

static void panel_end_transaction(void)
{
    cdj850_intc_set_pin(PNL_XBUSY_IRQ, false);
    if (panel.frame >= 2) {
        panel.transactions++;
        memcpy(panel.last_tx, panel.cur_tx, sizeof(panel.last_tx));
        if (!panel.reported) {
            panel.reported = true;
            info_report("cdj850: panel peer: first full transaction "
                        "completed (%u frames) at %" PRIu64 " ms",
                        panel.frame, cdj850_vms());
        }
    } else {
        panel.aborted++;
    }
    panel.state = PANEL_IDLE;
    panel_schedule(panel.period_ns);
}

static void panel_exchange(void)
{
    uint8_t rx[PANEL_FRAME_LEN] = { 0 };
    uint8_t tx[PANEL_FRAME_LEN];
    unsigned n = panel.frame;       /* 0-based index of this frame */

    if (n == 0) {
        panel_frame_type1(rx);
    } else if (n == 1) {
        panel_frame_type2(rx);
    }                               /* frame 3: 16 zero bytes */
    cdj850_scif1_exchange(rx, tx);
    memcpy(panel.cur_tx[n], tx, PANEL_FRAME_LEN);
    if (cdj850_trace()) {
        char hx[3 * PANEL_FRAME_LEN];

        panel_hex(hx, tx);
        info_report("cdj850: panel txn %u frame %u MAIN->panel %s %"
                    PRIu64 " ms", panel.seq, n + 1, hx, cdj850_vms());
        panel_hex(hx, rx);
        info_report("cdj850: panel txn %u frame %u panel->MAIN %s %"
                    PRIu64 " ms", panel.seq, n + 1, hx, cdj850_vms());
    }
    panel.frame++;
}

static bool panel_main_ready(void)
{
    return cdj850_scif1_tx_pending() == PANEL_FRAME_LEN &&
           cdj850_scif1_tie();
}

/*
 * A frame is due. MAIN's ISR may not have run yet (interrupt latency in the
 * emulated CPU is not bounded by the 200/750 us delays), so the peer polls
 * for MAIN's answer up to PANEL_POLL_MAX_NS before it decides:
 *   MAIN reloaded 16 bytes with TIE set  -> clock the next frame;
 *   TIE dropped (the TXI1 ISR ended the transaction, spec section 3, or a
 *   frame-1 request that never came)       -> transaction over;
 *   neither within the limit               -> transaction over.
 * TIE still set with an empty TX FIFO means the TXI1 ISR has not run yet.
 */
static void panel_tick(void *opaque)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    (void)opaque;

    if (panel.state == PANEL_IDLE) {
        if (!cdj850_port_main_xbusy()) {
            panel_schedule(panel.period_ns);
            return;
        }
        panel.seq++;
        panel.frame = 0;
        panel.state = PANEL_WAIT_FRAME;
        panel.due_ns = now + PANEL_FIRST_DELAY_NS;
        cdj850_intc_set_pin(PNL_XBUSY_IRQ, true);
        panel_schedule(PANEL_FIRST_DELAY_NS);
        return;
    }

    if (panel_main_ready() && panel.frame < PANEL_NFRAMES) {
        panel_exchange();
        panel.due_ns = now + PANEL_GAP_NS;
        panel_schedule(PANEL_GAP_NS);
        return;
    }
    if (now < panel.due_ns) {       /* woke early (restored timer) */
        panel_schedule(panel.due_ns - now);
        return;
    }
    if ((panel.frame == 0 || cdj850_scif1_tie()) &&
        now - panel.due_ns < PANEL_POLL_MAX_NS) {
        panel_schedule(PANEL_POLL_NS);
        return;
    }
    if (panel.frame == 0 && cdj850_trace()) {
        info_report("cdj850: panel txn %u: MAIN has not loaded frame 1 "
                    "(tx=%u tie=%d), dropping PNL_XBUSY %" PRIu64 " ms",
                    panel.seq, cdj850_scif1_tx_pending(), cdj850_scif1_tie(),
                    cdj850_vms());
    }
    if (panel.frame > 0 && cdj850_scif1_tie()) {
        panel.timeouts++;       /* MAIN left TIE set: no end-of-transaction */
        if (cdj850_trace()) {
            info_report("cdj850: panel txn %u: no answer from MAIN after "
                        "frame %u, ending %" PRIu64 " ms", panel.seq,
                        panel.frame, cdj850_vms());
        }
    }
    panel_end_transaction();
}

static const VMStateDescription vmstate_cdj850_panel = {
    .name = "cdj850-panel-peer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(timer, typeof(panel)),
        VMSTATE_UINT32(state, typeof(panel)),
        VMSTATE_UINT32(frame, typeof(panel)),
        VMSTATE_UINT32(transactions, typeof(panel)),
        VMSTATE_UINT32(aborted, typeof(panel)),
        VMSTATE_UINT32(timeouts, typeof(panel)),
        VMSTATE_INT64(due_ns, typeof(panel)),
        VMSTATE_UINT32(seq, typeof(panel)),
        VMSTATE_BOOL(reported, typeof(panel)),
        VMSTATE_UINT8_2DARRAY(last_tx, typeof(panel), PANEL_NFRAMES,
                              PANEL_FRAME_LEN),
        VMSTATE_UINT8_2DARRAY(cur_tx, typeof(panel), PANEL_NFRAMES,
                              PANEL_FRAME_LEN),
        VMSTATE_END_OF_LIST()
    }
};

unsigned cdj850_panel_peer_transactions(void)
{
    return panel.transactions;
}

const uint8_t *cdj850_panel_peer_last_tx(unsigned n)
{
    return n < PANEL_NFRAMES ? panel.last_tx[n] : NULL;
}

void cdj850_panel_peer_init(void)
{
    const char *e = getenv("CDJ850_PANEL");
    const char *p = getenv("CDJ850_PANEL_PERIOD_MS");
    unsigned ms = PANEL_PERIOD_MS_DEFAULT;

    if (e && *e == '0') {
        info_report("cdj850: panel peer disabled (CDJ850_PANEL=0)");
        return;
    }
    if (p && *p) {
        ms = strtoul(p, NULL, 0);
        if (!ms) {
            ms = PANEL_PERIOD_MS_DEFAULT;
        }
    }
    panel.enabled = true;
    panel.period_ns = (uint64_t)ms * 1000000;
    panel.state = PANEL_IDLE;
    panel.timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, panel_tick, NULL);
    vmstate_register_any(NULL, &vmstate_cdj850_panel, &panel);
    panel_schedule(panel.period_ns);
}
