/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdj_common.h"
#include "cdj_getenv.h"
/*
 * What every board's interrupt controller does around QEMU's sh_intc: set it
 * up from the board's tables, return it to power-on on a machine reset, and
 * report its state at exit. The tables themselves -- which source has which
 * vector, mask bit and priority field -- are the SoC's and stay with the
 * board.
 *
 * sh_intc treats a set mask bit as enabled, where the SH-4A's mask registers
 * mask; a board therefore wires each mask-clear register as sh_intc's set
 * register and the mask register as its clear register.
 */

typedef struct CdjIntc {
    struct intc_desc *desc;
    struct intc_mask_reg *mask;
    int nmask;
    struct intc_prio_reg *prio;
    int nprio;
} CdjIntc;

/* sh_intc keeps its register values in the board's static arrays and nothing
 * else resets them; a stale timer enable would fire before the firmware has
 * refilled its dispatch table, and reset the machine again. */
static void cdj_intc_reset(void *opaque)
{
    CdjIntc *s = opaque;
    int i;

    for (i = 0; i < s->nmask; i++) {
        s->mask[i].value = 0;
    }
    for (i = 0; i < s->nprio; i++) {
        s->prio[i].value = 0;
    }
    for (i = 0; i < s->desc->nr_sources; i++) {
        s->desc->sources[i].asserted = 0;
        s->desc->sources[i].enable_count = 0;
        s->desc->sources[i].pending = 0;
    }
    s->desc->pending = 0;
}

void cdj_intc_setup(MemoryRegion *sysmem, SuperHCPU *cpu, struct intc_desc *desc,
                    int nr_sources,
                    struct intc_mask_reg *mask, int nmask,
                    struct intc_prio_reg *prio, int nprio,
                    struct intc_vect *vect, int nvect,
                    struct intc_group *groups, int ngroups)
{
    CdjIntc *s = g_new0(CdjIntc, 1);

    sh_intc_init(sysmem, desc, nr_sources, mask, nmask, prio, nprio);
    sh_intc_register_sources(desc, vect, nvect, groups, ngroups);
    cpu->env.intc_handle = desc;
    s->desc = desc;
    s->mask = mask;
    s->nmask = nmask;
    s->prio = prio;
    s->nprio = nprio;
    qemu_register_reset(cdj_intc_reset, s);
}

/* sh_intc forwards a source only when enable_count reaches enable_max, and
 * the CPU accepts it only above SR.IMASK; print both for every source that is
 * asserted or pending, and always for vectors in [lo, hi]. */
void cdj_intc_report(struct intc_desc *desc, int nr_sources, uint16_t lo, uint16_t hi)
{
    int i;

    for (i = 1; i < nr_sources && i < desc->nr_sources; i++) {
        struct intc_source *src = &desc->sources[i];

        if (src->asserted || src->pending || (src->vect >= lo && src->vect <= hi)) {
            info_report("intc: src=%u vect=0x%03x asserted=%d pending=%d "
                        "enable_count=%d enable_max=%d prio=%d",
                        i, src->vect, src->asserted, src->pending,
                        src->enable_count, src->enable_max,
                        sh_intc_vector_priority(desc, src->vect));
        }
    }
}
