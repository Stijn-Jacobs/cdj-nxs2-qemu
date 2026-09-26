/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SH7724 MSIOF register file; see cdj.h for the registers modelled. */
#include "cdj.h"
#include "cdj_getenv.h"
typedef struct CdjMsiofState {
    MemoryRegion iomem;
    const char *name;
    bool dsp;                    /* this port carries the DSP peer */
    uint32_t reg[CDJ_MSIOF_SIZE / 4];
    uint64_t reads[CDJ_MSIOF_SIZE / 4];
    uint64_t wr[CDJ_MSIOF_SIZE / 4];
    uint64_t writes;
    Notifier exit;
} CdjMsiofState;

static uint64_t cdj_msiof_read(void *opaque, hwaddr offset, unsigned size)
{
    CdjMsiofState *s = opaque;
    unsigned idx = (offset & (CDJ_MSIOF_SIZE - 1)) >> 2;

    s->reads[idx]++;

    /* Receive FIFO: there is no peer on the other end, so it stays empty. */
    if ((offset & ~3) == CDJ_MSIOF_SIRFDR) {
        return 0;
    }
    /* SIFCTR TFUA reads 0x40 (empty) on the DSP port only; MSIOF1 (the GUI
     * link) keeps plain store-and-return behaviour. */
    if (s->dsp && (offset & ~3) == CDJ_MSIOF_SIFCTR) {
        return (s->reg[idx] & ~(uint32_t)(0x7F << 20))
             | ((uint32_t)CDJ_MSIOF_TFUA_EMPTY << 20);
    }
    return s->reg[idx];
}

static void cdj_msiof_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    CdjMsiofState *s = opaque;

    if ((offset & ~3) == CDJ_MSIOF_SICTR) {
        value &= ~(uint64_t)CDJ_MSIOF_SICTR_RST;   /* reset completes at once */
    }
    s->reg[(offset & (CDJ_MSIOF_SIZE - 1)) >> 2] = (uint32_t)value;
    s->wr[(offset & (CDJ_MSIOF_SIZE - 1)) >> 2]++;
    s->writes++;
}

static const MemoryRegionOps cdj_msiof_ops = {
    .read = cdj_msiof_read,
    .write = cdj_msiof_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void cdj_msiof_summary(Notifier *n, void *opaque)
{
    CdjMsiofState *s = container_of(n, CdjMsiofState, exit);
    unsigned i;
    uint64_t total = 0;

    for (i = 0; i < ARRAY_SIZE(s->reads); i++) {
        total += s->reads[i];
    }
    if (!total && !s->writes) {
        return;
    }

    info_report("%s: %" PRIu64 " reads, %" PRIu64 " writes, SICTR=0x%08x",
                s->name, total, s->writes, s->reg[CDJ_MSIOF_SICTR >> 2]);
    for (i = 0; i < ARRAY_SIZE(s->reads); i++) {
        if (s->reads[i] > 1000) {
            info_report("%s:   +0x%02x read %" PRIu64 " times (spin?)",
                        s->name, i << 2, s->reads[i]);
        }
    }
    for (i = 0; i < ARRAY_SIZE(s->wr); i++) {
        if (s->wr[i]) {
            info_report("%s:   +0x%02x written %" PRIu64 " times (last 0x%08x)",
                        s->name, i << 2, s->wr[i], s->reg[i]);
        }
    }
}

void cdj_msiof(MemoryRegion *sysmem, const char *name, hwaddr addr,
                      bool dsp)
{
    CdjMsiofState *s = g_new0(CdjMsiofState, 1);

    s->name = g_strdup(name);
    s->dsp = dsp;
    s->exit.notify = cdj_msiof_summary;
    qemu_add_exit_notifier(&s->exit);
    memory_region_init_io(&s->iomem, NULL, &cdj_msiof_ops, s, name,
                          CDJ_MSIOF_SIZE);
    memory_region_add_subregion_overlap(sysmem, A7ADDR(addr), &s->iomem, 1);
}

