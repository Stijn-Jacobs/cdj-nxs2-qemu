/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "cdj_usbf.h"
#include "usb_r8a66597.h"
#include "qemu/timer.h"
#include "qemu/bswap.h"
#include "cmt.h"
#include "dmac.h"
#include "intc.h"
#include "port.h"
#include "trace.h"
#include "usb.h"

/*
 * SH7263 USB 2.0 host/function module at 0xFFFC1C00 (SH7263 Hardware
 * Manual R01UH0459EJ0400 section 25), as a front end to the shared
 * R8A66597-family host model in hw/cdj/common/usb_r8a66597.c ("cdj.usb").
 * This file translates register offsets, bit positions and byte order;
 * the shared model only gained two properties, both off by default and set
 * here: "pipe-ep0" and "dcp-per-packet" (reasons at cdj850_usb_init()).
 *
 * Established (manual Table 25.2, MAIN firmware, service manual):
 *
 * - The SH7263 register layout is NOT the SH7724 one the shared model
 *   implements. An earlier ledger entry claimed an exact match; the manual
 *   and the firmware's own accesses (CFIFOSEL at +0x1E, PIPECFG at +0x66,
 *   FIFO ports at +0x10/+0x14/+0x18, transceiver setup at +0x84) disagree.
 *   Identical: SYSCFG +0x00, INTENB0..DCPCTR +0x30..+0x60, PIPESEL +0x64,
 *   PIPEnCTR +0x70..+0x7C. Moved: SYSSTS +0x02 (model +0x04), DVSTCTR
 *   +0x04 (+0x08), CFIFO +0x10 (+0x14), D0FIFO +0x14 (+0x100), D1FIFO
 *   +0x18 (+0x120), CFIFOSEL/CTR +0x1E/+0x20 (+0x20/+0x22), D0FIFOSEL/CTR
 *   +0x24/+0x26 (+0x28/+0x2A), D1FIFOSEL/CTR +0x2A/+0x2C (+0x2C/+0x2E),
 *   PIPECFG/BUF/MAXP/PERI +0x66..+0x6C (+0x68..+0x6E). SH7263-only, held
 *   here: TESTMODE +0x06, CFBCFG/D0FBCFG/D1FBCFG +0x0A/+0x0C/+0x0E,
 *   CFIFOSIE +0x22, D0FIFOTRN +0x28, D1FIFOTRN +0x2E, USBACSWR +0x84.
 * - DEVSEL (target device address in host mode) is DCPMAXP/PIPEMAXP bits
 *   15:14 (25.3.29, 25.3.34); the model reads bits 15:12. Translated.
 * - The transaction counter is per DMA FIFO port: DnFIFOSEL bit 9 TRENB,
 *   bit 8 TRCLR, count in DnFIFOTRN (25.3.7, 25.3.10). The model keeps it
 *   per pipe (PIPEnTRE/PIPEnTRN); it is copied to the pipe named by the
 *   port's CURPIPE.
 * - INTSTS1 bits 10:8 (BEMP/NRDY/BRDY) mirror INTSTS0 (25.3.17).
 * - INTSTS0 bit 7 VBSTS is the VBUS pin. Service manual 10.2 MAIN ASSY
 *   (IC103 pin 102): USB_ABSEL (PB10) low routes the USB-B port's VBUS_B
 *   to the pin, high ties it to 5 V. So VBSTS = PB10 here (no PC is ever
 *   connected to the B port).
 * - FIFO port byte order (25.3.5, Tables 25.5-25.7): xFBCFG bit 8 FEND = 0
 *   (what the firmware programs) puts the first buffer byte in bits 31:24,
 *   i.e. memory order on this big-endian CPU, for 8/16/32-bit accesses at
 *   the port's base address. The model packs the first byte in bits 7:0.
 *   FEND = 1 is the model's order.
 * - Interrupt: USBI is vector 140, priority IPR08 bits 15:12 (manual
 *   Table 6.4; IPR08 = 0xFFFE0C04). The firmware sets priority 6 at
 *   0x0C018F56 and registers ISR 0x0C10B20C, which dispatches on the USB
 *   mode word 0x0CEE505C (1 = host -> 0x0C018F44 -> 0x0C019878). The
 *   module's interrupt output is a level: it stays asserted while any
 *   enabled status bit is set. That level is kept here and re-presented
 *   (100 us poll) while asserted, and withdrawn when it drops.
 * - Control transfers (driver 0x0C01B31E..0x0C01B4E2): setup via SUREQ,
 *   data stage on the DCP with DCPCFG = 0 (non-continuous, one BRDY per
 *   packet), status stage of a control read as a zero-length OUT through
 *   pipe 5 (PIPECFG 0x8010: EPNUM 0, DIR transmit; CFIFOCTR BVAL, then
 *   PIPE5CTR = SQSET | BUF), because DCPCTR CCPL must stay 0 in host mode
 *   (25.3.30). No-data requests end with an IN on the DCP.
 * - Board wiring (service manual 10.2 MAIN ASSY 1/4 and 10.15): PB11 =
 *   USB_VBUSON (A-port power), PB10 = USB_ABSEL (TS3USB30 switch IC107, high
 *   = A port), PE10 = USB_CDETFLG (over-current flag, high = normal; see
 *   xc/intc.c and xc/port.c).
 *
 * Assumed / not modelled:
 * - The USB-A device sees the bus regardless of USB_VBUSON/USB_ABSEL; the
 *   firmware sets both (PBDRL = 0x0C08) before arming BCHGE, so the
 *   attach report of the shared model (BCHG 500 ms after BCHGE is armed)
 *   falls after power-up either way.
 * - DMA: the shared model's own DREQ hook (cdj_dmac_dreq(), SH-4A DMAC)
 *   is a no-op on this board. Instead, after every register write the
 *   DnFIFO request state is offered to the SH7263 DMAC model (xc/dmac.c,
 *   USB_DMA0/USB_DMA1); see cdj850_usb_dma_request() for when a request is
 *   taken to be active, and usb_tx_dma() for the transmit direction.
 * - CFIFOSIE, TESTMODE, USBACSWR and the FWAIT/TENDE fields are storage.
 *
 * CDJ850_TRACE=1 logs every access with SH7263 offsets; CDJ_USB_DEBUG=1 is
 * the shared model's own transfer log (model offsets).
 */
#define CDJ850_USB_BASE     0xFFFC1C00
#define CDJ850_USB_SIZE     0x100
#define CDJ850_USBI_VECTOR  140
#define CDJ850_INTC_IPR08   0x404          /* 0xFFFE0C04 */
#define CDJ850_USB_POLL_NS  100000

/* SH7263 offsets (manual Table 25.2) that need more than a pass-through. */
#define U_SYSCFG     0x00
#define U_SYSSTS     0x02
#define U_DVSTCTR    0x04
#define U_TESTMODE   0x06
#define U_CFBCFG     0x0A
#define U_D0FBCFG    0x0C
#define U_D1FBCFG    0x0E
#define U_FIFO_LO    0x10                  /* CFIFO, D0FIFO, D1FIFO */
#define U_FIFO_HI    0x1C
#define U_CFIFOSEL   0x1E
#define U_CFIFOCTR   0x20
#define U_D0FIFOSEL  0x24
#define U_D0FIFOCTR  0x26
#define U_D0FIFOTRN  0x28
#define U_D1FIFOSEL  0x2A
#define U_D1FIFOCTR  0x2C
#define U_D1FIFOTRN  0x2E
#define U_INTSTS0    0x40
#define U_INTSTS1    0x42
#define U_DCPMAXP    0x5E
#define U_PIPESEL    0x64
#define U_PIPECFG    0x66
#define U_PIPEBUF    0x68
#define U_PIPEMAXP   0x6A
#define U_PIPEPERI   0x6C
#define U_USBACSWR   0x84                  /* 32-bit, two halves here */

#define U_FEND       0x0100                /* xFBCFG bit 8 */
#define U_TRENB      0x0200                /* DnFIFOSEL bit 9 */
#define U_TRCLR      0x0100                /* DnFIFOSEL bit 8 */
#define U_VBSTS      0x0080                /* INTSTS0 bit 7 */
#define U_PB10_ABSEL 0x0400

typedef struct {
    MemoryRegion iomem;
    DeviceState *host_dev;                 /* the cdj.usb host device */
    MemoryRegion *model;                   /* the cdj.usb host register file */
    CdjUsbfState *usbf;                    /* USB-B R8A66597 function device */
    MemoryRegion *usbf_mmio;               /* cdj.usbf register file */
    SuperHCPU *cpu;
    QEMUTimer *poll;
    Notifier exit;
    bool level;                            /* host model's interrupt output */
    bool usbf_level;                       /* function model's interrupt output */
    uint16_t loc[CDJ850_USB_SIZE / 2];     /* SH7263-only registers */
    uint8_t unimp_logged[CDJ850_USB_SIZE / 2];
    unsigned raised;                       /* vector 140 requests queued */
    uint16_t pipecfg[CDJ_USB_NR_PIPES];    /* PIPECFG per pipe, for DIR */
    bool tx_dreq[3];                       /* D0/D1FIFO transmit request */
} Cdj850Usb;

static Cdj850Usb *cdj850_usb;

static const hwaddr usb_fifo_model[CDJ_USB_NR_FIFO_PORTS] = {
    CDJ_USB_CFIFO, CDJ_USB_D0FIFO, CDJ_USB_D1FIFO,
};

/* SH7263 offset -> model offset for the 16-bit registers the model holds,
 * or -1 for registers held here. */
static int usb_map(hwaddr off)
{
    switch (off) {
    case U_SYSCFG:    return CDJ_USB_SYSCFG;
    case U_SYSSTS:    return CDJ_USB_SYSSTS0;
    case U_DVSTCTR:   return CDJ_USB_DVSTCTR0;
    case U_CFIFOSEL:  return CDJ_USB_CFIFOSEL;
    case U_CFIFOCTR:  return CDJ_USB_CFIFOCTR;
    case U_D0FIFOSEL: return CDJ_USB_D0FIFOSEL;
    case U_D0FIFOCTR: return CDJ_USB_D0FIFOCTR;
    case U_D1FIFOSEL: return CDJ_USB_D1FIFOSEL;
    case U_D1FIFOCTR: return CDJ_USB_D1FIFOCTR;
    case U_PIPESEL:   return CDJ_USB_PIPESEL;
    case U_PIPECFG:   return CDJ_USB_PIPECFG;
    case U_PIPEBUF:   return CDJ_USB_PIPEBUF;
    case U_PIPEMAXP:  return CDJ_USB_PIPEMAXP;
    case U_PIPEPERI:  return CDJ_USB_PIPEPERI;
    default:
        break;
    }
    /* INTENB0 (+0x30) .. DCPCTR (+0x60) and PIPE1CTR (+0x70) .. PIPE7CTR
     * (+0x7C) sit at the same offsets in both layouts. */
    if ((off >= 0x30 && off <= 0x60) || (off >= 0x70 && off <= 0x7C)) {
        return off;
    }
    return -1;
}

static uint64_t mr_rd(MemoryRegion *mr, hwaddr moff, unsigned size)
{
    uint64_t v = 0;

    memory_region_dispatch_read(mr, moff, &v, size_memop(size) | MO_LE,
                                MEMTXATTRS_UNSPECIFIED);
    return v;
}

static void mr_wr(MemoryRegion *mr, hwaddr moff, uint64_t v, unsigned size)
{
    memory_region_dispatch_write(mr, moff, v, size_memop(size) | MO_LE,
                                 MEMTXATTRS_UNSPECIFIED);
}

static bool usb_func_mode(const Cdj850Usb *s)
{
    return s->usbf_mmio && !(mr_rd(s->model, CDJ_USB_SYSCFG, 2) & 0x0040);
}

static MemoryRegion *active_mr(Cdj850Usb *s)
{
    return usb_func_mode(s) ? s->usbf_mmio : s->model;
}

static uint64_t active_rd(Cdj850Usb *s, hwaddr moff, unsigned size)
{
    return mr_rd(active_mr(s), moff, size);
}

static void active_wr(Cdj850Usb *s, hwaddr moff, uint64_t v, unsigned size)
{
    mr_wr(active_mr(s), moff, v, size);
}

/* The model updates its interrupt output on writes only; a FIFO read can
 * raise BRDY (next instalment of a long IN transfer). A write to a model
 * offset no SH7263 register maps to has no effect beyond re-evaluating the
 * output. */
#define MODEL_SCRATCH 0x0FFE

static void model_sync_irq(Cdj850Usb *s)
{
    mr_wr(s->model, MODEL_SCRATCH, 0, 2);
}

/* DEVSEL: SH7263 bits 15:14, model bits 15:12. */
static uint16_t maxp_to_model(uint16_t v)
{
    return (v & 0x07FF) | (((v >> 14) & 3) << 12);
}

static uint16_t maxp_from_model(uint16_t v)
{
    return (v & 0x07FF) | (((v >> 12) & 3) << 14);
}

/* Copy DnFIFOSEL TRENB + DnFIFOTRN to the model's per-pipe counter for the
 * pipe the port names (n = 1 or 2). */
static void usb_sync_trn(Cdj850Usb *s, unsigned n)
{
    uint16_t sel;
    unsigned pipe;
    unsigned trn_off = n == 1 ? U_D0FIFOTRN : U_D1FIFOTRN;

    if (usb_func_mode(s)) {
        return;
    }
    sel = mr_rd(s->model, n == 1 ? CDJ_USB_D0FIFOSEL : CDJ_USB_D1FIFOSEL, 2);
    pipe = sel & 7;
    if (pipe < 1 || pipe > 5) {
        return;
    }
    mr_wr(s->model, CDJ_USB_PIPETRN(pipe), s->loc[trn_off / 2], 2);
    mr_wr(s->model, CDJ_USB_PIPETRE(pipe), (sel & U_TRENB) ? 0x0200 : 0, 2);
}

/*
 * USB_DMA0/USB_DMA1 transfer request for D0FIFO (n = 1) / D1FIFO (n = 2),
 * with DnFIFOSEL DREQE set:
 * - receiving pipe ("receive FIFO full"): data to read (DTLN), or a counted
 *   transfer still running (PID = BUF, TRENB, count not yet reached, no
 *   NRDY) whose next instalment the model fetches when the FIFO is read;
 * - transmitting pipe ("transmit FIFO empty"): only while the PIPEnCTR
 *   write that sets PID = BUF is being handled, see usb_tx_dma().
 */
bool cdj850_usb_dma_request(unsigned n)
{
    Cdj850Usb *s = cdj850_usb;
    uint16_t sel;
    unsigned pipe;

    if (!s || (n != 1 && n != 2)) {
        return false;
    }
    sel = active_rd(s, n == 1 ? CDJ_USB_D0FIFOSEL : CDJ_USB_D1FIFOSEL, 2);
    pipe = sel & 7;
    if (!(sel & 0x1000) || !pipe) {
        return false;
    }
    if (s->pipecfg[pipe] & CDJ_USB_DIR) {
        return s->tx_dreq[n];
    }
    if (active_rd(s, n == 1 ? CDJ_USB_D0FIFOCTR : CDJ_USB_D1FIFOCTR, 2) &
        CDJ_USB_DTLN) {
        return true;
    }
    return !usb_func_mode(s) && pipe <= 5 &&
           (mr_rd(s->model, CDJ_USB_PIPECTR(pipe), 2) & CDJ_USB_PID_MASK) ==
               CDJ_USB_PID_BUF &&
           (mr_rd(s->model, CDJ_USB_PIPETRE(pipe), 2) & 0x0200) &&
           mr_rd(s->model, CDJ_USB_PIPETRN(pipe), 2) != 0 &&
           !(mr_rd(s->model, CDJ_USB_NRDYSTS, 2) & (1u << pipe));
}

/*
 * Transmit DMA. The firmware arms the DMAC (SAR = buffer, DAR = D0FIFO,
 * CHCR = 0x1815) and DnFIFOSEL (DREQE, CURPIPE = the bulk-out pipe) first,
 * then sets the pipe's PID to BUF (0x0C01B9xx-0x0C01BA2E). On hardware the
 * empty buffer then requests data, the DMAC fills it and the module sends
 * it. The shared model transmits what the buffer holds at the moment PID =
 * BUF is written, so the DMAC is served synchronously here, before that
 * write is passed on. A DMAC armed only after PID = BUF is not served.
 */
static void usb_tx_dma(Cdj850Usb *s, unsigned pipe)
{
    unsigned n;

    if (pipe >= CDJ_USB_NR_PIPES || !(s->pipecfg[pipe] & CDJ_USB_DIR)) {
        return;
    }
    for (n = 1; n <= 2; n++) {
        uint16_t sel = active_rd(s, n == 1 ? CDJ_USB_D0FIFOSEL
                                           : CDJ_USB_D1FIFOSEL, 2);

        if ((sel & 0x1000) && (sel & 7) == pipe) {
            s->tx_dreq[n] = true;
            cdj850_dmac_serve(n == 1 ? CDJ850_DMARS_USB_DMA0
                                     : CDJ850_DMARS_USB_DMA1);
            s->tx_dreq[n] = false;
        }
    }
}

static void usb_dreq_check(Cdj850Usb *s)
{
    if (cdj850_usb_dma_request(1)) {
        cdj850_dmac_request(CDJ850_DMARS_USB_DMA0);
    }
    if (cdj850_usb_dma_request(2)) {
        cdj850_dmac_request(CDJ850_DMARS_USB_DMA1);
    }
}

static unsigned usb_prio(void)
{
    return (cdj850_intc_reg16(CDJ850_INTC_IPR08) >> 12) & 0xF;
}

/* Present vector 140 while the level is asserted and enabled. */
static void usb_irq_eval(Cdj850Usb *s)
{
    unsigned prio = usb_prio();
    bool lvl = usb_func_mode(s) ? s->usbf_level : s->level;

    if (!lvl) {
        cdj850_irq_withdraw(s->cpu, CDJ850_USBI_VECTOR);
        return;
    }
    if (prio && !cdj850_irq_pending(CDJ850_USBI_VECTOR)) {
        s->raised++;
        if (cdj850_trace() || getenv("CDJ850_IRQ_DEBUG")) {
            info_report("cdj850: usb USBI -> vector %u level %u at %" PRIu64
                        " ms", CDJ850_USBI_VECTOR, prio, cdj850_vms());
        }
        cdj850_irq_raise(s->cpu, CDJ850_USBI_VECTOR, prio);
    }
    /* Level semantics: re-present after the ISR returns if still asserted,
     * and pick up a priority written after the line rose. */
    timer_mod(s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              CDJ850_USB_POLL_NS);
}

static void usb_irq_poll(void *opaque)
{
    usb_irq_eval(opaque);
}

static void usb_irq_in(void *opaque, int n, int level)
{
    Cdj850Usb *s = opaque;

    s->level = level != 0;
    usb_irq_eval(s);
}

static void usb_usbf_irq_in(void *opaque, int n, int level)
{
    Cdj850Usb *s = opaque;

    s->usbf_level = level != 0;
    usb_irq_eval(s);
}

void cdj850_usb_mux(uint16_t pbdrl)
{
    Cdj850Usb *s = cdj850_usb;

    if (s && s->usbf) {
        cdj_usbf_set_cable(s->usbf, !(pbdrl & U_PB10_ABSEL));
    }
}

static void usb_log_unimp(Cdj850Usb *s, hwaddr off, bool write)
{
    if (!s->unimp_logged[off / 2]) {
        s->unimp_logged[off / 2] = 1;
        qemu_log_mask(LOG_UNIMP, "cdj850.usb: %s of unmodelled register "
                      "+0x%02x (storage only)\n", write ? "write" : "read",
                      (unsigned)off);
    }
}

static uint16_t usb_reg_read(Cdj850Usb *s, hwaddr off)
{
    int moff = usb_map(off);
    uint16_t v;

    switch (off) {
    case U_TESTMODE:
    case U_CFBCFG:
    case U_D0FBCFG:
    case U_D1FBCFG:
    case U_D0FIFOTRN:
    case U_D1FIFOTRN:
    case U_USBACSWR:
    case U_USBACSWR + 2:
        return s->loc[off / 2];
    default:
        break;
    }
    if (moff < 0) {
        usb_log_unimp(s, off, false);
        return s->loc[off / 2];
    }
    v = active_rd(s, moff, 2);
    switch (off) {
    case U_INTSTS0:
        v &= ~U_VBSTS;
        if (cdj850_port_pbdrl() & U_PB10_ABSEL) {
            v |= U_VBSTS;
        } else if (s->usbf && cdj_usbf_has_vbus(s->usbf)) {
            v |= U_VBSTS;
        }
        break;
    case U_INTSTS1:
        v |= active_rd(s, CDJ_USB_INTSTS0, 2) &
             (CDJ_USB_BRDY | CDJ_USB_NRDY | CDJ_USB_BEMP);
        break;
    case U_DCPMAXP:
    case U_PIPEMAXP:
        v = maxp_from_model(v);
        break;
    default:
        break;
    }
    return v;
}

static void usb_reg_write(Cdj850Usb *s, hwaddr off, uint16_t v)
{
    int moff = usb_map(off);

    switch (off) {
    case U_SYSCFG:
        /* SYSCFG is shared across the Host/Function role bit (DCFM): keep both
         * halves updated so FUN_0c002c02 / FUN_0c0d19f8 read-modify-writes carry
         * USBE/HSE across the DCFM flip cleanly. */
        mr_wr(s->model, CDJ_USB_SYSCFG, v, 2);
        if (s->usbf_mmio) {
            mr_wr(s->usbf_mmio, CDJ_USB_SYSCFG, v, 2);
        }
        usb_irq_eval(s);
        return;
    case U_TESTMODE:
    case U_CFBCFG:
    case U_D0FBCFG:
    case U_D1FBCFG:
    case U_USBACSWR:
    case U_USBACSWR + 2:
        s->loc[off / 2] = v;
        return;
    case U_D0FIFOTRN:
    case U_D1FIFOTRN:
        s->loc[off / 2] = v;
        usb_sync_trn(s, off == U_D0FIFOTRN ? 1 : 2);
        return;
    case U_DCPMAXP:
    case U_PIPEMAXP:
        v = maxp_to_model(v);
        break;
    case U_PIPECFG:
        s->pipecfg[active_rd(s, CDJ_USB_PIPESEL, 2) % CDJ_USB_NR_PIPES] = v;
        break;
    default:
        break;
    }
    if (moff < 0) {
        usb_log_unimp(s, off, true);
        s->loc[off / 2] = v;
        return;
    }
    if (off >= 0x70 && off <= 0x7C && (v & CDJ_USB_PID_MASK) == CDJ_USB_PID_BUF) {
        usb_tx_dma(s, (off - 0x70) / 2 + 1);
    }
    if (off == U_D0FIFOSEL || off == U_D1FIFOSEL) {
        unsigned n = off == U_D0FIFOSEL ? 1 : 2;

        if (v & U_TRCLR) {           /* write-1 action, reads back 0 */
            /* TRCLR clears the current counter, not the preset written
             * to DnFIFOTRN (25.4.4(e)). usb_sync_trn() reloads the shared
             * model's remaining count from that unchanged preset. */
            v &= ~U_TRCLR;
        }
        active_wr(s, moff, v, 2);
        usb_sync_trn(s, n);
        return;
    }
    active_wr(s, moff, v, 2);
}

static uint64_t usb_fifo_access(Cdj850Usb *s, hwaddr off, uint64_t val,
                                unsigned size, bool write)
{
    unsigned port = (off - U_FIFO_LO) / 4;
    bool fend = s->loc[(U_CFBCFG + 2 * port) / 2] & U_FEND;
    hwaddr moff = usb_fifo_model[port];

    if (write) {
        if (!fend) {
            val = size == 4 ? bswap32(val) : size == 2 ? bswap16(val) : val;
        }
        active_wr(s, moff, val, size);
        return 0;
    }
    val = active_rd(s, moff, size);
    if (!usb_func_mode(s)) {
        model_sync_irq(s);
    }
    if (!fend) {
        val = size == 4 ? bswap32(val) : size == 2 ? bswap16(val) : val;
    }
    return val;
}

static uint64_t cdj850_usb_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Usb *s = opaque;
    uint64_t v;

    if (off >= U_FIFO_LO && off < U_FIFO_HI) {
        v = usb_fifo_access(s, off, 0, size, false);
    } else if (size == 4) {
        v = ((uint64_t)usb_reg_read(s, off & ~1) << 16) |
            usb_reg_read(s, (off & ~1) + 2);
    } else if (size == 2) {
        v = usb_reg_read(s, off & ~1);
    } else {
        v = usb_reg_read(s, off & ~1);
        v = (off & 1) ? v & 0xff : v >> 8;
    }
    if (cdj850_trace()) {
        info_report("cdj850: usb read +0x%02x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)v,
                    s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_usb_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    Cdj850Usb *s = opaque;

    if (cdj850_trace()) {
        info_report("cdj850: usb write +0x%02x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
    if (off >= U_FIFO_LO && off < U_FIFO_HI) {
        usb_fifo_access(s, off, val, size, true);
    } else if (size == 4) {
        usb_reg_write(s, off & ~1, val >> 16);
        usb_reg_write(s, (off & ~1) + 2, val & 0xffff);
    } else if (size == 2) {
        usb_reg_write(s, off & ~1, val);
    } else {
        /* Byte write to a 16-bit register: merge with the other half. */
        uint16_t old = usb_reg_read(s, off & ~1);

        usb_reg_write(s, off & ~1, (off & 1) ? (old & 0xff00) | (val & 0xff)
                                             : (old & 0x00ff) | (val << 8));
    }
    /* A started pipe or a newly wired DnFIFO may raise a DMA request. */
    usb_dreq_check(s);
}

static const MemoryRegionOps cdj850_usb_ops = {
    .read = cdj850_usb_read,
    .write = cdj850_usb_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

/* Register view at exit, SH7263 names. */
static void cdj850_usb_dump(Notifier *n, void *unused)
{
    Cdj850Usb *s = container_of(n, Cdj850Usb, exit);

    info_report("cdj850: usb SYSCFG=0x%04x SYSSTS=0x%04x DVSTCTR=0x%04x "
                "INTSTS0=0x%04x INTENB0=0x%04x INTSTS1=0x%04x INTENB1=0x%04x "
                "DCPCTR=0x%04x level=%d IPR08.USB=%u raised=%u",
                usb_reg_read(s, U_SYSCFG), usb_reg_read(s, U_SYSSTS),
                usb_reg_read(s, U_DVSTCTR), usb_reg_read(s, U_INTSTS0),
                usb_reg_read(s, 0x30), usb_reg_read(s, U_INTSTS1),
                usb_reg_read(s, 0x32), usb_reg_read(s, 0x60), s->level,
                usb_prio(), s->raised);
    info_report("cdj850: usb BRDYSTS=0x%04x/ENB 0x%04x NRDYSTS=0x%04x/ENB "
                "0x%04x BEMPSTS=0x%04x/ENB 0x%04x CFIFOSEL=0x%04x "
                "D0FIFOSEL=0x%04x D1FIFOSEL=0x%04x",
                usb_reg_read(s, 0x46), usb_reg_read(s, 0x36),
                usb_reg_read(s, 0x48), usb_reg_read(s, 0x38),
                usb_reg_read(s, 0x4A), usb_reg_read(s, 0x3A),
                usb_reg_read(s, U_CFIFOSEL), usb_reg_read(s, U_D0FIFOSEL),
                usb_reg_read(s, U_D1FIFOSEL));
}

static int usb_post_load(void *opaque, int version_id)
{
    cdj850_usb_mux(cdj850_port_pbdrl());
    return 0;
}

static const VMStateDescription vmstate_cdj850_usb = {
    .name = "cdj850-usb",
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = usb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_TIMER_PTR(poll, Cdj850Usb),
        VMSTATE_BOOL(level, Cdj850Usb),
        VMSTATE_BOOL_V(usbf_level, Cdj850Usb, 2),
        VMSTATE_UINT16_ARRAY(loc, Cdj850Usb, CDJ850_USB_SIZE / 2),
        VMSTATE_UINT32(raised, Cdj850Usb),
        VMSTATE_UINT16_ARRAY(pipecfg, Cdj850Usb, CDJ_USB_NR_PIPES),
        VMSTATE_BOOL_ARRAY(tx_dreq, Cdj850Usb, 3),
        VMSTATE_END_OF_LIST()
    }
};

static void cdj850_usb_reset(void *opaque)
{
    Cdj850Usb *s = opaque;

    timer_del(s->poll);
    cdj_usb_reset_host(s->host_dev);
    cdj_usbf_reset(s->usbf);
    s->level = false;
    s->usbf_level = false;
    s->raised = 0;
    memset(s->pipecfg, 0, sizeof(s->pipecfg));
    memset(s->tx_dreq, 0, sizeof(s->tx_dreq));
    memset(s->loc, 0, sizeof(s->loc));
    /* Reset values (manual Table 25.2). */
    s->loc[U_TESTMODE / 2] = 0x0100;
    s->loc[U_CFBCFG / 2] = 0x000F;
    s->loc[U_D0FBCFG / 2] = 0x000F;
    s->loc[U_D1FBCFG / 2] = 0x000F;
    cdj850_irq_withdraw(s->cpu, CDJ850_USBI_VECTOR);
    cdj850_usb_mux(cdj850_port_pbdrl());
}

void cdj850_usb_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Usb *s = g_new0(Cdj850Usb, 1);
    DeviceState *dev = qdev_new("cdj.usb");
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->cpu = cpu;
    s->host_dev = dev;
    cdj850_usb = s;
    /* Reset values (manual Table 25.2). */
    s->loc[U_TESTMODE / 2] = 0x0100;
    s->loc[U_CFBCFG / 2] = 0x000F;
    s->loc[U_D0FBCFG / 2] = 0x000F;
    s->loc[U_D1FBCFG / 2] = 0x000F;

    /* The firmware sends the control-read status stage through pipe 5
     * set to EPNUM 0 (PIPECFG 0x8010, then CFIFOCTR BVAL and PIPE5CTR BUF
     * at 0x0C01B434..0x0C01B478); see the shared model's property. */
    qdev_prop_set_bit(dev, "pipe-ep0", true);
    /* DCPCFG = 0 (CNTMD clear, 0x0C01B330): non-continuous mode, one BRDY
     * per received packet (25.3.28, 25.4 control data stage). The driver
     * ends a control read on a short or zero-length packet and keeps
     * waiting when a whole buffer is a multiple of DCPMAXP (0x0C01AF36). */
    qdev_prop_set_bit(dev, "dcp-per-packet", true);
    sysbus_realize_and_unref(sbd, &error_fatal);
    /* The model's register file is reached only through this front end;
     * it is not mapped into the address space. */
    s->model = sysbus_mmio_get_region(sbd, 0);
    sysbus_connect_irq(sbd, 0, qemu_allocate_irq(usb_irq_in, s, 0));

    s->usbf = cdj_usbf_init(NULL, qemu_allocate_irq(usb_usbf_irq_in, s, 0));
    s->usbf_mmio = sysbus_mmio_get_region(SYS_BUS_DEVICE(s->usbf), 0);
    if (getenv("CDJ_USBF_HOST")) {
        cdj_usbf_host_init(s->usbf);
    }
    cdj850_usb_mux(cdj850_port_pbdrl());

    s->poll = timer_new_ns(QEMU_CLOCK_VIRTUAL, usb_irq_poll, s);
    qemu_register_reset(cdj850_usb_reset, s);
    vmstate_register_any(NULL, &vmstate_cdj850_usb, s);

    memory_region_init_io(&s->iomem, NULL, &cdj850_usb_ops, s, "cdj850.usb",
                          CDJ850_USB_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, CDJ850_USB_BASE, &s->iomem, 1);
    s->exit.notify = cdj850_usb_dump;
    cdj_add_exit_report(&s->exit);
}
