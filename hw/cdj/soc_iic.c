/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj.h"
#include "cdj_getenv.h"
/* ---------------------------------------------------------------------------
 * IIC0 and IIC1, I2C bus interface (manual section 32).
 *
 * Without a slave the DSP bring-up write at 0x083265AE times out waiting for
 * DTEI and the boot ends in E-7010 DSP DEVICE ERROR. With CDJ_IIC_SLAVE this
 * models master transmit and receive (manual 32.4): ICCR start/stop, DTE per
 * byte, WAIT at the ack point, with slaves answering at chosen addresses.
 * Unset, the bus is idle and nothing answers.
 *
 * The driver unmasks its own sources: IPRH at 0x083AF952/0x083AF9A8 and IMCR7
 * at 0x083AF986/0x083AF9D2 (0x0F for channel 1, 0xF0 for channel 0).
 *
 * Knobs:
 *   CDJ_IIC_SLAVE    set    -- model the bus at all (default off)
 *   CDJ_IIC_CH       0|1|both -- which channel gets the slave (default 1, the
 *                               DSP channel)
 *   CDJ_IIC_ADDR     0x30   -- comma-separated 7-bit addresses that answer.
 *                             The DSP bring-up also writes to 0x2c
 *                             (0x08501166).
 *   CDJ_IIC_BYTE_US  90     -- time for one byte plus its ack. A real byte at
 *                             the driver's ~320 kHz is ~28 us; the margin keeps
 *                             the next DTEI out of the handler that caused it.
 *   CDJ_IIC_STUCK_MS 50     -- how long DTE/WAIT may stay high with no ICDR or
 *                             ICCR access before the line is muted (see
 *                             cdj_iic_stuck). Fractional values are accepted.
 *   CDJ_IIC_STUCK_DROP set  -- on timeout clear DTE/WAIT instead, which aborts
 *                             the transfer
 *   CDJ_IIC_NACK     set    -- answer an unknown address with a NACK instead of
 *                             silence (see cdj_iic_tx_byte)
 *   CDJ_IIC_RX_BYTE  0xFF   -- what a read returns for a command with no entry
 *                             in the reply table
 *   CDJ_IIC_REPLY    cmd:answer,... -- what a read returns after that byte was
 *                             written. Defaults to 0x00:0x05,0x01:0x01, the
 *                             values 0x08214D98 checks.
 *   CDJ_IIC_DEBUG    set    -- log every byte
 */
#define CDJ_IIC_SIZE        0x20
#define CDJ_IIC_ICDR        0x00    /* data                                 */
#define CDJ_IIC_ICCR        0x04    /* control: ICE|RACK|TRS|BBSY|SCP       */
#define CDJ_IIC_ICSR        0x08    /* status: SCLM|SDAM|BUSY|AL|TACK|...   */
#define CDJ_IIC_ICIC        0x0C    /* enables ALE|TACKE|WAITE|DTEE         */
#define CDJ_IIC_ICCL        0x10    /* SCL low period                       */
#define CDJ_IIC_ICCH        0x14    /* SCL high period                      */
#define CDJ_IIC_ICSR_IDLE   0xC0    /* SCLM=1 SDAM=1 BUSY=0 -- reset value  */

#define CDJ_ICCR_ICE        0x80
#define CDJ_ICCR_RACK       0x40
#define CDJ_ICCR_TRS        0x10
#define CDJ_ICCR_BBSY       0x04
#define CDJ_ICCR_SCP        0x01

#define CDJ_ICSR_SCLM       0x80
#define CDJ_ICSR_SDAM       0x40
#define CDJ_ICSR_BUSY       0x10
#define CDJ_ICSR_AL         0x08
#define CDJ_ICSR_TACK       0x04
#define CDJ_ICSR_WAIT       0x02
#define CDJ_ICSR_DTE        0x01
#define CDJ_ICSR_WCLR       (CDJ_ICSR_AL | CDJ_ICSR_TACK | CDJ_ICSR_WAIT)

#define CDJ_ICIC_ALE        0x08
#define CDJ_ICIC_TACKE      0x04
#define CDJ_ICIC_WAITE      0x02
#define CDJ_ICIC_DTEE       0x01

typedef struct CdjIicState {
    MemoryRegion iomem;
    const char *name;
    unsigned ch;
    uint8_t reg[CDJ_IIC_SIZE];
    uint64_t reads[CDJ_IIC_SIZE];
    uint64_t wr[CDJ_IIC_SIZE];
    uint64_t writes;

    /* Everything below is inert unless CDJ_IIC_SLAVE is set. */
    bool slave_on;
    bool nack_unknown;
    bool debug;
    bool addr_ok[128];          /* which 7-bit addresses answer            */
    const char *addr_list;      /* what was asked for, for the log line    */
    int64_t byte_ns;

    qemu_irq irq[CDJ_IIC_NR_IRQ];
    bool irq_level[CDJ_IIC_NR_IRQ];
    QEMUTimer *bus;
    bool arm_wait;

    uint8_t status;             /* live BUSY|AL|TACK|WAIT|DTE               */
    bool have_addr;
    uint8_t addr7;              /* 7-bit address of the current transfer    */
    bool addressed;
    bool reading;               /* R/W bit of the address byte              */
    uint8_t rx;                 /* byte the slave hands back on a read      */
    uint8_t reply[256];         /* read answer, indexed by the last byte written */
    uint8_t cmd;                /* last data byte written to an addressed slave */
    bool rx_hold;               /* a received byte is held at the ack point */
    bool stop_pending;          /* stop asked for at that ack point         */

    /* Stuck-line watchdog, see cdj_iic_stuck(). */
    QEMUTimer *guard;
    int64_t stuck_ns;
    int64_t mute_ns;            /* current back-off, doubles per mute cycle */
    bool drop_on_stuck;         /* CDJ_IIC_STUCK_DROP                       */
    uint64_t ack_seq;           /* bumped by ICDR and ICCR accesses only    */
    uint64_t irq_seq;           /* ack_seq at the moment the line went high */
    bool irq_muted;             /* line held low while the flag stays set   */
    bool stuck_seen;            /* this assertion already counted and logged */
    uint64_t stuck;

    uint64_t starts;
    uint64_t stops;
    uint64_t completed;
    uint64_t unanswered;
    uint64_t aborted;
    uint64_t tx_bytes;
    FILE *cap_tx;               /* CDJ_CAPTURE sink, per channel, opened lazily */
    uint64_t rx_bytes;
    uint64_t dte_raises;
    Notifier exit;
} CdjIicState;

/* The DSP bring-up channel (IIC1), when modelled. The PFC uses its byte
 * count to decide when to report the DSP ready. */
static CdjIicState *cdj_iic_dsp_channel;

uint64_t cdj_dsp_i2c_tx_bytes(void)
{
    return cdj_iic_dsp_channel ? cdj_iic_dsp_channel->tx_bytes : 0;
}

/* SCLM and SDAM always read released; the firmware only uses them in its
 * (ICSR & 0xD0) == 0xC0 bus-free test. TACK and WAIT are gated by their
 * enables (manual 32.3.3). */
static uint8_t cdj_iic_icsr(CdjIicState *s)
{
    uint8_t icic = s->reg[CDJ_IIC_ICIC];
    uint8_t v = CDJ_ICSR_SCLM | CDJ_ICSR_SDAM;

    v |= s->status & (CDJ_ICSR_BUSY | CDJ_ICSR_AL | CDJ_ICSR_DTE);
    if (icic & CDJ_ICIC_TACKE) {
        v |= s->status & CDJ_ICSR_TACK;
    }
    if (icic & CDJ_ICIC_WAITE) {
        v |= s->status & CDJ_ICSR_WAIT;
    }
    return v;
}

/* The four sources are level lines: DTEI is requested while DTE and DTEE are
 * both set (manual 32.3.4). Only changes are pushed. */
static void cdj_iic_update_irq(CdjIicState *s)
{
    uint8_t icic = s->reg[CDJ_IIC_ICIC];
    bool want[CDJ_IIC_NR_IRQ];
    unsigned i;

    want[CDJ_IIC_IRQ_AL] = (s->status & CDJ_ICSR_AL) && (icic & CDJ_ICIC_ALE);
    want[CDJ_IIC_IRQ_TACK] = (s->status & CDJ_ICSR_TACK) &&
                             (icic & CDJ_ICIC_TACKE);
    want[CDJ_IIC_IRQ_WAIT] = (s->status & CDJ_ICSR_WAIT) &&
                             (icic & CDJ_ICIC_WAITE) && !s->irq_muted;
    want[CDJ_IIC_IRQ_DTE] = (s->status & CDJ_ICSR_DTE) &&
                            (icic & CDJ_ICIC_DTEE) && !s->irq_muted;

    for (i = 0; i < CDJ_IIC_NR_IRQ; i++) {
        if (want[i] == s->irq_level[i]) {
            continue;
        }
        s->irq_level[i] = want[i];
        if (i == CDJ_IIC_IRQ_DTE && want[i]) {
            s->dte_raises++;
        }
        if (s->irq[i]) {
            qemu_set_irq(s->irq[i], want[i]);
        }
    }
}

/* A new DTE/WAIT assertion: reset the watchdog state. */
static void cdj_iic_line_armed(CdjIicState *s)
{
    s->irq_seq = s->ack_seq;
    s->irq_muted = false;
    s->stuck_seen = false;
    s->mute_ns = s->stuck_ns;
    if (s->guard) {
        timer_mod(s->guard,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->stuck_ns);
    }
}

/* Stuck-line watchdog. DTE and WAIT are level interrupts, so a handler that
 * returns without touching ICDR or ICCR re-enters at once and starves the
 * CPU. If the line stays high with no ICDR/ICCR access for stuck_ns, mute the
 * interrupt but leave the transfer state intact, so it still completes when
 * the guest gets to it; unmute after a back-off that doubles each cycle.
 * Clearing DTE/WAIT instead (CDJ_IIC_STUCK_DROP) aborts the transfer, which
 * shows up as E-7206 AUTH CHIP ERROR. */
static void cdj_iic_stuck(void *opaque)
{
    CdjIicState *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (!(s->status & (CDJ_ICSR_DTE | CDJ_ICSR_WAIT)) ||
        s->ack_seq != s->irq_seq) {
        if (s->irq_muted) {
            s->irq_muted = false;
            cdj_iic_update_irq(s);
        }
        return;
    }

    if (s->drop_on_stuck) {
        s->stuck++;
        warn_report("%s: DTE/WAIT held %" PRId64 " us with no ICDR/ICCR access "
                    "-- dropping the line so the guest is not starved",
                    s->name, s->stuck_ns / 1000);
        s->status &= ~(CDJ_ICSR_DTE | CDJ_ICSR_WAIT);
        cdj_iic_update_irq(s);
        return;
    }

    if (!s->irq_muted) {
        if (!s->stuck_seen) {
            s->stuck_seen = true;
            s->stuck++;
            warn_report("%s: DTE/WAIT held %" PRId64 " ms with no ICDR/ICCR "
                        "access -- muting the line so the guest is not "
                        "starved; the transfer is left standing", s->name,
                        s->stuck_ns / 1000000);
        }
        s->irq_muted = true;
        cdj_iic_update_irq(s);
        timer_mod(s->guard, now + s->mute_ns);
        /* Back-off capped at 64 deadlines (3.2 s at the default). */
        if (s->mute_ns < 64 * s->stuck_ns) {
            s->mute_ns *= 2;
        }
        return;
    }

    s->irq_muted = false;
    cdj_iic_update_irq(s);
    timer_mod(s->guard, now + s->stuck_ns);
}

/* One byte time later the controller is ready again. With WAITE set the wait
 * comes first and the byte only completes once the guest clears ICSR.WAIT.
 *
 * A received byte raises DTE and WAIT together: WAIT because the controller
 * holds before the ack clock, DTE because the byte is in ICDR. The driver's
 * receive is WAIT-driven (handler 0x083B0742): it sets RACK and DTEE at the
 * ack point, and DTEI state 8 (0x083B0BE6) then reads ICDR.
 */
static void cdj_iic_bus_event(void *opaque)
{
    CdjIicState *s = opaque;

    if (!(s->reg[CDJ_IIC_ICCR] & CDJ_ICCR_ICE) ||
        !(s->status & CDJ_ICSR_BUSY)) {
        return;
    }
    if (s->reading && s->addressed &&
        !(s->reg[CDJ_IIC_ICCR] & CDJ_ICCR_TRS)) {
        s->rx_hold = true;
        s->status |= CDJ_ICSR_DTE;
        if (s->reg[CDJ_IIC_ICIC] & CDJ_ICIC_WAITE) {
            s->status |= CDJ_ICSR_WAIT;
        }
        cdj_iic_line_armed(s);
        cdj_iic_update_irq(s);
        return;
    }
    if (s->arm_wait && (s->reg[CDJ_IIC_ICIC] & CDJ_ICIC_WAITE)) {
        s->status |= CDJ_ICSR_WAIT;
    } else {
        s->status |= CDJ_ICSR_DTE;
    }
    cdj_iic_line_armed(s);
    cdj_iic_update_irq(s);
}

static void cdj_iic_arm(CdjIicState *s, bool via_wait)
{
    /* Transmits run with DTEE only; reads switch to WAITE (0x083B0CEC sets
     * ICIC = 0xCE before the address byte), so both branches are used. */
    s->arm_wait = via_wait;
    if (s->byte_ns <= 0) {
        /* CDJ_IIC_BYTE_US=0: complete inside the guest's own store, before
         * the handler that issued it has updated its state. */
        cdj_iic_bus_event(s);
        return;
    }
    timer_mod(s->bus, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + s->byte_ns);
}

static void cdj_iic_tx_byte(CdjIicState *s, uint8_t v)
{
    /* CDJ_CAPTURE: every byte MAIN writes, per channel, whether or not a
     * slave answers (<prefix>-i2c-ch<n>.bin). */
    {
        char suffix[16];

        snprintf(suffix, sizeof(suffix), "i2c-ch%u", s->ch);
        cdj_capture_write(&s->cap_tx, suffix, &v, 1);
    }

    s->status &= ~CDJ_ICSR_DTE;

    if (!s->have_addr) {
        s->have_addr = true;
        s->addr7 = (v >> 1) & 0x7F;
        s->addressed = s->addr_ok[(v >> 1) & 0x7F];
        /* Address bit 0 is R/W. The auth chip (slave 0x10 on channel 0) is
         * checked with a read and a write; see 0x08214D98. */
        s->reading = (v & 1) != 0;
        if (s->debug) {
            info_report("%s: addr 0x%02x %s -> %s", s->name, (v >> 1) & 0x7F,
                        (v & 1) ? "read" : "write",
                        s->addressed ? "ack" : "no device");
        }
        if (!s->addressed) {
            s->unanswered++;
            /* A real bus NACKs an absent slave (driver error -97 instead of
             * the -50 timeout); off by default, CDJ_IIC_NACK. */
            if (s->nack_unknown) {
                s->status |= CDJ_ICSR_TACK;
            }
            /* Release the bus: the driver's error path may not issue a
             * stop, and a stuck BUSY fails the bus-free poll at 0x083B02D0
             * for the rest of the boot. */
            s->status &= ~(CDJ_ICSR_BUSY | CDJ_ICSR_DTE | CDJ_ICSR_WAIT);
            s->have_addr = false;
            timer_del(s->bus);
            cdj_iic_update_irq(s);
            return;
        }
    } else {
        /* CDJ_CAPTURE: the bytes an addressed slave took on the DSP
         * channel (<prefix>-i2c.bin). */
        if (s == cdj_iic_dsp_channel) {
            static FILE *cap;

            cdj_capture_write(&cap, "i2c", &v, 1);
        }
        s->tx_bytes++;
        if (s == cdj_iic_dsp_channel && s->addr7 == C6X_I2C_ADDR &&
            !s->reading) {
            cdj_c6x_i2c_byte(v);
        }
        /* The auth chip answers a read according to the byte written in
         * the previous transaction; see cdj_iic_rx_byte. */
        s->cmd = v;
        if (s->debug) {
            info_report("%s: tx 0x%02x", s->name, v);
        }
    }
    cdj_iic_arm(s, true);
    cdj_iic_update_irq(s);
}

/* Hand the guest the received byte: s->reply[] indexed by the last byte
 * written (CDJ_IIC_REPLY, CDJ_IIC_RX_BYTE). The DSP transaction itself is
 * write-only. */
static uint8_t cdj_iic_rx_byte(CdjIicState *s)
{
    uint8_t v;

    if (!(s->status & CDJ_ICSR_BUSY) || (s->reg[CDJ_IIC_ICCR] & CDJ_ICCR_TRS)) {
        return 0xFF;
    }
    v = s->reply[s->cmd];
    s->status &= ~CDJ_ICSR_DTE;
    s->rx_bytes++;
    s->rx_hold = false;
    if (s->stop_pending) {
        /* The stop requested at the ack point takes effect now that the
         * byte is read; the task polls for a free bus straight after
         * (0x083B04CC). */
        s->stops++;
        s->completed++;
        s->status &= ~(CDJ_ICSR_BUSY | CDJ_ICSR_DTE | CDJ_ICSR_WAIT);
        s->have_addr = false;
        s->addressed = false;
        s->reading = false;
        s->stop_pending = false;
        timer_del(s->bus);
        timer_del(s->guard);
    } else {
        cdj_iic_arm(s, true);
    }
    cdj_iic_update_irq(s);
    if (s->debug) {
        info_report("%s: rx 0x%02x (answer to cmd 0x%02x)", s->name, v, s->cmd);
    }
    return v;
}

static void cdj_iic_iccr_write(CdjIicState *s, uint8_t v)
{
    uint8_t old = s->reg[CDJ_IIC_ICCR];

    s->ack_seq++;
    s->reg[CDJ_IIC_ICCR] = v;

    if (!(v & CDJ_ICCR_ICE)) {
        if (s->status & CDJ_ICSR_BUSY) {
            s->aborted++;
        }
        s->status = 0;
        s->have_addr = false;
        s->addressed = false;
        s->reading = false;
        s->rx_hold = false;
        s->stop_pending = false;
        timer_del(s->bus);
        timer_del(s->guard);
        cdj_iic_update_irq(s);
        return;
    }

    /* Manual 32.3.2: DTE is cleared when TRS changes. */
    if ((old ^ v) & CDJ_ICCR_TRS) {
        s->status &= ~CDJ_ICSR_DTE;
        if ((s->status & CDJ_ICSR_BUSY) && !(v & CDJ_ICCR_TRS)) {
            cdj_iic_arm(s, true);
        }
    }

    /* BBSY only strobes with SCP written 0; SCP written 1 is ignored. */
    if (!(v & CDJ_ICCR_SCP)) {
        if (v & CDJ_ICCR_BBSY) {
            s->starts++;
            s->status |= CDJ_ICSR_BUSY;
            s->status &= ~CDJ_ICSR_DTE;
            s->have_addr = false;
            s->addressed = false;
            cdj_iic_arm(s, false);
        } else if (s->reading && s->rx_hold) {
            /* Stop requested while a read byte is held at the ack point
             * (ICCR = 0xC0 at 0x083B07E0). Defer it until the guest has read
             * ICDR: nack, stop, then the last byte is collected. */
            s->stop_pending = true;
        } else if (s->status & CDJ_ICSR_BUSY) {
            /* Only a busy bus can be stopped; ICCR = 0x80 on an idle bus is
             * configuration. */
            s->stops++;
            if (s->addressed) {
                s->completed++;
            }
            s->status &= ~(CDJ_ICSR_BUSY | CDJ_ICSR_DTE | CDJ_ICSR_WAIT);
            s->have_addr = false;
            s->addressed = false;
            s->reading = false;
            s->rx_hold = false;
            s->stop_pending = false;
            timer_del(s->bus);
            timer_del(s->guard);
        }
    }
    cdj_iic_update_irq(s);
}

static void cdj_iic_icsr_write(CdjIicState *s, uint8_t v)
{
    uint8_t was = s->status;

    /* Manual 32.3.3: "Only 0 can be written, for flag clearing." */
    s->status &= (uint8_t)(v | ~CDJ_ICSR_WCLR);

    /* Clearing WAIT starts the next byte, unless a received byte is still
     * held for the guest to read. */
    if ((was & CDJ_ICSR_WAIT) && !(s->status & CDJ_ICSR_WAIT) &&
        (s->status & CDJ_ICSR_BUSY) && !s->rx_hold) {
        cdj_iic_arm(s, false);
    }
    cdj_iic_update_irq(s);
}

static uint64_t cdj_iic_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjIicState *s = opaque;
    unsigned idx = offset & (CDJ_IIC_SIZE - 1);

    s->reads[idx]++;

    if (!s->slave_on) {
        switch (idx) {
        case CDJ_IIC_ICSR:
            /* Idle and released; nothing on the bus answers. */
            return CDJ_IIC_ICSR_IDLE;
        case CDJ_IIC_ICDR:
            return 0;           /* no slave drives the bus */
        default:
            return s->reg[idx];
        }
    }

    switch (idx) {
    case CDJ_IIC_ICSR:
        return cdj_iic_icsr(s);
    case CDJ_IIC_ICDR:
        s->ack_seq++;
        return cdj_iic_rx_byte(s);
    case CDJ_IIC_ICCR:
        /* SCP always reads 1 (manual 32.3.2), so a read-modify-write cannot
         * re-strobe the bit that issues start and stop conditions. */
        return s->reg[idx] | CDJ_ICCR_SCP;
    default:
        return s->reg[idx];
    }
}

static void cdj_iic_write(void *opaque, hwaddr offset, uint64_t value,
                          unsigned size)
{
    CdjIicState *s = opaque;
    unsigned idx = offset & (CDJ_IIC_SIZE - 1);
    uint8_t v = (uint8_t)value;

    s->wr[idx]++;
    s->writes++;

    if (!s->slave_on) {
        s->reg[idx] = v;
        return;
    }

    switch (idx) {
    case CDJ_IIC_ICDR:
        s->ack_seq++;
        s->reg[idx] = v;
        if ((s->status & CDJ_ICSR_BUSY) &&
            (s->reg[CDJ_IIC_ICCR] & CDJ_ICCR_TRS)) {
            cdj_iic_tx_byte(s, v);
        }
        break;
    case CDJ_IIC_ICCR:
        cdj_iic_iccr_write(s, v);
        break;
    case CDJ_IIC_ICSR:
        cdj_iic_icsr_write(s, v);
        break;
    case CDJ_IIC_ICIC:
        /* Bits 7:4 are reserved and read 0 (manual 32.3.4); the firmware
         * writes 0xCD. */
        s->reg[idx] = v & 0x0F;
        cdj_iic_update_irq(s);
        break;
    default:
        s->reg[idx] = v;
        break;
    }
}

static const MemoryRegionOps cdj_iic_ops = {
    .read = cdj_iic_read,
    .write = cdj_iic_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void cdj_iic_summary(Notifier *n, void *opaque)
{
    CdjIicState *s = container_of(n, CdjIicState, exit);
    unsigned i;
    uint64_t total = 0;

    for (i = 0; i < CDJ_IIC_SIZE; i++) {
        total += s->reads[i];
    }
    if (!total && !s->writes) {
        return;
    }

    info_report("%s: %" PRIu64 " reads, %" PRIu64 " writes",
                s->name, total, s->writes);
    for (i = 0; i < CDJ_IIC_SIZE; i++) {
        if (s->reads[i] > 1000) {
            info_report("%s:   +0x%02x read %" PRIu64 " times (spin?)",
                        s->name, i, s->reads[i]);
        }
    }
    if (!s->slave_on) {
        return;
    }
    info_report("%s: starts=%" PRIu64 " stops=%" PRIu64 " completed=%" PRIu64
                " tx=%" PRIu64 " rx=%" PRIu64 " DTEI rises=%" PRIu64
                " stuck=%" PRIu64,
                s->name, s->starts, s->stops, s->completed,
                s->tx_bytes, s->rx_bytes, s->dte_raises, s->stuck);
    if (s->unanswered) {
        warn_report("%s: %" PRIu64 " transfer(s) addressed a device that is "
                    "not modelled -- these time out", s->name, s->unanswered);
    }
    if (s->aborted) {
        warn_report("%s: %" PRIu64 " transfer(s) were cut short by an ICE "
                    "clear while the bus was busy", s->name, s->aborted);
    }
    if (s->starts > s->completed) {
        warn_report("%s: %" PRIu64 " of %" PRIu64 " transfers did not finish",
                    s->name, s->starts - s->completed, s->starts);
    }
}

/* On machine reset, drop any transfer in progress and lower the lines. */
static void cdj_iic_reset(void *opaque)
{
    CdjIicState *s = opaque;
    unsigned i;

    timer_del(s->bus);
    timer_del(s->guard);
    memset(s->reg, 0, sizeof(s->reg));
    s->status = 0;
    s->have_addr = false;
    s->addressed = false;
    s->irq_muted = false;
    s->stuck_seen = false;
    s->mute_ns = s->stuck_ns;
    for (i = 0; i < CDJ_IIC_NR_IRQ; i++) {
        if (s->irq_level[i] && s->irq[i]) {
            qemu_set_irq(s->irq[i], 0);
        }
        s->irq_level[i] = false;
    }
}

/* The DSP transaction runs on IIC1, the default slave channel; IIC0 only
 * gets one through CDJ_IIC_CH. */
static bool cdj_iic_slave_on(unsigned ch)
{
    const char *sel;

    if (!getenv("CDJ_IIC_SLAVE")) {
        return false;
    }
    sel = getenv("CDJ_IIC_CH");
    if (!sel) {
        return ch == 1;
    }
    if (!strcmp(sel, "both") || !strcmp(sel, "all")) {
        return true;
    }
    return (unsigned)strtoul(sel, NULL, 0) == ch;
}

void cdj_iic(MemoryRegion *sysmem, const char *name, hwaddr addr,
                    unsigned ch, qemu_irq *irq)
{
    CdjIicState *s = g_new0(CdjIicState, 1);
    unsigned i;

    s->name = g_strdup(name);
    s->ch = ch;
    s->rx = 0xFF;
    s->exit.notify = cdj_iic_summary;
    qemu_add_exit_notifier(&s->exit);

    if (cdj_iic_slave_on(ch)) {
        const char *e;

        s->slave_on = true;
        s->nack_unknown = getenv("CDJ_IIC_NACK") != NULL;
        s->debug = getenv("CDJ_IIC_DEBUG") != NULL;
        e = getenv("CDJ_IIC_ADDR");
        s->addr_list = e ? e : "0x30";
        {
            const char *p = s->addr_list;

            while (*p) {
                char *end;
                unsigned long a = strtoul(p, &end, 0);

                if (end == p) {
                    break;      /* not a number -- stop rather than spin */
                }
                s->addr_ok[a & 0x7F] = true;
                p = end;
                while (*p == ',' || *p == ' ') {
                    p++;
                }
            }
        }
        /* Default read answer; 0xFF is ICDR's reset value. */
        e = getenv("CDJ_IIC_RX_BYTE");
        s->rx = e ? (uint8_t)strtoul(e, NULL, 0) : 0xFF;
        memset(s->reply, s->rx, sizeof(s->reply));
        /* The auth-chip check at 0x08214D98 writes 0x00 and expects 0x05
         * back, then writes 0x01 and expects 0x01. Failing it raises E-7206
         * AUTH CHIP ERROR. */
        s->reply[0x00] = 0x05;
        s->reply[0x01] = 0x01;
        e = getenv("CDJ_IIC_REPLY");
        if (e) {
            const char *p = e;

            while (*p) {
                char *end;
                unsigned long cmd = strtoul(p, &end, 0);

                if (end == p || *end != ':') {
                    break;      /* not a cmd:answer pair -- stop rather than spin */
                }
                p = end + 1;
                s->reply[cmd & 0xFF] = (uint8_t)strtoul(p, &end, 0);
                if (end == p) {
                    break;
                }
                p = end;
                while (*p == ',' || *p == ' ') {
                    p++;
                }
            }
        }
        e = getenv("CDJ_IIC_BYTE_US");
        s->byte_ns = (e ? (int64_t)strtoll(e, NULL, 0) : 90) * 1000;
        e = getenv("CDJ_IIC_STUCK_MS");
        /* Fractional, so sub-millisecond deadlines can be tested. */
        s->stuck_ns = (int64_t)((e ? strtod(e, NULL) : 50) * 1000000.0);
        if (s->stuck_ns < 1000) {
            s->stuck_ns = 1000;
        }
        s->mute_ns = s->stuck_ns;
        s->drop_on_stuck = getenv("CDJ_IIC_STUCK_DROP") != NULL;
        for (i = 0; i < CDJ_IIC_NR_IRQ && irq; i++) {
            s->irq[i] = irq[i];
        }
        s->bus = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_iic_bus_event, s);
        s->guard = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj_iic_stuck, s);
        if (ch == 1) {
            cdj_iic_dsp_channel = s;
        }
        qemu_register_reset(cdj_iic_reset, s);
        info_report("%s: slave %s answering, %" PRId64 " us per byte%s",
                    s->name, s->addr_list, s->byte_ns / 1000,
                    s->nack_unknown ? ", other addresses NACKed" : "");
    }

    memory_region_init_io(&s->iomem, NULL, &cdj_iic_ops, s, name,
                          CDJ_IIC_SIZE);
    memory_region_add_subregion_overlap(sysmem, A7ADDR(addr), &s->iomem, 1);
}

