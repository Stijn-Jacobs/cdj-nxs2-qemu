/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "adc.h"
#include "trace.h"

/*
 * A/D converter at 0xFFFE5800 (SH7263 manual section 22): ADDRA-H at
 * +0x00..+0x0E, ADCSR at +0x20. Only single mode is modelled, which is
 * all the firmware uses on this boot path.
 *
 * Driver evidence (live boot): ADCSR <- 0x0083 (CKS div, MDS=000 single
 * mode, CH=011 AN3), ADCSR <- 0x2000 (ADST start), then a tight size-2
 * poll of ADCSR until ADF (bit 15) sets, then a 16-bit read of ADDRD
 * (+0x06, AN3's register). The service manual's adjustment page wires
 * this input to the TEMPO slider ("ADIN 3.3 V to 0 V: TEMPO slider"),
 * so AN3 is the tempo slider and the conversion result is its position.
 *
 * Model: an ADST write starts a count-based conversion (same rationale
 * as the SSU fake transfer: not time-based, so log stalls cannot wedge
 * it). While it runs ADCSR reads show ADST set and ADF clear; when it
 * drains, ADST auto-clears, ADF sets (manual 22.4.1), and the result
 * latches into the selected channel's ADDR. The result is mid-scale
 * (0x200 in bits 15-6 = slider centered = 0% pitch, the bench state);
 * every other ADDR reads back the last latched value (power-on zero).
 * ADF clears on a write with bit 15 == 0 after it was read set (the
 * driver's standard read-then-clear). ADIE is never set by the driver,
 * so no ADI interrupt is raised.
 */
#define CDJ850_ADC_BASE    0xFFFE5800
#define CDJ850_ADC_SIZE    0x28
#define CDJ850_ADC_CSR     0x20

#define CDJ850_ADC_ADF    0x8000u
#define CDJ850_ADC_ADST   0x2000u
#define CDJ850_ADC_BUSY_READS 16
/* Mid-scale 10-bit result, stored in ADDR bits 15-6 (manual 22.3.1). */
#define CDJ850_ADC_MID    (0x200u << 6)

typedef struct {
    MemoryRegion iomem;
    SuperHCPU *cpu;
    uint8_t reg[CDJ850_ADC_SIZE];
    uint32_t busy_reads;
    bool converting;
    bool adf;
} Cdj850Adc;

static uint16_t adc_get(const Cdj850Adc *s, hwaddr off)
{
    return (uint16_t)(s->reg[off] << 8 | s->reg[off + 1]);
}

static void adc_put(Cdj850Adc *s, hwaddr off, uint16_t v)
{
    s->reg[off] = v >> 8;
    s->reg[off + 1] = v & 0xff;
}

static uint64_t cdj850_adc_read(void *opaque, hwaddr off, unsigned size)
{
    Cdj850Adc *s = opaque;
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i < size; i++) {
        v = (v << 8) | s->reg[(off + i) & (CDJ850_ADC_SIZE - 1)];
    }
    if (off <= CDJ850_ADC_CSR && CDJ850_ADC_CSR < off + size) {
        unsigned shift = (off + size - 1 - CDJ850_ADC_CSR) * 8;
        uint16_t csr = adc_get(s, CDJ850_ADC_CSR);

        if (s->converting && s->busy_reads) {
            s->busy_reads--;
            if (!s->busy_reads) {
                unsigned ch = csr & 7;
                /* Conversion ends: ADST auto-clears, ADF sets, the
                 * result latches into the selected channel (manual
                 * 22.4.1 steps 2-4). */
                s->converting = false;
                s->adf = true;
                adc_put(s, ch * 2, CDJ850_ADC_MID);
            }
        }
        csr &= ~(CDJ850_ADC_ADF | CDJ850_ADC_ADST);
        if (s->converting) {
            csr |= CDJ850_ADC_ADST;
        }
        if (s->adf) {
            csr |= CDJ850_ADC_ADF;
        }
        v &= ~((uint64_t)0xffff << shift);
        v |= (uint64_t)csr << shift;
    }
    if (cdj850_trace()) {
        info_report("cdj850: adc read +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)v,
                    s->cpu->env.pc, cdj850_vms());
    }
    return v;
}

static void cdj850_adc_write(void *opaque, hwaddr off, uint64_t val,
                             unsigned size)
{
    Cdj850Adc *s = opaque;
    unsigned i;

    for (i = 0; i < size; i++) {
        s->reg[(off + size - 1 - i) & (CDJ850_ADC_SIZE - 1)] = val >> (8 * i);
    }
    if (off <= CDJ850_ADC_CSR && CDJ850_ADC_CSR + 1 < off + size) {
        uint16_t csr = adc_get(s, CDJ850_ADC_CSR);

        if (!(val & CDJ850_ADC_ADF)) {
            s->adf = false;
        }
        /* ADST set starts a conversion; ADST written 0 halts it
         * (manual 22.4: switching mode/channels with ADST set is
         * prohibited, so the driver always stops first). */
        s->converting = (csr & CDJ850_ADC_ADST) != 0;
        if (s->converting) {
            s->busy_reads = CDJ850_ADC_BUSY_READS;
        }
        (void)csr;
    }
    if (cdj850_trace()) {
        info_report("cdj850: adc write +0x%x size %u = 0x%x at pc=0x%08x "
                    "%" PRIu64 " ms", (unsigned)off, size, (unsigned)val,
                    s->cpu->env.pc, cdj850_vms());
    }
}

static const MemoryRegionOps cdj850_adc_ops = {
    .read = cdj850_adc_read,
    .write = cdj850_adc_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static const VMStateDescription vmstate_cdj850_adc = {
    .name = "cdj850-adc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(reg, Cdj850Adc, CDJ850_ADC_SIZE),
        VMSTATE_UINT32(busy_reads, Cdj850Adc),
        VMSTATE_BOOL(converting, Cdj850Adc),
        VMSTATE_BOOL(adf, Cdj850Adc),
        VMSTATE_END_OF_LIST()
    }
};

void cdj850_adc_init(MemoryRegion *sysmem, SuperHCPU *cpu)
{
    Cdj850Adc *s = g_new0(Cdj850Adc, 1);

    s->cpu = cpu;
    vmstate_register_any(NULL, &vmstate_cdj850_adc, s);
    memory_region_init_io(&s->iomem, NULL, &cdj850_adc_ops, s,
                          "cdj850.adc", CDJ850_ADC_SIZE);
    /* Priority 1: sh7263.internal covers this range. */
    memory_region_add_subregion_overlap(sysmem, CDJ850_ADC_BASE, &s->iomem,
                                        1);
}
