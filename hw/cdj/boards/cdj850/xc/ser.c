/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "panel_link.h"
#include "ser.h"
#include "trace.h"

/*
 * SSU channel 0 at 0xFFFE7000 (SH7263 manual 16.3): the MAIN<->panel
 * synchronous serial link. Register use, all verified against MAIN's
 * drivers in live SDRAM:
 *   +3 SSER  TE (bit 7) / RE (bit 6). Boot upload: TE only. Panel
 *           exchange (0x0C077570/0x0C0777xx): TE+RE (0xC0).
 *   +0 SSCRH master (MSS, 0x80), +1 SSCRL clock-sync 8-bit (0x40) or
 *           16-bit (0x41), +2 SSMR MSB-first (0xC5/0x82 shapes).
 *   +4 SSSR  status. Bit 2 TDRE (TX-ready, polled between bytes), bit 1
 *           RDRF (RX-full, polled by the full-duplex exchange), bit 3
 *           TEND (transfer end, polled by the 16-bit command driver),
 *           bit 6 ORER (overrun). Reset value 0x04 (TDRE set).
 *   +6 SSTDR0 TX data. +7 SSTDR1 second byte in 16-bit mode.
 *   +0x0A SSRDR0 RX data: reading it clears RDRF (manual 16.3.5).
 *
 * Transfer model: a write to SSTDR kicks a count-based fake transfer
 * (not time-based, so log stalls cannot wedge it). While it runs +4
 * reports 0x08 (bit 3 set, TDRE clear): bit 3 is the transfer-progress
 * bit the drivers' BLD #3 loops spin on, and the `and #4' TDRE poll sees
 * not-ready. When the transfer drains, +4 reports TDRE set and bit 3
 * clear, releasing both loop shapes (this is why the upload tail and the
 * 16-bit post-TX loops, both "spin while bit 3, exit when clear",
 * terminate). If RE was enabled, the transfer latches one panel reply
 * byte (panel_link.c; 0x00 idle/no-key default) and sets RDRF; a latch
 * while RDRF is still set raises ORER instead, keeping the older byte,
 * per the manual. A +4 write of 0 clears the latched RX flags (the
 * drivers only ever write 0, at setup).
 */
#define CDJ850_SER_BASE    0xFFFE7000
#define CDJ850_SER_SIZE    0x10

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_SER_SIZE];
    uint32_t busy_reads;
    uint8_t rx_byte;
    bool rx_ready;
    bool orer;
} Cdj850Ser;

#define CDJ850_SER_BUSY_READS 16
/* Status bits on +4 (SSSR): bit 1 = RX-full (RDRF), bit 2 = TX-ready
 * (TDRE, polled), bit 3 = TEND (Transmit End per SSSR Table 16.3.5, not BS;
 * transfer progress: set while a transfer runs, clear when it drains -- the
 * shape both BLD #3 wait loops
 * require), bit 6 = overrun (ORER). Busy reports bit 3 WITHOUT bit 2, so
 * the firmware's `and #4' poll genuinely waits out the fake transfer
 * instead of seeing ready at once. */
#define CDJ850_SER_RDRF   0x02
#define CDJ850_SER_TXRDY  0x04
#define CDJ850_SER_BUSY   0x08
#define CDJ850_SER_ORER   0x40
/* Enable bits on +3 (SSER): bit 7 = transmit, bit 6 = receive. */
#define CDJ850_SER_TE     0x80
#define CDJ850_SER_RE     0x40

static uint64_t cdj850_ser_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Ser *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        hwaddr a = (off + i) & (CDJ850_SER_SIZE - 1);

        if (a == 0x0A) {
            /* SSRDR0: the panel reply byte; the read clears RDRF. */
            v = (v << 8) | s->rx_byte;
            s->rx_ready = false;
            if (cdj850_trace()) {
                info_report("cdj850: ser RX 0x%02x at pc=0x%08x "
                            "%" PRIu64 " ms", s->rx_byte, s->cpu->env.pc,
                            cdj850_vms());
            }
            continue;
        }
        v = (v << 8) | s->reg[a];
    }
    if (off <= 4 && 4 < off + size) {
        unsigned shift = (off + size - 1 - 4) * 8;

        v &= ~((uint64_t)(CDJ850_SER_RDRF | CDJ850_SER_TXRDY |
                          CDJ850_SER_BUSY | CDJ850_SER_ORER) << shift);
        if (s->busy_reads) {
            s->busy_reads--;
            v |= (uint64_t)CDJ850_SER_BUSY << shift;
        } else {
            v |= (uint64_t)CDJ850_SER_TXRDY << shift;
        }
        if (s->rx_ready) {
            v |= (uint64_t)CDJ850_SER_RDRF << shift;
        }
        if (s->orer) {
            v |= (uint64_t)CDJ850_SER_ORER << shift;
        }
    }
    if (cdj850_trace()) {
        info_report("cdj850: ser read +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)v,
                    s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_ser_write(void *opaque, hwaddr off, uint64_t val,
                              unsigned size)
{
    Cdj850Ser *s = opaque;
    unsigned i;
    bool kick = false;

    for (i = 0; i < size; i++) {
        hwaddr a = (off + size - 1 - i) & (CDJ850_SER_SIZE - 1);
        bool was_re = (s->reg[3] & CDJ850_SER_RE) != 0;

        s->reg[a] = val >> (8 * i);
        if (a == 4 && ((val >> (8 * i)) & 0xff) == 0) {
            /* Flag-clear write (the drivers only ever write 0 here,
             * at setup): drop the latched RX flags. */
            s->rx_ready = false;
            s->orer = false;
        }
        if (a == 3 && !(s->reg[3] & CDJ850_SER_RE)) {
            /* Receive disabled: no stale RDRF may survive it. */
            s->rx_ready = false;
        }
        if (a == 3 && !was_re && (s->reg[3] & CDJ850_SER_RE)) {
            /* Exchange start (SSER RE set): the panel reply frame
             * restarts at byte 0.
             * TODO(Task 8/9): RE-toggle cadence wasn't single-stepped --
             * alignment-consistent with observed RX order 00 00 00 01 01 01
             * but unverified; Task 8 should confirm frame-restart timing. */
            panel_exchange_start();
        }
    }
    if (off <= 3 && 3 < off + size) {
        kick = true;
    }
    if (off <= 6 && 6 < off + size) {
        /* TX byte: the firmware's serial output. */
        unsigned shift = (off + size - 1 - 6) * 8;
        unsigned byte = (val >> shift) & 0xff;

        kick = true;
        if (s->reg[3] & CDJ850_SER_RE) {
            /* Clocked in alongside the TX byte (master mode): latch
             * the panel reply now; the driver collects it after TDRE. */
            if (s->rx_ready) {
                s->orer = true;
            } else {
                s->rx_byte = panel_ssu_rx_byte();
                s->rx_ready = true;
            }
        }
        panel_ssu_tx_byte(byte);
        if (cdj850_trace()) {
            info_report("cdj850: ser TX 0x%02x ('%c') at pc=0x%08x "
                        "%" PRIu64 " ms", byte,
                        (byte >= 32 && byte < 127) ? byte : '.',
                        s->cpu->env.pc, cdj850_vms());
        }
    }
    if (kick) {
        s->busy_reads = CDJ850_SER_BUSY_READS;
    }
    if (cdj850_trace()) {
        info_report("cdj850: ser write +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
}

static const MemoryRegionOps cdj850_ser_ops = {
    .read = cdj850_ser_read,
    .write = cdj850_ser_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_ser = {
    .name = "cdj850-ser",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Ser, CDJ850_SER_SIZE),
        VMSTATE_UINT32(busy_reads, Cdj850Ser),
        VMSTATE_UINT8(rx_byte, Cdj850Ser),
        VMSTATE_BOOL(rx_ready, Cdj850Ser),
        VMSTATE_BOOL(orer, Cdj850Ser),
        VMSTATE_END_OF_LIST()
    }
};

void cdj850_ser_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Ser *s = g_new0(Cdj850Ser, 1);

    s->cpu = cpu;
    vmstate_register_any(NULL, &vmstate_cdj850_ser, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_ser_ops, s,
                          "cdj850.ser", CDJ850_SER_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, CDJ850_SER_BASE, &s->iomem,
                                        1);
}
