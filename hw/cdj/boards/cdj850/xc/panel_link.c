/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "panel_link.h"
#include "trace.h"
#include "vfd.h"

/*
 * CDJ-850 MAIN<->panel link model (panel CPU side).
 *
 * Hardware (service manual signal block diagram, connector CN2003): MAIN
 * (IC103, SH7263) talks to the panel CPU (IC1007) over clock-synchronous
 * serial plus MAIN_XBUSY/PNL_XBUSY handshake GPIOs. The SH7263 manual
 * identifies the serial block MAIN uses, 0xFFFE7000, as SSU channel 0
 * (SSCRH/SSCRL/SSMR/SSER/SSSR/SSTDR/SSRDR), and the handshake input the
 * firmware parks on, 0xFFFE391E bit 2, as Port C pin PC2 (PCPRL).
 *
 * What MAIN's drivers require (all verified against live SDRAM code):
 * - Boot upload (driver 0x0C077C3C): SSU master, clock-synchronous,
 *   MSB-first, TE only; 1140 TX bytes through SSTDR0 (+6) polling SSSR
 *   TDRE (bit 2). TX-only; no receive slot is ever polled here.
 * - Panel commands (driver 0x0C0781xx, 16-bit, 3x): words 0x1604, 0x10FF,
 *   0x11FF through SSTDR0/1 (+6/+7), TEND-polled. No receive slot is read.
 * - Panel exchange (driver 0x0C077570 wrapping 0x0C0777xx, full duplex,
 *   DjControlTask/cddabuffTask): per byte, TDRE-poll, write SSTDR0,
 *   RDRF-poll (SSSR bit 1), read SSRDR0 (+0x0A) into the caller's buffer,
 *   RDRF clears on the read (SSU manual 16.3.5). The SSU driver itself
 *   stores every byte unconditionally, but the CALLERS parse the 6-byte
 *   reply frame and gate progress on it:
 *     DjControlTask (0x0C014822): RX[3] & 0x0F, RX[4] and RX[5] must ALL
 *       be nonzero, else the exchange is discarded and the C2 poll
 *       repeats forever (observed: 247 identical polls with zeroed
 *       replies). A liveness gate: the panel must answer with real
 *       status, not silence.
 *     cddabuffTask (0x0C005F6E): if RX[1] and RX[2] are both nonzero and
 *       RX[0] bit 7 is clear, RX[0]'s high nibble (0x40/0x20/0x10) routes
 *       the big-endian value RX[1]<<8|RX[2] to a status word selected by
 *       the low nibble (0/1/2); RX[0] == 0 dispatches nothing and falls
 *       through to the same tail. So RX[0..2] == 0 ("no event to report")
 *       keeps cddabuff on its idle path either way.
 *   The served frame is therefore {0x00,0x00,0x00,0x01,0x01,0x01}: "no
 *   event" plus minimal nonzero status. Every byte of it is justified by
 *   the parse above; key-press events (RX[0] high nibble + value) are
 *   Task 9's via panel_send_key once a key is actually injected (until
 *   then the panel is idle and reports none).
 * - Handshake ACK (driver 0x0C08E8AE): waits for PC2 = 1 (xc/port.c
 *   straps it; the level is what the driver's own comparison dictates).
 *
 * Identity note: SCIF0 (0xFFFE8000, RX interrupt armed) is a separate
 * ASCII command console ("?", digits, CR per its RX handler), not the
 * panel link; SCIF1 (0xFFFE8800) is TX-side only here. Neither is modelled
 * in this task.
 */

#define PANEL_TX_RING 4096

/* Idle reply frame for the 6-byte status exchange, per the parse above. */
/* TODO(Task 8/9-a): DjControl accept-path stores (0x0C7F663C flag, 0x0C7F65BC
 * dst) read zero despite the gate at 0x0C014822 passing -- either a different
 * live reader among the 6 buffer refs or per-cycle clearing. Task 8 should
 * watch the store writers; park-equivalent here (periodic poll, no wedge). */
/* TODO(Task 9-b): RX-buffer tail byte 0x13 (byte 7) sits beyond consumed
 * [0..5] -- readers only consume [0..5]; if a longer frame exists it is
 * unmapped. Task 9 inherits the tail-byte question with key/FL mapping. */
static const uint8_t panel_idle_frame[6] =
    { 0x00, 0x00, 0x00, 0x01, 0x01, 0x01 };

/*
 * Task 8k: hold-at-reset frame override (EXPERIMENTAL test hook, default
 * OFF). $CDJ850_PANEL_RX holds six hex bytes (e.g. 40:01:01:01:01:01,
 * colons optional) served INSTEAD of the idle frame from the first
 * exchange after reset -- the MAIN-side equivalent of "keys held at
 * power-on" for the service-mode entries (manual §6: BACK+TEMPO panel
 * diag, TIME+CD SELECT drive diag, USB SELECT+RELOOP updater, BACK+USB
 * SELECT factory reset) and for the cddabuff non-idle branches.
 *
 * The bytes are operator-chosen test vectors, NOT real panel-matrix key
 * codes: the key-event reply format is unmapped (panel_send_key still
 * holds idle), so no value here may be cited as what the panel CPU
 * sends for a named key. Unset (or malformed) = OFF = byte-identical
 * to the idle default. Shapes stay driver-justified: keep RX[3]&0x0F,
 * RX[4], RX[5] nonzero (DjControl gate 0x0C014822) unless the run
 * explicitly probes the discard path.
 *
 * Task 8l: SEQUENCED mode (EXPERIMENTAL test hook, default OFF). New env
 * value syntax (keywords, NOT hex): $CDJ850_PANEL_RX=seq40|seq20|seq10|
 * seqall. Each 6-byte frame served is [SEQ_k,01,01,01,01,01] where SEQ_k
 * cycles through the route selectors for one bank triple (seq40: 40/41/42;
 * seq20: 20/21/22; seq10: 10/11/12) or all three triples in bank order
 * (seqall: 40/41/42/20/21/22/10/11/12). RX[1]/RX[2]=0x01 is the routed
 * value 0x0101 (as the Task 8k active-frame probes); tail 01 01 01 keeps
 * the DjControl gate (0x0C014822) passing. Sequencing rule: the sequence
 * advances one step per 6-byte frame at the byte-0 slot; panel_exchange_
 * start() only re-aligns rx_pos to 0 and never itself advances (a mid-
 * frame restart therefore serves the NEXT selector, not a repeat). First
 * frame after reset serves list[0]. Static-hold behavior is byte-identical
 * when sequenced mode is unused. All bytes remain synthetic test vectors
 * (disclaimed, never cited as key codes); shapes stay driver-justified
 * (cddabuff routing 0x0C005F6E + tail consumer 0x0C00600E).
 */
static uint8_t panel_held_frame[6];
static bool panel_hold_active;

/* Task 8l sequenced lists (routing shapes from disassembly, NOT key codes). */
static const uint8_t panel_seq40_list[3] =
    { 0x40, 0x41, 0x42 };
static const uint8_t panel_seq20_list[3] =
    { 0x20, 0x21, 0x22 };
static const uint8_t panel_seq10_list[3] =
    { 0x10, 0x11, 0x12 };
static const uint8_t panel_seqall_list[9] =
    { 0x40, 0x41, 0x42, 0x20, 0x21, 0x22, 0x10, 0x11, 0x12 };
static const uint8_t *panel_seq_list;
static unsigned panel_seq_len;
static unsigned panel_seq_pos;
static bool panel_seq_active;

void panel_link_init(void)
{
    const char *e = getenv("CDJ850_PANEL_RX");
    unsigned i, v;
    const char *p;

    if (!e || !*e) {
        return;
    }
    /* Task 8l sequenced keywords (new syntax; static hex path untouched). */
    if (strcmp(e, "seq40") == 0 || strcmp(e, "seq20") == 0 ||
        strcmp(e, "seq10") == 0 || strcmp(e, "seqall") == 0) {
        if (strcmp(e, "seq40") == 0) {
            panel_seq_list = panel_seq40_list;
            panel_seq_len = 3;
        } else if (strcmp(e, "seq20") == 0) {
            panel_seq_list = panel_seq20_list;
            panel_seq_len = 3;
        } else if (strcmp(e, "seq10") == 0) {
            panel_seq_list = panel_seq10_list;
            panel_seq_len = 3;
        } else {
            panel_seq_list = panel_seqall_list;
            panel_seq_len = 9;
        }
        panel_seq_pos = 0;
        panel_seq_active = true;
        panel_hold_active = false;
        info_report("cdj850: panel link: EXPERIMENTAL sequenced hold ON "
                    "(CDJ850_PANEL_RX=%s: per-frame RX[0] cycle, "
                    "RX[1]/RX[2]/tail 01: synthetic test vector, NOT "
                    "panel-matrix key codes; hardware capture replaces "
                    "this)", e);
        return;
    }
    p = e;
    for (i = 0; i < 6; i++) {
        while (*p == ':' || *p == ' ') {
            p++;
        }
        if (sscanf(p, "%2x", &v) != 1 || v > 0xFF) {
            info_report("cdj850: panel link: ignoring bad CDJ850_PANEL_RX "
                        "'%s' (want 6 hex bytes), holding idle", e);
            return;
        }
        panel_held_frame[i] = (uint8_t)v;
        p += 2;
    }
    while (*p == ':' || *p == ' ') {
        p++;
    }
    if (*p) {
        info_report("cdj850: panel link: ignoring bad CDJ850_PANEL_RX "
                    "'%s' (want 6 hex bytes), holding idle", e);
        return;
    }
    panel_hold_active = true;
    info_report("cdj850: panel link: EXPERIMENTAL hold-at-reset ON "
                "(CDJ850_PANEL_RX=%02x:%02x:%02x:%02x:%02x:%02x: synthetic "
                "test vector, NOT panel-matrix key codes; hardware capture "
                "replaces this)",
                panel_held_frame[0], panel_held_frame[1],
                panel_held_frame[2], panel_held_frame[3],
                panel_held_frame[4], panel_held_frame[5]);
}

static struct {
    uint8_t tx_ring[PANEL_TX_RING];
    unsigned tx_head;
    unsigned long tx_count;
    unsigned rx_pos;
    uint8_t pending_key;
    bool key_pending;
    bool key_logged;
    bool rx_logged;
} panel;

void panel_exchange_start(void)
{
    /* Task 8l: re-align only; the sequenced list advances at the byte-0
     * slot in panel_ssu_rx_byte(), never here. */
    panel.rx_pos = 0;
}

uint8_t panel_ssu_rx_byte(void)
{
    uint8_t b;
    const uint8_t *frame =
        panel_hold_active ? panel_held_frame : panel_idle_frame;

    if (!panel.rx_logged) {
        panel.rx_logged = true;
        if (panel_seq_active) {
            info_report("cdj850: panel link serving sequenced frames "
                        "(per-frame RX[0] cycle len %u, "
                        "RX[1]/RX[2]/tail 01)",
                        panel_seq_len);
        } else if (panel_hold_active) {
            info_report("cdj850: panel link serving held frame "
                        "(%02x %02x %02x %02x %02x %02x)",
                        frame[0], frame[1], frame[2],
                        frame[3], frame[4], frame[5]);
        } else {
            info_report("cdj850: panel link serving status frames "
                        "({00 00 00 01 01 01} idle)");
        }
    }
    if (panel.key_pending && !panel.key_logged) {
        /*
         * A key is queued (panel_send_key, Task 9's hook), but the
         * key-event frame format is not mapped yet: serving a stray
         * byte would desync the 6-byte frame the callers parse. Hold
         * the idle frame until the format is known.
         */
        panel.key_logged = true;
        info_report("cdj850: panel link key 0x%02x queued but key-event "
                    "format unmapped; holding idle frame",
                    panel.pending_key);
    }
    if (panel_seq_active) {
        /* Task 8l sequenced frame: [SEQ_k,01,01,01,01,01]. Advance one
         * step per frame at the byte-0 slot so cycling is per-exchange
         * regardless of the SSER RE-toggle cadence. */
        if (panel.rx_pos == 0) {
            b = panel_seq_list[panel_seq_pos];
            panel_seq_pos = (panel_seq_pos + 1) % panel_seq_len;
        } else {
            b = 0x01;
        }
        panel.rx_pos = (panel.rx_pos + 1) % sizeof(panel_idle_frame);
        return b;
    }
    b = frame[panel.rx_pos];
    panel.rx_pos = (panel.rx_pos + 1) % sizeof(panel_idle_frame);
    return b;
}

void panel_ssu_tx_byte(uint8_t b)
{
    panel.tx_ring[panel.tx_head] = b;
    panel.tx_head = (panel.tx_head + 1) % PANEL_TX_RING;
    panel.tx_count++;
    vfd_note_tx(b);
}

void panel_send_key(uint8_t code)
{
    panel.pending_key = code;
    panel.key_pending = true;
    panel.key_logged = false;
}
