/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cmt.h"
#include "scif.h"
#include "trace.h"

/*
 * SCIF0 at 0xFFFE8000 / SCIF1 at 0xFFFE8800 (SH7263 manual section 15).
 *
 * SCIF1 is the MAIN <-> panel-CPU link: MAIN runs it as a clock-synchronous
 * slave (SCSMR C/A = 1, SCSCR CKE = 10, external clock on SCK1). The panel
 * CPU is the clock master, so MAIN's SCFTDR bytes leave and 16 bytes arrive
 * only when the panel clocks a frame; xc/panel_peer.c plays that part
 * through cdj850_scif1_exchange(). SCIF0 is the ASCII debug console; it is
 * a TX-ready stub.
 *
 * SCIF0 (unchanged): SCFTDR bytes are captured under CDJ850_TRACE and the
 * FIFO drains instantly, so SCFSR TDFE + TEND always read back set and the
 * drivers' read-then-write-0 flag clears stick in storage. SCFRDR has no
 * receive data (nothing is plugged into the service console, so no RX
 * interrupt is ever raised) and SCFDR reads 0. SCSMR/SCBRR/SCSCR/SCFCR/
 * SCLSR are plain storage.
 *
 * SCIF1 registers (spec: the firmware's driver, MAIN image 0x0C0F9F7A init,
 * 0x0C0FA006 restart, IRQ5 ISR 0x0C0FA05E, TXI1 ISR 0x0C0FA13C):
 *   +0x08 SCSCR  storage; TIE = bit 7. Clearing TE (bit 5) empties the TX
 *                FIFO and clearing RE (bit 4) the RX FIFO: the firmware
 *                toggles both off and on every frame (0x0C0FA006) and
 *                relies on that to start each frame from empty FIFOs.
 *   +0x0C SCFTDR pushes a byte into the 16-byte TX FIFO (dropped when full).
 *   +0x10 SCFSR  TDFE (bit 5) and TEND (bit 6) read 0 while unsent TX bytes
 *                sit in the FIFO; they are set by the panel clocking a frame
 *                out (and read 1 after reset) and a write of 0 clears them.
 *                RDF (bit 1) and DR (bit 0) read 1 exactly while the RX FIFO
 *                is non-empty (RTRG = 1 byte in clock-synchronous mode; the
 *                firmware's TXI1 loop 0x0C0FA152 stops when both read 0)
 *                and ignore writes. ER, BRK, FER, PER and the error counts
 *                stay 0: they are asynchronous-mode flags, and the ISR
 *                skips the SCFRDR read of a byte flagged FER/PER.
 *   +0x14 SCFRDR pops the 16-byte RX FIFO (0 when empty).
 *   +0x18 SCFCR  storage; TFRST (bit 2) / RFRST (bit 1) empty the TX / RX
 *                FIFO when written 1.
 *   +0x1C SCFDR  FIFO counts: TX in bits 12:8, RX in bits 4:0.
 * RXI1/ERI1/BRI1 are never raised: the firmware has no handler for vectors
 * 196-198 and never sets RIE/REIE.
 *
 * TXI1 (vector 199) is level-like: asserted while TDFE reads 1 and SCSCR.TIE
 * is 1. The request is queued on the rising edge of that condition, on
 * panel frame completion, and re-evaluated after every register write. A
 * request already queued is not withdrawn if the condition drops before the
 * CPU takes it (the queue in xc/cmt.c has no cancel); the firmware's ISRs
 * clear TDFE before they leave, which is the case this has to survive.
 */
#define CDJ850_SCIF_SIZE    0x30
#define CDJ850_SCIF_TX      0x0C
#define CDJ850_SCIF_SR      0x10
#define CDJ850_SCIF_RX      0x14
#define CDJ850_SCIF_COUNT   0x1C
/* SCFSR ready bits: TDFE (TX FIFO empty) + TEND (transmit end). */
#define CDJ850_SCIF_READY   0x0060u
#define CDJ850_SCIF_SCSCR   0x09    /* low byte of the 16-bit SCSCR */
#define CDJ850_SCIF_SCFSR   0x11    /* low byte of the 16-bit SCFSR */
#define CDJ850_SCIF_SCFCR   0x19    /* low byte of the 16-bit SCFCR */
#define CDJ850_SCIF_FDR_T   0x1C
#define CDJ850_SCIF_FDR_R   0x1D
#define CDJ850_SCIF_FIFO    16
#define SCSCR_TIE           0x80
#define SCSCR_TE            0x20
#define SCSCR_RE            0x10
#define SCFCR_TFRST         0x04
#define SCFCR_RFRST         0x02
#define SCFSR_TEND          0x40
#define SCFSR_TDFE          0x20
#define SCFSR_RDF           0x02
#define SCFSR_DR            0x01
/*
 * SCIF1 TXI1 is SH-2A vector 199. The firmware programs IPR13 bits 7:4 = 9
 * for it (0x0C0F9F54..0x0C0F9F70, intc write +0x40e = 0x90); that value is
 * used as a constant here because xc/intc.c does not expose IPR13.
 */
#define CDJ850_SCIF1_TXI_VECTOR 199
#define CDJ850_SCIF1_TXI_LEVEL  9

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_SCIF_SIZE];
    unsigned unit;
    /* SCIF1 panel link only (unit 1) */
    uint8_t txf[CDJ850_SCIF_FIFO];
    uint8_t rxf[CDJ850_SCIF_FIFO];
    uint32_t tx_n;           /* bytes waiting in the TX FIFO */
    uint32_t rx_n;           /* bytes waiting in the RX FIFO */
    uint32_t rx_head;        /* index of the next byte SCFRDR returns */
    bool tdfe;               /* TDFE flag, before the "FIFO empty" gating */
    bool tend;
    bool irq_line;           /* TDFE && TIE as last evaluated */
    uint32_t frames;         /* frames the panel has clocked */
} Cdj850Scif;

static Cdj850Scif *cdj850_scif1;

static uint64_t cdj850_scif_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Scif *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) & (CDJ850_SCIF_SIZE - 1)];
    }
    if (off <= CDJ850_SCIF_SR + 1 && CDJ850_SCIF_SR + 1 < off + size) {
        /* Low byte of SCFSR: TDFE + TEND always read back set (reset
         * values; the instant-drain FIFO never fills). */
        unsigned shift = (off + size - 1 - (CDJ850_SCIF_SR + 1)) * 8;

        v |= (uint64_t)(CDJ850_SCIF_READY & 0xff) << shift;
    }
    if (cdj850_trace()) {
        info_report("cdj850: scif%u read +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", s->unit, (unsigned)off, size,
                    (unsigned)v, s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_scif_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    Cdj850Scif *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->reg[(off + size - 1 - i) & (CDJ850_SCIF_SIZE - 1)] = val >> (8 * i);
    }
    if (off <= CDJ850_SCIF_TX && CDJ850_SCIF_TX < off + size) {
        unsigned shift = (off + size - 1 - CDJ850_SCIF_TX) * 8;
        unsigned byte = (val >> shift) & 0xff;

        if (cdj850_trace()) {
            info_report("cdj850: scif%u TX 0x%02x ('%c') at pc=0x%08x "
                        "%" PRIu64 " ms", s->unit, byte,
                        (byte >= 32 && byte < 127) ? byte : '.',
                        s->cpu->env.pc, cdj850_vms());
        }
    } else if (cdj850_trace()) {
        info_report("cdj850: scif%u write +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", s->unit, (unsigned)off, size,
                    (unsigned)val, s->cpu->env.pc, cdj850_vms());
    }
}

/* ---- SCIF1: panel link, clock-synchronous slave ---------------------- */

static bool scif1_tie(Cdj850Scif *s)
{
    return s->reg[CDJ850_SCIF_SCSCR] & SCSCR_TIE;
}

static bool scif1_tdfe(Cdj850Scif *s)
{
    return s->tdfe && s->tx_n == 0;
}

static void scif1_raise(Cdj850Scif *s)
{
    if (cdj850_trace()) {
        info_report("cdj850: scif1 TXI1 vector %u level %u %" PRIu64 " ms",
                    CDJ850_SCIF1_TXI_VECTOR, CDJ850_SCIF1_TXI_LEVEL,
                    cdj850_vms());
    }
    cdj850_irq_raise(s->cpu, CDJ850_SCIF1_TXI_VECTOR,
                     CDJ850_SCIF1_TXI_LEVEL);
}

/* Queue TXI1 on the rising edge of (TDFE && TIE). */
static void scif1_update_irq(Cdj850Scif *s)
{
    bool line = scif1_tdfe(s) && scif1_tie(s);

    if (line && !s->irq_line) {
        scif1_raise(s);
    }
    s->irq_line = line;
}

static void scif1_rx_flush(Cdj850Scif *s)
{
    s->rx_n = 0;
    s->rx_head = 0;
}

static uint8_t scif1_read_byte(Cdj850Scif *s, hwaddr a)
{
    switch (a) {
    case CDJ850_SCIF_SR:            /* PER/FER counts */
        return 0;
    case CDJ850_SCIF_SCFSR:
        return (scif1_tdfe(s) ? SCFSR_TDFE : 0) |
               ((s->tend && s->tx_n == 0) ? SCFSR_TEND : 0) |
               (s->rx_n ? SCFSR_RDF | SCFSR_DR : 0);
    case CDJ850_SCIF_RX:
        if (s->rx_n) {
            uint8_t b = s->rxf[s->rx_head];

            s->rx_head = (s->rx_head + 1) % CDJ850_SCIF_FIFO;
            s->rx_n--;
            return b;
        }
        return 0;
    case CDJ850_SCIF_FDR_T:
        return s->tx_n & 0x1f;
    case CDJ850_SCIF_FDR_R:
        return s->rx_n & 0x1f;
    default:
        return s->reg[a];
    }
}

/*
 * Offsets are used unmasked here: an access never leaves the 0x30-byte
 * region. (SCIF0's handlers above mask with SIZE - 1 = 0x2F, which is not a
 * bit mask and aliases +0x11 to +0x01, +0x19 to +0x09 and so on; SCIF0 is
 * left exactly as it was.)
 */
static uint64_t cdj850_scif1_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Scif *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | scif1_read_byte(s, off + i);
    }
    if (cdj850_trace()) {
        info_report("cdj850: scif1 read +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms (tx=%u rx=%u)", (unsigned)off, size,
                    (unsigned)v, s->cpu->env.pc, cdj850_vms(), s->tx_n,
                    s->rx_n);
    }
    return v;
}

static void scif1_write_byte(Cdj850Scif *s, hwaddr a, uint8_t b)
{
    switch (a) {
    case CDJ850_SCIF_TX:
        s->reg[a] = b;
        if (s->tx_n < CDJ850_SCIF_FIFO) {
            s->txf[s->tx_n++] = b;
        }
        if (cdj850_trace()) {
            info_report("cdj850: scif1 TX 0x%02x ('%c') n=%u at pc=0x%08x "
                        "%" PRIu64 " ms", b, (b >= 32 && b < 127) ? b : '.',
                        s->tx_n, s->cpu->env.pc, cdj850_vms());
        }
        return;
    case CDJ850_SCIF_SR:
        return;
    case CDJ850_SCIF_SCFSR:
        /* Only 0 can be written to TDFE/TEND; RDF/DR follow the RX FIFO. */
        if (!(b & SCFSR_TDFE)) {
            s->tdfe = false;
        }
        if (!(b & SCFSR_TEND)) {
            s->tend = false;
        }
        return;
    case CDJ850_SCIF_RX:
    case CDJ850_SCIF_FDR_T:
    case CDJ850_SCIF_FDR_R:
        return;
    case CDJ850_SCIF_SCFCR:
        s->reg[a] = b;
        if (b & SCFCR_TFRST) {
            s->tx_n = 0;
        }
        if (b & SCFCR_RFRST) {
            scif1_rx_flush(s);
        }
        return;
    case CDJ850_SCIF_SCSCR:
        s->reg[a] = b;
        if (!(b & SCSCR_TE)) {
            s->tx_n = 0;
        }
        if (!(b & SCSCR_RE)) {
            scif1_rx_flush(s);
        }
        return;
    default:
        s->reg[a] = b;
        return;
    }
}

static void cdj850_scif1_write(void *opaque, hwaddr off, uint64_t val,
                               unsigned size)
{
    Cdj850Scif *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        scif1_write_byte(s, off + size - 1 - i, val >> (8 * i));
    }
    if (cdj850_trace() &&
        !(off <= CDJ850_SCIF_TX && CDJ850_SCIF_TX < off + size)) {
        info_report("cdj850: scif1 write +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
    scif1_update_irq(s);
}

unsigned cdj850_scif1_tx_pending(void)
{
    return cdj850_scif1 ? cdj850_scif1->tx_n : 0;
}

bool cdj850_scif1_tie(void)
{
    return cdj850_scif1 && scif1_tie(cdj850_scif1);
}

void cdj850_scif1_exchange(const uint8_t rx[16], uint8_t tx_out[16])
{
    Cdj850Scif *s = cdj850_scif1;
    unsigned i;

    memset(tx_out, 0, CDJ850_SCIF_FIFO);
    if (!s) {
        return;
    }
    /* The panel clocks out whatever MAIN has queued (TE on); the TX FIFO
     * has drained when the frame is done. */
    if (s->reg[CDJ850_SCIF_SCSCR] & SCSCR_TE) {
        memcpy(tx_out, s->txf, s->tx_n);
        s->tx_n = 0;
    }
    /* The same clocks shift 16 bytes into the RX FIFO (RE on, RX FIFO not
     * held in reset). All 16 are in before TXI1, spec section 3. */
    if ((s->reg[CDJ850_SCIF_SCSCR] & SCSCR_RE) &&
        !(s->reg[CDJ850_SCIF_SCFCR] & SCFCR_RFRST)) {
        for (i = 0; i < CDJ850_SCIF_FIFO && s->rx_n < CDJ850_SCIF_FIFO; i++) {
            s->rxf[(s->rx_head + s->rx_n) % CDJ850_SCIF_FIFO] = rx[i];
            s->rx_n++;
        }
    }
    s->frames++;
    s->tdfe = true;
    s->tend = true;
    if (cdj850_trace()) {
        info_report("cdj850: scif1 frame %u clocked, rx fifo=%u %" PRIu64
                    " ms", s->frames, s->rx_n, cdj850_vms());
    }
    if (scif1_tie(s)) {
        s->irq_line = true;
        scif1_raise(s);
    } else {
        s->irq_line = false;
    }
}

static const MemoryRegionOps cdj850_scif_ops = {
    .read = cdj850_scif_read,
    .write = cdj850_scif_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const MemoryRegionOps cdj850_scif1_ops = {
    .read = cdj850_scif1_read,
    .write = cdj850_scif1_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_scif = {
    .name = "cdj850-scif",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Scif, CDJ850_SCIF_SIZE),
        VMSTATE_UINT32(unit, Cdj850Scif),
        VMSTATE_UINT8_ARRAY_V(txf, Cdj850Scif, CDJ850_SCIF_FIFO, 2),
        VMSTATE_UINT8_ARRAY_V(rxf, Cdj850Scif, CDJ850_SCIF_FIFO, 2),
        VMSTATE_UINT32_V(tx_n, Cdj850Scif, 2),
        VMSTATE_UINT32_V(rx_n, Cdj850Scif, 2),
        VMSTATE_UINT32_V(rx_head, Cdj850Scif, 2),
        VMSTATE_BOOL_V(tdfe, Cdj850Scif, 2),
        VMSTATE_BOOL_V(tend, Cdj850Scif, 2),
        VMSTATE_BOOL_V(irq_line, Cdj850Scif, 2),
        VMSTATE_UINT32_V(frames, Cdj850Scif, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void cdj850_scif_reset(void *opaque)
{
    Cdj850Scif *s = opaque;

    memset(s->reg, 0, sizeof(s->reg));
    s->tx_n = s->rx_n = s->rx_head = 0;
    s->tdfe = s->tend = true;
    s->irq_line = false;
}

static void cdj850_scif_one(MemoryRegion *sysmem, SuperHCPU *cpu,
                            hwaddr base, unsigned unit, const char *name)
{
    Cdj850Scif *s = g_new0(Cdj850Scif, 1);

    s->cpu = cpu;
    s->unit = unit;
    if (unit == 1) {
        cdj850_scif1 = s;
    }
    qemu_register_reset(cdj850_scif_reset, s);
    cdj850_scif_reset(s);
    vmstate_register_any(NULL, &vmstate_cdj850_scif, s);
    memory_region_init_io(&s->iomem, NULL,
                          unit == 1 ? &cdj850_scif1_ops : &cdj850_scif_ops,
                          s, name, CDJ850_SCIF_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, base, &s->iomem, 1);
}

void cdj850_scif_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    cdj850_scif_one(sysmem, cpu, 0xFFFE8000, 0, "cdj850.scif0");
    cdj850_scif_one(sysmem, cpu, 0xFFFE8800, 1, "cdj850.scif1");
}
