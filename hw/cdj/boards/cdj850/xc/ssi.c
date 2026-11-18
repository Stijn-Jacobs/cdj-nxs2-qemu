/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "../dsp56300.h"
#include "dmac.h"
#include "ssi.h"
#include "trace.h"

/*
 * SH7263 SSI0/SSI1 (manual section 18), connected to DSP ESAI RX0/RX1.
 * A DMA request is asserted while the enabled transmitter's data register
 * is empty. A serial word is delivered every stereo slot at 44.1 kHz.
 *
 * With SSICR.DWL = 16 bits (the MAIN firmware writes SSICR = 0x90102:
 * DWL = SWL = 001) each 32-bit SSITDR write carries two 16-bit data
 * words, which go out as two serial words: bits 15:0 first, then 31:16
 * (PDTA = 1 swaps the order). FUN_0C076474 relies on this: it packs two
 * frames of one channel per write, and the DSP matches its 0xCE5C/0xCA5A
 * block headers as 16-bit words left-justified in the 24-bit ESAI word.
 * Other word lengths keep the 24-bit left-aligned interpretation.
 */
#define CDJ850_SSI_BASE    0xFFFFC000
#define CDJ850_SSI_SIZE    0x810
#define CDJ850_SSI_ST0     0x004
#define CDJ850_SSI_TDR0    0x008
#define CDJ850_SSI_CR1     0x800
#define CDJ850_SSI_ST1     0x804
#define CDJ850_SSI_TDR1    0x808
#define CDJ850_SSICR_EN    0x00000001u
#define CDJ850_SSICR_TRMD  0x00000002u
#define CDJ850_SSICR_DMEN  0x10000000u
#define CDJ850_SSICR_PDTA  0x00000200u
#define CDJ850_SSICR_DWL(cr) (((cr) >> 19) & 7u)
#define CDJ850_SSI_DWL_16  1u
/* Status-word ready bits: bit 0 = complete (bld/bf-polled), bit 24. */
#define CDJ850_SSI_READY   0x01000001u

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_SSI_SIZE];
    QEMUTimer *tx_timer;
    bool tx_full[2];
    uint32_t tx_word[2];        /* raw 32-bit SSITDR value */
    uint8_t tx_half[2];         /* 16-bit mode: halves already shifted out */
    uint32_t clock_remainder;   /* fraction of a slot, in ns * 88200 units */
    int64_t last_ns;            /* virtual time the slots were counted up to */
    bool in_tick;
} Cdj850Ssi;

static Cdj850Ssi *cdj850_ssi;

bool cdj850_ssi_tx_dma_request(unsigned ch)
{
    uint32_t cr;
    hwaddr base;

    if (!cdj850_ssi || ch > 1) {
        return false;
    }
    base = ch ? CDJ850_SSI_CR1 : 0;
    cr = ldl_be_p(&cdj850_ssi->reg[base]);
    return !cdj850_ssi->tx_full[ch] &&
           (cr & (CDJ850_SSICR_EN | CDJ850_SSICR_DMEN | CDJ850_SSICR_TRMD)) ==
           (CDJ850_SSICR_EN | CDJ850_SSICR_DMEN | CDJ850_SSICR_TRMD);
}

/*
 * Serial slots are counted from the virtual clock, not one timer per slot.
 * QEMU only services timers between TCG execution slices (about 1 ms on a
 * busy MAIN), so a per-slot timer chain (SSI tick -> DMAC timer -> SSITDR
 * -> next tick) moved one word per slice: a 45-word audio block took ~70 ms
 * instead of ~1 ms, MAIN's DEI2 handler (which re-enables IRQ7/DSPDREQ) ran
 * late, and the DSP was fed ~5 blocks/s instead of 525. Each tick now
 * shifts every slot that has elapsed and refills SSITDR from the DMAC
 * synchronously, so a block completes on time in virtual time.
 */
#define CDJ850_SSI_RATE      88200u      /* serial words per second */
#define CDJ850_SSI_TICK_NS   250000      /* nominal tick; work is clock-based */
#define CDJ850_SSI_MAX_SLOTS 8192u       /* catch-up cap after a long stall */

static bool cdj850_ssi_slot(Cdj850Ssi *s)
{
    bool running = false;
    unsigned ch;

    for (ch = 0; ch < 2; ch++) {
        uint32_t cr = ldl_be_p(s->reg + (ch ? CDJ850_SSI_CR1 : 0));
        uint8_t dmars = ch ? CDJ850_DMARS_SSI1_TX : CDJ850_DMARS_SSI0_TX;

        if ((cr & (CDJ850_SSICR_EN | CDJ850_SSICR_TRMD)) !=
                  (CDJ850_SSICR_EN | CDJ850_SSICR_TRMD)) {
            continue;
        }
        running = true;
        if (s->tx_full[ch]) {
            uint32_t w = s->tx_word[ch];

            if (CDJ850_SSICR_DWL(cr) == CDJ850_SSI_DWL_16) {
                bool high = (s->tx_half[ch] == 0) == !!(cr & CDJ850_SSICR_PDTA);
                uint32_t half = high ? w >> 16 : w & 0xFFFFu;

                dsp56300_esai_rx_push(ch, half << 8);
                if (++s->tx_half[ch] == 2) {
                    s->tx_half[ch] = 0;
                    s->tx_full[ch] = false;
                }
            } else {
                dsp56300_esai_rx_push(ch, (w >> 8) & 0xFFFFFFu);
                s->tx_full[ch] = false;
            }
        }
        if (cdj850_ssi_tx_dma_request(ch)) {
            cdj850_dmac_serve(dmars);
        }
    }
    return running;
}

static void cdj850_ssi_tx_tick(void *opaque)
{
    Cdj850Ssi *s = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint64_t acc, slots, i;
    bool running = true;

    acc = (uint64_t)(now > s->last_ns ? now - s->last_ns : 0) * CDJ850_SSI_RATE +
          s->clock_remainder;
    slots = acc / 1000000000ULL;
    s->clock_remainder = acc % 1000000000ULL;
    s->last_ns = now;
    if (slots > CDJ850_SSI_MAX_SLOTS) {
        slots = CDJ850_SSI_MAX_SLOTS;
    }
    s->in_tick = true;
    for (i = 0; i < slots && running; i++) {
        running = cdj850_ssi_slot(s);
    }
    s->in_tick = false;
    if (!slots) {
        running = (ldl_be_p(s->reg) | ldl_be_p(s->reg + CDJ850_SSI_CR1)) &
                  CDJ850_SSICR_EN;
    }
    if (running) {
        timer_mod(s->tx_timer, now + CDJ850_SSI_TICK_NS);
    }
}

/* (Re)start slot counting from now, when the transmitter was idle. */
static void cdj850_ssi_schedule(Cdj850Ssi *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (s->in_tick || timer_pending(s->tx_timer)) {
        return;
    }
    s->last_ns = now;
    s->clock_remainder = 0;
    timer_mod(s->tx_timer, now + 1000000000LL / CDJ850_SSI_RATE);
}

static uint64_t cdj850_ssi_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Ssi *s = opaque;
    uint64_t v = 0;
    unsigned i;
    static const hwaddr st[2] = { CDJ850_SSI_ST0, CDJ850_SSI_ST1 };
    int k;

    for (i = 0; i < size; i++) {
        v = (v << 8) | (off + i < CDJ850_SSI_SIZE ?
                        s->reg[off + i] : 0);
    }
    /* Status words report ready on top of the stored value. */
    for (k = 0; k < 2; k++) {
        unsigned b;

        for (b = 0; b < 4; b++) {
            hwaddr a = st[k] + b;

            if (a < off || a >= off + size) {
                continue;
            }
            if ((CDJ850_SSI_READY >> (b * 8)) & 0xff) {
                unsigned shift = (off + size - 1 - a) * 8;

                v |= (uint64_t)(((CDJ850_SSI_READY >> (b * 8)) & 0xff)
                                << shift);
            }
        }
    }
    if (cdj850_trace()) {
        info_report("cdj850: ssi read +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)v,
                    s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_ssi_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    Cdj850Ssi *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = off + size - 1 - i;

        if (a < CDJ850_SSI_SIZE) {
            s->reg[a] = val >> (8 * i);
        }
    }
    if ((off == CDJ850_SSI_TDR0 || off == CDJ850_SSI_TDR1) && size == 4) {
        unsigned ch = off == CDJ850_SSI_TDR1;
        s->tx_word[ch] = (uint32_t)val;
        s->tx_half[ch] = 0;
        s->tx_full[ch] = true;
    }
    if ((ldl_be_p(s->reg) | ldl_be_p(s->reg + CDJ850_SSI_CR1)) & CDJ850_SSICR_EN) {
        cdj850_ssi_schedule(s);
    }
    if (cdj850_ssi_tx_dma_request(0)) {
        cdj850_dmac_request(CDJ850_DMARS_SSI0_TX);
    }
    if (cdj850_ssi_tx_dma_request(1)) {
        cdj850_dmac_request(CDJ850_DMARS_SSI1_TX);
    }
    if (cdj850_trace()) {
        info_report("cdj850: ssi write +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
}

static const MemoryRegionOps cdj850_ssi_ops = {
    .read = cdj850_ssi_read,
    .write = cdj850_ssi_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_ssi = {
    .name = "cdj850-ssi",
    .version_id = 4,
    .minimum_version_id = 4,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Ssi, CDJ850_SSI_SIZE),
        VMSTATE_TIMER_PTR(tx_timer, Cdj850Ssi),
        VMSTATE_BOOL_ARRAY(tx_full, Cdj850Ssi, 2),
        VMSTATE_UINT32_ARRAY(tx_word, Cdj850Ssi, 2),
        VMSTATE_UINT8_ARRAY(tx_half, Cdj850Ssi, 2),
        VMSTATE_UINT32(clock_remainder, Cdj850Ssi),
        VMSTATE_INT64(last_ns, Cdj850Ssi),
        VMSTATE_END_OF_LIST()
    }
};

static void cdj850_ssi_reset(void *opaque)
{
    Cdj850Ssi *s = opaque;

    timer_del(s->tx_timer);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->tx_full, 0, sizeof(s->tx_full));
    memset(s->tx_word, 0, sizeof(s->tx_word));
    memset(s->tx_half, 0, sizeof(s->tx_half));
    s->clock_remainder = 0;
    s->last_ns = 0;
    s->in_tick = false;
}

void cdj850_ssi_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Ssi *s = g_new0(Cdj850Ssi, 1);

    s->cpu = cpu;
    s->tx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, cdj850_ssi_tx_tick, s);
    cdj850_ssi = s;
    qemu_register_reset(cdj850_ssi_reset, s);
    vmstate_register_any(NULL, &vmstate_cdj850_ssi, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_ssi_ops, s,
                          "cdj850.ssi", CDJ850_SSI_SIZE);
    /* Priority 2: carve out of the on-chip-hi RAM (priority 1). */
    memory_region_add_subregion_overlap(sysmem, CDJ850_SSI_BASE, &s->iomem,
                                        2);
}
