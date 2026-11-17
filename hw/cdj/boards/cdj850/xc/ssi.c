/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "ssi.h"
#include "trace.h"

/*
 * SSI0/SSI1 audio serial ports (NOT a mailbox: SH7263 hardware-manual
 * Table 18.2 names SSICR_0 = 0xFFFFC000, SSISR_0 = 0xFFFFC004,
 * SSITDR_0 = 0xFFFFC008, SSIRDR_0 = 0xFFFFC00C, channel 1 at 0xFFFFC800 —
 * the exact "mailbox" channels; Task 8a §2c):
 * minimal storage stub so the driver-init handshake proceeds.
 *
 * Firmware protocol (driver 0x0C075E40/0x0C075EF6/0x0C075F80/0x0C075FCA,
 * entered after the 0xFFFE7000 serial packet; pcs 0x0C075E5C-0x0C07605E):
 * 14 accesses (status read, clear, CR read, write 0/0x90102 = SSICR config
 * incl. DIEN bit24). Firmware arms SSI0 IRQ pri 7 (IPR14 = 0x7 at
 * 0x0C075EE0) and SSI1/3 + ROM-DEC IRQs (IPR15). There is NO post-init SSI
 * status/data/IRQ wait anywhere in the boot (window silent after init;
 * DMAC DAR = SSITDR with DE=0), so no reply event is stubbed here — a
 * stubbed IRQ would be a fabricated event. The streaming hook
 * (SSITDR + DMAC DAR/CHCR to the audio backend) rides with Task 8d.
 *
 * Data words read back the last-written value (RAM-identical); status
 * words additionally report both ready bits set (always-ready: the real
 * peer answers at once on the bench, and a fake busy delay only risks a
 * firmware-side timeout). The driver has dedicated init paths for a
 * bit0-clear data word (0x0C075F34) that also proceed under always-ready.
 *
 * Carved out of the on-chip-hi RAM window (the boot zero-fill
 * 0xFFFFFCA4-0xFFFFFEA3 does not cover it) with a higher-priority
 * overlap, as notes/cdj850-bringup.md provides.
 */
#define CDJ850_SSI_BASE    0xFFFFC000
#define CDJ850_SSI_SIZE    0x810
#define CDJ850_SSI_ST0     0x004
#define CDJ850_SSI_ST1     0x804
/* Status-word ready bits: bit 0 = complete (bld/bf-polled), bit 24. */
#define CDJ850_SSI_READY   0x01000001u

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_SSI_SIZE];
} Cdj850Ssi;

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
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Ssi, CDJ850_SSI_SIZE),
        VMSTATE_END_OF_LIST()
    }
};

void cdj850_ssi_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Ssi *s = g_new0(Cdj850Ssi, 1);

    s->cpu = cpu;
    vmstate_register_any(NULL, &vmstate_cdj850_ssi, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_ssi_ops, s,
                          "cdj850.ssi", CDJ850_SSI_SIZE);
    /* Priority 2: carve out of the on-chip-hi RAM (priority 1). */
    memory_region_add_subregion_overlap(sysmem, CDJ850_SSI_BASE, &s->iomem,
                                        2);
}
