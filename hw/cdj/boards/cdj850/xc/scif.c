/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "scif.h"
#include "trace.h"

/*
 * SCIF0 at 0xFFFE8000 / SCIF1 at 0xFFFE8800 (SH7263 manual section 15):
 * TX-ready stub with TX capture. Register use, all verified against
 * MAIN's drivers in live SDRAM:
 *   +0x00 SCSMR / +0x04 SCBRR / +0x08 SCSCR / +0x18 SCFCR / +0x24 SCLSR:
 *     configuration storage (SCIF1: 7-bit chars, TE+RE; SCIF0: RIE + TE
 *     + RE with IPR13 arming SCIF0/1 interrupts).
 *   +0x0C SCFTDR: TX bytes (SCIF0 driver 0x0C0FA9FA waits for SCFSR TDFE
 *     before each byte; SCIF1 driver likewise). Captured under
 *     CDJ850_TRACE; the FIFO is modelled as draining instantly.
 *   +0x10 SCFSR: TDFE (bit 5) + TEND (bit 6) always read back set (reset
 *     values; the instant-drain FIFO never fills), error/RX bits stay
 *     clear. The drivers' read-then-write-0 flag clears stick in storage.
 *   +0x14 SCFRDR: no receive data (nothing is plugged into the service
 *     console; no RX interrupt is ever raised, so the ASCII RX handler
 *     never runs). +0x1C SCFDR: FIFO counts read 0 (empty).
 *
 * SCIF0 is the ASCII service console (its RX handler parses "?", digits
 * and CR), not the panel link (SSU0, xc/ser.c); it is modelled here only
 * because boot's TX path parks on TDFE without it.
 */
#define CDJ850_SCIF_SIZE    0x30
#define CDJ850_SCIF_TX      0x0C
#define CDJ850_SCIF_SR      0x10
#define CDJ850_SCIF_RX      0x14
#define CDJ850_SCIF_COUNT   0x1C
/* SCFSR ready bits: TDFE (TX FIFO empty) + TEND (transmit end). */
#define CDJ850_SCIF_READY   0x0060u

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_SCIF_SIZE];
    unsigned unit;
} Cdj850Scif;

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

static const MemoryRegionOps cdj850_scif_ops = {
    .read = cdj850_scif_read,
    .write = cdj850_scif_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_scif = {
    .name = "cdj850-scif",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Scif, CDJ850_SCIF_SIZE),
        VMSTATE_UINT32(unit, Cdj850Scif),
        VMSTATE_END_OF_LIST()
    }
};

static void cdj850_scif_one(MemoryRegion *sysmem, SuperHCPU *cpu,
                            hwaddr base, unsigned unit, const char *name)
{
    Cdj850Scif *s = g_new0(Cdj850Scif, 1);

    s->cpu = cpu;
    s->unit = unit;
    vmstate_register_any(NULL, &vmstate_cdj850_scif, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_scif_ops, s, name,
                          CDJ850_SCIF_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, base, &s->iomem, 1);
}

void cdj850_scif_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    cdj850_scif_one(sysmem, cpu, 0xFFFE8000, 0, "cdj850.scif0");
    cdj850_scif_one(sysmem, cpu, 0xFFFE8800, 1, "cdj850.scif1");
}
